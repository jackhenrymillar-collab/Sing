#include "ModulePanel.h"

#include "../Parameters.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

namespace
{
constexpr float pi = 3.14159265f;
const juce::Colour lavender { 0xffb9acef };
const juce::Colour well = juce::Colour (0xff08081e).withAlpha (0.45f);

// Maps the pictures' 120 x 64 drawing units onto an area. The baseline sits at 56.
struct Units
{
    juce::Rectangle<float> r;
    float x (float u) const { return r.getX() + u * r.getWidth() / 120.0f; }
    float y (float v) const { return r.getY() + v * r.getHeight() / 64.0f; }
    float w (float u) const { return u * r.getWidth() / 120.0f; }
    float h (float v) const { return v * r.getHeight() / 64.0f; }
};

float hash (int i)
{
    const float x = std::sin ((float) i * 12.9898f) * 43758.5453f;
    return x - std::floor (x);
}

// The height of the n-th harmonic line in the pictures: falling gently with n.
float harmonicHeight (int n) { return 46.0f / (1.0f + (float) (n - 1) * 0.38f); }

void drawHarmonicLines (juce::Graphics& g, const Units& u, float offset, float alpha, float thickness)
{
    g.setColour (Colours::gold.withAlpha (alpha));
    for (int n = 1; n <= 9; ++n)
    {
        const float x = (float) (12 * n) + offset;
        if (x < 1.0f || x > 119.0f)
            continue;
        const float top = 56.0f - harmonicHeight (n);
        g.fillRoundedRectangle (u.x (x) - thickness * 0.5f, u.y (top), thickness, u.y (56.0f) - u.y (top), thickness * 0.5f);
    }
}

void drawNoiseBars (juce::Graphics& g, const Units& u, juce::Colour colour, float time, float jiggle)
{
    g.setColour (colour);
    for (int i = 0; i < 58; ++i)
    {
        const float x = 2.0f + (float) i * 2.0f;
        float hh = 3.0f + hash (i + 11) * 10.0f * (1.0f - (float) i / 90.0f);
        hh *= 1.0f + jiggle * 0.25f * std::sin (time * 3.0f + (float) i * 1.7f);
        g.fillRect (u.x (x), u.y (56.0f - hh), std::max (1.0f, u.w (1.2f)), u.h (hh));
    }
}

// Frequency on the filter picture's log axis, 20 Hz to 20 kHz across 120 units.
float filterUnit (float hz) { return 120.0f * std::log10 (std::max (hz, 20.0f) / 20.0f) / 3.0f; }
} // namespace

bool ModuleSettings::isOn (ModuleKind k) const noexcept
{
    switch (k)
    {
        case ModuleKind::window:    return true;
        case ModuleKind::onsets:    return onsetsOn;
        case ModuleKind::shift:     return shiftOn;
        case ModuleKind::smear:     return smearOn;
        case ModuleKind::tone:      return toneOn;
        case ModuleKind::harmonics: return harmonicsOn;
        case ModuleKind::filter:    return filterOn;
    }
    return true;
}

void drawModuleVisual (juce::Graphics& g, ModuleKind kind, juce::Rectangle<float> area, const ModuleSettings& s, float t)
{
    const Units u { area };
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());
    const auto base = Colours::muted.withAlpha (0.25f);

    switch (kind)
    {
        case ModuleKind::window:
        {
            // Overlapping windows, a quarter of a window apart, gliding by. Their width follows
            // the Window setting.
            const float width = juce::jmap (juce::jlimit (0.1f, 1.4f, s.windowSeconds), 0.1f, 1.4f, 22.0f, 84.0f);
            const float hop = width * 0.25f;
            const float phase = std::fmod (t * hop * 0.9f, hop);
            for (float start = -width - phase; start < 120.0f; start += hop)
            {
                juce::Path bell;
                for (int i = 0; i <= 32; ++i)
                {
                    const float f = (float) i / 32.0f;
                    const float v = 56.0f - 44.0f * 0.5f * (1.0f - std::cos (2.0f * pi * f));
                    const juce::Point<float> p { u.x (start + f * width), u.y (v) };
                    if (i == 0)
                        bell.startNewSubPath (p);
                    else
                        bell.lineTo (p);
                }
                const float centre = start + width * 0.5f;
                const float a = juce::jlimit (0.15f, 0.85f, 0.85f - std::abs (centre - 60.0f) / 80.0f);
                g.setColour (Colours::gold.withAlpha (a * 0.12f));
                g.fillPath (bell);
                g.setColour ((a > 0.7f ? Colours::goldLight : Colours::gold).withAlpha (a));
                g.strokePath (bell, juce::PathStrokeType (1.1f));
            }
            g.setColour (base);
            g.fillRect (u.x (0.0f), u.y (56.0f), u.w (120.0f), 0.6f);
            break;
        }

        case ModuleKind::onsets:
        {
            // A waveform whose attacks flash as they pass. Amount sets how bright.
            constexpr int spikes[] = { 5, 15, 23 };
            g.setColour (lavender.withAlpha (0.55f));
            for (int i = 0; i < 29; ++i)
            {
                float a = 0.08f;
                for (int sIndex : spikes)
                    if (i >= sIndex)
                        a = std::max (a, std::exp (-(float) (i - sIndex) / 3.2f));
                a *= 0.85f + 0.15f * hash (i + 3);
                const float hh = 3.0f + a * 48.0f, x = 3.0f + (float) i * 4.0f;
                g.fillRoundedRectangle (u.x (x), u.y (32.0f - hh * 0.5f), u.w (2.4f), u.h (hh), 1.0f);
            }
            const float amount = juce::jlimit (0.0f, 1.0f, s.onsets * 0.01f);
            constexpr float offsets[] = { 0.0f, 1.0f, 1.8f };
            for (int i = 0; i < 3; ++i)
            {
                const float x = 3.0f + (float) spikes[i] * 4.0f + 1.2f;
                const float p = std::fmod (t - offsets[i] + 30.0f, 3.0f) / 3.0f;
                const float flash = p < 0.08f ? p / 0.08f : (p < 0.35f ? 1.0f - (p - 0.08f) / 0.27f : 0.0f);
                const float a = 0.15f + 0.85f * flash * amount;
                g.setColour (Colours::goldLight.withAlpha (a));
                g.fillRect (u.x (x) - 0.7f, u.y (6.0f), 1.4f, u.y (58.0f) - u.y (6.0f));
                g.setColour (Colours::goldLight);
                g.fillEllipse (u.x (x) - 2.0f, u.y (6.0f) - 2.0f, 4.0f, 4.0f);
            }
            break;
        }

        case ModuleKind::shift:
        {
            // The harmonics' old places, dashed, and the harmonics moved off them by Shift.
            g.setColour (Colours::muted.withAlpha (0.25f));
            for (int n = 1; n <= 9; ++n)
            {
                const float top = 56.0f - harmonicHeight (n);
                for (float v = 56.0f; v > top; v -= 4.0f)
                    g.fillRect (u.x ((float) (12 * n)) - 0.8f, u.y (v) - u.h (2.0f), 1.6f, u.h (2.0f));
            }
            const float offset = juce::jlimit (-500.0f, 500.0f, s.shiftHz) / 500.0f * 9.0f * (0.8f + 0.2f * std::sin (t * 1.3f));
            drawHarmonicLines (g, u, offset, 1.0f, 2.2f);
            if (std::abs (s.shiftHz) >= 1.0f)
            {
                // An arrow the way they moved.
                const float dir = s.shiftHz > 0.0f ? 1.0f : -1.0f;
                const float x0 = 60.0f - dir * 7.0f, x1 = 60.0f + dir * 7.0f;
                juce::Path arrow;
                arrow.startNewSubPath (u.x (x0), u.y (5.0f));
                arrow.lineTo (u.x (x1), u.y (5.0f));
                arrow.startNewSubPath (u.x (x1 - dir * 3.0f), u.y (2.5f));
                arrow.lineTo (u.x (x1), u.y (5.0f));
                arrow.lineTo (u.x (x1 - dir * 3.0f), u.y (7.5f));
                g.setColour (Colours::goldLight);
                g.strokePath (arrow, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            break;
        }

        case ModuleKind::smear:
        {
            // Each harmonic spreads into a soft band, wider the more Smear.
            const float amount = juce::jlimit (0.0f, 1.0f, s.smear * 0.01f);
            for (int n = 1; n <= 9; ++n)
            {
                const float x = (float) (12 * n), top = 56.0f - harmonicHeight (n);
                const float spread = (0.4f + 5.5f * amount) * (0.85f + 0.15f * std::sin (t * 1.6f + (float) n));
                for (int layer = 4; layer >= 0; --layer)
                {
                    const float w = spread * (float) (layer + 1) / 5.0f;
                    g.setColour (Colours::gold.withAlpha (layer == 0 ? 0.9f : 0.16f));
                    g.fillRoundedRectangle (u.x (x - w) - 0.6f, u.y (top + (float) layer * 1.5f), u.w (2.0f * w) + 1.2f,
                                            u.y (56.0f) - u.y (top + (float) layer * 1.5f), 2.0f);
                }
            }
            break;
        }

        case ModuleKind::tone:
        {
            // Breath and noise on one side of the balance, sung harmonics on the other.
            const float tone = juce::jlimit (-1.0f, 1.0f, s.tone * 0.01f);
            const float tonal = std::min (1.0f, 1.0f + tone), noise = std::min (1.0f, 1.0f - tone);
            drawNoiseBars (g, u, lavender.withAlpha (0.08f + 0.6f * noise), t, 1.0f);
            drawHarmonicLines (g, u, 0.0f, 0.12f + 0.88f * tonal, 2.2f);
            break;
        }

        case ModuleKind::harmonics:
        {
            // A comb of glowing peaks on the key's harmonics, rising out of the noise. Width
            // sets how wide each peak is; Count how many light up.
            drawNoiseBars (g, u, lavender.withAlpha (0.16f), t, 0.5f);
            const float halfWidth = juce::jmap (std::log (juce::jlimit (1.0f, 200.0f, s.harmonicsCents)), 0.0f, std::log (200.0f), 0.7f, 5.5f);
            for (int n = 1; n <= 8; ++n)
            {
                const float c = 14.0f + 13.0f * (float) (n - 1);
                const float hh = 44.0f / (1.0f + 0.25f * (float) (n - 1));
                const bool lit = n <= s.harmonicsCount;
                const float breathe = 0.55f + 0.45f * std::sin (t * 2.6f - (float) n * 0.4f);
                juce::Path peak;
                peak.startNewSubPath (u.x (c - halfWidth * 2.2f), u.y (56.0f));
                peak.cubicTo (u.x (c - halfWidth), u.y (56.0f), u.x (c - halfWidth * 0.55f), u.y (56.0f - hh), u.x (c), u.y (56.0f - hh));
                peak.cubicTo (u.x (c + halfWidth * 0.55f), u.y (56.0f - hh), u.x (c + halfWidth), u.y (56.0f), u.x (c + halfWidth * 2.2f), u.y (56.0f));
                peak.closeSubPath();
                g.setColour (lit ? Colours::gold.withAlpha (0.35f + 0.65f * breathe) : Colours::muted.withAlpha (0.12f));
                g.fillPath (peak);
                g.setColour (Colours::goldLight.withAlpha (lit ? 0.6f : 0.15f));
                g.fillRect (u.x (c) - 0.4f, u.y (58.5f), 0.8f, u.h (3.0f));
            }
            break;
        }

        case ModuleKind::filter:
        {
            // A spectrum on a log axis, bright inside the band (or outside it, when Low is
            // above High and the band is cut out).
            const float lo = filterUnit (std::min (s.lowHz, s.highHz)), hi = filterUnit (std::max (s.lowHz, s.highHz));
            const bool stop = s.lowHz > s.highHz;
            for (int i = 0; i < 28; ++i)
            {
                const float x = 4.0f + (float) i * 4.0f;
                const float harm = (i % 3 == 1) ? 1.0f : 0.35f;
                float hh = 4.0f + harm * (40.0f - (float) i) * (0.75f + 0.25f * hash (i + 29));
                hh *= 1.0f + 0.08f * std::sin (t * 2.2f + (float) i);
                const float centre = x + 1.3f;
                const bool inside = centre >= lo && centre <= hi;
                const bool bright = inside != stop;
                g.setColour (bright ? Colours::goldLight : lavender.withAlpha (0.22f));
                g.fillRect (u.x (x), u.y (56.0f - hh), u.w (2.6f), u.h (hh));
            }
            const juce::Rectangle<float> band (u.x (lo), u.y (2.0f), std::max (1.0f, u.x (hi) - u.x (lo)), u.h (56.0f));
            g.setColour (Colours::gold.withAlpha (stop ? 0.0f : 0.08f));
            g.fillRoundedRectangle (band, 2.0f);
            g.setColour (Colours::gold.withAlpha (0.6f));
            g.drawRoundedRectangle (band, 2.0f, 0.8f);
            break;
        }
    }
}

//==============================================================================
PowerButton::PowerButton (const juce::String& moduleName)
{
    setTitle (moduleName + " on");
    setTooltip ("Switch " + moduleName.toLowerCase() + " on or off");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void PowerButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = getToggleState();
    if (on)
    {
        fillBlurredDisc (g, b.getCentre(), b.getWidth() * 0.5f, 8.0f, Colours::gold.withAlpha (0.45f));
        g.setColour (Colours::gold.withMultipliedBrightness (down ? 0.9f : 1.0f));
        g.fillEllipse (b);
    }
    else
    {
        g.setColour (Colours::night.withAlpha (highlighted ? 0.8f : 0.6f));
        g.fillEllipse (b);
        g.setColour (Colours::muted.withAlpha (highlighted ? 0.7f : 0.4f));
        g.drawEllipse (b.reduced (0.5f), 1.0f);
    }

    // The power symbol: an open ring and a stroke down into it.
    const auto c = b.getCentre();
    const float r = b.getWidth() * 0.2f;
    juce::Path icon;
    icon.addCentredArc (c.x, c.y + r * 0.1f, r, r, 0.0f, 0.75f, 2.0f * pi - 0.75f, true);
    icon.startNewSubPath (c.x, c.y - r * 1.25f);
    icon.lineTo (c.x, c.y);
    g.setColour (on ? Colours::ink : Colours::muted);
    g.strokePath (icon, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
ModuleTile::ModuleTile (ModuleKind k, const juce::String& n, bool fresh)
    : moduleKind (k), name (n), isNew (fresh)
{
    setTitle (n);
    if (k != ModuleKind::window)
    {
        powerButton = std::make_unique<PowerButton> (n);
        addAndMakeVisible (*powerButton);
    }
}

TextDragger& ModuleTile::addControl (const juce::String& caption, const juce::String& widestValue)
{
    controls.push_back (std::make_unique<TextDragger> (caption, false));
    widest.push_back (widestValue);
    addAndMakeVisible (*controls.back());
    return *controls.back();
}

void ModuleTile::update (const ModuleSettings& s, float time)
{
    settings = s;
    t = time;
    const bool on = s.isOn (moduleKind);
    for (auto& c : controls)
        c->setAlpha (on ? 1.0f : 0.55f);
    repaint();
}

juce::Rectangle<float> ModuleTile::visualArea() const
{
    return { 14.0f, 44.0f, (float) getWidth() - 28.0f, 70.0f };
}

void ModuleTile::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = settings.isOn (moduleKind);

    g.setColour (Colours::night.withAlpha (0.62f));
    g.fillRoundedRectangle (b, 14.0f);
    g.setColour (on ? Colours::gold.withAlpha (0.42f) : Colours::muted.withAlpha (0.16f));
    g.drawRoundedRectangle (b, 14.0f, 1.0f);

    const auto nameFont = Fonts::sans (11.0f, true, 0.18f);
    drawTrackedText (g, name, { 14.0f, 8.0f, 160.0f, 30.0f }, nameFont, juce::Justification::centredLeft, Colours::gold);
    if (isNew)
    {
        const float x = 14.0f + trackedTextWidth (name, nameFont) + 8.0f;
        const auto tagFont = Fonts::sans (9.0f, true, 0.18f);
        const juce::Rectangle<float> tag (x, 16.0f, trackedTextWidth ("NEW", tagFont) + 12.0f, 14.0f);
        g.setColour (lavender);
        g.fillRoundedRectangle (tag, 6.0f);
        drawTrackedText (g, "NEW", tag, tagFont, juce::Justification::centred, Colours::ink);
    }
    if (powerButton == nullptr)
        drawTrackedText (g, "ALWAYS ON", { (float) getWidth() - 110.0f, 8.0f, 96.0f, 30.0f }, Fonts::sans (9.0f, false, 0.16f),
                         juce::Justification::centredRight, Colours::muted);

    const auto area = visualArea();
    g.setColour (well);
    g.fillRoundedRectangle (area, 8.0f);
    {
        juce::Graphics::ScopedSaveState save (g);
        if (! on)
            g.setOpacity (0.35f);
        drawModuleVisual (g, moduleKind, area.reduced (8.0f, 4.0f), settings, on ? t : 0.0f);
    }
}

void ModuleTile::resized()
{
    if (powerButton != nullptr)
        powerButton->setBounds (getWidth() - 44, 6, 32, 32);
    float x = 14.0f;
    for (size_t i = 0; i < controls.size(); ++i)
    {
        const float w = controls[i]->preferredWidth (widest[i]);
        controls[i]->setBounds (juce::Rectangle<float> (x, 122.0f, w, 28.0f).toNearestInt());
        x += w + 16.0f;
    }
}

//==============================================================================
ResultTile::ResultTile() { setTitle ("The result"); }

void ResultTile::update (const ModuleSettings& s, float time)
{
    settings = s;
    t = time;
    repaint();
}

void ResultTile::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Colours::gold.withAlpha (0.06f));
    g.fillRoundedRectangle (b, 14.0f);
    {
        juce::Path outline;
        outline.addRoundedRectangle (b, 14.0f);
        juce::Path dashed;
        const float dashes[] = { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (Colours::gold.withAlpha (0.35f));
        g.fillPath (dashed);
    }
    drawTrackedText (g, "THE RESULT", { 14.0f, 8.0f, 160.0f, 30.0f }, Fonts::sans (11.0f, true, 0.18f),
                     juce::Justification::centredLeft, Colours::goldLight);

    const auto& s = settings;
    int count = 0;
    for (auto k : { ModuleKind::onsets, ModuleKind::shift, ModuleKind::smear, ModuleKind::tone, ModuleKind::harmonics, ModuleKind::filter })
        count += s.isOn (k) ? 1 : 0;
    drawTrackedText (g, juce::String (count) + " of 6 modules on", { 14.0f, 122.0f, b.getWidth() - 28.0f, 28.0f },
                     Fonts::sans (12.0f, false, 0.02f), juce::Justification::centredLeft, Colours::muted);

    // The spectrum of a note at 110 Hz on a log axis (50 Hz to 16 kHz), after each module.
    const juce::Rectangle<float> area (14.0f, 44.0f, b.getWidth() - 28.0f, 70.0f);
    g.setColour (well);
    g.fillRoundedRectangle (area, 8.0f);
    const auto plot = area.reduced (6.0f, 4.0f);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (plot.toNearestInt());
    const float logSpan = std::log (16000.0f / 50.0f);
    auto xOf = [&] (float hz) { return plot.getX() + plot.getWidth() * std::log (std::max (hz, 50.0f) / 50.0f) / logSpan; };
    auto hzOf = [&] (float x) { return 50.0f * std::exp ((x - plot.getX()) / plot.getWidth() * logSpan); };
    auto envelope = [] (float hz)
    {
        auto bump = [hz] (float centre, float spread) { const float d = std::log (hz / centre); return std::exp (-d * d / spread); };
        return 0.25f + 0.75f * bump (700.0f, 0.18f) + 0.55f * bump (1200.0f, 0.12f) + 0.35f * bump (2700.0f, 0.1f);
    };
    auto filterGain = [&] (float hz)
    {
        if (! s.filterOn)
            return 1.0f;
        const float lo = std::min (s.lowHz, s.highHz), hi = std::max (s.lowHz, s.highHz);
        const bool inside = hz >= lo && hz <= hi;
        return inside != (s.lowHz > s.highHz) ? 1.0f : 0.0f;
    };
    const float tone = s.toneOn ? juce::jlimit (-1.0f, 1.0f, s.tone * 0.01f) : 0.0f;
    const float tonal = std::min (1.0f, 1.0f + tone), noise = std::min (1.0f, 1.0f - tone);
    const float base = plot.getBottom();

    // Breath and noise: between the harmonics, unless Harmonics keeps only those.
    g.setColour (lavender.withAlpha ((s.harmonicsOn ? 0.1f : 0.4f) * noise));
    for (float x = plot.getX(); x < plot.getRight(); x += 3.0f)
    {
        const float hz = hzOf (x);
        const float hh = (4.0f + hash ((int) x + 7) * 18.0f * envelope (hz)) * filterGain (hz) * plot.getHeight() / 70.0f;
        g.fillRect (x, base - hh, 1.6f, hh);
    }

    // The harmonics, moved by Shift, spread by Smear, kept by Harmonics and Filter.
    const float shift = s.shiftOn ? s.shiftHz : 0.0f;
    const float width = 1.2f + (s.smearOn ? s.smear * 0.04f : 0.0f);
    const int last = s.harmonicsOn ? s.harmonicsCount : 200;
    const float breathe = 0.8f + 0.2f * std::sin (t * 2.0f);
    for (int n = 1; n <= last; ++n)
    {
        const float hz = 110.0f * (float) n + shift;
        if (hz > 16000.0f)
            break;
        if (hz < 50.0f)
            continue;
        const float gain = filterGain (hz) * tonal;
        if (gain <= 0.0f)
            continue;
        const float hh = std::min (plot.getHeight() - 4.0f, plot.getHeight() * 0.9f * envelope (hz) / std::pow ((float) n, 0.35f)) * breathe;
        const float x = xOf (hz);
        g.setColour (Colours::goldLight.withAlpha (0.85f * gain));
        g.fillRoundedRectangle (x - width * 0.5f, base - hh, width, hh, width * 0.5f);
    }
    if (s.filterOn)
    {
        g.setColour (Colours::gold.withAlpha (0.25f));
        for (float y = plot.getY(); y < plot.getBottom(); y += 5.0f)
        {
            g.fillRect (xOf (s.lowHz) - 0.5f, y, 1.0f, 2.5f);
            g.fillRect (xOf (s.highHz) - 0.5f, y, 1.0f, 2.5f);
        }
    }
}

//==============================================================================
AdvancedPanel::AdvancedPanel (juce::AudioProcessorValueTreeState& s) : state (s)
{
    setTitle ("Advanced");
    auto add = [this] (ModuleKind k, const juce::String& n, bool fresh) -> ModuleTile&
    {
        tiles.push_back (std::make_unique<ModuleTile> (k, n, fresh));
        addAndMakeVisible (*tiles.back());
        return *tiles.back();
    };
    auto attach = [this] (TextDragger& d, const char* id, std::function<juce::String (double)> format, const juce::String& tip)
    {
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, id, d));
        d.formatValue = std::move (format);
        auto* p = state.getParameter (id);
        d.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        d.setTooltip (tip);
    };
    auto power = [this] (ModuleTile& t, const char* id)
    {
        buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, id, *t.power()));
    };
    const juce::String minus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    auto& window = add (ModuleKind::window, "WINDOW", false);
    attach (window.addControl ("Size", "680 ms"), ParamID::window, [] (double v) { return formatSeconds ((float) v); },
            "How long a slice of the voice each moment hears: longer is smoother and slower to change");

    auto& onsets = add (ModuleKind::onsets, "ONSETS", false);
    power (onsets, ParamID::onsetsOn);
    attach (onsets.addControl ("Amount", "100%"), ParamID::onsets, [] (double v) { return formatPercent ((float) v); },
            "How much consonants and attacks pass at the recorded speed instead of smearing");

    auto& shift = add (ModuleKind::shift, "SHIFT", false);
    power (shift, ParamID::shiftOn);
    attach (shift.addControl ("By", minus + "500 Hz"), ParamID::shift, [] (double v) { return formatShift ((float) v); },
            "Moves every partial by the same number of Hz: bell-like, inharmonic shimmer");

    auto& smear = add (ModuleKind::smear, "SMEAR", false);
    power (smear, ParamID::smearOn);
    attach (smear.addControl ("Amount", "100%"), ParamID::smear, [] (double v) { return formatPercent ((float) v); },
            "Smears each frequency over its neighbours: wider, softer, less exact");

    auto& tone = add (ModuleKind::tone, "TONE", false);
    power (tone, ParamID::toneOn);
    attach (tone.addControl ("Balance", "Breath 100"), ParamID::tone, [] (double v) { return formatTone ((float) v); },
            "Down keeps only the breath and noise; up keeps only the sung harmonics");

    auto& harmonics = add (ModuleKind::harmonics, "HARMONICS", true);
    power (harmonics, ParamID::harmonicsOn);
    attach (harmonics.addControl ("Width", "200 ct"), ParamID::harmonicsWidth, [] (double v) { return formatCents ((float) v); },
            "How far either side of each harmonic still passes. Narrow is a pure, glassy chord");
    attach (harmonics.addControl ("Count", "64"), ParamID::harmonicsCount, [] (double v) { return juce::String (juce::roundToInt (v)); },
            "How many of the key's harmonics are kept");

    auto& filter = add (ModuleKind::filter, "FILTER", true);
    power (filter, ParamID::filterOn);
    attach (filter.addControl ("Low", "20.0 kHz"), ParamID::filterLow, [] (double v) { return formatHz ((float) v); },
            "The bottom of the band that is kept. Set it above High to cut the band out instead");
    attach (filter.addControl ("High", "20.0 kHz"), ParamID::filterHigh, [] (double v) { return formatHz ((float) v); },
            "The top of the band that is kept");

    addAndMakeVisible (result);
    addAndMakeVisible (resetButton);
    resetButton.setTooltip ("Every module back to how it starts");
    resetButton.onClick = [this] { resetAll(); };

    const auto now = readSettings();
    for (auto& t : tiles)
        t->update (now, 0.0f);
    result.update (now, 0.0f);
}

AdvancedPanel::~AdvancedPanel() = default;

ModuleSettings AdvancedPanel::readSettings() const
{
    auto value = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
    ModuleSettings s;
    s.windowSeconds = value (ParamID::window);
    s.onsets = value (ParamID::onsets);
    s.shiftHz = value (ParamID::shift);
    s.smear = value (ParamID::smear);
    s.tone = value (ParamID::tone);
    s.harmonicsCents = value (ParamID::harmonicsWidth);
    s.harmonicsCount = juce::roundToInt (value (ParamID::harmonicsCount));
    s.lowHz = value (ParamID::filterLow);
    s.highHz = value (ParamID::filterHigh);
    s.onsetsOn = value (ParamID::onsetsOn) > 0.5f;
    s.shiftOn = value (ParamID::shiftOn) > 0.5f;
    s.smearOn = value (ParamID::smearOn) > 0.5f;
    s.toneOn = value (ParamID::toneOn) > 0.5f;
    s.harmonicsOn = value (ParamID::harmonicsOn) > 0.5f;
    s.filterOn = value (ParamID::filterOn) > 0.5f;
    return s;
}

void AdvancedPanel::resetAll()
{
    for (auto* id : { ParamID::window, ParamID::onsetsOn, ParamID::onsets, ParamID::shiftOn, ParamID::shift, ParamID::smearOn, ParamID::smear,
                      ParamID::toneOn, ParamID::tone, ParamID::harmonicsOn, ParamID::harmonicsWidth, ParamID::harmonicsCount,
                      ParamID::filterOn, ParamID::filterLow, ParamID::filterHigh })
    {
        auto* p = state.getParameter (id);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getDefaultValue());
        p->endChangeGesture();
    }
}

void AdvancedPanel::frame (double seconds, bool animate, float activity)
{
    const auto dt = (float) (lastSeconds < 0.0 ? 0.0 : juce::jlimit (0.0, 0.1, seconds - lastSeconds));
    lastSeconds = seconds;
    // The pictures drift slowly when all is quiet and move freely while notes sound.
    if (animate)
        visualTime += dt * (0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, activity * 4.0f));
    const auto now = readSettings();
    for (auto& t : tiles)
        t->update (now, visualTime);
    result.update (now, visualTime);
}

void AdvancedPanel::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const auto capFont = Fonts::sans (12.0f, false, 0.26f);
    drawTrackedText (g, "ADVANCED", { 0.0f, 0.0f, 120.0f, headingHeight }, capFont, juce::Justification::centredLeft, Colours::gold);
    const float x = trackedTextWidth ("ADVANCED", capFont) + 18.0f;
    drawTrackedText (g, "Bloom's modules, in the order the sound passes through them. Tap a light to switch one off.",
                     { x, 0.0f, w - x - 140.0f, headingHeight }, Fonts::sans (12.0f), juce::Justification::centredLeft, Colours::muted);
}

void AdvancedPanel::resized()
{
    const float w = (float) getWidth();
    const float resetW = 32.0f + trackedTextWidth ("RESET ALL", Fonts::sans (12.0f, false, 0.18f)) + 2.0f;
    resetButton.setBounds (juce::Rectangle<float> (w - resetW, 0.0f, resetW, headingHeight).toNearestInt());

    constexpr float gap = 14.0f, rowGap = 12.0f;
    const float tileW = (w - 3.0f * gap) / 4.0f;
    const float top = headingHeight + 10.0f;
    for (size_t i = 0; i <= tiles.size(); ++i)
    {
        const int col = (int) i % 4, row = (int) i / 4;
        const juce::Rectangle<float> r (col * (tileW + gap), top + row * (ModuleTile::height + rowGap), tileW, ModuleTile::height);
        if (i < tiles.size())
            tiles[i]->setBounds (r.toNearestInt());
        else
            result.setBounds (r.toNearestInt());
    }
}

} // namespace sing::ui
