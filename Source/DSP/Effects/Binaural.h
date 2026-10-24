#pragma once

#include <array>

namespace sing
{

// Binaural beats: the left channel moved up in frequency by half the beat rate and the right
// moved down by half, so every partial sounds a few Hz apart in each ear, and the two ears
// together hear a slow beat (the brain does the mixing). Amount blends from the plain sound
// to the moved one; in between, each ear also beats a little on its own.
//
// The frequency shift is single-sideband: an analytic signal from a pair of allpass chains
// whose outputs stay 90 degrees apart across the audio band (Olli Niemitalo's published
// polyphase IIR Hilbert transformer), turned by a slow rotating phasor.
//
// Threading: everything is real-time safe.
class Binaural
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void set (bool on, float rateHz, float amount) noexcept;

    // In place, stereo.
    void process (float* left, float* right, int numSamples) noexcept;

private:
    struct Allpass4
    {
        std::array<float, 4> coeff {};
        std::array<float, 4> x1 {}, x2 {}, y1 {}, y2 {};
        float process (float x) noexcept;
        void reset() noexcept { x1 = x2 = y1 = y2 = {}; }
    };
    struct Channel
    {
        Allpass4 a, b;
        float delayed = 0.0f;   // path a, one sample late
        void reset() noexcept { a.reset(); b.reset(); delayed = 0.0f; }
    };

    double rate = 48000.0;
    Channel left, right;
    double phase = 0.0;
    float beatHz = 4.0f;
    float mix = 0.0f, mixTarget = 0.0f;
};

} // namespace sing
