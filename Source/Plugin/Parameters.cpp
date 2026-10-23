#include "Parameters.h"

namespace sing
{

juce::StringArray directionNames()
{
    return { "Forward", "Backward", "Back & forth" };
}

juce::String formatSeconds (float s)
{
    if (s < 0.995f)
        return juce::String (juce::roundToInt (s * 1000.0f)) + " ms";
    if (s < 9.95f)
        return juce::String (s, 1) + " s";
    return juce::String (juce::roundToInt (s)) + " s";
}

juce::String formatSemitones (float st)
{
    const float r = std::round (st * 10.0f) / 10.0f;
    if (std::abs (r) < 0.05f)
        return "0 st";
    const bool whole = std::abs (r - std::round (r)) < 0.05f;
    return (r > 0.0f ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (r), whole ? 0 : 1) + " st";
}

juce::String formatPercent (float p) { return juce::String (juce::roundToInt (p)) + "%"; }

juce::String formatGlide (float ms)
{
    return ms < 995.0f ? juce::String (juce::roundToInt (ms)) + " ms" : juce::String (ms / 1000.0f, 1) + " s";
}

juce::String formatDecibels (float db)
{
    if (db <= -35.95f)
        return juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e dB");
    const float r = std::round (db * 10.0f) / 10.0f;
    return (r > 0.0f ? "+" : (r < 0.0f ? juce::String::fromUTF8 ("\xe2\x88\x92") : juce::String())) + juce::String (std::abs (r), 1) + " dB";
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

    juce::AudioParameterFloatAttributes withText (std::function<juce::String (float)> format, const juce::String& label = {})
    {
        return juce::AudioParameterFloatAttributes()
            .withLabel (label)
            .withStringFromValueFunction ([format] (float v, int) { return format (v); });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    constexpr int v = 1; // parameter version hint for hosts

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::attack, v }, "Attack", skewed (0.0f, 20.0f, 2.0f), 1.5f, withText (formatSeconds)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::release, v }, "Release", skewed (0.0f, 30.0f, 4.0f), 6.0f, withText (formatSeconds)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::stretch, v }, "Stretch", Range { 0.0f, 100.0f, 0.1f }, 60.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::spread, v }, "Spread", Range { 0.0f, 100.0f, 0.1f }, 35.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::formant, v }, "Formant", Range { -12.0f, 12.0f, 0.1f }, 0.0f, withText (formatSemitones)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::hold, v }, "Hold", false));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamID::voices, v }, "Voices", 1, 16, 8));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::glide, v }, "Glide", skewed (0.0f, 2000.0f, 250.0f, 1.0f), 0.0f, withText (formatGlide)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::output, v }, "Output", Range { -36.0f, 6.0f, 0.1f }, 0.0f, withText (formatDecibels)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopStart, v }, "Loop Start", Range { 0.0f, 100.0f, 0.01f }, 30.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopLength, v }, "Loop Length", Range { 1.0f, 100.0f, 0.01f }, 40.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::loopFade, v }, "Loop Fade", Range { 0.0f, 100.0f, 0.1f }, 25.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::direction, v }, "Direction", directionNames(), 0));
    // Also the host's bypass switch (see SingProcessor::getBypassParameter). Clicking the evening star toggles it.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::bypass, v }, "Bypass", false));

    return layout;
}

} // namespace sing
