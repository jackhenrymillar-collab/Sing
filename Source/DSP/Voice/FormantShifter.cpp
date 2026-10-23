#include "FormantShifter.h"

#include <algorithm>
#include <cmath>

namespace sing
{

FormantShifter::FormantShifter()
{
    // Hann window over two periods: 0 at u = -1, 1 at u = 0, 0 at u = +1.
    window.resize ((size_t) windowSize + 1);
    for (int i = 0; i <= windowSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (6.28318530717958647692 * (double) i / (double) windowSize));
}

void FormantShifter::prepare (double sampleRate, float maxPeriodSeconds)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxPeriod = std::max (32.0f, maxPeriodSeconds * (float) rate);

    // Grains read back as far as about 4.5 periods before now.
    const int needed = (int) std::ceil (maxPeriod * 8.0f) + 64;
    ringSize = 1;
    while (ringSize < needed)
        ringSize <<= 1;
    ringMask = ringSize - 1;
    ringL.assign ((size_t) ringSize, 0.0f);
    ringR.assign ((size_t) ringSize, 0.0f);
    start (0.0f, 1.0f);
}

float FormantShifter::periodFor (float requested) const noexcept
{
    const float p = requested >= 2.0f ? requested : unpitchedGrainSeconds * (float) rate;
    return std::clamp (p, 16.0f, maxPeriod);
}

int FormantShifter::start (float periodSamples, float ratio) noexcept
{
    period = periodFor (periodSamples);
    setRatio (ratio);

    // A grain read at ratio r reaches period * |r - 1| ahead of its centre. Leave room for
    // the widest ratio, and for the period growing by half again under a bend or glide.
    delay = std::min ((int) std::ceil (std::min (maxPeriod, period * 1.5f) * (maxRatio - 1.0f)) + 4,
                      (int) maxPeriod + 4);

    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    for (auto& g : grains)
        g.active = false;
    now = 0;
    nextCentre = 0.0;
    return delay;
}

void FormantShifter::setPeriod (float periodSamples) noexcept
{
    period = periodFor (periodSamples);
}

void FormantShifter::setRatio (float ratio) noexcept
{
    targetRatio = std::clamp (ratio, minRatio, maxRatio);
}

void FormantShifter::scheduleGrains() noexcept
{
    // Grains are centred one period apart. Each spans a period either side, so at any moment
    // two overlap and their Hann windows sum to one.
    while (nextCentre - (double) period <= (double) now)
    {
        auto it = std::find_if (grains.begin(), grains.end(), [] (const Grain& g) { return ! g.active; });
        if (it == grains.end())
            return;

        it->active = true;
        it->centre = nextCentre;
        it->halfLength = period;
        it->ratio = targetRatio;
        nextCentre += (double) period;
    }
}

float FormantShifter::read (const std::vector<float>& ring, double t) const noexcept
{
    // 4-point Hermite interpolation, as in the granular engine.
    const double fl = std::floor (t);
    const long long i = (long long) fl;
    const float f = (float) (t - fl);
    const float xm1 = ring[(size_t) ((i - 1) & ringMask)];
    const float x0 = ring[(size_t) (i & ringMask)];
    const float x1 = ring[(size_t) ((i + 1) & ringMask)];
    const float x2 = ring[(size_t) ((i + 2) & ringMask)];
    const float c = 0.5f * (x1 - xm1);
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + 0.5f * (x2 - x0);
    const float b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

void FormantShifter::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const bool stereo = inR != nullptr && outR != nullptr;
    const double halfWindow = (double) (windowSize / 2);

    int done = 0;
    while (done < numSamples)
    {
        scheduleGrains();

        // Work in stretches where the set of sounding grains doesn't change: up to the next
        // grain start or the first grain end.
        long long segmentEnd = now + (numSamples - done);
        segmentEnd = std::min (segmentEnd, (long long) std::ceil (nextCentre - (double) period));
        bool allUnity = true;
        for (const auto& g : grains)
            if (g.active)
            {
                segmentEnd = std::min (segmentEnd, (long long) std::ceil (g.centre + (double) g.halfLength));
                allUnity &= g.ratio == 1.0f;
            }
        const int len = (int) std::max (1LL, segmentEnd - now);

        // The input goes into the ring first; in and out may be the same buffers.
        for (int s = 0; s < len; ++s)
        {
            ringL[(size_t) ((now + s) & ringMask)] = inL[done + s];
            ringR[(size_t) ((now + s) & ringMask)] = stereo ? inR[done + s] : 0.0f;
        }

        float* yL = outL + done;
        float* yR = stereo ? outR + done : nullptr;

        if (allUnity)
        {
            // At ratio 1 every grain reads the input in step and the windows sum to one, so
            // the output is the input, delay samples late.
            for (int s = 0; s < len; ++s)
            {
                const long long src = now + s - delay;
                yL[s] = src >= 0 ? ringL[(size_t) (src & ringMask)] : 0.0f;
                if (stereo)
                    yR[s] = src >= 0 ? ringR[(size_t) (src & ringMask)] : 0.0f;
            }
        }
        else
        {
            std::fill_n (yL, len, 0.0f);
            if (stereo)
                std::fill_n (yR, len, 0.0f);

            for (const auto& g : grains)
            {
                if (! g.active)
                    continue;

                const double t0 = (double) now;
                const double offset0 = t0 - g.centre;
                double wp = (offset0 / (double) g.halfLength + 1.0) * halfWindow;
                const double wStep = halfWindow / (double) g.halfLength;
                double src = g.centre - (double) delay + offset0 * (double) g.ratio;
                const double ratio = (double) g.ratio;

                for (int s = 0; s < len; ++s, wp += wStep, src += ratio)
                {
                    if (wp < 0.0)
                        continue;
                    const int wi = std::min ((int) wp, windowSize - 1);
                    const float w = window[(size_t) wi] + (float) (wp - (double) wi) * (window[(size_t) wi + 1] - window[(size_t) wi]);
                    const double t = t0 + (double) s;
                    // Never read past what has arrived, or further back than the ring holds.
                    const double at = std::clamp (src, t - (double) ringSize + 4.0, t - 2.0);
                    yL[s] += w * read (ringL, at);
                    if (stereo)
                        yR[s] += w * read (ringR, at);
                }
            }
        }

        now += len;
        done += len;
        for (auto& g : grains)
            if (g.active && (double) now >= g.centre + (double) g.halfLength)
                g.active = false;
    }
}

} // namespace sing
