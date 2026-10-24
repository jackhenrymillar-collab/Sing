#pragma once

#include <cstdint>

namespace sing
{

// Air: a gentle lift at the top, with a breath layer that follows the voice.
//
// - A high shelf at 8 kHz, up to +8 dB at full Air.
// - Breath: soft noise, band-passed to 3 to 10 kHz, whose level follows the voice's (an
//   envelope follower on what comes in), so it breathes with the notes and is silent
//   between them. At full Air it sits about 22 dB under the voice. Left and right get
//   different noise, so it is wide.
//
// At 0 it does nothing: the signal passes through untouched.
//
// Threading: everything is real-time safe after prepare().
class Air
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setAmount (float amount) noexcept;   // the Air knob, 0 to 1

    // In place, stereo.
    void process (float* left, float* right, int numSamples) noexcept;

private:
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() noexcept { z1 = z2 = 0.0f; }
    };

    void designShelf (float gainDb) noexcept;
    float noise (uint32_t& state) noexcept;

    double rate = 48000.0;
    float amount = 0.0f, smoothedAmount = 0.0f, smoothCoeff = 0.0f;
    float shelfDbDesigned = -1.0f;
    Biquad shelfL, shelfR;
    Biquad breathHighL, breathHighR, breathLowL, breathLowR;
    float envelope = 0.0f, attack = 0.0f, release = 0.0f;
    uint32_t seedL = 0x12345678u, seedR = 0x9abcdef1u;
};

} // namespace sing
