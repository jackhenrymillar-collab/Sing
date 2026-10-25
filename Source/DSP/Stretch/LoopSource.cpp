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
std::vector<float> renderForwardLoop (const float* x, int numSamples, const LoopSettings& loop, int& fadeUsed,
                                      bool linearFade = false)
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
            // Equal power for the long fades, over different sound; equal gain for the short
            // seam fade, over two stretches already matched to each other.
            const double gIn = linearFade ? t : std::sin (t * halfPi);
            const double gOut = linearFade ? 1.0 - t : std::cos (t * halfPi);
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
// Fade 0: no crossfade, but no hard cut at the seam either. Move the loop's end, by up to
// 12 ms either way, to where the voice looks most like it does at Start (the few samples
// either side of each), so the end runs on into Start as if the recording continued. Returns
// the new length. Works on the channels summed, so both channels keep the same loop.
// Returns 0 when there is nothing to match against (no voice before Start or after the end).
int matchSeam (const float* const* input, int numChannels, int numSamples, double sampleRate, int s, int len)
{
    const int reach = std::max (1, (int) std::lround (0.012 * sampleRate));
    const int half = std::max (2, (int) std::lround (0.0015 * sampleRate));
    const auto at = [&] (int i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            v += input[ch][i];
        return v;
    };
    // Compare only where both stretches exist.
    const int before = std::min (half, s);
    int bestEnd = -1;
    double bestCost = -1.0;
    for (int e = s + len - reach; e <= s + len + reach; ++e)
    {
        if (e - s < std::max (4, len / 2) || e + half >= numSamples || e - before < 0)
            continue;
        double cost = 0.0, energy = 1.0e-12;
        for (int i = -before; i < half; ++i)
        {
            const double a = at (e + i), b = at (s + i);
            cost += (a - b) * (a - b);
            energy += a * a + b * b;
        }
        cost /= energy;
        // A slight pull toward the length you set, so a flat stretch doesn't drift it.
        cost += 1.0e-3 * std::abs (e - (s + len)) / reach;
        if (bestCost < 0.0 || cost < bestCost)
        {
            bestCost = cost;
            bestEnd = e;
        }
    }
    return bestEnd < 0 ? 0 : bestEnd - s;
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

    const int numChannels = std::min (numInputChannels, 2);

    // Fade (nearly) 0 on a loop that wraps: match the seam, then smooth what step is left with
    // a 2 ms fade. That is too short to hear as a blend, and long enough not to click.
    const int seamFade = std::max (1, (int) std::lround (0.002 * sampleRate));
    bool seamMatched = false;
    // A loop that starts at the very beginning of the voice, or ends at its very end, has
    // nothing to join to and keeps its plain cut.
    if (settings.direction == LoopDirection::fromStart)
        settings.fade = 0;
    if (settings.direction != LoopDirection::pingPong && settings.direction != LoopDirection::fromStart && settings.fade < seamFade && settings.length > 8 * seamFade
        && settings.start >= seamFade)
        if (const int matched = matchSeam (input, numChannels, numSamples, sampleRate, settings.start, settings.length); matched > 0)
        {
            settings.length = matched;
            settings.fade = seamFade;
            seamMatched = true;
        }

    std::shared_ptr<LoopSource> source (new LoopSource());
    source->rate = sampleRate;
    source->loop = settings;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto loop = renderForwardLoop (input[ch], numSamples, settings, source->fadeUsed, seamMatched);

        switch (settings.direction)
        {
            case LoopDirection::forward:
                break;

            case LoopDirection::backward:
                std::reverse (loop.begin(), loop.end());
                break;

            case LoopDirection::fromStart:
                break;   // Fade is 0, so this is the voice from Start, as recorded

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
        // without checking for it. With no loop, the ends repeat instead.
        const int n = (int) loop.size();
        const bool wrap = settings.direction != LoopDirection::fromStart;
        std::vector<float> padded ((size_t) (guardBefore + n + guardAfter));
        for (int i = -guardBefore; i < n + guardAfter; ++i)
            padded[(size_t) (i + guardBefore)] = loop[(size_t) (wrap ? ((i % n) + n) % n : std::clamp (i, 0, n - 1))];

        source->channels.push_back (std::move (padded));
    }

    for (int m = 0; m < (int) StretchMode::count; ++m)
        if ((StretchMode) m != StretchMode::bloom || settings.analyseForBloom)
            source->analyses[(size_t) m] = analyseSource ((StretchMode) m, *source);

    return source;
}

double LoopSource::toVoicePosition (double b) const noexcept
{
    const double n = (double) bufferSize;
    b = place (b);

    switch (loop.direction)
    {
        case LoopDirection::forward:
        case LoopDirection::fromStart: return (double) loop.start + b;
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
        case LoopDirection::forward:
        case LoopDirection::fromStart: return offset;
        case LoopDirection::backward: return n - 1.0 - offset;
        case LoopDirection::pingPong: return std::min (offset, (double) loop.length - 1.0);
    }
    return 0.0;
}

} // namespace sing
