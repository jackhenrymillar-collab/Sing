#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace sing::ui
{

// The panel's background while Bloom is the engine: the blue hour deepened into a dreamy,
// psychedelic night. Violet overhead melts through magenta to a coral and gold glow, slow
// aurora haze drifts and changes colour, a huge faint flower turns behind the knobs, and
// colourful lights float up from the bottom, opening like blossoms as they rise and fading
// out high in the sky. Playing makes the lights glow brighter.
//
// Drawn at 1040 x 620 panel units. With motion off (Reduce Motion) it is one still picture.
class BloomSky
{
public:
    BloomSky();

    void setMotion (bool shouldAnimate) noexcept { motion = shouldAnimate; }
    void setBypassed (bool b) noexcept { bypassed = b; }
    void advance (double seconds) noexcept { time = seconds; }

    // How much is sounding, 0 to 1. Eased here, so it can jump.
    void setActivity (float amount) noexcept { activityTarget = juce::jlimit (0.0f, 1.0f, amount); }

    void draw (juce::Graphics&);

private:
    void rebuildBackground (float pixelScale);
    void drawHaze (juce::Graphics&, double t) const;
    void drawFlower (juce::Graphics&, double t) const;
    void drawLights (juce::Graphics&, double t) const;

    struct Star { float x, y, size, phase; };
    struct Light
    {
        float column, sway, swayRate, phase, radius, offset, period, hue;
        bool petals;
    };
    std::vector<Star> stars;
    std::vector<Light> lights;
    juce::Image background;
    float backgroundScale = 0.0f;
    bool motion = true, bypassed = false;
    double time = 0.0, lastTime = 0.0;
    float activity = 0.0f, activityTarget = 0.0f;
};

} // namespace sing::ui
