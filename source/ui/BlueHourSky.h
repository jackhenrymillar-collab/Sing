#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace sing::ui
{

// The sky behind the controls: the blue hour, deep blue overhead fading to a rose and
// peach glow where the sun has set. Faint stars twinkle high up, the glow breathes,
// two thin cloud wisps drift, and the evening star (Venus) shines top right. Venus is
// the bypass switch: bypassed, it dims and its glow goes out.
//
// It's drawn by the panel rather than being a component, so the panel's own text sits
// on top of it. Everything is in panel units (1040 x 620). The gradients are cached as
// images at screen resolution and rebuilt only when the pixel density changes, so a
// frame is a few image blits and 46 small dots.
class BlueHourSky
{
public:
    static constexpr float width = 1040.0f, height = 620.0f;

    BlueHourSky();

    // Moves the animation to `seconds`.
    void advance (double seconds);

    // With motion off the sky is a still picture and bypass changes are instant.
    void setMotion (bool shouldMove) noexcept { motion = shouldMove; }
    void setBypassed (bool);

    void draw (juce::Graphics&);

    // The evening star, in panel units. The panel puts the bypass button here.
    static juce::Rectangle<float> venusButtonBounds() noexcept { return { 877.0f, 93.0f, 48.0f, 48.0f }; }
    static juce::Point<float> venusCentre() noexcept { return { 901.0f, 117.0f }; }

private:
    struct Star { float x, y, size, delay; };

    void rebuildCaches (float pixelScale);
    void drawVenus (juce::Graphics&);
    float bypassAmount() const;

    std::vector<Star> stars;
    double now = 0.0;
    bool motion = true;

    // Bypass fades Venus out over 0.6 s.
    bool bypassed = false;
    float bypassFrom = 0.0f, bypassTo = 0.0f;
    double bypassStart = -10.0;

    // Caches, at screen resolution.
    float cachedScale = 0.0f;
    juce::Image backdrop;         // the gradient
    juce::Image glow;             // the rose and peach glow low down
    juce::Image wisps;            // both cloud wisps, in one strip
    juce::Image venusHalo, venusHaloDim;
    juce::Path venus;
};

} // namespace sing::ui
