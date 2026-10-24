#include "VoiceAnalysis.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sing
{

namespace
{
// YIN on one frame. Returns the period in samples, or 0, and the dip depth as aperiodicity.
float yinPeriod (const float* x, int window, int minLag, int maxLag, float& aperiodicity)
{
    std::vector<float> d ((size_t) maxLag + 2, 0.0f);

    // Step 2: difference function.
    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = (double) x[j] - (double) x[j + tau];
            sum += diff * diff;
        }
        d[(size_t) tau] = (float) sum;
    }

    // Step 3: cumulative mean normalised difference.
    std::vector<float> cmnd (d.size(), 1.0f);
    double running = 0.0;
    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        running += d[(size_t) tau];
        cmnd[(size_t) tau] = running > 0.0 ? (float) (d[(size_t) tau] * (double) tau / running) : 1.0f;
    }

    // Step 4: the first dip below the threshold, followed down to its minimum.
    constexpr float threshold = 0.15f;
    int best = -1;
    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (cmnd[(size_t) tau] < threshold)
        {
            while (tau + 1 <= maxLag && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }

    // No dip under the threshold: take the global minimum, but report it as aperiodic.
    if (best < 0)
    {
        best = minLag;
        for (int tau = minLag; tau <= maxLag; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }
    aperiodicity = cmnd[(size_t) best];

    // Step 5: parabolic interpolation around the dip.
    const float a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
    const float denom = a - 2.0f * b + c;
    const float shift = std::abs (denom) > 1.0e-12f ? 0.5f * (a - c) / denom : 0.0f;
    return (float) best + std::clamp (shift, -1.0f, 1.0f);
}

double rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt (s / std::max (1, n));
}
} // namespace

PitchEstimate detectPitch (const float* x, int numSamples, double sampleRate, float minHz, float maxHz)
{
    PitchEstimate result;
    if (x == nullptr || sampleRate <= 0.0 || minHz <= 0.0f || maxHz <= minHz)
        return result;

    const int minLag = std::max (2, (int) std::floor (sampleRate / maxHz));
    const int maxLag = (int) std::ceil (sampleRate / minHz);
    const int window = std::max (maxLag, (int) (0.025 * sampleRate));
    const int frameSpan = window + maxLag + 2;
    if (numSamples < frameSpan)
        return result;

    // Up to 24 frames spread over the region, skipping quiet ones.
    const TrimRange audible = findAudibleRange (x, numSamples, sampleRate, -30.0f);
    const int from = audible.start;
    const int to = std::max (from, audible.end - frameSpan);
    const int frames = std::clamp ((to - from) / std::max (1, window / 2), 1, 24);

    double peak = 0.0;
    for (int f = 0; f < frames; ++f)
    {
        const int pos = from + (int) ((double) (to - from) * (f + 0.5) / frames);
        peak = std::max (peak, rms (x + pos, window));
    }

    std::vector<float> periods;
    std::vector<float> aperiodicities;
    for (int f = 0; f < frames; ++f)
    {
        const int pos = from + (int) ((double) (to - from) * (f + 0.5) / frames);
        if (rms (x + pos, window) < peak * 0.1)
            continue;

        float ap = 1.0f;
        const float period = yinPeriod (x + pos, window, minLag, maxLag, ap);
        aperiodicities.push_back (ap);
        if (ap < 0.2f && period > 0.0f)
            periods.push_back (period);
    }

    if (aperiodicities.empty())
        return result;

    // Pitched when most measured frames found a clear period.
    if (periods.size() * 2 < aperiodicities.size())
        return result;

    std::sort (periods.begin(), periods.end());
    const float median = periods[periods.size() / 2];
    float meanAp = 0.0f;
    for (float a : aperiodicities)
        meanAp += a;
    meanAp /= (float) aperiodicities.size();

    result.hz = (float) (sampleRate / median);
    result.confidence = std::clamp (1.0f - meanAp, 0.0f, 1.0f);
    return result;
}

TrimRange findAudibleRange (const float* x, int numSamples, double sampleRate, float thresholdDb)
{
    TrimRange r { 0, std::max (0, numSamples) };
    if (x == nullptr || numSamples <= 0)
        return r;

    const int hop = std::max (1, (int) (0.01 * sampleRate));
    double loudest = 0.0;
    for (int i = 0; i < numSamples; i += hop)
        loudest = std::max (loudest, rms (x + i, std::min (hop, numSamples - i)));
    if (loudest <= 0.0)
        return r;

    const double threshold = loudest * std::pow (10.0, thresholdDb / 20.0);
    int first = -1, last = -1;
    for (int i = 0; i < numSamples; i += hop)
        if (rms (x + i, std::min (hop, numSamples - i)) >= threshold)
        {
            if (first < 0)
                first = i;
            last = std::min (numSamples, i + hop);
        }

    if (first < 0)
        return r;

    // Keep 10 ms either side, so soft onsets and tails survive.
    r.start = std::max (0, first - hop);
    r.end = std::min (numSamples, last + hop);
    return r;
}

float hzToMidi (float hz) noexcept
{
    return hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 60.0f;
}

float midiToHz (float note) noexcept
{
    return 440.0f * std::exp2 ((note - 69.0f) / 12.0f);
}

} // namespace sing
