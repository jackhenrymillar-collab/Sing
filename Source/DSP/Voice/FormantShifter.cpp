#include "FormantShifter.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr float maxGain = 16.0f;   // +-24 dB: no bin is pushed further than this
} // namespace

void FormantShifter::prepare (double sampleRate, float maxPeriodSeconds)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // Frames of at least four periods of the lowest pitch, and about 21 ms at the least.
    const double scale = rate / 48000.0;
    minSize = 1024;
    while ((double) minSize < 1024.0 * scale * 0.75)
        minSize *= 2;
    maxSize = minSize;
    while ((double) maxSize < 4.0 * maxPeriodSeconds * rate && maxSize < 16384)
        maxSize *= 2;

    ffts.clear();
    for (int s = minSize; s <= maxSize; s *= 2)
        ffts.emplace_back (s);

    const auto n = (size_t) maxSize;
    for (auto* v : { &inL, &inR, &outL, &outR, &accL, &accR, &frame, &window })
        v->assign (n, 0.0f);
    specL.assign (n + 2, 0.0f);
    specR.assign (n + 2, 0.0f);
    const size_t bins = n / 2 + 1;
    for (auto* v : { &power, &envelope, &logUpper, &gains })
        v->assign (bins, 0.0f);
    prefix.assign (bins + 1, 0.0);
    queue.assign (bins, 0);
    start (0.0f, 1.0f);
}

int FormantShifter::start (float periodSamples, float r) noexcept
{
    setPeriod (periodSamples);
    setRatio (r);

    // The frame: the power of two that holds four periods (unpitched: the shortest).
    size = minSize;
    if (period >= 2.0f)
        while (size < maxSize && (float) size < 4.0f * period)
            size *= 2;
    hop = size / 4;
    fft = &ffts[(size_t) std::lround (std::log2 ((double) size / minSize))];

    // Hann, scaled so analysis times synthesis window overlap-adds to exactly one at 4x.
    const float norm = 1.0f / std::sqrt (1.5f);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = norm * (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size));

    for (auto* v : { &inL, &inR, &outL, &outR, &accL, &accR })
        std::fill (v->begin(), v->end(), 0.0f);
    // Each note runs its frames at its own point in the hop, so a chord's FFTs don't all land
    // in the same audio block. The latency is the same wherever the frames fall.
    static unsigned int notes = 0;
    const auto turn = (unsigned int) (++notes * 2654435761u) >> 8;
    rover = size - hop + (int) ((unsigned long long) turn * (unsigned) hop >> 24);
    return latency();
}

void FormantShifter::setPeriod (float periodSamples) noexcept
{
    period = periodSamples >= 2.0f ? periodSamples : 0.0f;
}

void FormantShifter::setRatio (float r) noexcept
{
    // Within 5 cents of no change, make no change: the shifter then passes the sound
    // through untouched.
    ratio = std::abs (r - 1.0f) < 0.003f ? 1.0f : std::clamp (r, minRatio, maxRatio);
}

void FormantShifter::process (const float* xL, const float* xR, float* yL, float* yR, int numSamples) noexcept
{
    stereo = xR != nullptr && yR != nullptr;
    const int lag = size - hop;
    for (int s = 0; s < numSamples; ++s)
    {
        inL[(size_t) rover] = xL[s];
        inR[(size_t) rover] = stereo ? xR[s] : 0.0f;
        yL[s] = outL[(size_t) (rover - lag)];
        if (stereo)
            yR[s] = outR[(size_t) (rover - lag)];
        if (++rover >= size)
        {
            processFrame();
            rover = lag;
        }
    }
}

void FormantShifter::processFrame() noexcept
{
    const int n = size, bins = size / 2 + 1;
    const float* w = window.data();

    if (ratio == 1.0f)
    {
        // Nothing to reshape: the windows alone, which overlap-add back to the input.
        for (int i = 0; i < n; ++i)
        {
            const float ww = w[i] * w[i];
            accL[(size_t) i] += inL[(size_t) i] * ww;
            accR[(size_t) i] += inR[(size_t) i] * ww;
        }
    }
    else
    {
        // A centred note has the same left and right: one FFT does for both.
        const bool twoChannels = stereo && ! std::equal (inL.begin(), inL.begin() + n, inR.begin());
        for (int i = 0; i < n; ++i)
            frame[(size_t) i] = inL[(size_t) i] * w[i];
        fft->forward (frame.data(), specL.data());
        if (twoChannels)
        {
            for (int i = 0; i < n; ++i)
                frame[(size_t) i] = inR[(size_t) i] * w[i];
            fft->forward (frame.data(), specR.data());
        }
        for (int k = 0; k < bins; ++k)
        {
            float p = specL[(size_t) (2 * k)] * specL[(size_t) (2 * k)] + specL[(size_t) (2 * k + 1)] * specL[(size_t) (2 * k + 1)];
            if (twoChannels)
                p += specR[(size_t) (2 * k)] * specR[(size_t) (2 * k)] + specR[(size_t) (2 * k + 1)] * specR[(size_t) (2 * k + 1)];
            power[(size_t) k] = p;
        }
        traceEnvelope();

        // New envelope over old, bin by bin: the envelope read at frequency / ratio.
        const float inv = 1.0f / ratio;
        gains[0] = 1.0f;
        for (int k = 1; k < bins; ++k)
        {
            const float at = (float) k * inv;
            const int k0 = std::min ((int) at, bins - 1), k1 = std::min (k0 + 1, bins - 1);
            const float t = at - (float) k0;
            const float moved = envelope[(size_t) k0] + t * (envelope[(size_t) k1] - envelope[(size_t) k0]);
            gains[(size_t) k] = std::clamp (moved / envelope[(size_t) k], 1.0f / maxGain, maxGain);
        }
        for (int k = 0; k < bins; ++k)
        {
            const float g = gains[(size_t) k];
            specL[(size_t) (2 * k)] *= g;
            specL[(size_t) (2 * k + 1)] *= g;
            specR[(size_t) (2 * k)] *= g;
            specR[(size_t) (2 * k + 1)] *= g;
        }

        fft->inverse (specL.data(), frame.data());
        for (int i = 0; i < n; ++i)
            accL[(size_t) i] += frame[(size_t) i] * w[i];
        if (twoChannels)
        {
            fft->inverse (specR.data(), frame.data());
            for (int i = 0; i < n; ++i)
                accR[(size_t) i] += frame[(size_t) i] * w[i];
        }
        else if (stereo)
        {
            for (int i = 0; i < n; ++i)
                accR[(size_t) i] += frame[(size_t) i] * w[i];
        }
    }

    // The first hop is finished: hand it out, and move everything on by a hop.
    std::copy_n (accL.begin(), hop, outL.begin());
    std::copy_n (accR.begin(), hop, outR.begin());
    std::copy (accL.begin() + hop, accL.begin() + n, accL.begin());
    std::copy (accR.begin() + hop, accR.begin() + n, accR.begin());
    std::fill (accL.begin() + (n - hop), accL.begin() + n, 0.0f);
    std::fill (accR.begin() + (n - hop), accR.begin() + n, 0.0f);
    std::copy (inL.begin() + hop, inL.begin() + n, inL.begin());
    std::copy (inR.begin() + hop, inR.begin() + n, inR.begin());
}

void FormantShifter::traceEnvelope() noexcept
{
    // The envelope through the tops of the harmonics: a running maximum over about one and a
    // half harmonics (so it never sinks between them), then a moving average of its log to
    // round it off. Unpitched sound uses a fixed 200 Hz.
    const int bins = size / 2 + 1;
    const double binHz = rate / size;
    const double f0 = period >= 2.0f ? rate / period : 200.0;
    const int half = std::max (1, (int) std::lround (0.75 * f0 / binHz));

    float loudest = 0.0f;
    for (int k = 0; k < bins; ++k)
    {
        power[(size_t) k] = std::sqrt (power[(size_t) k]);   // magnitude now
        loudest = std::max (loudest, power[(size_t) k]);
    }
    const float floor = std::max (loudest * 1.0e-4f, 1.0e-12f);

    int head = 0, tail = 0, next = 0;
    for (int k = 0; k < bins; ++k)
    {
        const int hi = std::min (bins - 1, k + half);
        for (; next <= hi; ++next)
        {
            while (tail > head && power[(size_t) queue[(size_t) tail - 1]] <= power[(size_t) next])
                --tail;
            queue[(size_t) tail++] = next;
        }
        while (queue[(size_t) head] < k - half)
            ++head;
        logUpper[(size_t) k] = std::log (power[(size_t) queue[(size_t) head]] + floor);
    }
    prefix[0] = 0.0;
    for (int k = 0; k < bins; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + logUpper[(size_t) k];
    for (int k = 0; k < bins; ++k)
    {
        const int lo = std::max (0, k - half), hi = std::min (bins, k + half + 1);
        envelope[(size_t) k] = std::exp ((float) ((prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo)));
    }
}

} // namespace sing
