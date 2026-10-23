#include "VoiceData.h"

#include <algorithm>

namespace sing::dsp
{

namespace
{
    // Level 0 keeps 0.2125 of its (2x) rate: 0.425 of the voice's own rate, so
    // 20.4 kHz for a 48 kHz recording. Stopband from 0.25, the voice's own Nyquist.
    constexpr double kKeep = 0.2125;
    constexpr double kBuildAttenuation = 150.0;

    // Each lower level keeps the same share of its own rate. Its filter (at the rate of
    // the level above) passes kKeep / 2 and stops by 0.1375, so anything not fully
    // removed lands below 0.275 of the new rate, inside the gap the play-time kernels
    // and the output filter take care of.
    constexpr double kDecimateStop = 0.1375;

    std::vector<float> padded (std::size_t length)
    {
        return std::vector<float> (length + 2 * static_cast<std::size_t> (VoiceData::kPad), 0.0f);
    }
}

const std::vector<float>& VoiceData::upsampleFilter()
{
    static const auto h = designLowpass ((kKeep + 0.25) * 0.5, 0.25 - kKeep, kBuildAttenuation, 2.0);
    return h;
}

const std::vector<float>& VoiceData::decimateFilter()
{
    static const auto h = designLowpass ((kKeep * 0.5 + kDecimateStop) * 0.5, kDecimateStop - kKeep * 0.5, kBuildAttenuation);
    return h;
}

std::unique_ptr<VoiceData> VoiceData::build (const float* mono, std::size_t numSamples, double sampleRate, float root)
{
    std::unique_ptr<VoiceData> v (new VoiceData());
    v->sourceRate = sampleRate;
    v->sourceLength = numSamples;
    v->rootNote = root;

    // Level 0: upsample 2x. Output n = sum over taps i of h[i] * u[n - i + half],
    // where u is the voice with zeros between its samples.
    {
        const auto& h = upsampleFilter();
        const auto half = static_cast<std::ptrdiff_t> (h.size() / 2);
        const auto n = numSamples * 2;
        Level level;
        level.length = n;
        level.data = padded (n);

        // The input with enough zeros around it that no bounds checks are needed.
        const auto margin = static_cast<std::size_t> (half / 2 + 2);
        std::vector<float> x (numSamples + 2 * margin, 0.0f);
        std::copy (mono, mono + numSamples, x.begin() + static_cast<std::ptrdiff_t> (margin));

        // Split the filter into its two phases.
        std::vector<float> phase[2];
        for (std::size_t i = 0; i < h.size(); ++i)
            phase[i % 2].push_back (h[i]);

        float* out = level.data.data() + kPad;
        for (std::size_t o = 0; o < n; ++o)
        {
            // Taps i with (o - i + half) even hit real samples m = (o - i + half) / 2.
            const auto oi = static_cast<std::ptrdiff_t> (o);
            const std::size_t parity = static_cast<std::size_t> ((oi + half) & 1);
            const auto& ph = phase[parity];
            // i = parity + 2j  ->  m = (o + half - parity) / 2 - j
            const std::ptrdiff_t m0 = (oi + half - static_cast<std::ptrdiff_t> (parity)) / 2 + static_cast<std::ptrdiff_t> (margin);
            const float* xp = x.data() + m0;
            float acc = 0.0f;
            const auto taps = static_cast<std::ptrdiff_t> (ph.size());
            for (std::ptrdiff_t j = 0; j < taps; ++j)
                acc += ph[static_cast<std::size_t> (j)] * xp[-j];
            out[o] = acc;
        }
        v->levels.push_back (std::move (level));
    }

    // Lower levels, each half the rate of the one above, until they get very short.
    const auto& h = decimateFilter();
    const auto half = static_cast<std::ptrdiff_t> (h.size() / 2);
    for (int k = 1; k < 8; ++k)
    {
        const auto& above = v->levels.back();
        if (above.length < 256)
            break;

        // Pad the level above generously so the filter can run off its ends.
        std::vector<float> x (above.length + 2 * static_cast<std::size_t> (half) + 4, 0.0f);
        std::copy (above.data.begin() + kPad, above.data.begin() + kPad + static_cast<std::ptrdiff_t> (above.length),
                   x.begin() + half + 2);

        Level level;
        level.length = (above.length + 1) / 2;
        level.data = padded (level.length);
        float* out = level.data.data() + kPad;
        for (std::size_t m = 0; m < level.length; ++m)
        {
            // Centred on the level above's sample 2m, so every level lines up in time.
            const float* xp = x.data() + 2 * m + 2;
            float acc = 0.0f;
            for (std::size_t i = 0; i < h.size(); ++i)
                acc += h[i] * xp[i];
            out[m] = acc;
        }
        v->levels.push_back (std::move (level));
    }
    return v;
}

} // namespace sing::dsp
