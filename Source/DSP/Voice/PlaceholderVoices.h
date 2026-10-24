#pragma once

#include <vector>

namespace sing
{

// Synthetic stand-ins for the factory voices until the recorded ones exist. Each is a sung
// vowel made by formant synthesis: a glottal source with vibrato, shaped by four vowel
// formants (Peterson and Barney's 1952 measurements), plus a little breath noise. They are
// generated, so there is nothing to license.
enum class PlaceholderVowel
{
    ooh,      // breathy "ooh"
    ah,       // soft "ah"
    hum,      // closed-mouth "mm"
    breath    // unpitched breaths
};

struct PlaceholderVoice
{
    std::vector<float> samples;   // mono
    double sampleRate = 48000.0;
    float rootHz = 0.0f;          // 0 for breath
};

PlaceholderVoice makePlaceholderVoice (PlaceholderVowel vowel, double sampleRate = 48000.0, float seconds = 5.0f);

} // namespace sing
