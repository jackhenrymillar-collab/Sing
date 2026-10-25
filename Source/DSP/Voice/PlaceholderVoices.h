#pragma once

#include <vector>

namespace sing
{

// Synthetic stand-ins for the factory voices until the recorded ones exist. Each sung voice is
// made by formant synthesis: a harmonic glottal source with vibrato, shaped by vowel formants
// (Peterson and Barney's 1952 measurements). They are pure harmonic tone with no breath noise;
// the harmonics above the top formant are lifted instead, for shine. Only Two Breaths is noise.
// They are generated, so there is nothing to license.
enum class PlaceholderVowel
{
    ooh,      // soft "ooh"
    ah,       // soft "ah"
    hum,      // closed-mouth "mm"
    breath,   // unpitched breaths

    // Voices that move: the vowel, vibrato, brightness or loudness changes across the recording,
    // so where the loop sits and how far Stretch spreads it changes the sound.
    openingOoh,     // "ooh" opens to "ah" and closes again
    risingEe,       // "ah" brightens through "eh" to "ee"
    swellingAlto,   // "oh" that starts straight and swells, vibrato growing with it
    wakingAh,       // a near-pure "ah" whose harmonics open out until it is full and bright
    smallChoir,     // six singers on one "ah", drifting to "oh"
    lowOm,          // a low "oh" that closes to "mm"
    highEe,         // a light, high "ee" with quick vibrato
    slowWah,        // "ooh" and "ah" in turn, opening and closing like a slow wah
    overtoneDrone,  // a throat-sung drone whose whistling overtone sweeps up and down
    fallingSigh     // an "ah" that sighs down a tone, softening as it falls
};

constexpr PlaceholderVowel allPlaceholderVowels[] {
    PlaceholderVowel::ooh, PlaceholderVowel::ah, PlaceholderVowel::hum, PlaceholderVowel::breath,
    PlaceholderVowel::openingOoh, PlaceholderVowel::risingEe, PlaceholderVowel::swellingAlto,
    PlaceholderVowel::wakingAh, PlaceholderVowel::smallChoir, PlaceholderVowel::lowOm,
    PlaceholderVowel::highEe, PlaceholderVowel::slowWah, PlaceholderVowel::overtoneDrone,
    PlaceholderVowel::fallingSigh
};

struct PlaceholderVoice
{
    std::vector<float> samples;   // mono
    double sampleRate = 48000.0;
    float rootHz = 0.0f;          // 0 for breath
};

// How long the factory copy of each voice is: 5 s for the first four, 8 s for the ones that move.
float placeholderSeconds (PlaceholderVowel vowel);

// The moving voices are written for 8 s and squeezed or stretched to fit other lengths.
PlaceholderVoice makePlaceholderVoice (PlaceholderVowel vowel, double sampleRate = 48000.0, float seconds = 5.0f);

} // namespace sing
