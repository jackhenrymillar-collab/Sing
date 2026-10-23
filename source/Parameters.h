#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace sing
{

// Parameter IDs are part of saved sessions and presets: never rename one.
namespace ParamID
{
    // The loop, as fractions of the voice's length.
    inline constexpr auto loopStart  = "loopstart";  // 0..1
    inline constexpr auto loopLength = "looplength"; // 0.005..1
    inline constexpr auto loopFade   = "loopfade";   // 0..1: each fade is this share of half the loop
    inline constexpr auto loopMode   = "loopmode";   // Forward, Backward, Back & forth

    inline constexpr auto attack  = "attack";   // seconds
    inline constexpr auto release = "release";  // seconds
    inline constexpr auto stretch = "stretch";  // %
    inline constexpr auto spread  = "spread";   // %
    inline constexpr auto formant = "formant";  // semitones
    inline constexpr auto air     = "air";      // dB
    inline constexpr auto space   = "space";    // %

    inline constexpr auto hold    = "hold";
    inline constexpr auto octDown = "octdown";
    inline constexpr auto octUp   = "octup";
    inline constexpr auto tape    = "tape";

    inline constexpr auto voices  = "voices";   // polyphony
    inline constexpr auto glide   = "glide";    // ms
    inline constexpr auto output  = "output";   // dB
    inline constexpr auto bypass  = "bypass";
}

juce::StringArray loopModeNames(); // FORWARD, BACKWARD, BACK & FORTH

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace sing
