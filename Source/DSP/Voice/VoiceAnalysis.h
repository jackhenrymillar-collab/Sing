#pragma once

namespace sing
{

struct PitchEstimate
{
    float hz = 0.0f;          // 0 when no steady pitch was found
    float confidence = 0.0f;  // 0 to 1: how periodic the voice is where it was measured
    bool pitched() const noexcept { return hz > 0.0f; }
};

// Finds the root pitch of a voice, using the YIN method (de Cheveigné and Kawahara, 2002):
// a difference function, its cumulative mean normalisation, an absolute threshold and
// parabolic interpolation. Frames across the region are measured and the median of the
// confident ones is returned. Breaths and whispers come back unpitched.
PitchEstimate detectPitch (const float* samples, int numSamples, double sampleRate,
                           float minHz = 60.0f, float maxHz = 1000.0f);

// The audible part of a voice: where it rises above, and last falls below, a level relative
// to its loudest 10 ms (default -50 dB).
struct TrimRange
{
    int start = 0;
    int end = 0;   // one past the last sample
};

TrimRange findAudibleRange (const float* samples, int numSamples, double sampleRate, float thresholdDb = -50.0f);

// Pitch helpers.
float hzToMidi (float hz) noexcept;
float midiToHz (float note) noexcept;

} // namespace sing
