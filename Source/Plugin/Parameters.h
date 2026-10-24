#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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
}

// The order matches LoopDirection.
juce::StringArray directionNames();

// The Stretch engines, in parameter order: Grain (granular) and Bloom (spectral smear).
juce::StringArray engineNames();

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// Text for the panel and the host.
juce::String formatSeconds (float seconds);
juce::String formatSemitones (float semitones);
juce::String formatPercent (float percent);
juce::String formatGlide (float ms);
juce::String formatShift (float hz);
juce::String formatTone (float t);
juce::String formatDecibels (float db);
juce::String formatHz (float hz);
juce::String formatCents (float cents);

} // namespace sing
