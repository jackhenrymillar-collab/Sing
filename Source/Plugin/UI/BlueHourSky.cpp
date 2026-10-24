#include "BlueHourSky.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

namespace
{
    constexpr float kWidth = panelWidth, kHeight = panelHeight;
    // The sky was first drawn for a 1040 x 620 panel; its glow and clouds stretch to fit.
    constexpr float kStretchX = kWidth / 1040.0f, kStretchY = kHeight / 620.0f;
    constexpr double twoPi = 6.28318530717958647692;

    // A slow, smooth 0..1 wave, like the mockup's ease-in-out keyframes.
    float breathe (double t, double period, double phase = 0.0)
    {
        return static_cast<float> (0.5 - 0.5 * std::cos (twoPi * (t / period + phase)));
    }
}

BlueHourSky::BlueHourSky()
{
    // The mockup's stars: the same seeded generator, so they sit in the same places.
    juce::int64 seed = 41;
    auto rnd = [&seed]
    {
        seed = (seed * 16807) % 2147483647;
        return static_cast<float> (seed - 1) / 2147483646.0f;
    };
    for (int i = 0; i < 46; ++i)
    {
        const float x = std::round (rnd() * kWidth), y = std::round (rnd() * 200.0f);
        const float size = rnd() > 0.88f ? 2.0f : 1.0f;
        stars.push_back ({ x, y, size, rnd() });
    }
}

juce::Path BlueHourSky::starShape (float r)
{
    // The mockup's 44-unit star: four long points with narrow waists.
    juce::Path p;
    const float k = r / 20.0f;
    p.startNewSubPath (0.0f, -20.0f * k);
    p.lineTo (2.2f * k, -2.2f * k);
    p.lineTo (20.0f * k, 0.0f);
    p.lineTo (2.2f * k, 2.2f * k);
    p.lineTo (0.0f, 20.0f * k);
    p.lineTo (-2.2f * k, 2.2f * k);
    p.lineTo (-20.0f * k, 0.0f);
    p.lineTo (-2.2f * k, -2.2f * k);
    p.closeSubPath();
    return p;
}

void BlueHourSky::rebuildBackground (float pixelScale)
{
    backgroundScale = pixelScale;
    background = juce::Image (juce::Image::RGB, juce::roundToInt (kWidth * pixelScale), juce::roundToInt (kHeight * pixelScale), false);
    juce::Graphics g (background);
    g.addTransform (juce::AffineTransform::scale (pixelScale));

    // linear-gradient(180deg, #0e1236 0%, #1d2360 34%, #3d3a7e 56%, #7e5f9a 72%, #d495a6 88%, #f2bfa2 100%)
    juce::ColourGradient sky (juce::Colour (0xff0e1236), 0.0f, 0.0f, juce::Colour (0xfff2bfa2), 0.0f, kHeight, false);
    sky.addColour (0.34, juce::Colour (0xff1d2360));
    sky.addColour (0.56, juce::Colour (0xff3d3a7e));
    sky.addColour (0.72, juce::Colour (0xff7e5f9a));
    sky.addColour (0.88, juce::Colour (0xffd495a6));
    g.setGradientFill (sky);
    g.fillAll();
}

void BlueHourSky::draw (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - backgroundScale) > 0.01f || ! background.isValid())
        rebuildBackground (pixelScale);
    g.drawImageTransformed (background, juce::AffineTransform::scale (1.0f / backgroundScale));

    const double t = motion ? time : 0.0;

    // Faint stars, high in the sky, twinkling between 0.2 and 0.85 over 5 s.
    for (const auto& s : stars)
    {
        const float alpha = motion ? 0.2f + 0.65f * breathe (t, 5.0, s.phase) : 0.5f;
        g.setColour (juce::Colour (0xfffff8ee).withAlpha (alpha));
        g.fillEllipse (s.x, s.y, s.size, s.size);
    }

    g.saveState();
    g.addTransform (juce::AffineTransform::scale (kStretchX, kStretchY));

    // The glow where the sun went down, breathing between 0.55 and 0.9 over 6 s.
    {
        const float alpha = motion ? 0.55f + 0.35f * breathe (t, 6.0) : 0.7f;
        const juce::Rectangle<float> glow (120.0f, 360.0f, 820.0f, 380.0f);
        juce::ColourGradient grad (juce::Colour (0xffffc4aa).withAlpha (0.55f * alpha), glow.getCentre(),
                                   juce::Colour (0xfff096aa).withAlpha (0.0f), glow.getCentre().translated (glow.getWidth() * 0.5f, 0.0f), true);
        grad.addColour (0.55, juce::Colour (0xfff096aa).withAlpha (0.18f * alpha));
        g.saveState();
        g.addTransform (juce::AffineTransform::scale (1.0f, glow.getHeight() / glow.getWidth(), glow.getCentreX(), glow.getCentreY()));
        g.setGradientFill (grad);
        g.fillEllipse (glow.withSizeKeepingCentre (glow.getWidth(), glow.getWidth()));
        g.restoreState();
    }

    // Two wisps of cloud drifting 50 units back and forth over 46 s.
    {
        const float drift = motion ? -50.0f * breathe (t, 92.0) : 0.0f;
        auto wisp = [&g] (juce::Rectangle<float> r, juce::Colour c)
        {
            juce::ColourGradient grad (c, r.getCentre(), c.withAlpha (0.0f), r.getCentre().translated (r.getWidth() * 0.5f, 0.0f), true);
            g.saveState();
            g.addTransform (juce::AffineTransform::scale (1.0f, r.getHeight() / r.getWidth(), r.getCentreX(), r.getCentreY()));
            g.setGradientFill (grad);
            g.fillEllipse (r.withSizeKeepingCentre (r.getWidth(), r.getWidth()));
            g.restoreState();
        };
        wisp ({ -40.0f + 60.0f + drift, 102.0f, 420.0f, 26.0f }, juce::Colour (0xffc8aadc).withAlpha (0.22f));
        wisp ({ -40.0f + 560.0f + drift, 92.0f, 360.0f, 22.0f }, juce::Colour (0xffe6bedc).withAlpha (0.18f));
    }
    g.restoreState();

    drawEveningStar (g, t, motion, bypassed);
}

void BlueHourSky::drawEveningStar (juce::Graphics& g, double t, bool motion, bool bypassed)
{
    // The evening star: a soft halo and the four-pointed star, gently pulsing over 6 s.
    {
        const auto c = starCentre();
        const float pulse = motion ? breathe (t, 6.0) : 0.5f;
        const float scale = 1.0f + 0.08f * pulse;
        const float glowAlpha = bypassed ? 0.15f : 0.85f * (0.9f + 0.1f * pulse);

        juce::ColourGradient halo (juce::Colour (0xfffffaeb).withAlpha (glowAlpha), c,
                                   juce::Colour (0xffdcc8ff).withAlpha (0.0f), c.translated (55.0f * scale, 0.0f), true);
        halo.addColour (0.45, juce::Colour (0xffdcc8ff).withAlpha (0.18f));
        g.setGradientFill (halo);
        g.fillEllipse (juce::Rectangle<float> (110.0f * scale, 110.0f * scale).withCentre (c));

        const float alpha = bypassed ? 0.35f : 1.0f;
        g.setColour (juce::Colour (0xfffffaf0).withAlpha (alpha));
        g.fillPath (starShape (20.0f * scale), juce::AffineTransform::translation (c));
        g.setColour (juce::Colour (0xfffffdf6).withAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (9.0f * scale, 9.0f * scale).withCentre (c));
    }
}

} // namespace sing::ui
