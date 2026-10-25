#pragma once

#include "Effects/Biquad.h"
#include "Effects/ReverbBlocks.h"
#include "Voice/StftPitchShifter.h"

namespace sing
{

// Shimmer: a halo above the voice, made inside the Space reverb. What goes into the reverb
// is also sent through a pitch shifter, an octave or a fifth up, and fed in with it; and the
// reverb's own tail goes round through the shifter again, so each pass rises another step.
//
// - Amount: how much of the shifted voice goes in (the first step of the halo), on a
//   squared scale so the first part of the control stays subtle.
// - Rise: how much of the tail goes round again. Higher climbs further before it fades.
//
// The shift is Sing's STFT pitch shifter (the one the keys use in STFT mode), so every
// partial lands cleanly on the new pitch; a delay-line shifter cancels itself on some notes.
// Its latency (about 85 ms) only delays the halo a little, as a pre-delay would. Before
// the shifter, a band-pass (200 Hz to 6 kHz) keeps rumble out; each pass round climbs out of
// the band, so the halo always fades away. Rise is scaled down for long Decays (a long tail
// gives the loop more to send round), and a soft limit on the loop keeps any setting from
// running away.
//
// Space calls process() once per step with the reverb's input and an earlier step's tail; it
// writes what to add to the input.
//
// Threading: prepare() allocates. Everything else is real-time safe.
class Shimmer
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    // interval: 0 an octave up, 1 a fifth up.
    void set (bool on, float amount, float rise, int interval) noexcept;
    void setDecay (float seconds) noexcept;
    bool isActive() const noexcept { return on || amountNow.getCurrent() > 0.0f || riseNow.getCurrent() > 0.0f; }

    // in: the reverb's send; tail: the reverb's output from the block before. Writes what
    // to add to the send into out. Any of them may be the same buffer as another.
    void process (const float* inL, const float* inR, const float* tailL, const float* tailR,
                  float* outL, float* outR, int numSamples) noexcept;

    static float ratioFor (int interval) noexcept { return interval == 1 ? 1.49830708f : 2.0f; }
    // The loop gain at the top of Rise, before the band-pass and the shift lose some.
    static constexpr float maxRise = 0.4f;

private:
    double rate = 48000.0;
    StftPitchShifter shifter;
    float ratio = 2.0f, decaySeconds = 6.3f, riseSetting = 0.0f;
    void updateRise() noexcept;
    Biquad lowL, lowR, highL, highR;
    reverb::Smoother amountNow, riseNow;
    bool on = false;
};

} // namespace sing
