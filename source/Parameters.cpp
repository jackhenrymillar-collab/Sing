#include "Parameters.h"

namespace sing
{

juce::StringArray loopModeNames()
{
    return { "Forward", "Backward", "Back & forth" };
}

namespace
{
    using Range = juce::NormalisableRange<float>;

    Range skewed (float min, float max, float centre, float step = 0.0f)
    {
        Range r { min, max, step };
        r.setSkewForCentre (centre);
        return r;
    }

    juce::AudioParameterFloatAttributes withLabel (const juce::String& label, int decimals = 1)
    {
        return juce::AudioParameterFloatAttributes()
            .withLabel (label)
            .withStringFromValueFunction ([decimals] (float v, int) { return juce::String (v, decimals); });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    constexpr int v = 1; // parameter version hint for hosts

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopStart, v }, "Loop Start", Range { 0.0f, 1.0f }, 0.3f, withLabel ("", 3)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopLength, v }, "Loop Length", skewed (0.005f, 1.0f, 0.25f), 0.4f, withLabel ("", 3)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopFade, v }, "Loop Fade", Range { 0.0f, 100.0f, 0.1f }, 30.0f, withLabel ("%", 0)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::loopMode, v }, "Loop Direction", loopModeNames(), 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::attack, v }, "Attack", skewed (0.001f, 20.0f, 1.5f), 1.0f, withLabel ("s", 2)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::release, v }, "Release", skewed (0.01f, 30.0f, 4.0f), 5.0f, withLabel ("s", 2)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::stretch, v }, "Stretch", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withLabel ("%", 0)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::spread, v }, "Spread", Range { 0.0f, 100.0f, 0.1f }, 30.0f, withLabel ("%", 0)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::formant, v }, "Formant", Range { -12.0f, 12.0f, 0.01f }, 0.0f, withLabel ("st", 1)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::air, v }, "Air", Range { 0.0f, 8.0f, 0.01f }, 2.0f, withLabel ("dB", 1)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::space, v }, "Space", Range { 0.0f, 100.0f, 0.1f }, 50.0f, withLabel ("%", 0)));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::hold, v }, "Hold", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::octDown, v }, "-1 Oct", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::octUp, v }, "+1 Oct", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::tape, v }, "Tape", false));

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamID::voices, v }, "Voices", 1, 16, 8));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::glide, v }, "Glide", skewed (0.0f, 2000.0f, 200.0f), 0.0f, withLabel ("ms", 0)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::output, v }, "Output", Range { -24.0f, 12.0f, 0.1f }, 0.0f, withLabel ("dB", 1)));
    // Also the host's bypass switch. Clicking the evening star toggles it.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::bypass, v }, "Bypass", false));

    return layout;
}

} // namespace sing
