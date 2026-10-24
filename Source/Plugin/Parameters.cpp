#include "Parameters.h"

namespace sing
{

juce::StringArray directionNames()
{
    return { "Forward", "Backward", "Back & forth" };
}

juce::StringArray engineNames()
{
    return { "Grain", "Bloom" };
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

juce::String formatShift (float hz)
{
    const int r = juce::roundToInt (hz);
    if (r == 0)
        return "0 Hz";
    return (r > 0 ? juce::String ("+") : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))) + juce::String (std::abs (r)) + " Hz";
}

juce::String formatTone (float t)
{
    const int r = juce::roundToInt (t);
    if (r == 0)
        return "Even";
    return (r > 0 ? juce::String ("Pure ") : juce::String ("Breath ")) + juce::String (std::abs (r));
}

juce::String formatHz (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";
    const float k = hz / 1000.0f;
    return juce::String (k, k < 9.95f ? 1 : 0) + " kHz";
}

juce::String formatCents (float c) { return juce::String (juce::roundToInt (c)) + " ct"; }

juce::String formatBeat (float hz) { return juce::String (hz, hz < 9.95f ? 1 : 0) + " Hz"; }

juce::StringArray filterSlopeNames() { return { "Sharp", "6 dB", "12 dB" }; }

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
    // Added with Bloom, Air and Space. Their defaults leave older sessions and presets
    // sounding as they did.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::engine, v }, "Engine", engineNames(), 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::air, v }, "Air", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::space, v }, "Space", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    // Added with Bloom's spectral controls; 0 leaves the sound as before.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::shift, v }, "Shift", Range { -500.0f, 500.0f, 1.0f }, 0.0f, withText (formatShift)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::smear, v }, "Smear", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::window, v }, "Window", skewed (0.1f, 40.0f, 1.4f), 0.68f, withText (formatSeconds)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::tone, v }, "Tone", Range { -100.0f, 100.0f, 1.0f }, 0.0f, withText (formatTone)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::keepVowel, v }, "Keep Vowel", true));
    // Bloom's Advanced modules. The switches for the older modules start on, so older
    // sessions sound as they did; Harmonics and Filter start off.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::onsetsOn, v }, "Onsets On", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::onsets, v }, "Onsets", Range { 0.0f, 100.0f, 0.1f }, 100.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::shiftOn, v }, "Shift On", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::smearOn, v }, "Smear On", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::toneOn, v }, "Tone On", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::harmonicsOn, v }, "Harmonics On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::harmonicsWidth, v }, "Harmonics Width", skewed (1.0f, 200.0f, 25.0f), 25.0f, withText (formatCents)));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamID::harmonicsCount, v }, "Harmonics Count", 1, 64, 16));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::filterOn, v }, "Filter On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::filterLow, v }, "Filter Low", skewed (20.0f, 20000.0f, 640.0f), 120.0f, withText (formatHz)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::filterHigh, v }, "Filter High", skewed (20.0f, 20000.0f, 640.0f), 8000.0f, withText (formatHz)));
    // The modules from the method's own implementations. All start off.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::filterSlope, v }, "Filter Slope", filterSlopeNames(), 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::compressorOn, v }, "Compressor On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::compressor, v }, "Compressor", Range { 0.0f, 100.0f, 0.1f }, 50.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::octavesOn, v }, "Octaves On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveDown2, v }, "Octave -2", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveDown1, v }, "Octave -1", Range { 0.0f, 100.0f, 0.1f }, 50.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveRoot, v }, "Octave 0", Range { 0.0f, 100.0f, 0.1f }, 100.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveUp1, v }, "Octave +1", Range { 0.0f, 100.0f, 0.1f }, 30.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveUp15, v }, "Octave +1 Fifth", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::octaveUp2, v }, "Octave +2", Range { 0.0f, 100.0f, 0.1f }, 0.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::binauralOn, v }, "Binaural On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::binauralRate, v }, "Binaural Rate", skewed (0.1f, 30.0f, 4.0f), 4.0f, withText (formatBeat)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::binauralAmount, v }, "Binaural Amount", Range { 0.0f, 100.0f, 0.1f }, 100.0f, withText (formatPercent)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::spaceFilterOn, v }, "Space Filter On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::spaceLow, v }, "Space Low Cut", skewed (20.0f, 2000.0f, 200.0f), 150.0f, withText (formatHz)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::spaceHigh, v }, "Space High Cut", skewed (1000.0f, 20000.0f, 6000.0f), 9000.0f, withText (formatHz)));
    // Also the host's bypass switch (see SingProcessor::getBypassParameter). Clicking the evening star toggles it.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::bypass, v }, "Bypass", false));

    return layout;
}

} // namespace sing
