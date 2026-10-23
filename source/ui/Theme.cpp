#include "Theme.h"
#include "SingAssets.h"

#include <cmath>

namespace sing::ui
{

Fonts::Fonts()
{
    auto load = [] (const char* data, int size)
    {
        return juce::Typeface::createSystemTypefaceFor (data, static_cast<size_t> (size));
    };
    serifMedium   = load (SingAssets::CormorantGaramondMedium_ttf,   SingAssets::CormorantGaramondMedium_ttfSize);
    serifSemiBold = load (SingAssets::CormorantGaramondSemiBold_ttf, SingAssets::CormorantGaramondSemiBold_ttfSize);
    sansRegular   = load (SingAssets::JostRegular_ttf,               SingAssets::JostRegular_ttfSize);
    sansMedium    = load (SingAssets::JostMedium_ttf,                SingAssets::JostMedium_ttfSize);
}

juce::Font Fonts::make (juce::Typeface::Ptr face, float px, float letterSpacing)
{
    juce::Font f { juce::FontOptions (face).withPointHeight (px) };
    if (! juce::exactlyEqual (letterSpacing, 0.0f))
        f = f.withExtraKerningFactor (letterSpacing * px / f.getHeight());
    return f;
}

juce::Font Fonts::serif (float px, bool semiBold, float letterSpacing)
{
    const juce::SharedResourcePointer<Fonts> fonts;
    return make (semiBold ? fonts->serifSemiBold : fonts->serifMedium, px, letterSpacing);
}

juce::Font Fonts::sans (float px, bool medium, float letterSpacing)
{
    const juce::SharedResourcePointer<Fonts> fonts;
    return make (medium ? fonts->sansMedium : fonts->sansRegular, px, letterSpacing);
}

//==============================================================================
namespace
{
    // Coverage of a blurred edge: 1 well inside, 0 well outside.
    float edge (float distanceInside, float sigma)
    {
        return 0.5f * (1.0f + std::erf (distanceInside / (sigma * std::sqrt (2.0f))));
    }
}

void fillBlurredDisc (juce::Graphics& g, juce::Point<float> centre, float radius, float blur, juce::Colour colour)
{
    // CSS blur radius B gives a Gaussian with sigma = B / 2.
    const float sigma = std::max (0.5f, blur * 0.5f);
    const float outer = radius + 3.0f * sigma;

    juce::ColourGradient grad (colour, centre, colour.withAlpha (0.0f), centre.translated (outer, 0.0f), true);
    grad.clearColours();
    constexpr int steps = 14;
    for (int i = 0; i <= steps; ++i)
    {
        const float d = outer * static_cast<float> (i) / steps;
        grad.addColour (static_cast<double> (i) / steps, colour.withMultipliedAlpha (edge (radius - d, sigma)));
    }
    g.setGradientFill (grad);
    g.fillEllipse (juce::Rectangle<float> (outer * 2.0f, outer * 2.0f).withCentre (centre));
}

void drawGlow (juce::Graphics& g, juce::Rectangle<float> bounds, float cornerRadius, float blur, juce::Colour colour)
{
    constexpr int rings = 6;
    for (int i = rings; i >= 1; --i)
    {
        const float t = static_cast<float> (i) / rings;
        const float grow = blur * t;
        const float alpha = colour.getFloatAlpha() * (1.0f - t) * (1.0f - t) * 0.9f + 0.02f;
        g.setColour (colour.withAlpha (alpha));
        g.drawRoundedRectangle (bounds.expanded (grow * 0.5f), cornerRadius + grow * 0.5f, grow);
    }
}

void gaussianBlur (juce::Image& image, float sigma)
{
    if (sigma < 0.3f || ! image.isValid())
        return;

    image = image.convertedToFormat (juce::Image::ARGB);
    const int radius = static_cast<int> (std::ceil (sigma * 3.0f));
    std::vector<float> kernel (static_cast<size_t> (radius * 2 + 1));
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i)
        sum += kernel[static_cast<size_t> (i + radius)] = std::exp (-0.5f * static_cast<float> (i * i) / (sigma * sigma));
    for (auto& k : kernel)
        k /= sum;

    const int w = image.getWidth(), h = image.getHeight();
    std::vector<float> src (static_cast<size_t> (w * h * 4)), tmp (src.size());

    {
        const juce::Image::BitmapData bd (image, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const auto* p = bd.getPixelPointer (x, y);
                auto* d = &src[static_cast<size_t> ((y * w + x) * 4)];
                for (int c = 0; c < 4; ++c)
                    d[c] = p[c];
            }
    }

    auto pass = [&] (const std::vector<float>& in, std::vector<float>& out, bool horizontal)
    {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                float acc[4] = {};
                for (int k = -radius; k <= radius; ++k)
                {
                    const int sx = horizontal ? std::clamp (x + k, 0, w - 1) : x;
                    const int sy = horizontal ? y : std::clamp (y + k, 0, h - 1);
                    const float wk = kernel[static_cast<size_t> (k + radius)];
                    const auto* s = &in[static_cast<size_t> ((sy * w + sx) * 4)];
                    for (int c = 0; c < 4; ++c)
                        acc[c] += s[c] * wk;
                }
                auto* d = &out[static_cast<size_t> ((y * w + x) * 4)];
                for (int c = 0; c < 4; ++c)
                    d[c] = acc[c];
            }
    };
    pass (src, tmp, true);
    pass (tmp, src, false);

    const juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            auto* p = bd.getPixelPointer (x, y);
            const auto* s = &src[static_cast<size_t> ((y * w + x) * 4)];
            for (int c = 0; c < 4; ++c)
                p[c] = static_cast<juce::uint8> (std::clamp (std::lround (s[c]), 0L, 255L));
        }
}

float trackedTextWidth (const juce::String& text, const juce::Font& font)
{
    const float tracking = font.getHeight() * font.getExtraKerningFactor();
    return juce::GlyphArrangement::getStringWidth (font, text) - tracking;
}

void drawTrackedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                      const juce::Font& font, juce::Justification just, juce::Colour colour)
{
    // JUCE adds the letter spacing after the last glyph too. Shift the box so the
    // visible text sits where CSS would put it.
    const float tracking = font.getHeight() * font.getExtraKerningFactor();
    if (just.testFlags (juce::Justification::horizontallyCentred))
        area = area.translated (tracking * 0.5f, 0.0f);
    else if (just.testFlags (juce::Justification::right))
        area = area.translated (tracking, 0.0f);

    g.setColour (colour);
    g.setFont (font);
    g.drawText (text, area, just, false);
}

} // namespace sing::ui
