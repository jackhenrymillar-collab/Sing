#include "VoiceAnalysis.h"

#include "Common/RealFFT.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sing
{

namespace
{
// YIN on one frame. Returns the period in samples, or 0, and the dip depth as aperiodicity.
// YIN steps 3 to 5 on a difference function d (d[tau] for tau 1 to maxLag + 1).
float pickPeriod (const std::vector<float>& d, int minLag, int maxLag, float& aperiodicity);

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
    return pickPeriod (d, minLag, maxLag, aperiodicity);
}

float pickPeriod (const std::vector<float>& d, int minLag, int maxLag, float& aperiodicity)
{
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

float PitchTrack::periodAt (double position) const noexcept
{
    if (periods.empty())
        return 0.0f;
    const long i = std::lround ((position - firstCentre) / hopSamples);
    return periods[(size_t) std::clamp (i, 0L, (long) periods.size() - 1)];
}

PitchTrack trackPitch (const float* x, int numSamples, double sampleRate, float minHz, float maxHz, double hopSeconds)
{
    PitchTrack track;
    if (x == nullptr || sampleRate <= 0.0 || minHz <= 0.0f || maxHz <= minHz)
        return track;

    const int minLag = std::max (2, (int) std::floor (sampleRate / maxHz));
    const int maxLag = (int) std::ceil (sampleRate / minHz);
    const int window = std::max (maxLag, (int) (0.025 * sampleRate));
    const int span = window + maxLag + 2;
    if (numSamples < span)
        return track;

    track.hopSamples = std::max (1.0, hopSeconds * sampleRate);
    track.firstCentre = 0.5 * span;
    const int frames = 1 + (int) ((numSamples - span) / track.hopSamples);

    // The difference function through FFTs: d(tau) = E(a) + E(b shifted by tau) - 2 r(tau),
    // where r is the cross-correlation of the window with the span after it.
    int size = 4;
    while (size < window + span)
        size *= 2;
    RealFFT fft (size);
    std::vector<float> a ((size_t) size), b ((size_t) size), fa ((size_t) size + 2), fb ((size_t) size + 2), r ((size_t) size);
    std::vector<double> energy ((size_t) numSamples + 1, 0.0);
    for (int i = 0; i < numSamples; ++i)
        energy[(size_t) i + 1] = energy[(size_t) i] + (double) x[i] * x[i];

    double loudest = 0.0;
    for (int f = 0; f < frames; ++f)
    {
        const int pos = (int) (f * track.hopSamples);
        loudest = std::max (loudest, energy[(size_t) (pos + window)] - energy[(size_t) pos]);
    }

    std::vector<float> d ((size_t) maxLag + 2, 0.0f);
    track.periods.assign ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        const int pos = (int) (f * track.hopSamples);
        const double ea = energy[(size_t) (pos + window)] - energy[(size_t) pos];
        if (ea < loudest * 1.0e-3 || ea <= 0.0)   // more than 30 dB below the loudest frame
            continue;

        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (b.begin(), b.end(), 0.0f);
        std::copy (x + pos, x + pos + window, a.begin());
        std::copy (x + pos, x + pos + span, b.begin());
        fft.forward (a.data(), fa.data());
        fft.forward (b.data(), fb.data());
        for (int k = 0; k <= size / 2; ++k)
        {
            // conj(A) * B
            const float ar = fa[(size_t) (2 * k)], ai = -fa[(size_t) (2 * k + 1)];
            const float br = fb[(size_t) (2 * k)], bi = fb[(size_t) (2 * k + 1)];
            fa[(size_t) (2 * k)] = ar * br - ai * bi;
            fa[(size_t) (2 * k + 1)] = ar * bi + ai * br;
        }
        fft.inverse (fa.data(), r.data());

        for (int tau = 1; tau <= maxLag + 1; ++tau)
        {
            const double eb = energy[(size_t) (pos + tau + window)] - energy[(size_t) (pos + tau)];
            d[(size_t) tau] = (float) std::max (0.0, ea + eb - 2.0 * (double) r[(size_t) tau]);
        }

        float ap = 1.0f;
        const float period = pickPeriod (d, minLag, maxLag, ap);
        if (ap < 0.2f && period > 0.0f)
            track.periods[(size_t) f] = period;
    }

    // A median of five smooths over single-frame mistakes (octave jumps, a stray unvoiced
    // frame inside a vowel).
    std::vector<float> smoothed (track.periods.size());
    for (int f = 0; f < frames; ++f)
    {
        std::array<float, 5> w {};
        for (int j = -2; j <= 2; ++j)
            w[(size_t) (j + 2)] = track.periods[(size_t) std::clamp (f + j, 0, frames - 1)];
        std::sort (w.begin(), w.end());
        smoothed[(size_t) f] = w[2];
    }
    track.periods = std::move (smoothed);
    return track;
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
