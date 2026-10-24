#include "BloomSky.h"
#include "BlueHourSky.h"

#include <cmath>

namespace sing::ui
{

namespace
{
    constexpr float kWidth = 1040.0f, kHeight = 620.0f;
    constexpr double twoPi = 6.28318530717958647692;
    constexpr int kLights = 58;

    // The time the still picture shows when motion is off: lights spread up the sky.
    constexpr double kStillTime = 37.0;

    float breathe (double t, double period, double phase = 0.0)
    {
        return static_cast<float> (0.5 - 0.5 * std::cos (twoPi * (t / period + phase)));
    }

    // A repeatable 0..1 value for an integer, so a light that starts over gets a new place.
    float hash01 (int a, int b)
    {
        auto h = static_cast<uint32_t> (a) * 0x9E3779B1u ^ static_cast<uint32_t> (b) * 0x85EBCA77u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return static_cast<float> (h & 0xffffff) / 16777215.0f;
    }

    // The lights come in complementary pairs: each light's heart is one colour and its halo
    // the colour opposite it on the wheel. The pairs turn slowly round the wheel over time.
    struct Pair { juce::Colour heart, halo; };
    const Pair kPairs[] = {
        { juce::Colour (0xffff3fb4), juce::Colour (0xff3dffa0) },   // magenta and spring green
        { juce::Colour (0xff2fd9ff), juce::Colour (0xffff7a3d) },   // cyan and orange
        { juce::Colour (0xff8a4dff), juce::Colour (0xffffe14d) },   // violet and yellow
        { juce::Colour (0xff3d6bff), juce::Colour (0xffffb23d) },   // blue and amber
        { juce::Colour (0xffff3d5e), juce::Colour (0xff2fffe0) },   // red-pink and turquoise
        { juce::Colour (0xffc6ff3d), juce::Colour (0xffb43dff) },   // lime and purple
    };
    constexpr int kNumPairs = static_cast<int> (std::size (kPairs));

    Pair lightPair (float hue, double t)
    {
        const int i = std::min (kNumPairs - 1, static_cast<int> (hue * kNumPairs));
        const auto turn = static_cast<float> (std::fmod (t / 300.0, 1.0));
        return { kPairs[i].heart.withRotatedHue (turn), kPairs[i].halo.withRotatedHue (turn) };
    }

    juce::Colour lightColour (float hue, double t)
    {
        const auto p = lightPair (std::fmod (hue * 2.0f, 1.0f), t);
        return hue * 2.0f < 1.0f ? p.heart : p.halo;
    }

    // A soft round glow: bright in the middle, fading to nothing at the edge.
    void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour colour, float alpha)
    {
        juce::ColourGradient grad (colour.withAlpha (alpha), c, colour.withAlpha (0.0f), c.translated (radius, 0.0f), true);
        grad.addColour (0.35, colour.withAlpha (alpha * 0.55f));
        g.setGradientFill (grad);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
    }
}

BloomSky::BloomSky()
{
    // A few faint stars high up, fewer than the blue hour's: the night is full of colour.
    juce::int64 seed = 73;
    auto rnd = [&seed]
    {
        seed = (seed * 16807) % 2147483647;
        return static_cast<float> (seed - 1) / 2147483646.0f;
    };
    for (int i = 0; i < 30; ++i)
    {
        const float x = std::round (rnd() * kWidth), y = std::round (rnd() * 170.0f);
        stars.push_back ({ x, y, rnd() > 0.85f ? 2.0f : 1.0f, rnd() });
    }

    for (int i = 0; i < kLights; ++i)
    {
        Light l;
        l.column = rnd();
        l.sway = 10.0f + 26.0f * rnd();
        l.swayRate = 0.04f + 0.08f * rnd();
        l.phase = rnd();
        l.radius = 12.0f + 32.0f * rnd() * rnd();   // mostly small, a few large
        l.period = 16.0f + 18.0f * rnd();           // seconds to rise from the bottom to the top
        l.offset = rnd() * l.period;
        l.hue = rnd();
        l.petals = rnd() > 0.62f;
        lights.push_back (l);
    }
}

void BloomSky::rebuildBackground (float pixelScale)
{
    backgroundScale = pixelScale;
    background = juce::Image (juce::Image::RGB, juce::roundToInt (kWidth * pixelScale), juce::roundToInt (kHeight * pixelScale), false);
    juce::Graphics g (background);
    g.addTransform (juce::AffineTransform::scale (pixelScale));

    juce::ColourGradient sky (juce::Colour (0xff05030f), 0.0f, 0.0f, juce::Colour (0xff3a1438), 0.0f, kHeight, false);
    sky.addColour (0.30, juce::Colour (0xff0c0724));
    sky.addColour (0.55, juce::Colour (0xff170b33));
    sky.addColour (0.75, juce::Colour (0xff22103c));
    sky.addColour (0.90, juce::Colour (0xff2e1240));
    g.setGradientFill (sky);
    g.fillAll();
}

void BloomSky::drawHaze (juce::Graphics& g, double t) const
{
    // Four great soft clouds of colour drifting on slow loops, each slowly changing hue.
    struct Haze { juce::Colour colour; float alpha, cx, cy, ax, ay, w, h; double px, py; };
    const Haze haze[] = {
        { juce::Colour (0xff3fd8d0), 0.14f, 260.0f, 170.0f, 160.0f, 50.0f, 640.0f, 260.0f, 61.0, 47.0 },
        { juce::Colour (0xff9b5cff), 0.16f, 760.0f, 230.0f, 180.0f, 60.0f, 700.0f, 300.0f, 73.0, 53.0 },
        { juce::Colour (0xffff4fb3), 0.13f, 480.0f, 420.0f, 220.0f, 40.0f, 820.0f, 300.0f, 89.0, 59.0 },
        { juce::Colour (0xffffd27a), 0.08f, 560.0f, 600.0f, 200.0f, 30.0f, 900.0f, 260.0f, 97.0, 67.0 },
    };
    for (const auto& h : haze)
    {
        const float x = h.cx + h.ax * std::sin (static_cast<float> (twoPi * t / h.px));
        const float y = h.cy + h.ay * std::sin (static_cast<float> (twoPi * t / h.py + 1.3));
        const auto colour = h.colour.withRotatedHue (static_cast<float> (0.08 * std::sin (twoPi * t / 150.0)));
        g.saveState();
        g.addTransform (juce::AffineTransform::scale (1.0f, h.h / h.w, x, y));
        glow (g, { x, y }, h.w * 0.5f, colour, h.alpha);
        g.restoreState();
    }
}

void BloomSky::drawFlower (juce::Graphics& g, double t) const
{
    // A huge, faint flower behind the knobs, turning once every three minutes: twelve
    // petals in two rings, coloured round the wheel.
    const juce::Point<float> c (520.0f, 640.0f);
    const float turn = static_cast<float> (twoPi * t / 180.0);
    const float open = 0.85f + 0.15f * breathe (t, 14.0);
    for (int ring = 0; ring < 2; ++ring)
    {
        const int petals = 12;
        const float length = (ring == 0 ? 330.0f : 220.0f) * open;
        const float width = ring == 0 ? 120.0f : 90.0f;
        const float alpha = ring == 0 ? 0.11f : 0.14f;
        for (int i = 0; i < petals; ++i)
        {
            const float angle = (ring == 0 ? turn : -turn * 1.3f) + static_cast<float> (twoPi) * (static_cast<float> (i) + 0.5f * ring) / petals;
            const auto colour = lightColour (std::fmod (static_cast<float> (i) / petals + 0.5f * ring, 1.0f), t);
            juce::Path petal;
            petal.addEllipse (-width * 0.5f, -length, width, length);
            const auto toPlace = juce::AffineTransform::rotation (angle).translated (c);
            const auto tip = juce::Point<float> (0.0f, -length * 0.85f).transformedBy (toPlace);
            juce::ColourGradient grad (colour.withAlpha (alpha), c, colour.withAlpha (0.0f), tip, false);
            g.setGradientFill (grad);
            g.fillPath (petal, toPlace);
        }
    }
}

void BloomSky::drawLights (juce::Graphics& g, double t) const
{
    const float brightness = (0.75f + 0.55f * activity) * (bypassed ? 0.35f : 1.0f);
    for (int i = 0; i < static_cast<int> (lights.size()); ++i)
    {
        const auto& l = lights[static_cast<size_t> (i)];
        const double cycle = (t + l.offset) / l.period;
        const int round = static_cast<int> (std::floor (cycle));
        const float u = static_cast<float> (cycle - round);   // 0 at the bottom, 1 at the top

        // Each time round the light starts from a new place along the bottom.
        const float column = std::fmod (l.column + hash01 (i, round), 1.0f);
        const float x = -20.0f + 1080.0f * column + l.sway * std::sin (static_cast<float> (twoPi * (l.swayRate * t + l.phase)));
        const float y = kHeight + 40.0f - u * (kHeight - 40.0f);

        // It opens as it rises, like a blossom, then fades high in the sky.
        const float bloom = std::pow (std::sin (juce::MathConstants<float>::pi * std::min (1.0f, u * 1.4f)), 0.6f);
        const float fade = u < 0.08f ? u / 0.08f : std::pow (1.0f - u, 0.8f);
        const float radius = l.radius * (0.45f + 0.75f * bloom);
        const float alpha = 0.62f * fade * brightness;
        if (alpha < 0.005f)
            continue;

        const auto pair = lightPair (l.hue, t);
        const auto colour = pair.heart;
        const juce::Point<float> c (x, y);
        glow (g, c, radius * 2.4f, pair.halo, alpha * 0.6f);

        if (l.petals && bloom > 0.3f)
        {
            // Six soft petals round the bigger lights, slowly turning.
            const float spin = static_cast<float> (twoPi * (t / 30.0 + l.phase));
            const float r = radius * 1.1f * bloom;
            for (int p = 0; p < 6; ++p)
            {
                const float a = spin + static_cast<float> (twoPi) * static_cast<float> (p) / 6.0f;
                const auto pc = c.translated (r * std::cos (a), r * std::sin (a));
                g.saveState();
                g.addTransform (juce::AffineTransform::rotation (a + juce::MathConstants<float>::halfPi, pc.x, pc.y));
                g.addTransform (juce::AffineTransform::scale (0.55f, 1.0f, pc.x, pc.y));
                glow (g, pc, r * 0.75f, (p % 2 == 0 ? pair.heart : pair.halo), alpha * 0.55f);
                g.restoreState();
            }
        }

        glow (g, c, radius, colour.brighter (0.25f), alpha);
        // A small bright heart, tinted with the light's colour rather than white.
        g.setColour (colour.brighter (0.9f).withAlpha (std::min (1.0f, alpha * 1.4f)));
        g.fillEllipse (juce::Rectangle<float> (radius * 0.24f, radius * 0.24f).withCentre (c));
    }
}

void BloomSky::draw (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - backgroundScale) > 0.01f || ! background.isValid())
        rebuildBackground (pixelScale);
    g.drawImageTransformed (background, juce::AffineTransform::scale (1.0f / backgroundScale));

    const double t = motion ? time : kStillTime;

    // Ease the glow towards what is sounding: up in about a fifth of a second, down in one.
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.1, time - lastTime));
    lastTime = time;
    const float rate = activityTarget > activity ? 5.0f : 1.0f;
    activity += (activityTarget - activity) * (motion ? std::min (1.0f, dt * rate) : 1.0f);

    for (const auto& s : stars)
    {
        const float alpha = motion ? 0.15f + 0.55f * breathe (t, 5.0, s.phase) : 0.4f;
        g.setColour (juce::Colour (0xfffff8ee).withAlpha (alpha));
        g.fillEllipse (s.x, s.y, s.size, s.size);
    }

    drawHaze (g, t);
    drawFlower (g, t);
    drawLights (g, t);
    BlueHourSky::drawEveningStar (g, t, motion, bypassed);
}

} // namespace sing::ui
