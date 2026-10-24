#include "BloomAnalysis.h"

#include "Common/RealFFT.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sing
{

namespace
{
// Fine structure is stored as round((ln(value) + logOffset) * logScale) in 16 bits: steps of
// about 0.004 dB over a range of e^-16 to e^16, far finer than anyone can hear.
constexpr float logOffset = 16.0f;
constexpr float logScale = 2048.0f;
constexpr int minSize = 1024;
constexpr int maxFrames = 384;           // caps memory for very long loops
constexpr double pi = 3.14159265358979323846;

// Peaks closer than this to a stronger one are its vibrato sidebands: 3 % of the frequency,
// and at least 8 Hz (set in bins below).
constexpr float vibratoReach = 0.03f;

// The magnitude of a Hann window's spectrum, d bins from its centre, relative to the centre:
// the main lobe, two bins either side.
double hannLobe (double d) noexcept
{
    d = std::abs (d);
    if (d >= 2.0)
        return 0.0;
    if (d < 1.0e-6)
        return 1.0;
    if (std::abs (d - 1.0) < 1.0e-6)
        return 0.5;
    return std::abs (std::sin (pi * d) / (pi * d) / (1.0 - d * d));
}

uint16_t encode (float value) noexcept
{
    const float l = (std::log (std::max (value, 1.0e-30f)) + logOffset) * logScale;
    return (uint16_t) std::clamp (std::lround (l), 0L, 65535L);
}
} // namespace

const float* BloomAnalysis::decodeTable() noexcept
{
    static const auto table = []
    {
        auto t = std::make_unique<std::array<float, 65536>>();
        for (int i = 0; i < 65536; ++i)
            (*t)[(size_t) i] = std::exp ((float) i / logScale - logOffset);
        return t;
    }();
    return table->data();
}

int BloomAnalysis::chooseSize (int bufferSize, double sampleRate, double windowSeconds) noexcept
{
    // The power of two nearest to the window length...
    const double target = std::clamp (windowSeconds, minWindowSeconds, maxWindowSeconds) * sampleRate;
    int size = minSize;
    while ((double) size * 1.414 < target)
        size *= 2;
    // ...but no longer than 98% of the loop, or the window holds the loop more than once and
    // it buzzes at the loop rate. Near that limit every frame holds most of the loop, so
    // Stretch gets very subtle: that is allowed, for a deliberately pushed setting.
    while (size > minSize && (double) size > 0.98 * (double) bufferSize)
        size /= 2;
    return size;
}

std::shared_ptr<const BloomAnalysis> BloomAnalysis::build (const LoopSource& source)
{
    const int length = source.size();
    if (length <= 0)
        return nullptr;

    auto a = std::make_shared<BloomAnalysis>();
    a->size = chooseSize (length, source.sampleRate(), source.settings().bloomWindowSeconds);
    a->bins = a->size / 2 + 1;
    a->binSpacing = (float) (source.sampleRate() / a->size);
    a->envSpacing = (float) (source.sampleRate() * 0.5 / (envelopePoints - 1));

    // Frames every eighth of a window, fewer for very long loops. The hop is not a whole
    // number of samples so the frames tile the loop exactly.
    const double hopTarget = std::max ((double) a->size / 8.0, (double) length / maxFrames);
    a->frames = std::max (1, (int) std::lround ((double) length / hopTarget));
    a->hopSamples = (double) length / a->frames;

    const int n = a->size, bins = a->bins;
    std::vector<float> window ((size_t) n);
    double windowPower = 0.0;
    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (i + 0.5) / n));
        windowPower += (double) window[(size_t) i] * window[(size_t) i];
    }
    const float density = (float) (1.0 / std::sqrt (windowPower));

    // Envelope widths: wider than the gap between harmonics, so the envelope follows the
    // vowel and not the individual harmonics.
    const float pitchHz = source.settings().pitchPeriod > 0.0f ? (float) source.sampleRate() / source.settings().pitchPeriod : 0.0f;
    const float smoothHz = std::max (200.0f, 1.5f * pitchHz);
    const int halfWidth = std::max (1, (int) std::lround (0.5f * smoothHz / a->binSpacing));

    RealFFT fft (n);
    std::vector<float> frame ((size_t) n), spectrum ((size_t) n + 2), mag ((size_t) bins), logMag ((size_t) bins),
        env ((size_t) bins);
    std::vector<double> prefix ((size_t) bins + 1);
    std::vector<int> peakList, owner ((size_t) bins), maxQueue ((size_t) bins);
    std::vector<float> floorEnv ((size_t) bins), logUpper ((size_t) bins);
    const int minReachBins = std::max (1, (int) std::ceil (8.0f / a->binSpacing));
    a->fineData.resize ((size_t) a->frames * (size_t) bins);
    a->envData.resize ((size_t) a->frames * envelopePoints);
    a->peakData.assign ((size_t) a->frames * (size_t) bins, noPeak);

    const int channels = source.numChannels();
    const bool lockHarmonics = source.settings().bloomLockHarmonics;
    a->locked = lockHarmonics;
    for (int f = 0; f < a->frames; ++f)
    {
        // The window is centred on the frame's position, reading round the loop.
        const long centre = std::lround (f * a->hopSamples);
        for (int i = 0; i < n; ++i)
        {
            long idx = (centre - n / 2 + i) % length;
            if (idx < 0)
                idx += length;
            float s = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                s += source.channel (ch)[idx];
            frame[(size_t) i] = s / (float) channels * window[(size_t) i];
        }
        fft.forward (frame.data(), spectrum.data());

        float peak = 0.0f;
        for (int k = 0; k < bins; ++k)
        {
            mag[(size_t) k] = std::hypot (spectrum[(size_t) (2 * k)], spectrum[(size_t) (2 * k + 1)]) * density;
            peak = std::max (peak, mag[(size_t) k]);
        }
        const float floor = std::max (peak * 1.0e-6f, 1.0e-12f);

        // The floor: a moving average of log magnitude, which sits between the harmonics.
        // Harmonics are found against it.
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            logMag[(size_t) k] = std::log (mag[(size_t) k] + floor);
            prefix[(size_t) k + 1] = prefix[(size_t) k] + logMag[(size_t) k];
        }
        for (int k = 0; k < bins; ++k)
        {
            const int lo = std::max (0, k - halfWidth), hi = std::min (bins, k + halfWidth + 1);
            floorEnv[(size_t) k] = std::exp ((float) ((prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo)));
        }

        // The envelope: the shape through the tops of the harmonics (a running maximum,
        // then smoothed). Taking the average instead would sink between harmonics, by an
        // amount that depends on where they fall, and moving the harmonics against it would
        // make some notes far louder than others.
        {
            int head = 0, tail = 0;   // a queue of bins with falling magnitudes
            int next = 0;
            for (int k = 0; k < bins; ++k)
            {
                const int hi = std::min (bins - 1, k + halfWidth);
                for (; next <= hi; ++next)
                {
                    while (tail > head && mag[(size_t) maxQueue[(size_t) tail - 1]] <= mag[(size_t) next])
                        --tail;
                    maxQueue[(size_t) tail++] = next;
                }
                while (maxQueue[(size_t) head] < k - halfWidth)
                    ++head;
                logUpper[(size_t) k] = std::log (mag[(size_t) maxQueue[(size_t) head]] + floor);
            }
        }
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + logUpper[(size_t) k];
        float loudest = 0.0f;
        for (int k = 0; k < bins; ++k)
        {
            const int lo = std::max (0, k - halfWidth), hi = std::min (bins, k + halfWidth + 1);
            env[(size_t) k] = std::exp ((float) ((prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo)));
            loudest = std::max (loudest, env[(size_t) k]);
        }
        // Where the voice has next to nothing (above its top harmonics, say), the envelope is
        // nearly zero and the fine structure, divided by it, would be loud noise. Moved by the
        // pitch to where the envelope is high, that noise would be heard. Keep the envelope
        // within 60 dB of its loudest point.
        for (int k = 0; k < bins; ++k)
            env[(size_t) k] = std::max (env[(size_t) k], loudest * 1.0e-3f);

        // Harmonics: local maxima standing at least 6 dB over the envelope. Vibrato splits a
        // harmonic into several close peaks (sidebands a few Hz apart, since the window is
        // long), and played back steadily those beat against each other at the vibrato rate.
        // So peaks within reach of a stronger one join it, and each harmonic is redrawn as
        // one clean main lobe of the analysis window, at the centre of its energy, holding
        // all of it. What is left between harmonics stays as noise, with random phases.
        // With harmonics unlocked (the classic method) the harmonics are still found, so the
        // Tone control can tell them from the noise, but the spectrum is kept exactly as it is.
        peakList.clear();
        for (int k = 2; k < bins - 2; ++k)
        {
            const float m = mag[(size_t) k];
            if (m >= 2.0f * floorEnv[(size_t) k] && m > mag[(size_t) k - 1] && m >= mag[(size_t) k + 1])
                peakList.push_back (k);
        }
        std::sort (peakList.begin(), peakList.end(), [&] (int x, int y) { return mag[(size_t) x] > mag[(size_t) y]; });

        std::fill (owner.begin(), owner.end(), -1);
        auto* peaks = a->peakData.data() + (size_t) f * (size_t) bins;
        for (const int k : peakList)
        {
            if (owner[(size_t) k] >= 0)
                continue;   // a sideband of a stronger harmonic
            const int reach = std::max (minReachBins, (int) std::ceil (vibratoReach * (float) k));
            int lo = k, hi = k;
            while (lo > 1 && k - lo < reach && owner[(size_t) lo - 1] < 0)
                --lo;
            while (hi < bins - 2 && hi - k < reach && owner[(size_t) hi + 1] < 0)
                ++hi;

            // The harmonic's energy over the noise floor between harmonics, and its centre.
            float floorMag = mag[(size_t) lo];
            for (int b = lo; b <= hi; ++b)
                floorMag = std::min (floorMag, mag[(size_t) b]);
            double energy = 0.0, moment = 0.0;
            for (int b = lo; b <= hi; ++b)
            {
                owner[(size_t) b] = k;
                const double e = (double) mag[(size_t) b] * mag[(size_t) b] - (double) floorMag * floorMag;
                energy += e;
                moment += e * b;
            }
            const double middle = energy > 0.0 ? moment / energy : (double) k;

            if (! lockHarmonics)
            {
                // Classic: just mark the harmonic's main lobe as tonal.
                for (int b = lo; b <= hi; ++b)
                    if (std::abs ((double) b - middle) < 2.0)
                        peaks[b] = (int16_t) std::clamp (std::lround ((middle - (double) b) * peakScale), -32767L, 32767L);
                continue;
            }

            // Redraw: the noise floor everywhere, plus the main lobe at the centre.
            double lobePower = 0.0;
            for (int b = lo; b <= hi; ++b)
            {
                const double w = hannLobe ((double) b - middle);
                lobePower += w * w;
            }
            const double amp = lobePower > 0.0 ? std::sqrt (energy / lobePower) : 0.0;
            for (int b = lo; b <= hi; ++b)
            {
                const double w = hannLobe ((double) b - middle);
                mag[(size_t) b] = (float) std::sqrt ((double) floorMag * floorMag + amp * amp * w * w);
                if (w > 0.0)
                    peaks[b] = (int16_t) std::clamp (std::lround ((middle - (double) b) * peakScale), -32767L, 32767L);
            }
        }

        auto* fine = a->fineData.data() + (size_t) f * (size_t) bins;
        for (int k = 0; k < bins; ++k)
            fine[k] = encode (mag[(size_t) k] / env[(size_t) k]);

        // The envelope, resampled to a fixed number of points.
        auto* e = a->envData.data() + (size_t) f * envelopePoints;
        for (int p = 0; p < envelopePoints; ++p)
        {
            const float pos = (float) p * a->envSpacing / a->binSpacing;
            const int k0 = std::min ((int) pos, bins - 1), k1 = std::min (k0 + 1, bins - 1);
            const float t = pos - (float) k0;
            e[p] = env[(size_t) k0] + (env[(size_t) k1] - env[(size_t) k0]) * t;
        }
    }

    a->onsetList = findOnsets (source);
    return a;
}

std::vector<BloomAnalysis::Onset> BloomAnalysis::findOnsets (const LoopSource& source)
{
    std::vector<Onset> found;
    const int length = source.size();
    constexpr int n = 1024;
    const double rate = source.sampleRate();
    const int hopLen = std::max (64, (int) std::lround (rate * 0.005));   // 5 ms
    if (length < 4 * n)
        return found;
    const int count = length / hopLen;

    // 24 bands, evenly spaced in log frequency from 100 Hz to 12 kHz (or Nyquist).
    constexpr int bands = 24;
    const double topHz = std::min (12000.0, 0.45 * rate);
    std::array<int, bands + 1> edges {};
    for (int b = 0; b <= bands; ++b)
        edges[(size_t) b] = std::clamp ((int) std::lround (100.0 * std::pow (topHz / 100.0, (double) b / bands) * n / rate), 1, n / 2);

    RealFFT fft (n);
    std::vector<float> window ((size_t) n), frame ((size_t) n), spectrum ((size_t) n + 2);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (i + 0.5) / n));
    std::vector<float> level ((size_t) count * bands);
    const int channels = source.numChannels();
    float loudest = -1000.0f;
    for (int t = 0; t < count; ++t)
    {
        const long centre = (long) t * hopLen;
        for (int i = 0; i < n; ++i)
        {
            long idx = (centre - n / 2 + i) % length;
            if (idx < 0)
                idx += length;
            float v = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                v += source.channel (ch)[idx];
            frame[(size_t) i] = v * window[(size_t) i];
        }
        fft.forward (frame.data(), spectrum.data());
        for (int b = 0; b < bands; ++b)
        {
            double e = 1.0e-20;
            for (int k = edges[(size_t) b]; k < std::max (edges[(size_t) b] + 1, edges[(size_t) b + 1]); ++k)
                e += (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)]
                   + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
            const auto d = (float) (10.0 * std::log10 (e));
            level[(size_t) t * bands + (size_t) b] = d;
            loudest = std::max (loudest, d);
        }
    }

    // Quiet bands can't make an onset: anything 50 dB under the loudest band counts as silence.
    for (auto& d : level)
        d = std::max (d, loudest - 50.0f);

    // The flux: the average rise in dB over the bands, one frame against the frame 10 ms
    // before (rises spread over two frames still count in full).
    std::vector<float> flux ((size_t) count);
    for (int t = 0; t < count; ++t)
    {
        const int before = (t - 2 + count) % count;
        float rise = 0.0f;
        for (int b = 0; b < bands; ++b)
            rise += std::max (0.0f, level[(size_t) t * bands + (size_t) b] - level[(size_t) before * bands + (size_t) b]);
        flux[(size_t) t] = rise / bands;
    }

    // Peaks over the local median (about 0.2 s either side), the strongest within 50 ms.
    constexpr float minRise = 4.0f;    // dB on average over the bands, over the median
    constexpr float fullRise = 12.0f;  // a rise this far over the median is strength 1
    const int span = std::max (1, (int) std::lround (0.2 * rate / hopLen));
    const int near = std::max (1, (int) std::lround (0.05 * rate / hopLen));
    std::vector<float> around;
    for (int t = 0; t < count; ++t)
    {
        const float x = flux[(size_t) t];
        if (x < minRise)
            continue;
        bool peak = true;
        for (int d = 1; d <= near && peak; ++d)
            peak = x >= flux[(size_t) ((t + d) % count)] && x > flux[(size_t) ((t - d + count) % count)];
        if (! peak)
            continue;
        around.clear();
        for (int d = -span; d <= span; ++d)
            around.push_back (flux[(size_t) ((t + d + count) % count)]);
        std::nth_element (around.begin(), around.begin() + (long) around.size() / 2, around.end());
        const float over = x - around[around.size() / 2];
        if (over >= minRise)
            found.push_back ({ (double) t * hopLen, std::clamp ((over - minRise) / (fullRise - minRise), 0.0f, 1.0f) * 0.75f + 0.25f });
    }
    return found;
}

} // namespace sing
