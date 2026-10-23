#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace sing::dsp
{

// In-place radix-2 complex FFT. Forward uses e^(-i...), inverse e^(+i...) without scaling.
class Fft
{
public:
    void prepare (int size)
    {
        n = size;
        bits = 0;
        while ((1 << bits) < n) ++bits;
        twiddle.resize (static_cast<std::size_t> (n / 2));
        for (int k = 0; k < n / 2; ++k)
            twiddle[static_cast<std::size_t> (k)] = std::polar (1.0f, -2.0f * std::numbers::pi_v<float> * static_cast<float> (k) / static_cast<float> (n));
        reversed.resize (static_cast<std::size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed[static_cast<std::size_t> (i)] = r;
        }
    }

    int size() const noexcept { return n; }

    void transform (std::complex<float>* x, bool inverse) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int r = reversed[static_cast<std::size_t> (i)];
            if (r > i) std::swap (x[i], x[r]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, stride = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    auto w = twiddle[static_cast<std::size_t> (k * stride)];
                    if (inverse) w = std::conj (w);
                    const auto a = x[i + k];
                    const auto b = x[i + k + half] * w;
                    x[i + k] = a + b;
                    x[i + k + half] = a - b;
                }
        }
    }

private:
    int n = 0, bits = 0;
    std::vector<std::complex<float>> twiddle;
    std::vector<int> reversed;
};

// Moves the vowel's character (its formants) up or down without changing pitch.
//
// Short-time Fourier analysis (Hann windows, 75% overlap). For each frame the smooth
// spectral envelope is found with the cepstrum (the log spectrum with its fine harmonic
// ripple lifted off), then every bin is scaled by envelope(f / shift) / envelope(f).
// The harmonics stay where they are; the envelope over them slides. Both channels get
// the same gains, so the stereo image is untouched. At zero shift every gain is exactly
// one, and the analysis and resynthesis windows reconstruct the input perfectly, so it is
// always running and the delay never changes.
class FormantShifter
{
public:
    void prepare (double sampleRate)
    {
        frame = 1024;
        while (frame * 48000.0 < sampleRate * 1024.0 * 0.75) frame *= 2; // about 21 ms at any rate
        hop = frame / 4;
        fft.prepare (frame);
        window.resize (static_cast<std::size_t> (frame));
        for (int i = 0; i < frame; ++i)
            window[static_cast<std::size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * std::numbers::pi_v<float> * static_cast<float> (i) / static_cast<float> (frame));
        lifter = std::max (8, static_cast<int> (sampleRate * 0.0011));
        inL.assign (static_cast<std::size_t> (frame), 0.0f);
        inR.assign (static_cast<std::size_t> (frame), 0.0f);
        accL.assign (static_cast<std::size_t> (2 * frame), 0.0f);
        accR.assign (static_cast<std::size_t> (2 * frame), 0.0f);
        spec.resize (static_cast<std::size_t> (frame));
        cep.resize (static_cast<std::size_t> (frame));
        logEnv.resize (static_cast<std::size_t> (frame / 2 + 1));
        gains.resize (static_cast<std::size_t> (frame / 2 + 1));
        reset();
    }

    void reset()
    {
        std::fill (inL.begin(), inL.end(), 0.0f);
        std::fill (inR.begin(), inR.end(), 0.0f);
        std::fill (accL.begin(), accL.end(), 0.0f);
        std::fill (accR.begin(), accR.end(), 0.0f);
        writePos = 0;
        counter = 0;
        time = 0;
    }

    int latency() const noexcept { return frame; }

    // Semitones: + makes the voice smaller and brighter, - larger and darker.
    void setShift (float semitones) noexcept { shift = std::exp2 (std::clamp (semitones, -24.0f, 24.0f) / 12.0f); }

    void process (float& l, float& r) noexcept
    {
        const auto f = static_cast<std::size_t> (frame);
        inL[writePos] = l;
        inR[writePos] = r;
        writePos = (writePos + 1) % f;

        const auto t = static_cast<std::size_t> (time % (2 * frame));
        l = accL[t];
        r = accR[t];
        accL[t] = 0.0f;
        accR[t] = 0.0f;
        ++time;

        if (++counter >= hop)
        {
            counter = 0;
            processFrame();
        }
    }

private:
    void processFrame() noexcept
    {
        const int n = frame;
        const auto f = static_cast<std::size_t> (n);

        // The last `frame` inputs, oldest first, windowed; left in real, right in imaginary.
        for (std::size_t i = 0; i < f; ++i)
        {
            const auto src = (writePos + i) % f;
            spec[i] = { inL[src] * window[i], inR[src] * window[i] };
        }
        fft.transform (spec.data(), false);

        const bool active = std::abs (shift - 1.0f) > 1.0e-4f;
        if (active)
        {
            // Power of both channels per bin, then its log (half the log power = log magnitude).
            for (int k = 0; k <= n / 2; ++k)
            {
                const auto a = spec[static_cast<std::size_t> (k)];
                const auto b = std::conj (spec[static_cast<std::size_t> ((n - k) % n)]);
                const auto xl = 0.5f * (a + b);
                const auto xr = std::complex<float> (0.0f, -0.5f) * (a - b);
                const float power = 0.5f * (std::norm (xl) + std::norm (xr));
                const float lv = 0.5f * std::log (power + 1.0e-12f);
                cep[static_cast<std::size_t> (k)] = { lv, 0.0f };
                if (k > 0 && k < n / 2)
                    cep[static_cast<std::size_t> (n - k)] = { lv, 0.0f };
            }
            fft.transform (cep.data(), true);
            // Keep the low quefrencies (the envelope), with a short taper.
            for (int q = 0; q < n; ++q)
            {
                const int d = std::min (q, n - q);
                float w = 0.0f;
                if (d < lifter) w = 1.0f;
                else if (d < lifter + lifter / 2) w = 0.5f + 0.5f * std::cos (std::numbers::pi_v<float> * static_cast<float> (d - lifter) / static_cast<float> (lifter / 2));
                cep[static_cast<std::size_t> (q)] *= w / static_cast<float> (n);
            }
            fft.transform (cep.data(), false);
            for (int k = 0; k <= n / 2; ++k)
                logEnv[static_cast<std::size_t> (k)] = cep[static_cast<std::size_t> (k)].real();

            // gain(f) = env(f / shift) / env(f), limited to +-24 dB.
            const float top = static_cast<float> (n / 2);
            for (int k = 0; k <= n / 2; ++k)
            {
                const float src = std::min (static_cast<float> (k) / shift, top);
                const int i0 = static_cast<int> (src);
                const int i1 = std::min (i0 + 1, n / 2);
                const float fr = src - static_cast<float> (i0);
                const float warped = logEnv[static_cast<std::size_t> (i0)] + fr * (logEnv[static_cast<std::size_t> (i1)] - logEnv[static_cast<std::size_t> (i0)]);
                gains[static_cast<std::size_t> (k)] = std::exp (std::clamp (warped - logEnv[static_cast<std::size_t> (k)], -2.763f, 2.763f));
            }
            // A real gain on both halves of the packed spectrum keeps both channels real.
            for (int k = 0; k < n; ++k)
            {
                const int bin = k <= n / 2 ? k : n - k;
                spec[static_cast<std::size_t> (k)] *= gains[static_cast<std::size_t> (bin)];
            }
        }

        fft.transform (spec.data(), true);

        // Overlap-add. Hann analysis times Hann synthesis at 75% overlap sums to 1.5.
        const float scale = 1.0f / (1.5f * static_cast<float> (n));
        const auto base = static_cast<std::size_t> (time % (2 * frame));
        for (std::size_t i = 0; i < f; ++i)
        {
            const auto t = (base + i) % (2 * f);
            accL[t] += spec[i].real() * window[i] * scale;
            accR[t] += spec[i].imag() * window[i] * scale;
        }
    }

    Fft fft;
    int frame = 1024, hop = 256, lifter = 53;
    float shift = 1.0f;
    std::vector<float> window, inL, inR, accL, accR, logEnv, gains;
    std::vector<std::complex<float>> spec, cep;
    std::size_t writePos = 0;
    int counter = 0;
    long long time = 0;
};

} // namespace sing::dsp
