#pragma once

#include "Effects/Biquad.h"
#include "Effects/ReverbBlocks.h"

#include <vector>

namespace sing
{

// Tape: the warm, worn sound of a voice played back from tape. Four controls, 0 to 1:
//
// - Wow: a slow sway in pitch (about half a hertz, a little uneven, up to about 30 cents),
//   as from a reel that turns unevenly.
// - Flutter: a quick shiver in pitch (7 and 11 Hz, up to about 12 cents).
// - Drive: soft saturation, up to about +18 dB into a tanh curve, with the level made up so
//   the voice stays about as loud.
// - Age: a gentle roll-off of the highs, 12 dB per octave from 22 kHz (0) down to 2.5 kHz (1).
//
// Both channels share one transport, so wow and flutter move them together and the stereo
// image stays put. Wow and flutter are a modulated delay; Tape fades in and out over about 50 ms
// when switched, so the short delay never clicks. Off, it costs nothing and the signal
// passes untouched.
//
// Threading: prepare() allocates. process() is real-time safe.
class Tape
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void set (bool on, float wow, float flutter, float drive, float age) noexcept;

    // In place, stereo.
    void process (float* left, float* right, int numSamples) noexcept;

    bool isRunning() const noexcept { return running; }

    // The drive's gain into the curve for a Drive setting (0 to 1).
    static float driveGainFor (float drive) noexcept;
    // Age's cut-off for an Age setting (0 to 1).
    static float ageHzFor (float age) noexcept;

private:
    double rate = 48000.0;
    reverb::DelayLine lineL, lineR;
    reverb::Smoother mix, wowDepth, flutterDepth, driveGain;
    Biquad ageL, ageR;
    float ageNow = 22000.0f, ageTarget = 22000.0f;
    double wowPhase1 = 0.0, wowPhase2 = 0.0, flutterPhase1 = 0.0, flutterPhase2 = 0.0;
    float drift = 0.0f, driftTarget = 0.0f;
    int driftCountdown = 0;
    uint32_t seed = 0x2545f491u;
    bool on = false, running = false;

    void updateAge (int samples) noexcept;
    float nextRandom() noexcept;
};

} // namespace sing
