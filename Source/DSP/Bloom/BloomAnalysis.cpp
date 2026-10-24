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
constexpr double targetSeconds = 0.68;   // 32768 samples at 48 kHz
constexpr int minSize = 1024;
constexpr int maxFrames = 384;           // caps memory for very long loops
constexpr double pi = 3.14159265358979323846;

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

int BloomAnalysis::chooseSize (int bufferSize, double sampleRate) noexcept
{
    // The power of two nearest to the target length...
    const double target = targetSeconds * sampleRate;
    int size = minSize;
    while ((double) size * 1.414 < target)
        size *= 2;
    // ...but no longer than the loop, or the window holds the loop more than once and it
    // buzzes at the loop rate.
    while (size > minSize && size > bufferSize)
        size /= 2;
    return size;
}

std::shared_ptr<const BloomAnalysis> BloomAnalysis::build (const LoopSource& source)
{
    const int length = source.size();
    if (length <= 0)
        return nullptr;

    auto a = std::make_shared<BloomAnalysis>();
    a->size = chooseSize (length, source.sampleRate());
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

    // Envelope smoothing: wider than the gap between harmonics, so it follows the vowel and
    // not the individual harmonics.
    const float pitchHz = source.settings().pitchPeriod > 0.0f ? (float) source.sampleRate() / source.settings().pitchPeriod : 0.0f;
    const float smoothHz = std::max (200.0f, 1.5f * pitchHz);
    const int halfWidth = std::max (1, (int) std::lround (0.5f * smoothHz / a->binSpacing));

    RealFFT fft (n);
    std::vector<float> frame ((size_t) n), spectrum ((size_t) n + 2), mag ((size_t) bins), logMag ((size_t) bins),
        env ((size_t) bins);
    std::vector<double> prefix ((size_t) bins + 1);
    a->fineData.resize ((size_t) a->frames * (size_t) bins);
    a->envData.resize ((size_t) a->frames * envelopePoints);

    const int channels = source.numChannels();
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

        // Moving average of log magnitude.
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            logMag[(size_t) k] = std::log (mag[(size_t) k] + floor);
            prefix[(size_t) k + 1] = prefix[(size_t) k] + logMag[(size_t) k];
        }
        for (int k = 0; k < bins; ++k)
        {
            const int lo = std::max (0, k - halfWidth), hi = std::min (bins, k + halfWidth + 1);
            env[(size_t) k] = std::exp ((float) ((prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo)));
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

    return a;
}

} // namespace sing
