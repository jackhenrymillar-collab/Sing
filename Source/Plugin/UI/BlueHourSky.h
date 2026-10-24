#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace sing::ui
{

// The panel's background: the blue hour, deep blue overhead fading to a rose and peach glow
// where the sun has set, with faint stars, slow wisps of cloud and Venus, the evening star.
// Drawn at 1040 x 620 panel units.
class BlueHourSky
{
public:
    BlueHourSky();

    void setMotion (bool shouldAnimate) noexcept { motion = shouldAnimate; }
    void setBypassed (bool b) noexcept { bypassed = b; }
    void advance (double seconds) noexcept { time = seconds; }

    void draw (juce::Graphics&);

    // The evening star, which is the bypass switch.
    static juce::Point<float> starCentre() noexcept { return { 901.0f, 117.0f }; }
    static juce::Rectangle<float> starButtonBounds() noexcept { return juce::Rectangle<float> (48.0f, 48.0f).withCentre (starCentre()); }

    // The four-pointed star shape, centred on 0,0 with the given radius.
    static juce::Path starShape (float radius);

private:
    void rebuildBackground (float pixelScale);

    struct Star { float x, y, size, phase; };
    std::vector<Star> stars;
    juce::Image background;
    float backgroundScale = 0.0f;
    bool motion = true, bypassed = false;
    double time = 0.0;
};

} // namespace sing::ui
