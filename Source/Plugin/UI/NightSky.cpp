#include "NightSky.h"
#include "BlueHourSky.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

namespace
{
    constexpr float kWidth = panelWidth, kHeight = panelHeight;
    constexpr double twoPi = 6.28318530717958647692;
    constexpr int kStars = 230;

    // The time the still picture shows when motion is off.
    constexpr double kStillTime = 37.0;

    // Shades of warm red, orange and warm yellow, like embers and candle flames.
    constexpr int kNumColours = 6;
    const juce::Colour kColours[kNumColours] = {
        juce::Colour (0xffff3d2e),   // warm red
        juce::Colour (0xffff5a26),   // red-orange
        juce::Colour (0xffff8a2a),   // orange
        juce::Colour (0xffffa53a),   // amber
        juce::Colour (0xffffc94a),   // warm yellow
        juce::Colour (0xffffdb6b),   // golden yellow
    };

    // A soft round glow: bright in the middle, fading to nothing at the edge.
    void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour colour, float alpha)
    {
        juce::ColourGradient grad (colour.withAlpha (alpha), c, colour.withAlpha (0.0f), c.translated (radius, 0.0f), true);
        grad.addColour (0.3, colour.withAlpha (alpha * 0.5f));
        g.setGradientFill (grad);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
    }

    float ease (float now, float target, float dt, float seconds)
    {
        return now + (target - now) * std::min (1.0f, dt / seconds);
    }
}

float NightSky::firefliesFor (float spread) noexcept
{
    return 8.0f + (float) (maxFireflies - 8) * juce::jlimit (0.0f, 1.0f, spread);
}

float NightSky::speedFor (float stretch) noexcept
{
    return 1.0f - 0.88f * juce::jlimit (0.0f, 1.0f, stretch);
}

NightSky::NightSky()
{
    juce::int64 seed = 97;
    auto rnd = [&seed]
    {
        seed = (seed * 16807) % 2147483647;
        return static_cast<float> (seed - 1) / 2147483646.0f;
    };

    // Sharp stars all over, thinning out towards the bottom.
    for (int i = 0; i < kStars; ++i)
    {
        const float x = rnd() * kWidth, y = kHeight * std::pow (rnd(), 1.4f);
        const float big = rnd();
        stars.push_back ({ x, y, big > 0.93f ? 2.2f : (big > 0.7f ? 1.5f : 1.0f), 0.35f + 0.65f * rnd(), rnd(), 0.08f + 0.25f * rnd() });
    }

    // The fireflies, in a random order, so whichever number Spread shows is spread evenly
    // over the sky and across the three colours.
    for (int i = 0; i < maxFireflies; ++i)
    {
        Firefly f;
        f.cx = kWidth * (0.1f + 0.8f * rnd());
        f.cy = kHeight * (0.1f + 0.8f * rnd());
        f.ax = 120.0f + 260.0f * rnd();
        f.ay = 80.0f + 200.0f * rnd();
        f.bx = 30.0f + 70.0f * rnd();
        f.by = 30.0f + 60.0f * rnd();
        f.fx1 = 0.025f + 0.035f * rnd();
        f.fy1 = 0.025f + 0.035f * rnd();
        f.fx2 = 0.07f + 0.08f * rnd();
        f.fy2 = 0.07f + 0.08f * rnd();
        f.px1 = (float) twoPi * rnd();
        f.py1 = (float) twoPi * rnd();
        f.px2 = (float) twoPi * rnd();
        f.py2 = (float) twoPi * rnd();
        f.radius = 2.2f + 1.6f * rnd();
        f.pulseRate = 0.15f + 0.3f * rnd();
        f.pulsePhase = rnd();
        f.colour = i % kNumColours;
        fireflies.push_back (f);
    }
}

void NightSky::setKnobs (float space, float spread, float stretch) noexcept
{
    bloomTarget = juce::jlimit (0.0f, 1.0f, space);
    countTarget = firefliesFor (spread);
    speedTarget = speedFor (stretch);
}

void NightSky::advance (double seconds) noexcept
{
    const bool first = lastTime < 0.0;
    const auto dt = static_cast<float> (first ? 0.0 : juce::jlimit (0.0, 0.1, seconds - lastTime));
    lastTime = seconds;
    time = seconds;
    if (! motion || first)
    {
        activity = activityTarget;
        bloom = bloomTarget;
        count = countTarget;
        speed = speedTarget;
        flight = motion ? seconds * speed : kStillTime;
        return;
    }
    // Turning a knob eases the sky over a fraction of a second, so it never jumps.
    activity = ease (activity, activityTarget, dt, activityTarget > activity ? 0.2f : 1.0f);
    bloom = ease (bloom, bloomTarget, dt, 0.4f);
    count = ease (count, countTarget, dt, 0.6f);
    speed = ease (speed, speedTarget, dt, 0.5f);
    flight += static_cast<double> (dt * speed);
}

void NightSky::rebuildBackground (float pixelScale)
{
    backgroundScale = pixelScale;
    background = juce::Image (juce::Image::RGB, juce::roundToInt (kWidth * pixelScale), juce::roundToInt (kHeight * pixelScale), false);
    juce::Graphics g (background);
    g.addTransform (juce::AffineTransform::scale (pixelScale));

    // Crisp and dark: near-black indigo overhead to a deep violet at the bottom.
    juce::ColourGradient sky (juce::Colour (0xff07051a), 0.0f, 0.0f, juce::Colour (0xff261246), 0.0f, kHeight, false);
    sky.addColour (0.45, juce::Colour (0xff0f0928));
    sky.addColour (0.75, juce::Colour (0xff1a0d38));
    g.setGradientFill (sky);
    g.fillAll();

    // One soft glow per colour, drawn once and stamped for every firefly: much cheaper
    // than filling a gradient for each of them every frame.
    const int size = 128;
    for (int k = 0; k < kNumColours; ++k)
    {
        glows[(size_t) k] = juce::Image (juce::Image::ARGB, size, size, true);
        juce::Graphics gg (glows[(size_t) k]);
        glow (gg, { size * 0.5f, size * 0.5f }, size * 0.5f, kColours[k], 1.0f);
    }
}

void NightSky::drawFireflies (juce::Graphics& g) const
{
    // Each firefly wanders on two slow loops at once, so its path never quite repeats, and
    // pulses gently in its own rhythm. Space widens and brightens the glow round each one.
    const auto tau = static_cast<float> (flight);
    const float brightness = (0.8f + 0.4f * activity) * (bypassed ? 0.35f : 1.0f);
    const float haloRadius = 2.5f + 7.0f * bloom;
    const float haloAlpha = 0.3f + 0.25f * bloom;
    for (int i = 0; i < static_cast<int> (fireflies.size()); ++i)
    {
        const float shown = juce::jlimit (0.0f, 1.0f, count - static_cast<float> (i));
        if (shown <= 0.0f)
            break;
        const auto& f = fireflies[static_cast<size_t> (i)];
        const float x = f.cx + f.ax * std::sin (f.fx1 * tau + f.px1) + f.bx * std::sin (f.fx2 * tau + f.px2);
        const float y = f.cy + f.ay * std::sin (f.fy1 * tau + f.py1) + f.by * std::sin (f.fy2 * tau + f.py2);
        const auto pulseAngle = static_cast<float> (twoPi * (static_cast<double> (f.pulseRate) * time + f.pulsePhase));
        const float pulse = 0.5f + 0.5f * std::sin (motion ? pulseAngle : static_cast<float> (twoPi * f.pulsePhase));
        const float alpha = (0.55f + 0.45f * pulse) * shown * brightness;

        const juce::Point<float> c (x, y);
        const auto colour = kColours[f.colour];
        const float r = f.radius * (0.9f + 0.2f * pulse);
        const auto stamp = [&g, &c, this, &f] (float radius, float a)
        {
            const auto& image = glows[(size_t) f.colour];
            g.setOpacity (std::min (1.0f, a));
            g.drawImage (image, juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
        };
        if (bloom > 0.01f)
            stamp (r * haloRadius * 1.8f, haloAlpha * 0.45f * alpha);
        stamp (r * haloRadius, haloAlpha * alpha);
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (std::min (1.0f, alpha)));
        g.fillEllipse (juce::Rectangle<float> (r * 1.3f, r * 1.3f).withCentre (c));
    }
}

void NightSky::draw (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - backgroundScale) > 0.01f || ! background.isValid())
        rebuildBackground (pixelScale);
    g.drawImageTransformed (background, juce::AffineTransform::scale (1.0f / backgroundScale));

    const double t = motion ? time : kStillTime;
    for (const auto& s : stars)
    {
        // A slow, slight twinkle: the stars stay sharp.
        const auto twinkle = motion ? static_cast<float> (0.8 + 0.2 * std::sin (twoPi * (s.rate * t + s.phase))) : 1.0f;
        g.setColour (juce::Colour (0xfff4f0ff).withAlpha (s.brightness * twinkle * (bypassed ? 0.6f : 1.0f)));
        g.fillEllipse (juce::Rectangle<float> (s.size, s.size).withCentre ({ s.x, s.y }));
    }

    drawFireflies (g);
    BlueHourSky::drawEveningStar (g, t, motion, bypassed);
}

} // namespace sing::ui
