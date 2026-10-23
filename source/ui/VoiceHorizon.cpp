#include "VoiceHorizon.h"
#include "Parameters.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

namespace
{
    constexpr float kGap = 2.0f;                                            // between bars
    constexpr float kBarWidth = (960.0f - kGap * (VoiceHorizon::numBars - 1)) / VoiceHorizon::numBars;
    constexpr float kMaxBar = 110.0f, kMinBar = 4.0f;                      // bar heights
    constexpr float kMinLength = 0.005f;                                    // the loop length parameter's floor
    constexpr float kGrabWidth = 6.0f;                                      // either side of a handle

    juce::RangedAudioParameter& param (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = state.getParameter (id);
        jassert (p != nullptr);
        return *p;
    }

    float valueOf (const juce::RangedAudioParameter& p) { return p.convertFrom0to1 (p.getValue()); }
}

//==============================================================================
VoiceHorizon::VoiceHorizon (juce::AudioProcessorValueTreeState& s)
    : startParam (param (s, ParamID::loopStart)),
      lengthParam (param (s, ParamID::loopLength)),
      fadeParam (param (s, ParamID::loopFade))
{
    setTitle ("Voice and loop");
    setDescription ("Drag the gold handles to set the loop, or drag the lit stretch to move it");
    bars.fill (kMinBar);
    heads.fill (-1.0f);
    headAlpha.fill (0.0f);
    loop = readLoop();
}

VoiceHorizon::~VoiceHorizon() = default;

juce::Rectangle<float> VoiceHorizon::wave() const
{
    return waveArea().translated (-static_cast<float> (getX()), -static_cast<float> (getY()));
}

float VoiceHorizon::xFor (float fraction) const
{
    const auto w = wave();
    return w.getX() + w.getWidth() * fraction;
}

float VoiceHorizon::fractionAt (float x) const
{
    const auto w = wave();
    return (x - w.getX()) / w.getWidth();
}

VoiceHorizon::Loop VoiceHorizon::readLoop() const
{
    Loop l;
    l.start = juce::jlimit (0.0f, 1.0f, valueOf (startParam));
    l.end = juce::jlimit (l.start, 1.0f, l.start + valueOf (lengthParam));
    l.fade = juce::jlimit (0.0f, 1.0f, valueOf (fadeParam) / 100.0f);
    return l;
}

//==============================================================================
void VoiceHorizon::setWaveform (const std::vector<float>& peaks)
{
    // Each bar is the loudest point of its stretch, scaled so the loudest bar is full height.
    std::array<float, numBars> peak {};
    if (! peaks.empty())
    {
        const auto n = peaks.size();
        for (size_t i = 0; i < static_cast<size_t> (numBars); ++i)
        {
            const size_t from = i * n / numBars;
            const size_t to = std::max (from + 1, (i + 1) * n / numBars);
            for (size_t k = from; k < to && k < n; ++k)
                peak[i] = std::max (peak[i], std::abs (peaks[k]));
        }
    }
    const float loudest = *std::max_element (peak.begin(), peak.end());
    for (size_t i = 0; i < bars.size(); ++i)
        bars[i] = std::max (kMinBar, std::round (kMaxBar * (loudest > 1.0e-6f ? peak[i] / loudest : 0.0f)));

    ++waveVersion;
    repaint();
}

void VoiceHorizon::setLoading (bool isLoading)
{
    if (loading != isLoading)
    {
        loading = isLoading;
        repaint();
    }
}

void VoiceHorizon::update (const std::array<float, maxPlayheads>& playheads, bool motion)
{
    bool changed = false;

    const auto now = readLoop();
    if (now != loop)
    {
        loop = now;
        changed = true;
    }

    // Each playhead fades in and out over a few frames, keeping its last place as it goes.
    for (size_t i = 0; i < heads.size(); ++i)
    {
        const bool on = playheads[i] >= 0.0f;
        if (on && ! juce::approximatelyEqual (heads[i], playheads[i]))
        {
            heads[i] = playheads[i];
            changed = true;
        }
        const float target = on ? 1.0f : 0.0f;
        if (! juce::approximatelyEqual (headAlpha[i], target))
        {
            headAlpha[i] = motion ? (on ? std::min (1.0f, headAlpha[i] + 0.34f) : std::max (0.0f, headAlpha[i] - 0.2f)) : target;
            changed = true;
        }
    }

    if (changed)
        repaint();
}

//==============================================================================
void VoiceHorizon::rebuildImage (float pixelScale)
{
    imageScale = pixelScale;
    imageLoop = loop;
    imageVersion = waveVersion;
    imageLoading = loading;

    const auto w = wave();
    image = juce::Image (juce::Image::ARGB, std::max (1, juce::roundToInt (static_cast<float> (getWidth()) * pixelScale)),
                         std::max (1, juce::roundToInt (static_cast<float> (getHeight()) * pixelScale)), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (pixelScale));

    const float x0 = w.getX() + std::round (w.getWidth() * loop.start);
    const float x1 = w.getX() + std::round (w.getWidth() * loop.end);
    auto barRect = [&w] (size_t i, float h)
    {
        return juce::Rectangle<float> (w.getX() + static_cast<float> (i) * (kBarWidth + kGap), w.getCentreY() - h * 0.5f, kBarWidth, h);
    };
    auto inLoop = [this] (size_t i)
    {
        const float t = static_cast<float> (i) / numBars;
        return t >= loop.start && t <= loop.end;
    };

    // While a new voice is on its way the old one fades back.
    const float alpha = loading ? 0.35f : 1.0f;

    // The lit bars glow: box-shadow 0 0 8px rgba(255,214,150,0.45), blurred at half resolution.
    {
        const float res = pixelScale * 0.5f;
        const auto area = w.expanded (16.0f);
        juce::Image glow (juce::Image::ARGB, juce::roundToInt (area.getWidth() * res), juce::roundToInt (area.getHeight() * res), true);
        {
            juce::Graphics gg (glow);
            gg.addTransform (juce::AffineTransform::translation (-area.getX(), -area.getY()).scaled (res));
            gg.setColour (juce::Colour (255, 214, 150).withAlpha (0.45f));
            for (size_t i = 0; i < bars.size(); ++i)
                if (inLoop (i))
                    gg.fillRoundedRectangle (barRect (i, bars[i]), 2.0f);
        }
        gaussianBlur (glow, 4.0f * res);
        g.setOpacity (alpha);
        g.drawImageTransformed (glow, juce::AffineTransform::scale (1.0f / res).translated (area.getX(), area.getY()));
        g.setOpacity (1.0f);
    }

    // The bars: dark outside the loop, gold inside, mirrored about the middle.
    juce::ColourGradient lit (Colours::goldPale.withAlpha (alpha), 0.0f, w.getCentreY() - kMaxBar * 0.5f,
                              Colours::goldPale.withAlpha (alpha), 0.0f, w.getCentreY() + kMaxBar * 0.5f, false);
    lit.addColour (0.5, Colours::gold.withAlpha (alpha));
    const auto dark = juce::Colour (26, 22, 64).withAlpha (0.72f * alpha);
    for (size_t i = 0; i < bars.size(); ++i)
    {
        const auto r = barRect (i, bars[i]);
        if (inLoop (i))
        {
            // Each lit bar runs pale, gold, pale over its own height, like the mockup's.
            juce::ColourGradient own (lit);
            own.point1 = { 0.0f, r.getY() };
            own.point2 = { 0.0f, r.getBottom() };
            g.setGradientFill (own);
        }
        else
        {
            g.setColour (dark);
        }
        g.fillRoundedRectangle (r, 2.0f);
    }

    // The loop: a soft gold wash that fades in and out across its fades, framed top and bottom.
    const float width = x1 - x0;
    if (width > 0.5f)
    {
        const auto region = juce::Rectangle<float> (x0, w.getY() - 6.0f, width, 132.0f);
        const auto wash = Colours::gold.withAlpha (0.16f);
        const double fadePos = std::round (loop.fade * 50.0f) / 100.0;
        juce::ColourGradient grad (wash.withAlpha (0.0f), region.getX(), 0.0f, wash.withAlpha (0.0f), region.getRight(), 0.0f, false);
        grad.addColour (juce::jlimit (0.001, 0.499, fadePos), wash);
        grad.addColour (juce::jlimit (0.501, 0.999, 1.0 - fadePos), wash);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (region, std::min (10.0f, width * 0.5f));

        // border-top and border-bottom, curving away into the rounded corners.
        g.setColour (Colours::gold.withAlpha (0.5f));
        const float inset = std::min (6.0f, width * 0.5f);
        g.fillRect (region.getX() + inset, region.getY(), width - inset * 2.0f, 1.0f);
        g.fillRect (region.getX() + inset, region.getBottom() - 1.0f, width - inset * 2.0f, 1.0f);

        // The fades as a ramp: up from the bottom corner, across the top, back down.
        const float fadePx = width * loop.fade * 0.5f;
        juce::Path ramp;
        ramp.startNewSubPath (x0, region.getY() + 126.0f);
        ramp.lineTo (x0 + fadePx, region.getY() + 6.0f);
        ramp.lineTo (x1 - fadePx, region.getY() + 6.0f);
        ramp.lineTo (x1, region.getY() + 126.0f);
        g.setColour (Colours::gold.withAlpha (0.85f));
        g.strokePath (ramp, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered));
    }
}

const juce::Image& VoiceHorizon::playheadSprite()
{
    // A 2 x 132 line with box-shadow 0 0 10px, as an alpha mask tinted when drawn.
    if (! sprite.isValid() || ! juce::approximatelyEqual (spriteScale, imageScale))
    {
        spriteScale = imageScale;
        const float res = imageScale;
        sprite = juce::Image (juce::Image::ARGB, juce::roundToInt (32.0f * res), juce::roundToInt (162.0f * res), true);
        {
            juce::Graphics g (sprite);
            g.addTransform (juce::AffineTransform::scale (res));
            g.setColour (juce::Colours::white);
            g.fillRect (15.0f, 15.0f, 2.0f, 132.0f);
        }
        gaussianBlur (sprite, 5.0f * res);
    }
    return sprite;
}

void VoiceHorizon::paint (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (! image.isValid() || ! juce::approximatelyEqual (pixelScale, imageScale) || imageVersion != waveVersion
        || imageLoop != loop || imageLoading != loading)
        rebuildImage (pixelScale);
    g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / imageScale));

    const auto w = wave();

    // A glowing line where each note is playing.
    const auto& glow = playheadSprite();
    for (size_t i = 0; i < heads.size(); ++i)
    {
        if (headAlpha[i] <= 0.01f || heads[i] < 0.0f)
            continue;
        const float x = xFor (juce::jlimit (0.0f, 1.0f, heads[i]));
        // Narrow the clip to the sprite first: JUCE's software renderer otherwise builds
        // a mask over the whole clip region for a tinted image.
        const auto place = juce::AffineTransform::scale (1.0f / spriteScale).translated (x - 16.0f, w.getY() - 21.0f);
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (glow.getBounds().toFloat().transformedBy (place).getSmallestIntegerContainer().expanded (1));
            g.setColour (juce::Colour (255, 236, 200).withAlpha (0.9f * headAlpha[i]));
            g.drawImageTransformed (glow, place, true);
        }
        g.setColour (juce::Colour (0xfffff6e6).withAlpha (headAlpha[i]));
        g.fillRect (x - 1.0f, w.getY() - 6.0f, 2.0f, 132.0f);
    }

    // The handles at each end of the loop, standing 10 proud of the lit stretch.
    const bool hot = grab != Grab::none || isMouseOver();
    g.setColour (Colours::gold.withAlpha (loading ? 0.5f : 1.0f));
    for (float f : { loop.start, loop.end })
    {
        const float x = w.getX() + std::round (w.getWidth() * f);
        g.fillRect (x - 1.0f, w.getY() - 16.0f, 2.0f, 152.0f);
        if (hot)
            g.fillRoundedRectangle (x - 3.0f, w.getY() - 18.0f, 6.0f, 6.0f, 1.5f);
    }
}

//==============================================================================
VoiceHorizon::Grab VoiceHorizon::grabAt (juce::Point<float> p) const
{
    const float xs = xFor (loop.start), xe = xFor (loop.end);
    const float ds = std::abs (p.x - xs), de = std::abs (p.x - xe);
    // When the handles nearly touch, pick the nearer one (the end on a tie, so a
    // zero-length loop can still be opened out).
    if (ds <= kGrabWidth || de <= kGrabWidth)
        return de <= ds ? Grab::end : Grab::start;
    if (p.x > xs && p.x < xe)
        return Grab::body;
    return Grab::none;
}

void VoiceHorizon::mouseMove (const juce::MouseEvent& e)
{
    const auto g = grabAt (e.position);
    setMouseCursor (g == Grab::body ? juce::MouseCursor::DraggingHandCursor
                  : g == Grab::none ? juce::MouseCursor::NormalCursor
                                    : juce::MouseCursor::LeftRightResizeCursor);
    repaint();
}

void VoiceHorizon::mouseDown (const juce::MouseEvent& e)
{
    grab = grabAt (e.position);
    if (grab == Grab::none)
        return;
    dragStart = loop;
    dragFrom = fractionAt (e.position.x);
    startParam.beginChangeGesture();
    lengthParam.beginChangeGesture();
}

void VoiceHorizon::mouseDrag (const juce::MouseEvent& e)
{
    if (grab == Grab::none)
        return;

    // Shift for fine moves.
    const float delta = (fractionAt (e.position.x) - dragFrom) * (e.mods.isShiftDown() ? 0.2f : 1.0f);
    const float length = dragStart.end - dragStart.start;
    switch (grab)
    {
        case Grab::start:
        {
            const float start = juce::jlimit (0.0f, dragStart.end - kMinLength, dragStart.start + delta);
            setLoop (start, dragStart.end - start);
            break;
        }
        case Grab::end:
        {
            const float end = juce::jlimit (dragStart.start + kMinLength, 1.0f, dragStart.end + delta);
            setLoop (dragStart.start, end - dragStart.start);
            break;
        }
        case Grab::body:
            setLoop (juce::jlimit (0.0f, std::max (0.0f, 1.0f - length), dragStart.start + delta), length);
            break;
        case Grab::none:
            break;
    }
}

void VoiceHorizon::mouseUp (const juce::MouseEvent&)
{
    if (grab == Grab::none)
        return;
    startParam.endChangeGesture();
    lengthParam.endChangeGesture();
    grab = Grab::none;
    repaint();
}

void VoiceHorizon::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-clicking a handle or the loop puts the loop back where the plugin starts it.
    if (grabAt (e.position) == Grab::none)
        return;
    for (auto* p : { &startParam, &lengthParam })
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getDefaultValue());
        p->endChangeGesture();
    }
}

void VoiceHorizon::setLoop (float start, float length)
{
    startParam.setValueNotifyingHost (startParam.convertTo0to1 (start));
    lengthParam.setValueNotifyingHost (lengthParam.convertTo0to1 (length));
    loop = readLoop();
    repaint();
}

} // namespace sing::ui
