#pragma once

#include "Effects/Aurora.h"
#include "Effects/Biquad.h"
#include "Effects/Shimmer.h"

namespace sing
{

// Space: a long reverb on the whole instrument, so the pad blooms into a huge wash. It is
// Overcast's Aurora machine (whose core follows Cloud Seed by Ghost Note Audio, MIT) with
// three controls:
//
// - Level (the Space knob): how loud the reverb is, and nothing else. 0 is off and the
//   signal passes untouched; the dry voice is never turned down.
// - Decay: how long the tail rings (RT60), 0.5 to 60 s.
// - Size: how big the room is (Aurora's line lengths, about 30 to 300 ms). Bigger rooms
//   also wait a little longer before the reverb starts (pre-delay 10 to 40 ms).
//
// Sessions from before these were split had one knob that set everything at once; see
// legacy() for how such a setting maps onto the three.
//
// The reverb hears the voice through a low cut (150 Hz), so a long tail stays clear
// instead of booming. Its modulation and diffusion are fixed at Overcast's pad settings.
// Its own filter (setFilter) can cut lows and highs from the wet sound alone, 12 dB per
// octave each; the dry voice is untouched.
//
// Shimmer (setShimmer) feeds a pitched-up copy of the send, and of the reverb's own tail,
// back into the reverb, so held notes grow a halo above them; see Shimmer.
//
// Threading: prepare() allocates. process() is real-time safe. When Level is turned down
// to 0, the wet fades out, and once it is silent the reverb stops running (and costs
// nothing) until Level comes back up.
class Space
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void setLevel (float level) noexcept;       // the Space knob, 0 to 1
    void setDecay (float seconds) noexcept;     // RT60, 0.5 to 60 s
    void setSize (float size) noexcept;         // 0 to 1
    // The wet sound's low and high cuts. Off, they open right out. Changes glide over about
    // 50 ms, so moving them never clicks.
    void setFilter (bool on, float lowHz, float highHz) noexcept;
    // Shimmer: amount and rise 0 to 1; interval 0 an octave up, 1 a fifth up.
    void setShimmer (bool on, float amount, float rise, int interval) noexcept;

    // In place, stereo.
    void process (float* left, float* right, int numSamples) noexcept;

    bool isRunning() const noexcept { return running; }

    static constexpr float minDecaySeconds = 0.5f, maxDecaySeconds = 60.0f;

    // The reverb's gain for a Level (0 to 1): an audio taper, with 1 at about 82% and
    // +3.5 dB at the top.
    static float wetGainFor (float level) noexcept;
    static float preDelayMsFor (float size) noexcept;

    // The old one-knob Space: what it set, and the Level, Decay and Size that sound the
    // same. The old knob also turned the dry voice down (to 0.6 at the top); Level keeps
    // the same balance of reverb to voice, so old settings sound the same, the voice up to
    // 4.4 dB louder at the very top.
    struct Legacy
    {
        float size, decaySeconds, preDelayMs, wet, dry;   // what the old knob set
        float level;                                      // the Level with the same balance
    };
    static Legacy legacy (float amount) noexcept;

private:
    double rate = 48000.0;
    reverb::Aurora aurora;
    reverb::DelayLine preDelayL, preDelayR;
    reverb::Smoother wetGain, preDelay;
    reverb::OnePoleLowpass lowL, lowR;
    float lowCoeff = 0.0f;
    std::vector<float> sendL, sendR, wetL, wetR;
    float level = 0.0f, decaySeconds = 6.3f, size = 0.675f;
    bool running = false;
    void updateReverb() noexcept;
    Biquad wetLowL, wetLowR, wetHighL, wetHighR;
    float lowTarget = 10.0f, highTarget = 22000.0f, lowNow = 10.0f, highNow = 22000.0f;
    void updateFilter (int samples) noexcept;
    int silentSamples = 0;
    Shimmer shimmer;
    reverb::DelayLine tailL, tailR;   // the reverb's output, for Shimmer to go round again
    std::vector<float> loopL, loopR, shimL, shimR;
    static constexpr int shimmerChunk = 256;
};

} // namespace sing
