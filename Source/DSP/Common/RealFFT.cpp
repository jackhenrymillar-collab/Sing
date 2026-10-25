#include "RealFFT.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

namespace sing
{

namespace
{
constexpr double twoPi = 6.283185307179586476925;
constexpr int maxSharedOrder = 23;   // up to 8M points: a 40 s window at 192 kHz

std::array<std::atomic<const RealFFT*>, maxSharedOrder + 1>& sharedTable()
{
    static std::array<std::atomic<const RealFFT*>, maxSharedOrder + 1> table {};
    return table;
}

int orderOf (int size) noexcept
{
    int order = 0;
    while ((1 << order) < size)
        ++order;
    return (1 << order) == size ? order : -1;
}
} // namespace

void RealFFT::makeShared (int size)
{
    static std::mutex lock;
    static std::array<std::unique_ptr<RealFFT>, maxSharedOrder + 1> owned;
    const int order = orderOf (size);
    if (order < 2 || order > maxSharedOrder)
        return;
    const std::scoped_lock hold (lock);
    if (owned[(size_t) order] == nullptr)
    {
        owned[(size_t) order] = std::make_unique<RealFFT> (size);
        sharedTable()[(size_t) order].store (owned[(size_t) order].get(), std::memory_order_release);
    }
}

const RealFFT* RealFFT::shared (int size) noexcept
{
    const int order = orderOf (size);
    if (order < 2 || order > maxSharedOrder)
        return nullptr;
    return sharedTable()[(size_t) order].load (std::memory_order_acquire);
}

void RealFFT::prepare (int size)
{
    assert (size >= 4 && (size & (size - 1)) == 0);
    n = size;
    m = size / 2;
    stages = 0;
    while ((1 << stages) < m)
        ++stages;

    twiddle.resize ((size_t) m);   // m/2 complex
    for (int k = 0; k < m / 2; ++k)
    {
        twiddle[(size_t) (2 * k)] = (float) std::cos (twoPi * k / m);
        twiddle[(size_t) (2 * k + 1)] = (float) -std::sin (twoPi * k / m);
    }

    split.resize ((size_t) (m + 2));   // m/2 + 1 complex
    for (int k = 0; k <= m / 2; ++k)
    {
        split[(size_t) (2 * k)] = (float) std::cos (twoPi * k / n);
        split[(size_t) (2 * k + 1)] = (float) -std::sin (twoPi * k / n);
    }

    reversed.resize ((size_t) m);
    for (int i = 0; i < m; ++i)
    {
        int r = 0;
        for (int b = 0; b < stages; ++b)
            if (i & (1 << b))
                r |= 1 << (stages - 1 - b);
        reversed[(size_t) i] = r;
    }

    scratch.assign ((size_t) n, 0.0f);
}

void RealFFT::bitReverse (float* z) const noexcept
{
    for (int i = 0; i < m; ++i)
    {
        const int j = reversed[(size_t) i];
        if (j > i)
        {
            std::swap (z[2 * i], z[2 * j]);
            std::swap (z[2 * i + 1], z[2 * j + 1]);
        }
    }
}

void RealFFT::stage (float* z, int s, bool inverse) const noexcept
{
    stagePart (z, s, inverse, 0, m / 2, 1.0f);
}

void RealFFT::stagePart (float* z, int s, bool inverse, int from, int to, float scale) const noexcept
{
    // Stage s combines blocks of 2^s into blocks of 2^(s+1): m / 2 butterflies, of which this
    // runs from to to (butterfly t is pair j = t mod 2^s of block t / 2^s). scale multiplies
    // both outputs of each butterfly (used for the inverse's 1 / m on its last stage).
    const int half = 1 << s;
    const int step = m / (2 * half);   // twiddle stride
    const float sign = inverse ? -1.0f : 1.0f;

    for (int t = from; t < to; ++t)
    {
        const int j = t & (half - 1);
        const int block = (t >> s) << (s + 1);
        const float wr = twiddle[(size_t) (2 * j * step)];
        const float wi = sign * twiddle[(size_t) (2 * j * step + 1)];
        float* a = z + 2 * (block + j);
        float* b = z + 2 * (block + j + half);
        const float tr = b[0] * wr - b[1] * wi;
        const float ti = b[0] * wi + b[1] * wr;
        b[0] = (a[0] - tr) * scale;
        b[1] = (a[1] - ti) * scale;
        a[0] = (a[0] + tr) * scale;
        a[1] = (a[1] + ti) * scale;
    }
}

void RealFFT::complexFFT (float* z, bool inverse) const noexcept
{
    bitReverse (z);
    for (int s = 0; s < stages; ++s)
        stage (z, s, inverse);
}

void RealFFT::forward (const float* input, float* spectrum) noexcept
{
    // Even samples as the real part, odd as the imaginary part.
    float* z = scratch.data();
    for (int i = 0; i < n; ++i)
        z[i] = input[i];
    complexFFT (z, false);

    // Split: E[k] = (Z[k] + conj Z[m-k]) / 2, O[k] = (Z[k] - conj Z[m-k]) / 2i,
    // X[k] = E[k] + W^k O[k] with W = exp(-2 pi i / n).
    for (int k = 0; k <= m / 2; ++k)
    {
        const int mk = (m - k) % m;
        const float zr = z[2 * (k % m)], zi = z[2 * (k % m) + 1];
        const float cr = z[2 * mk], ci = -z[2 * mk + 1];

        const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
        // (a + ib) / 2i = (b - ia) / 2
        const float orr = 0.5f * (zi - ci), oi = -0.5f * (zr - cr);

        const float wr = split[(size_t) (2 * k)], wi = split[(size_t) (2 * k + 1)];
        const float tr = orr * wr - oi * wi, ti = orr * wi + oi * wr;

        spectrum[2 * k] = er + tr;
        spectrum[2 * k + 1] = ei + ti;

        // The mirrored bin m - k: X[m-k] = conj(E[k]) - conj(W^k O[k]) ... from the same E and O.
        if (k != 0 && k != m - k)
        {
            spectrum[2 * (m - k)] = er - tr;
            spectrum[2 * (m - k) + 1] = -(ei - ti);
        }
    }

    // Bins 0 and m (Nyquist) are real.
    const float z0r = z[0], z0i = z[1];
    spectrum[0] = z0r + z0i;
    spectrum[1] = 0.0f;
    spectrum[2 * m] = z0r - z0i;
    spectrum[2 * m + 1] = 0.0f;
}

void RealFFT::unpack (const float* spectrum, float* work, int from, int to) const noexcept
{
    // Back to Z: E[k] = (X[k] + conj X[m-k]) / 2, O[k] = (X[k] - conj X[m-k]) / (2 W^k),
    // Z[k] = E[k] + i O[k].
    for (int k = from; k < to; ++k)
    {
        const float xr = spectrum[2 * k], xi = spectrum[2 * k + 1];
        const float cr = spectrum[2 * (m - k)], ci = -spectrum[2 * (m - k) + 1];

        const float er = 0.5f * (xr + cr), ei = 0.5f * (xi + ci);
        const float dr = 0.5f * (xr - cr), di = 0.5f * (xi - ci);

        // Divide by W^k: multiply by conj(W^k).
        float wr, wi;
        if (k <= m / 2)
        {
            wr = split[(size_t) (2 * k)];
            wi = -split[(size_t) (2 * k + 1)];
        }
        else
        {
            // W^k for k > m/2: W^k = -conj(W^(m-k)), since W^m = -1.
            wr = -split[(size_t) (2 * (m - k))];
            wi = -split[(size_t) (2 * (m - k) + 1)];
        }
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;

        // Z = E + i O
        work[2 * k] = er - oi;
        work[2 * k + 1] = ei + orr;
    }
}

void RealFFT::inverseStep (int step, const float* spectrum, float* work) const noexcept
{
    if (step == 0)
    {
        unpack (spectrum, work, 0, m);
        bitReverse (work);
        return;
    }
    stagePart (work, step - 1, true, 0, m / 2, step == stages ? 1.0f / (float) m : 1.0f);
}

void RealFFT::inverseChunk (int unit, int chunks, const float* spectrum, float* work) const noexcept
{
    const int step = unit / chunks, piece = unit % chunks;
    if (step == 0)
    {
        unpack (spectrum, work, m * piece / chunks, m * (piece + 1) / chunks);
        return;
    }
    if (step == 1)
    {
        // Bit reversal, a share of the indices at a time. Each pair is swapped once, by the
        // piece holding its lower index.
        for (int i = m * piece / chunks, end = m * (piece + 1) / chunks; i < end; ++i)
        {
            const int j = reversed[(size_t) i];
            if (j > i)
            {
                std::swap (work[2 * i], work[2 * j]);
                std::swap (work[2 * i + 1], work[2 * j + 1]);
            }
        }
        return;
    }
    const int s = step - 2;
    const int butterflies = m / 2;
    stagePart (work, s, true, butterflies * piece / chunks, butterflies * (piece + 1) / chunks,
               s == stages - 1 ? 1.0f / (float) m : 1.0f);
}

void RealFFT::inverse (const float* spectrum, float* output) noexcept
{
    for (int s = 0; s < inverseStepCount(); ++s)
        inverseStep (s, spectrum, output);
}

} // namespace sing
