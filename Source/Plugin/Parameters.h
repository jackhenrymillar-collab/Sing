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
juce::String formatDecibels (float db);

} // namespace sing
