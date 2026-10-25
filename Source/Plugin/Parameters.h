#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>

namespace sing
{

// Parameter IDs are part of saved sessions and presets: never rename one.
namespace ParamID
{
    inline constexpr auto attack     = "attack";
    inline constexpr auto release    = "release";
    inline constexpr auto stretch    = "stretch";
    inline constexpr auto spread     = "spread";
    inline constexpr auto formant    = "formant";
    inline constexpr auto hold       = "hold";
    inline constexpr auto voices     = "voices";
    inline constexpr auto glide      = "glide";
    inline constexpr auto output     = "output";
    inline constexpr auto bypass     = "bypass";
    inline constexpr auto loopStart  = "loopstart";
    inline constexpr auto loopLength = "looplength";
    inline constexpr auto loopFade   = "loopfade";
    inline constexpr auto direction  = "direction";
    inline constexpr auto engine     = "engine";
    inline constexpr auto air        = "air";
    inline constexpr auto space      = "space";
    inline constexpr auto shift      = "shift";
    inline constexpr auto smear      = "smear";
    inline constexpr auto tone       = "tone";
    inline constexpr auto window     = "window";
    inline constexpr auto keepVowel  = "keepvowel";
    // Bloom's Advanced modules: an on/off switch for each, and the two newest modules.
    inline constexpr auto onsetsOn   = "onsetson";
    inline constexpr auto onsets     = "onsets";
    inline constexpr auto shiftOn    = "shifton";
    inline constexpr auto smearOn    = "smearon";
    inline constexpr auto toneOn     = "toneon";
    inline constexpr auto harmonicsOn    = "harmonicson";
    inline constexpr auto harmonicsWidth = "harmonicswidth";
    inline constexpr auto harmonicsCount = "harmonicscount";
    inline constexpr auto filterOn   = "filteron";
    inline constexpr auto filterLow  = "filterlow";
    inline constexpr auto filterHigh = "filterhigh";
    // Added with the modules from the method's own implementations.
    inline constexpr auto filterSlope    = "filterslope";
    inline constexpr auto compressorOn   = "compressoron";
    inline constexpr auto compressor     = "compressor";
    inline constexpr auto octavesOn      = "octaveson";
    inline constexpr auto octaveDown2    = "octm2";
    inline constexpr auto octaveDown1    = "octm1";
    inline constexpr auto octaveRoot     = "oct0";
    inline constexpr auto octaveUp1      = "octp1";
    inline constexpr auto octaveUp15     = "octp15";
    inline constexpr auto octaveUp2      = "octp2";
    inline constexpr auto binauralOn     = "binauralon";
    inline constexpr auto binauralRate   = "binauralrate";
    inline constexpr auto binauralAmount = "binauralamount";
    inline constexpr auto spaceFilterOn  = "spacefilteron";
    inline constexpr auto spaceLow       = "spacelow";
    inline constexpr auto spaceHigh      = "spacehigh";
    // Space's own Decay and Size. Before these, the Space knob set the reverb's length and
    // size too; now it is the reverb's level alone.
    inline constexpr auto spaceDecay     = "spacedecay";
    inline constexpr auto spaceSize      = "spacesize";
    // Movement: an orbit for each big knob that can move, Wander for the loop, and the
    // strip that sets the shape and speed for all of them.
    inline constexpr auto orbitStretch   = "orbitstretch";
    inline constexpr auto orbitSpread    = "orbitspread";
    inline constexpr auto orbitFormant   = "orbitformant";
    inline constexpr auto orbitAir       = "orbitair";
    inline constexpr auto orbitSpace     = "orbitspace";
    inline constexpr auto wander         = "wander";
    inline constexpr auto moveShape      = "moveshape";
    inline constexpr auto moveSpeed      = "movespeed";
    inline constexpr auto moveSync       = "movesync";
    inline constexpr auto moveBars       = "movebars";
    inline constexpr auto moveEach       = "moveeach";
}

// The order matches LoopDirection.
juce::StringArray directionNames();

// The Stretch engines, in parameter order: Grain (granular) and Bloom (spectral smear).
juce::StringArray engineNames();

// The Filter's slopes, in parameter order: Sharp, 6 dB and 12 dB per octave.
juce::StringArray filterSlopeNames();

// Movement's shapes, in parameter order: Drift, Wave and Breath.
juce::StringArray moveShapeNames();

// Movement's synced lengths, in parameter order, and each one in bars.
juce::StringArray moveBarNames();
float moveBarsAt (int index);

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// Settings saved before Space had its own Decay and Size (no spacedecay in them): sets
// Decay, Size and Level to sound as the old one-knob Space did. Leaves anything newer as
// it is. Values are in parameter units, keyed by ID.
void upgradeOldSpace (std::map<juce::String, float>& values);

// Text for the panel and the host.
juce::String formatSeconds (float seconds);
juce::String formatSemitones (float semitones);
juce::String formatPercent (float percent);
juce::String formatGlide (float ms);
juce::String formatShift (float hz);
juce::String formatBeat (float hz);
juce::String formatTone (float t);
juce::String formatDecibels (float db);
juce::String formatHz (float hz);
juce::String formatCents (float cents);
juce::String formatOrbit (float percent);

} // namespace sing
