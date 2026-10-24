#pragma once

#include "Effects/Aurora.h"

namespace sing
{

// Space: a long reverb on the whole instrument, so the pad blooms into a huge wash. It is
// Overcast's Aurora machine (whose core follows Cloud Seed by Ghost Note Audio, MIT) with
// one knob that sets everything at once:
//
// | Space | Size   | Decay | Pre-delay | Wet          | Dry   |
// | 0     | (off: the signal passes untouched)                |
// | 0.25  | small  | 3.6 s | 18 ms     | quiet        | full  |
// | 0.5   | medium | 6.3 s | 25 ms     | even         | 0.9   |
// | 1     | huge   | 20 s  | 40 ms     | mostly wet   | 0.6   |
//
// The reverb hears the voice through a low cut (150 Hz), so a long tail stays clear
// instead of booming. Its modulation and diffusion are fixed at Overcast's pad settings.
//
// Threading: prepare() allocates. process() is real-time safe. When Space is turned down
// to 0, the wet fades out, and once it is silent the reverb stops running (and costs
// nothing) until Space comes back up.
class Space
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void setAmount (float amount) noexcept;   // the Space knob, 0 to 1

    // In place, stereo.
    void process (float* left, float* right, int numSamples) noexcept;

    bool isRunning() const noexcept { return running; }

    // The knob's settings, for the docs and tests.
    struct Mapping
    {
        float size, decaySeconds, preDelayMs, wet, dry;
    };
    static Mapping mapping (float amount) noexcept;

private:
    double rate = 48000.0;
    reverb::Aurora aurora;
    reverb::DelayLine preDelayL, preDelayR;
    reverb::Smoother wetGain, dryGain, preDelay;
    reverb::OnePoleLowpass lowL, lowR;
    float lowCoeff = 0.0f;
    std::vector<float> sendL, sendR, wetL, wetR;
    float amount = 0.0f;
    bool running = false;
    int silentSamples = 0;
};

} // namespace sing
