#include "LoopSource.h"

#include "StretchEngine.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double halfPi = 1.57079632679489661923;

// Renders one channel of the loop, forwards, with the seam crossfaded.
// Returns the samples; fadeUsed reports the crossfade length actually applied.
std::vector<float> renderForwardLoop (const float* x, int numSamples, const LoopSettings& loop, int& fadeUsed)
{
    const int s = loop.start;
    const int len = loop.length;
    const int fade = std::clamp (loop.fade, 0, len / 2);

    if (fade == 0)
    {
        fadeUsed = 0;
        return std::vector<float> (x + s, x + s + len);
    }

    // Preferred: crossfade the loop's tail into the audio just before Start. The loop keeps its
    // exact length, and the last sample flows into Start as it did in the recording.
    if (s >= fade)
    {
        std::vector<float> out (x + s, x + s + len);
        for (int i = 0; i < fade; ++i)
        {
            const double t = ((double) i + 0.5) / (double) fade;
            const double gIn = std::sin (t * halfPi);
            const double gOut = std::cos (t * halfPi);
            out[(size_t) (len - fade + i)] = (float) (gOut * x[s + len - fade + i] + gIn * x[s - fade + i]);
        }
        fadeUsed = fade;
        return out;
    }

    // Start is too close to the beginning of the voice for that. Crossfade the loop's own tail
    // over its head instead, which shortens the loop by the fade length.
    (void) numSamples;
    const int outLen = len - fade;
    std::vector<float> out (x + s, x + s + outLen);
    for (int i = 0; i < fade; ++i)
    {
        const double t = ((double) i + 0.5) / (double) fade;
        const double gIn = std::sin (t * halfPi);
        const double gOut = std::cos (t * halfPi);
        out[(size_t) i] = (float) (gIn * x[s + i] + gOut * x[s + outLen + i]);
    }
    fadeUsed = fade;
    return out;
}
} // namespace

std::shared_ptr<const LoopSource> LoopSource::build (const float* const* input,
                                                     int numInputChannels,
                                                     int numSamples,
                                                     double sampleRate,
                                                     LoopSettings settings)
{
    if (input == nullptr || numInputChannels <= 0 || numSamples < 4 || sampleRate <= 0.0)
        return nullptr;

    settings.start = std::clamp (settings.start, 0, numSamples - 4);
    settings.length = std::clamp (settings.length <= 0 ? numSamples - settings.start : settings.length,
                                  4, numSamples - settings.start);

    std::shared_ptr<LoopSource> source (new LoopSource());
    source->rate = sampleRate;
    source->loop = settings;

    const int numChannels = std::min (numInputChannels, 2);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto loop = renderForwardLoop (input[ch], numSamples, settings, source->fadeUsed);

        switch (settings.direction)
        {
            case LoopDirection::forward:
                break;

            case LoopDirection::backward:
                std::reverse (loop.begin(), loop.end());
                break;

            case LoopDirection::pingPong:
            {
                // Bounce: no seam to hide, so use the raw loop without the crossfade.
                std::vector<float> raw (input[ch] + settings.start, input[ch] + settings.start + settings.length);
                loop = raw;
                loop.insert (loop.end(), raw.rbegin() + 1, raw.rend() - 1);
                source->fadeUsed = 0;
                break;
            }
        }

        source->bufferSize = (int) loop.size();

        // Pad with wrapped-around samples so readers can interpolate across the seam
        // without checking for it.
        const int n = (int) loop.size();
        std::vector<float> padded ((size_t) (guardBefore + n + guardAfter));
        for (int i = -guardBefore; i < n + guardAfter; ++i)
            padded[(size_t) (i + guardBefore)] = loop[(size_t) (((i % n) + n) % n)];

        source->channels.push_back (std::move (padded));
    }

    for (int m = 0; m < (int) StretchMode::count; ++m)
        source->analyses[(size_t) m] = analyseSource ((StretchMode) m, *source);

    return source;
}

double LoopSource::toVoicePosition (double b) const noexcept
{
    const double n = (double) bufferSize;
    b = std::fmod (b, n);
    if (b < 0.0)
        b += n;

    switch (loop.direction)
    {
        case LoopDirection::forward:  return (double) loop.start + b;
        case LoopDirection::backward: return (double) loop.start + (n - 1.0 - b);
        case LoopDirection::pingPong:
        {
            const double len = (double) loop.length;
            return (double) loop.start + (b < len ? b : 2.0 * len - 2.0 - b);
        }
    }
    return (double) loop.start;
}

double LoopSource::fromVoicePosition (double v) const noexcept
{
    const double n = (double) bufferSize;
    double offset = std::clamp (v - (double) loop.start, 0.0, std::max (0.0, n - 1.0));

    switch (loop.direction)
    {
        case LoopDirection::forward:  return offset;
        case LoopDirection::backward: return n - 1.0 - offset;
        case LoopDirection::pingPong: return std::min (offset, (double) loop.length - 1.0);
    }
    return 0.0;
}

} // namespace sing
