#pragma once

#include <cstddef>
#include <vector>

namespace sing::dsp
{

// The factory voices, sung by a small formant synthesiser so they need no recordings
// (and so no licences). Harmonics are summed one by one below the Nyquist limit, shaped
// by a vocal tract of five resonances, with breath noise through the same resonances,
// slow vibrato, natural pitch wobble and a soft onset. Deterministic: the same voice
// comes out every time.
enum class FactoryVoice { hollowChoir = 0, distantAh = 1, glassHum = 2, lastLight = 3 };

struct SynthesisedVoice
{
    std::vector<float> samples; // mono
    double sampleRate = 48000.0;
    float rootNote = 60.0f;
};

SynthesisedVoice synthesise (FactoryVoice, double sampleRate = 48000.0);

// Finds the main pitch of a voice (YIN, Cheveigné and Kawahara 2002), as a MIDI note with
// cents. Returns a negative number when there's no clear pitch (a breath, a whisper).
float detectPitch (const float* samples, std::size_t numSamples, double sampleRate);

// Trims leading and trailing near-silence (below -50 dB of the peak), keeping 10 ms either side.
void trimSilence (std::vector<float>& samples, double sampleRate);

} // namespace sing::dsp
