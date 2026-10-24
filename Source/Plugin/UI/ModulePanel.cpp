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

// Frequency on the filter picture's log axis, 20 Hz to 20 kHz across 120 units, and back.
float filterUnit (float hz) { return 120.0f * std::log10 (std::max (hz, 20.0f) / 20.0f) / 3.0f; }
float unitHz (float unit) { return 20.0f * std::pow (10.0f, unit * 3.0f / 120.0f); }

// How much of a frequency a low and high cut keeps: slope 0 is a hard edge, 1 and 2 are 6
// and 12 dB per octave. Low above High cuts the band out instead.
float cutGain (float hz, float low, float high, int slope)
{
    if (slope <= 0)
    {
        const bool inside = hz >= std::min (low, high) && hz <= std::max (low, high);
        return inside != (low > high) ? 1.0f : 0.0f;
    }
    const float order = (float) slope;
    const float hp = 1.0f / std::sqrt (1.0f + std::pow (low / hz, 2.0f * order));
    const float lp = 1.0f / std::sqrt (1.0f + std::pow (hz / high, 2.0f * order));
    if (low > high)
        return std::min (1.0f, std::sqrt (hp * hp + lp * lp));
    return hp * lp;
}

// A spectrum of bars on the log axis, each lit by how much the cut keeps.
void drawCutSpectrum (juce::Graphics& g, const Units& u, float low, float high, int slope, float t, juce::Colour lit, juce::Colour dim)
{
    for (int i = 0; i < 28; ++i)
    {
        const float x = 4.0f + (float) i * 4.0f;
        const float harm = (i % 3 == 1) ? 1.0f : 0.35f;
        float hh = 4.0f + harm * (40.0f - (float) i) * (0.75f + 0.25f * hash (i + 29));
        hh *= 1.0f + 0.08f * std::sin (t * 2.2f + (float) i);
        const float gain = cutGain (unitHz (x + 1.3f), low, high, slope);
        g.setColour (dim.interpolatedWith (lit, gain));
        g.fillRect (u.x (x), u.y (56.0f - hh), u.w (2.6f), u.h (hh));
    }
}
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
        case ModuleKind::compressor:  return compressorOn;
        case ModuleKind::octaves:     return octavesOn;
        case ModuleKind::binaural:    return binauralOn;
        case ModuleKind::spaceFilter: return spaceFilterOn;
    }
    return true;
}

bool worksInBothEngines (ModuleKind k) noexcept
{
    return k == ModuleKind::binaural || k == ModuleKind::spaceFilter;
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
            // Overlapping windows, half a window apart, gliding by. Their width follows the
            // Window setting.
            const float span = std::log (40.0f / 0.1f);
            const float width = juce::jmap (std::log (juce::jlimit (0.1f, 40.0f, s.windowSeconds) / 0.1f), 0.0f, span, 22.0f, 96.0f);
            const float hop = width * 0.5f;
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
            // The softer slopes fade the bars out gradually past the edges.
            const float lo = filterUnit (std::min (s.lowHz, s.highHz)), hi = filterUnit (std::max (s.lowHz, s.highHz));
            const bool stop = s.lowHz > s.highHz;
            drawCutSpectrum (g, u, s.lowHz, s.highHz, s.filterSlope, t, Colours::goldLight, lavender.withAlpha (0.22f));
            const juce::Rectangle<float> band (u.x (lo), u.y (2.0f), std::max (1.0f, u.x (hi) - u.x (lo)), u.h (56.0f));
            g.setColour (Colours::gold.withAlpha (stop ? 0.0f : 0.08f));
            g.fillRoundedRectangle (band, 2.0f);
            g.setColour (Colours::gold.withAlpha (0.6f));
            g.drawRoundedRectangle (band, 2.0f, 0.8f);
            break;
        }

        case ModuleKind::compressor:
        {
            // Loudness over time scrolling by: the loud and quiet moments draw in towards a
            // steady line, more so the higher the Amount.
            const float amount = juce::jlimit (0.0f, 1.0f, s.compressor * 0.01f);
            constexpr float target = 26.0f;
            const float scroll = t * 3.0f;
            const int first = (int) std::floor (scroll);
            const float frac = scroll - (float) first;
            for (int i = 0; i < 31; ++i)
            {
                const int n = first + i;
                const float raw = 6.0f + 44.0f * (0.5f + 0.5f * std::sin ((float) n * 0.55f)) * (0.6f + 0.4f * hash (n + 101));
                const float hh = raw + (target - raw) * amount * 0.85f;
                const float x = 2.0f + ((float) i - frac) * 4.0f;
                g.setColour (lavender.withAlpha (0.35f));
                g.fillRect (u.x (x), u.y (56.0f - raw), u.w (2.6f), 1.0f);
                g.setColour (Colours::gold.withAlpha (0.85f));
                g.fillRoundedRectangle (u.x (x), u.y (56.0f - hh), u.w (2.6f), u.h (hh), 1.0f);
            }
            g.setColour (Colours::goldLight.withAlpha (0.25f + 0.5f * amount));
            for (float x = 0.0f; x < 120.0f; x += 5.0f)
                g.fillRect (u.x (x), u.y (56.0f - target) - 0.5f, u.w (2.5f), 1.0f);
            break;
        }

        case ModuleKind::octaves:
        {
            // The voice's harmonics, and copies an octave or two down (lavender) and up
            // (pale gold), each as tall as its level.
            constexpr float ratios[ModuleSettings::numOctaves] = { 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f };
            const juce::Colour colours[ModuleSettings::numOctaves] = { lavender, lavender, Colours::gold, Colours::goldLight,
                                                                       Colours::goldLight, Colours::cream };
            const float fundamental = 14.0f;
            for (int i = 0; i < ModuleSettings::numOctaves; ++i)
            {
                const float level = juce::jlimit (0.0f, 1.0f, s.octaveLevels[(size_t) i] * 0.01f);
                if (level <= 0.0f)
                    continue;
                const float spacing = fundamental * ratios[i];
                const float thickness = ratios[i] < 1.0f ? 1.6f : 2.2f;
                for (int n = 1; (float) n * spacing < 119.0f; ++n)
                {
                    const float x = (float) n * spacing;
                    const float breathe = 0.85f + 0.15f * std::sin (t * 1.8f + (float) i * 1.3f + (float) n * 0.5f);
                    const float hh = 50.0f * std::sqrt (level) * breathe / (1.0f + 0.012f * x);
                    g.setColour (colours[i].withAlpha (0.45f + 0.55f * level));
                    g.fillRoundedRectangle (u.x (x) - thickness * 0.5f, u.y (56.0f - hh), thickness, u.h (hh), thickness * 0.5f);
                }
            }
            g.setColour (base);
            g.fillRect (u.x (0.0f), u.y (56.0f), u.w (120.0f), 0.6f);
            break;
        }

        case ModuleKind::binaural:
        {
            // Two tones, one in each ear, a few Hz apart: where they meet they swell and fade
            // at the beat rate.
            const float amount = juce::jlimit (0.0f, 1.0f, s.binauralAmount * 0.01f);
            const float beat = juce::jlimit (0.1f, 30.0f, s.beatHz);
            const float drift = t * std::min (beat, 6.0f) * 0.9f;
            juce::Path left, right, beatPath;
            for (int i = 0; i <= 120; ++i)
            {
                const float x = (float) i;
                const float phase = x * 0.55f;
                const float offset = amount * (x / 120.0f * 2.0f * pi + drift);
                const float yl = 30.0f - 13.0f * std::sin (phase + t * 2.0f);
                const float yr = 30.0f - 13.0f * std::sin (phase + t * 2.0f + offset);
                const float swell = std::abs (std::cos (0.5f * offset));
                const juce::Point<float> pl { u.x (x), u.y (yl) }, pr { u.x (x), u.y (yr) }, pb { u.x (x), u.y (56.0f - 9.0f * swell) };
                if (i == 0)
                {
                    left.startNewSubPath (pl);
                    right.startNewSubPath (pr);
                    beatPath.startNewSubPath (pb);
                }
                else
                {
                    left.lineTo (pl);
                    right.lineTo (pr);
                    beatPath.lineTo (pb);
                }
            }
            g.setColour (lavender.withAlpha (0.75f));
            g.strokePath (right, juce::PathStrokeType (1.1f));
            g.setColour (Colours::gold.withAlpha (0.9f));
            g.strokePath (left, juce::PathStrokeType (1.1f));
            g.setColour (Colours::goldLight.withAlpha (0.5f));
            g.strokePath (beatPath, juce::PathStrokeType (1.0f));
            const auto capFont = Fonts::sans (8.5f, false, 0.12f);
            drawTrackedText (g, "L", { u.x (1.0f), u.y (2.0f), 12.0f, 10.0f }, capFont, juce::Justification::centredLeft, Colours::gold);
            drawTrackedText (g, "R", { u.x (1.0f) + 10.0f, u.y (2.0f), 12.0f, 10.0f }, capFont, juce::Justification::centredLeft, lavender);
            break;
        }

        case ModuleKind::spaceFilter:
        {
            // The reverb's wash across the spectrum, kept between Low and High. The dry voice
            // is untouched, so a faint outline shows what the cut takes away.
            const float lo = filterUnit (std::min (s.spaceLowHz, s.spaceHighHz)), hi = filterUnit (std::max (s.spaceLowHz, s.spaceHighHz));
            for (int i = 0; i < 58; ++i)
            {
                const float x = 2.0f + (float) i * 2.0f;
                float hh = 8.0f + 30.0f * (0.6f + 0.4f * hash (i + 57)) * (1.0f - (float) i / 110.0f);
                hh *= 0.85f + 0.15f * std::sin (t * 1.4f + (float) i * 0.7f);
                const float gain = cutGain (unitHz (x + 0.6f), s.spaceLowHz, s.spaceHighHz, 2);
                g.setColour (lavender.withAlpha (0.14f));
                g.fillRect (u.x (x), u.y (56.0f - hh), std::max (1.0f, u.w (1.2f)), u.h (hh));
                g.setColour (lavender.interpolatedWith (Colours::goldLight, 0.35f).withAlpha (0.85f * gain));
                g.fillRect (u.x (x), u.y (56.0f - hh * gain), std::max (1.0f, u.w (1.2f)), u.h (hh * gain));
            }
            g.setColour (Colours::gold.withAlpha (0.5f));
            for (float y = 4.0f; y < 56.0f; y += 5.0f)
            {
                g.fillRect (u.x (lo) - 0.5f, u.y (y), 1.0f, u.h (2.5f));
                g.fillRect (u.x (hi) - 0.5f, u.y (y), 1.0f, u.h (2.5f));
            }
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

TextDragger& ModuleTile::addControl (const juce::String& caption)
{
    controls.push_back (std::make_unique<TextDragger> (caption, false));
    controls.back()->setRowStyle (true);
    addAndMakeVisible (*controls.back());
    return *controls.back();
}

VerticalFader& ModuleTile::addFader (const juce::String& caption)
{
    faders.push_back (std::make_unique<VerticalFader> (caption));
    addAndMakeVisible (*faders.back());
    return *faders.back();
}

PillToggle& ModuleTile::addChoice (const juce::String& text)
{
    choices.push_back (std::make_unique<PillToggle> (text, 22.0f, 10.0f));
    choices.back()->setClickingTogglesState (false);
    addAndMakeVisible (*choices.back());
    return *choices.back();
}

void ModuleTile::update (const ModuleSettings& s, float time, bool resting)
{
    settings = s;
    t = time;
    rest = resting;
    const bool on = s.isOn (moduleKind);
    for (auto& c : controls)
        c->setAlpha (on ? 1.0f : 0.55f);
    for (auto& f : faders)
        f->setAlpha (on ? 1.0f : 0.55f);
    for (auto& c : choices)
        c->setAlpha (on ? 1.0f : 0.55f);
    repaint();
}

juce::Rectangle<float> ModuleTile::visualArea() const
{
    return { 14.0f, 40.0f, (float) getWidth() - 28.0f, 56.0f };
}

void ModuleTile::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = settings.isOn (moduleKind);
    const bool lit = on && ! rest;

    g.setColour (Colours::night.withAlpha (0.62f));
    g.fillRoundedRectangle (b, 14.0f);
    g.setColour (lit ? Colours::gold.withAlpha (0.42f) : Colours::muted.withAlpha (0.16f));
    g.drawRoundedRectangle (b, 14.0f, 1.0f);

    const auto nameFont = Fonts::sans (11.0f, true, 0.18f);
    drawTrackedText (g, name, { 14.0f, 6.0f, 160.0f, 30.0f }, nameFont, juce::Justification::centredLeft, Colours::gold);
    if (isNew)
    {
        const float x = 14.0f + trackedTextWidth (name, nameFont) + 8.0f;
        const auto tagFont = Fonts::sans (9.0f, true, 0.18f);
        const juce::Rectangle<float> tag (x, 14.0f, trackedTextWidth ("NEW", tagFont) + 12.0f, 14.0f);
        g.setColour (lavender);
        g.fillRoundedRectangle (tag, 6.0f);
        drawTrackedText (g, "NEW", tag, tagFont, juce::Justification::centred, Colours::ink);
    }
    if (powerButton == nullptr)
        drawTrackedText (g, "ALWAYS ON", { (float) getWidth() - 110.0f, 6.0f, 96.0f, 30.0f }, Fonts::sans (9.0f, false, 0.16f),
                         juce::Justification::centredRight, Colours::muted);

    const auto area = visualArea();
    g.setColour (well);
    g.fillRoundedRectangle (area, 8.0f);
    {
        juce::Graphics::ScopedSaveState save (g);
        if (! lit)
            g.setOpacity (0.35f);
        drawModuleVisual (g, moduleKind, area.reduced (8.0f, 3.0f), settings, lit ? t : 0.0f);
    }
}

void ModuleTile::resized()
{
    const float w = (float) getWidth();
    if (powerButton != nullptr)
        powerButton->setBounds (getWidth() - 44, 4, 32, 32);

    float y = 104.0f;
    for (auto& c : controls)
    {
        c->setBounds (juce::Rectangle<float> (14.0f, y, w - 28.0f, 24.0f).toNearestInt());
        y += 26.0f;
    }
    if (! faders.empty())
    {
        const float fw = (w - 28.0f) / (float) faders.size();
        for (size_t i = 0; i < faders.size(); ++i)
            faders[i]->setBounds (juce::Rectangle<float> (14.0f + fw * (float) i, 102.0f, fw, 78.0f).toNearestInt());
    }
    if (! choices.empty())
    {
        constexpr float gap = 4.0f;
        const float cw = (w - 28.0f - gap * (float) (choices.size() - 1)) / (float) choices.size();
        for (size_t i = 0; i < choices.size(); ++i)
            choices[i]->setBounds (juce::Rectangle<float> (14.0f + (cw + gap) * (float) i, y + 4.0f, cw, 22.0f).toNearestInt());
    }
}

//==============================================================================
ModulesPanel::ModulesPanel (juce::AudioProcessorValueTreeState& s) : state (s)
{
    setTitle ("Modules");
    auto add = [this] (ModuleKind k, const juce::String& n, bool fresh) -> ModuleTile&
    {
        tiles.push_back (std::make_unique<ModuleTile> (k, n, fresh));
        addAndMakeVisible (*tiles.back());
        return *tiles.back();
    };
    auto setUp = [this] (juce::Slider& d, const char* id, const juce::String& tip)
    {
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, id, d));
        auto* p = state.getParameter (id);
        d.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        d.setTooltip (tip);
    };
    auto attach = [setUp] (TextDragger& d, const char* id, std::function<juce::String (double)> format, const juce::String& tip)
    {
        d.formatValue = std::move (format);
        setUp (d, id, tip);
    };
    auto power = [this] (ModuleTile& t, const char* id)
    {
        buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, id, *t.power()));
    };
    auto percent = [] (double v) { return formatPercent ((float) v); };
    auto hz = [] (double v) { return formatHz ((float) v); };

    auto& window = add (ModuleKind::window, "WINDOW", false);
    attach (window.addControl ("Size"), ParamID::window, [] (double v) { return formatSeconds ((float) v); },
            "How long a slice of the voice each moment hears: longer is smoother and slower to change, up to 40 s");

    auto& onsets = add (ModuleKind::onsets, "ONSETS", false);
    power (onsets, ParamID::onsetsOn);
    attach (onsets.addControl ("Amount"), ParamID::onsets, percent,
            "How much consonants and attacks pass at the recorded speed instead of smearing");

    auto& shift = add (ModuleKind::shift, "SHIFT", false);
    power (shift, ParamID::shiftOn);
    attach (shift.addControl ("By"), ParamID::shift, [] (double v) { return formatShift ((float) v); },
            "Moves every partial by the same number of Hz: bell-like, inharmonic shimmer");

    auto& smear = add (ModuleKind::smear, "SMEAR", false);
    power (smear, ParamID::smearOn);
    attach (smear.addControl ("Amount"), ParamID::smear, percent,
            "Blurs each harmonic into its neighbours: wider, softer, a wash rather than a note");

    auto& tone = add (ModuleKind::tone, "TONE", false);
    power (tone, ParamID::toneOn);
    attach (tone.addControl ("Balance"), ParamID::tone, [] (double v) { return formatTone ((float) v); },
            "Down keeps only the breath and noise; up keeps only the sung harmonics");

    auto& harmonics = add (ModuleKind::harmonics, "HARMONICS", false);
    power (harmonics, ParamID::harmonicsOn);
    attach (harmonics.addControl ("Width"), ParamID::harmonicsWidth, [] (double v) { return formatCents ((float) v); },
            "How far either side of each harmonic still passes. Narrow is a pure, glassy chord");
    attach (harmonics.addControl ("Count"), ParamID::harmonicsCount, [] (double v) { return juce::String (juce::roundToInt (v)); },
            "How many of the key's harmonics are kept");

    auto& filter = add (ModuleKind::filter, "FILTER", false);
    power (filter, ParamID::filterOn);
    attach (filter.addControl ("Low"), ParamID::filterLow, hz,
            "The bottom of the band that is kept. Set it above High to cut the band out instead");
    attach (filter.addControl ("High"), ParamID::filterHigh, hz, "The top of the band that is kept");
    {
        const auto names = filterSlopeNames();
        for (int i = 0; i < 3; ++i)
        {
            auto& b = filter.addChoice (names[i].toUpperCase());
            b.setTooltip (i == 0 ? juce::String ("A hard edge at Low and High")
                                 : "A gentle slope of " + names[i] + " per octave past Low and High");
            b.onClick = [this, i] { slopeAttachment->setValueAsCompleteGesture ((float) i); };
            slopeButtons[(size_t) i] = &b;
        }
        slopeAttachment = std::make_unique<juce::ParameterAttachment> (
            *state.getParameter (ParamID::filterSlope),
            [this] (float value)
            {
                const int chosen = juce::roundToInt (value);
                for (int i = 0; i < 3; ++i)
                    slopeButtons[(size_t) i]->setToggleState (i == chosen, juce::dontSendNotification);
            });
        slopeAttachment->sendInitialUpdate();
    }

    auto& compressor = add (ModuleKind::compressor, "COMPRESSOR", true);
    power (compressor, ParamID::compressorOn);
    attach (compressor.addControl ("Amount"), ParamID::compressor, percent,
            "Evens out the loud and quiet moments of the voice, so the pad holds a steady level");

    auto& octaves = add (ModuleKind::octaves, "OCTAVES", true);
    power (octaves, ParamID::octavesOn);
    {
        const char* ids[ModuleSettings::numOctaves] = { ParamID::octaveDown2, ParamID::octaveDown1, ParamID::octaveRoot,
                                                        ParamID::octaveUp1, ParamID::octaveUp15, ParamID::octaveUp2 };
        const char* captions[ModuleSettings::numOctaves] = { "-2 oct", "-1 oct", "Root", "+1 oct", "+1 oct 5th", "+2 oct" };
        const char* tips[ModuleSettings::numOctaves] = { "Two octaves down", "One octave down", "The voice as it is",
                                                         "One octave up", "An octave and a fifth up", "Two octaves up" };
        for (int i = 0; i < ModuleSettings::numOctaves; ++i)
        {
            auto& f = octaves.addFader (juce::String (juce::CharPointer_UTF8 (captions[i])).replace ("-", juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))));
            f.formatValue = percent;
            setUp (f, ids[i], tips[i]);
        }
    }

    auto& binaural = add (ModuleKind::binaural, "BINAURAL", true);
    power (binaural, ParamID::binauralOn);
    attach (binaural.addControl ("Rate"), ParamID::binauralRate, [] (double v) { return formatBeat ((float) v); },
            "How fast the beat between your ears pulses. Best heard on headphones");
    attach (binaural.addControl ("Amount"), ParamID::binauralAmount, percent, "How much of the beat to mix in");

    auto& spaceFilter = add (ModuleKind::spaceFilter, "SPACE FILTER", true);
    power (spaceFilter, ParamID::spaceFilterOn);
    attach (spaceFilter.addControl ("Low"), ParamID::spaceLow, hz, "Cuts the reverb below this, so the low end stays clear");
    attach (spaceFilter.addControl ("High"), ParamID::spaceHigh, hz, "Cuts the reverb above this, for a darker space");

    addAndMakeVisible (resetButton);
    resetButton.setTooltip ("Every module back to how it starts");
    resetButton.onClick = [this] { resetAll(); };

    const auto now = readSettings();
    for (auto& t : tiles)
        t->update (now, 0.0f, false);
}

ModulesPanel::~ModulesPanel() = default;

ModuleSettings ModulesPanel::readSettings() const
{
    auto value = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
    auto on = [&value] (const char* id) { return value (id) > 0.5f; };
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
    s.filterSlope = juce::roundToInt (value (ParamID::filterSlope));
    s.compressor = value (ParamID::compressor);
    const char* octaveIds[ModuleSettings::numOctaves] = { ParamID::octaveDown2, ParamID::octaveDown1, ParamID::octaveRoot,
                                                          ParamID::octaveUp1, ParamID::octaveUp15, ParamID::octaveUp2 };
    for (int i = 0; i < ModuleSettings::numOctaves; ++i)
        s.octaveLevels[(size_t) i] = value (octaveIds[i]);
    s.beatHz = value (ParamID::binauralRate);
    s.binauralAmount = value (ParamID::binauralAmount);
    s.spaceLowHz = value (ParamID::spaceLow);
    s.spaceHighHz = value (ParamID::spaceHigh);
    s.onsetsOn = on (ParamID::onsetsOn);
    s.shiftOn = on (ParamID::shiftOn);
    s.smearOn = on (ParamID::smearOn);
    s.toneOn = on (ParamID::toneOn);
    s.harmonicsOn = on (ParamID::harmonicsOn);
    s.filterOn = on (ParamID::filterOn);
    s.compressorOn = on (ParamID::compressorOn);
    s.octavesOn = on (ParamID::octavesOn);
    s.binauralOn = on (ParamID::binauralOn);
    s.spaceFilterOn = on (ParamID::spaceFilterOn);
    return s;
}

void ModulesPanel::resetAll()
{
    for (auto* id : { ParamID::window, ParamID::onsetsOn, ParamID::onsets, ParamID::shiftOn, ParamID::shift, ParamID::smearOn, ParamID::smear,
                      ParamID::toneOn, ParamID::tone, ParamID::harmonicsOn, ParamID::harmonicsWidth, ParamID::harmonicsCount,
                      ParamID::filterOn, ParamID::filterLow, ParamID::filterHigh, ParamID::filterSlope,
                      ParamID::compressorOn, ParamID::compressor, ParamID::octavesOn, ParamID::octaveDown2, ParamID::octaveDown1,
                      ParamID::octaveRoot, ParamID::octaveUp1, ParamID::octaveUp15, ParamID::octaveUp2,
                      ParamID::binauralOn, ParamID::binauralRate, ParamID::binauralAmount,
                      ParamID::spaceFilterOn, ParamID::spaceLow, ParamID::spaceHigh })
    {
        auto* p = state.getParameter (id);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getDefaultValue());
        p->endChangeGesture();
    }
}

void ModulesPanel::setBloomEngine (bool isBloom)
{
    if (bloom == isBloom)
        return;
    bloom = isBloom;
    for (auto& t : tiles)
        t->setAlpha (bloom || worksInBothEngines (t->kind()) ? 1.0f : 0.45f);
    repaint();
}

void ModulesPanel::frame (double seconds, bool animate, float activity)
{
    const auto dt = (float) (lastSeconds < 0.0 ? 0.0 : juce::jlimit (0.0, 0.1, seconds - lastSeconds));
    lastSeconds = seconds;
    // The pictures drift slowly when all is quiet and move freely while notes sound.
    if (animate)
        visualTime += dt * (0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, activity * 4.0f));
    const auto now = readSettings();
    for (auto& t : tiles)
        t->update (now, visualTime, ! bloom && ! worksInBothEngines (t->kind()));
}

void ModulesPanel::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const auto capFont = Fonts::sans (12.0f, false, 0.26f);
    drawTrackedText (g, "MODULES", { 0.0f, 0.0f, 120.0f, headingHeight }, capFont, juce::Justification::centredLeft, Colours::gold);
    const float x = trackedTextWidth ("MODULES", capFont) + 18.0f;
    const juce::String note = bloom ? "How Bloom shapes the sound. Tap a light to switch a module on or off."
                                    : "Most modules shape Bloom and rest while Grain plays. Binaural and Space filter work in both.";
    drawTrackedText (g, note, { x, 0.0f, w - x - 140.0f, headingHeight }, Fonts::sans (12.0f), juce::Justification::centredLeft, Colours::muted);
}

void ModulesPanel::resized()
{
    const float w = (float) getWidth();
    const float resetW = 32.0f + trackedTextWidth ("RESET ALL", Fonts::sans (12.0f, false, 0.18f)) + 2.0f;
    resetButton.setBounds (juce::Rectangle<float> (w - resetW, 0.0f, resetW, headingHeight).toNearestInt());

    const float tileW = (w - (float) (columns - 1) * gap) / (float) columns;
    const float top = headingHeight + 8.0f;
    int cell = 0;
    for (auto& t : tiles)
    {
        const int span = t->kind() == ModuleKind::octaves ? 2 : 1;
        if (cell % columns + span > columns)
            cell += columns - cell % columns;
        const int col = cell % columns, row = cell / columns;
        const juce::Rectangle<float> r ((float) col * (tileW + gap), top + (float) row * (ModuleTile::height + gap),
                                        tileW * (float) span + gap * (float) (span - 1), ModuleTile::height);
        t->setBounds (r.toNearestInt());
        cell += span;
    }
}

} // namespace sing::ui
