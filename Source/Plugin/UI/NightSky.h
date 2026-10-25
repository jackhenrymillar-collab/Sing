#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

namespace sing::ui
{

// The panel's background, in both engines: a crisp, dark purple night sky full of sharp
// stars, with small red, green and blue fireflies drifting slowly all over it. Three knobs
// steer them:
//
// - Space: the bigger the reverb, the wider and brighter each firefly's glow (its bloom).
// - Spread: more copies of the voice, more fireflies (from a handful to well over a hundred).
// - Stretch: the slower the voice, the slower the fireflies fly.
//
// Playing makes them glow a little brighter. The evening star (the bypass switch) sits in
// the top right as before.
//
// Drawn at panelWidth x panelHeight panel units (Theme.h). With motion off (Reduce Motion) it is one still picture.
class NightSky
{
public:
    NightSky();

    void setMotion (bool shouldAnimate) noexcept { motion = shouldAnimate; }
    void setBypassed (bool b) noexcept { bypassed = b; }
    void advance (double seconds) noexcept;

    // How much is sounding, 0 to 1. Eased here, so it can jump.
    void setActivity (float amount) noexcept { activityTarget = juce::jlimit (0.0f, 1.0f, amount); }

    // The knobs that steer the fireflies, each 0 to 1. Eased here, so they can jump.
    void setKnobs (float space, float spread, float stretch) noexcept;

    void draw (juce::Graphics&);

    static constexpr int maxFireflies = 140;

    // How many fireflies a Spread shows, and how fast they fly (1 is their pace at Stretch 0).
    static float firefliesFor (float spread) noexcept;
    static float speedFor (float stretch) noexcept;

private:
    void rebuildBackground (float pixelScale);
    void drawFireflies (juce::Graphics&) const;

    struct Star { float x, y, size, brightness, phase, rate; };
    struct Firefly
    {
        float cx, cy, ax, ay, bx, by;      // centre and the reach of its two slow wanders
        float fx1, fy1, fx2, fy2;          // their rates
        float px1, py1, px2, py2;          // and phases
        float radius, pulseRate, pulsePhase;
        int colour;                        // 0 red, 1 green, 2 blue
    };
    std::vector<Star> stars;
    std::vector<Firefly> fireflies;
    juce::Image background;
    std::array<juce::Image, 3> glows;      // one soft glow per colour
    float backgroundScale = 0.0f;
    bool motion = true, bypassed = false;
    double time = 0.0, lastTime = -1.0;
    double flight = 0.0;                   // the fireflies' own clock, slowed by Stretch
    float activity = 0.0f, activityTarget = 0.0f;
    float bloom = 0.0f, count = 20.0f, speed = 1.0f;
    float bloomTarget = 0.0f, countTarget = 20.0f, speedTarget = 1.0f;
};

} // namespace sing::ui
