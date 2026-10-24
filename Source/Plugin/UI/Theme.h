#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Colours, fonts and drawing helpers for the blue hour panel (shared with Overcast's night sky).
// Sizes are in panel units: the panel is laid out at panelWidth x panelHeight and scaled as a whole.
namespace sing::ui
{

inline constexpr float panelWidth = 1280.0f, panelHeight = 920.0f;

namespace Colours
{
    // The blue hour, from the mockup.
    inline const juce::Colour gold      { 0xfff3cf8a };
    inline const juce::Colour goldLight { 0xffffe2a8 };
    inline const juce::Colour cream     { 0xfffff3d6 };
    inline const juce::Colour ivory     { 0xfffbf5fb };
    inline const juce::Colour text      { 0xfff6eef8 };
    inline const juce::Colour soft      { 0xffded6ef };
    inline const juce::Colour muted     { 0xffc8c0dc };
    inline const juce::Colour night     { 0xff101234 };   // rgb(16,18,52)
    inline const juce::Colour ink       { 0xff1d1230 };   // dark text on gold and on the sunset
    inline const juce::Colour dusk      { 0xff3a2440 };   // captions on the sunset
    inline const juce::Colour shadow    { 0xff1a1640 };   // the voice outside the loop
    inline const juce::Colour glass     = juce::Colour (0xff101234).withAlpha (0.55f);
    inline const juce::Colour hairline  = juce::Colour (0xfff3cf8a).withAlpha (0.3f);
}

// The two typefaces from the mockup, embedded so the panel looks the same on every Mac.
// Both are under the SIL Open Font License (see resources/fonts).
class Fonts
{
public:
    Fonts();

    // letterSpacing is in ems, like CSS letter-spacing.
    static juce::Font serif (float px, bool semiBold = false, float letterSpacing = 0.0f);
    static juce::Font sans (float px, bool medium = false, float letterSpacing = 0.0f);

private:
    static juce::Font make (juce::Typeface::Ptr, float px, float letterSpacing);

    juce::Typeface::Ptr serifMedium, serifSemiBold, sansRegular, sansMedium;
};

// A soft disc like a CSS box-shadow: a circle of `radius` blurred with `blur`
// (the CSS blur length, so the edge softens over about that distance).
void fillBlurredDisc (juce::Graphics&, juce::Point<float> centre, float radius, float blur, juce::Colour);

// Same idea for a rounded rectangle glow, drawn as a few soft strokes. Cheap enough per frame.
void drawGlow (juce::Graphics&, juce::Rectangle<float> bounds, float cornerRadius, float blur, juce::Colour);

// In-place Gaussian blur of an ARGB image. sigma is in pixels of that image.
void gaussianBlur (juce::Image&, float sigma);

// Draws text with CSS-style letter spacing, without the trailing gap the spacing adds.
void drawTrackedText (juce::Graphics&, const juce::String&, juce::Rectangle<float>, const juce::Font&,
                      juce::Justification, juce::Colour);
float trackedTextWidth (const juce::String&, const juce::Font&);

} // namespace sing::ui
