#include "BlueHourSky.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

namespace
{
    constexpr float kPi = juce::MathConstants<float>::pi;

    // Timings from the approved mockup.
    constexpr float kTwinklePeriod = 5.0f;  // stars: opacity 0.2 .. 0.85 .. 0.2
    constexpr float kBreathePeriod = 6.0f;  // the glow: opacity 0.55 .. 0.9 .. 0.55
    constexpr float kVenusPeriod = 6.0f;    // Venus's glow: scale 1 .. 1.08, opacity 0.9 .. 1
    constexpr float kDriftPeriod = 46.0f;   // the wisps drift 50 units left and back
    constexpr double kBypassFade = 0.6;

    // The areas the cached layers cover, in panel units.
    const juce::Rectangle<float> kGlowArea { 120.0f, 360.0f, 820.0f, 260.0f }; // cut off by the panel's bottom
    const juce::Rectangle<float> kWispArea { 10.0f, 88.0f, 880.0f, 44.0f };
    const juce::Rectangle<float> kHaloArea { 840.0f, 56.0f, 122.0f, 122.0f };

    float easeInOut (float t) { return 0.5f - 0.5f * std::cos (kPi * juce::jlimit (0.0f, 1.0f, t)); }

    // A CSS keyframe loop of `period` seconds that goes there and back within one cycle
    // (0%, 100%: low; 50%: high), eased each way. 0..1.
    float pulse (double t, float period, float delay)
    {
        double u = std::fmod (t - static_cast<double> (delay), static_cast<double> (period)) / period;
        if (u < 0.0)
            u += 1.0;
        const auto f = static_cast<float> (u);
        return easeInOut (f < 0.5f ? f * 2.0f : 2.0f - f * 2.0f);
    }

    // An "alternate" animation: there and back again over two periods, eased each way.
    float alternate (double t, float period)
    {
        const double u = std::fmod (t, 2.0 * period) / period;
        const double w = u < 0.0 ? u + 2.0 : u;
        return easeInOut (static_cast<float> (w < 1.0 ? w : 2.0 - w));
    }

    // The mockup's seeded random numbers (Park-Miller), so the plugin matches it.
    struct Rng
    {
        explicit Rng (std::int64_t s) : seed (s) {}
        float operator()()
        {
            seed = (seed * 16807) % 2147483647;
            return static_cast<float> (static_cast<double> (seed - 1) / 2147483646.0);
        }
        std::int64_t seed;
    };

    juce::Colour rgba (int r, int g, int b, float a)
    {
        return juce::Colour (static_cast<juce::uint8> (r), static_cast<juce::uint8> (g), static_cast<juce::uint8> (b), a);
    }

    // CSS radial-gradient(closest-side, ...) filling the ellipse inscribed in `box`.
    void ellipseGradient (juce::Graphics& g, juce::Rectangle<float> box, std::initializer_list<std::pair<float, juce::Colour>> stops)
    {
        const auto centre = box.getCentre();
        const float rx = box.getWidth() * 0.5f, ry = box.getHeight() * 0.5f;
        juce::Graphics::ScopedSaveState save (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, ry / rx, centre.x, centre.y));
        juce::ColourGradient grad (stops.begin()->second, centre, (stops.end() - 1)->second, centre.translated (rx, 0.0f), true);
        for (const auto& [pos, colour] : stops)
            grad.addColour (static_cast<double> (pos), colour);
        g.setGradientFill (grad);
        g.fillEllipse (juce::Rectangle<float> (rx * 2.0f, rx * 2.0f).withCentre (centre));
    }

    juce::Image renderArea (juce::Rectangle<float> area, float scale, const std::function<void (juce::Graphics&)>& paint)
    {
        juce::Image image (juce::Image::ARGB, std::max (1, juce::roundToInt (area.getWidth() * scale)),
                           std::max (1, juce::roundToInt (area.getHeight() * scale)), true);
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::translation (-area.getX(), -area.getY()).scaled (scale));
        paint (g);
        return image;
    }

    // Draws a cached layer at `area` (plus an offset), snapped to whole pixels so it's
    // blitted rather than resampled.
    void blit (juce::Graphics& g, const juce::Image& image, juce::Rectangle<float> area, float scale, float opacity,
               juce::Point<float> offset = {})
    {
        if (opacity <= 0.002f || ! image.isValid())
            return;
        const float x = std::round ((area.getX() + offset.x) * scale) / scale;
        const float y = std::round ((area.getY() + offset.y) * scale) / scale;
        g.setOpacity (juce::jmin (1.0f, opacity));
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale).translated (x, y));
        g.setOpacity (1.0f);
    }
}

//==============================================================================
BlueHourSky::BlueHourSky()
{
    // Faint stars, high in the sky only: the mockup's 46, from the same seed.
    Rng rnd (41);
    for (int i = 0; i < 46; ++i)
    {
        Star s {};
        s.x = std::round (rnd() * 1040.0f);
        s.y = std::round (rnd() * 200.0f);
        s.size = rnd() > 0.88f ? 2.0f : 1.0f;
        s.delay = -std::round (rnd() * 50.0f) / 10.0f;
        stars.push_back (s);
    }

    // The evening star: a long four-pointed star with a bright centre (the mockup's
    // 44-unit SVG, placed over the bypass button).
    venus = juce::Drawable::parseSVGPath ("M22 2 L24.2 19.8 L42 22 L24.2 24.2 L22 42 L19.8 24.2 L2 22 L19.8 19.8 Z");
    venus.addEllipse (22.0f - 4.5f, 22.0f - 4.5f, 9.0f, 9.0f);
    venus.applyTransform (juce::AffineTransform::translation (venusCentre().x - 22.0f, venusCentre().y - 22.0f));
}

void BlueHourSky::setBypassed (bool shouldBypass)
{
    const bool first = bypassStart <= -5.0;
    if (shouldBypass == bypassed && ! first)
        return;
    bypassFrom = bypassAmount();
    bypassed = shouldBypass;
    bypassTo = bypassed ? 1.0f : 0.0f;
    bypassStart = now;
    if (! motion || first) // the panel opens as it is, with no fade
        bypassFrom = bypassTo;
}

float BlueHourSky::bypassAmount() const
{
    if (! motion)
        return bypassTo;
    const float u = static_cast<float> ((now - bypassStart) / kBypassFade);
    return bypassFrom + (bypassTo - bypassFrom) * easeInOut (u);
}

void BlueHourSky::advance (double seconds)
{
    now = seconds;
}

//==============================================================================
void BlueHourSky::rebuildCaches (float pixelScale)
{
    cachedScale = pixelScale;

    // linear-gradient(180deg, #0e1236 0%, #1d2360 34%, #3d3a7e 56%, #7e5f9a 72%, #d495a6 88%, #f2bfa2 100%)
    backdrop = renderArea ({ 0.0f, 0.0f, width, height }, pixelScale, [] (juce::Graphics& g)
    {
        juce::ColourGradient grad (juce::Colour (0xff0e1236), 0.0f, 0.0f, juce::Colour (0xfff2bfa2), 0.0f, height, false);
        grad.addColour (0.34, juce::Colour (0xff1d2360));
        grad.addColour (0.56, juce::Colour (0xff3d3a7e));
        grad.addColour (0.72, juce::Colour (0xff7e5f9a));
        grad.addColour (0.88, juce::Colour (0xffd495a6));
        g.setGradientFill (grad);
        g.fillRect (0.0f, 0.0f, width, height);
    });

    // The glow where the sun went down: an 820 x 380 ellipse centred below the knobs.
    glow = renderArea (kGlowArea, pixelScale, [] (juce::Graphics& g)
    {
        ellipseGradient (g, { 120.0f, 360.0f, 820.0f, 380.0f },
                         { { 0.0f, rgba (255, 196, 170, 0.55f) }, { 0.55f, rgba (240, 150, 170, 0.18f) }, { 1.0f, rgba (240, 150, 170, 0.0f) } });
    });

    // Two faint wisps of cloud, under the header.
    wisps = renderArea (kWispArea, pixelScale, [] (juce::Graphics& g)
    {
        ellipseGradient (g, { 20.0f, 102.0f, 420.0f, 26.0f }, { { 0.0f, rgba (200, 170, 220, 0.22f) }, { 1.0f, rgba (200, 170, 220, 0.0f) } });
        ellipseGradient (g, { 520.0f, 92.0f, 360.0f, 22.0f }, { { 0.0f, rgba (230, 190, 220, 0.18f) }, { 1.0f, rgba (230, 190, 220, 0.0f) } });
    });

    // Venus's glow, lit and bypassed: a 110-unit disc, warm white in the middle.
    auto halo = [pixelScale] (float centreAlpha)
    {
        return renderArea (kHaloArea, pixelScale, [centreAlpha] (juce::Graphics& g)
        {
            ellipseGradient (g, juce::Rectangle<float> (110.0f, 110.0f).withCentre (venusCentre()),
                             { { 0.0f, rgba (255, 250, 235, centreAlpha) }, { 0.45f, rgba (220, 200, 255, 0.18f) }, { 1.0f, rgba (220, 200, 255, 0.0f) } });
        });
    };
    venusHalo = halo (0.85f);
    venusHaloDim = halo (0.15f);
}

void BlueHourSky::drawVenus (juce::Graphics& g)
{
    const float off = bypassAmount();

    // The glow swells by 8% and brightens a little every 6 s.
    const float k = motion ? pulse (now, kVenusPeriod, 0.0f) : 1.0f;
    const float scale = 1.0f + 0.08f * k;
    const float opacity = 0.9f + 0.1f * k;
    const auto c = venusCentre();
    const auto place = juce::AffineTransform::scale (1.0f / cachedScale)
                           .translated (kHaloArea.getX(), kHaloArea.getY())
                           .scaled (scale, scale, c.x, c.y);
    if (off < 0.999f)
    {
        g.setOpacity (opacity * (1.0f - off));
        g.drawImageTransformed (venusHalo, place);
    }
    if (off > 0.001f)
    {
        g.setOpacity (opacity * off);
        g.drawImageTransformed (venusHaloDim, place);
    }
    g.setOpacity (1.0f);

    // The star itself: full brightness, or 35% while bypassed.
    g.setColour (juce::Colour (0xfffffaf0).withAlpha (1.0f - 0.65f * off));
    g.fillPath (venus);
}

void BlueHourSky::draw (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - cachedScale) > 0.01f || ! backdrop.isValid())
        rebuildCaches (pixelScale);

    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImageTransformed (backdrop, juce::AffineTransform::scale (1.0f / cachedScale));

    // Stars twinkle between 20% and 85%. Held still, they rest a little below their peak.
    const juce::Colour starColour (0xfffff8ee);
    for (const auto& s : stars)
    {
        const float a = motion ? 0.2f + 0.65f * pulse (now, kTwinklePeriod, s.delay) : 0.7f;
        g.setColour (starColour.withAlpha (a));
        g.fillEllipse (s.x, s.y, s.size, s.size);
    }

    // The glow breathes between 55% and 90%.
    blit (g, glow, kGlowArea, cachedScale, motion ? 0.55f + 0.35f * pulse (now, kBreathePeriod, 0.0f) : 0.8f);

    const float drift = motion ? -50.0f * alternate (now, kDriftPeriod) : 0.0f;
    blit (g, wisps, kWispArea, cachedScale, 1.0f, { drift, 0.0f });

    drawVenus (g);
}

} // namespace sing::ui
