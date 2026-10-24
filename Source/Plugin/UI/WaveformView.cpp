#include "WaveformView.h"
#include "Parameters.h"
#include "Theme.h"

#include "Stretch/GranularEngine.h"

namespace sing::ui
{

namespace
{
    constexpr int kBars = 120;
    constexpr float kGap = 2.0f;
    constexpr float kMinLengthPct = 1.0f;

    // Copy tints: warm white in the centre, cooler to the left, warmer to the right.
    const juce::Colour kCentre { 0xfffff3d6 }, kLeft { 0xffa9b8ff }, kRight { 0xffffbe96 };

    juce::Colour panColour (float pan)
    {
        const float a = juce::jlimit (0.0f, 1.0f, std::abs (pan));
        return kCentre.interpolatedWith (pan < 0.0f ? kLeft : kRight, a);
    }

    juce::String speedText (float speed)
    {
        if (speed <= 0.0f)
            return "HELD";
        if (speed > 0.5f)
            return juce::String (speed, 2) + "x";
        return "1/" + juce::String (juce::roundToInt (1.0f / speed)) + " SPEED";
    }
}

WaveformView::WaveformView (juce::AudioProcessorValueTreeState& s)
    : state (s),
      startAttachment (*s.getParameter (ParamID::loopStart), [this] (float v) { startPct = v; repaint(); }),
      lengthAttachment (*s.getParameter (ParamID::loopLength), [this] (float v) { lengthPct = v; repaint(); }),
      fadeAttachment (*s.getParameter (ParamID::loopFade), [this] (float v) { fadePct = v; repaint(); }),
      stretchParam (s.getRawParameterValue (ParamID::stretch)),
      holdParam (s.getRawParameterValue (ParamID::hold))
{
    setTitle ("Voice and loop");
    setDescription ("Drag the handles to set the loop, or drag inside it to move it");
    startAttachment.sendInitialUpdate();
    lengthAttachment.sendInitialUpdate();
    fadeAttachment.sendInitialUpdate();
}

void WaveformView::setDarkSky (bool dark)
{
    if (dark == darkSky)
        return;
    darkSky = dark;
    barsScale = 0.0f;   // redraw the bars
    repaint();
}

void WaveformView::setVoice (VoiceData::Ptr v)
{
    if (v != voice)
    {
        voice = std::move (v);
        repaint();
    }
}

void WaveformView::setLights (const std::array<Light, maxLights>& newLights, bool show, double clockSeconds)
{
    const bool any = std::any_of (newLights.begin(), newLights.end(), [] (const Light& l) { return l.note >= 0; });
    const bool had = std::any_of (lights.begin(), lights.end(), [] (const Light& l) { return l.note >= 0; });
    lights = newLights;
    clockMs = clockSeconds * 1000.0;
    rememberTails (clockMs);
    if (any || had || show != showLights)
    {
        showLights = show;
        repaint();
    }
}

void WaveformView::rememberTails (double nowMs)
{
    // Forget notes that have gone.
    for (auto& t : tails)
        if (t.used && std::none_of (lights.begin(), lights.end(), [&] (const Light& l) { return l.note >= 0 && l.noteId == t.noteId; }))
            t = {};

    for (const auto& l : lights)
    {
        if (l.note < 0 || l.copy != 0)
            continue;
        auto it = std::find_if (tails.begin(), tails.end(), [&] (const Tail& t) { return t.used && t.noteId == l.noteId; });
        if (it == tails.end())
            it = std::find_if (tails.begin(), tails.end(), [] (const Tail& t) { return ! t.used; });
        if (it == tails.end())
            continue;
        auto& t = *it;
        if (t.count > 0 && nowMs < t.times[(size_t) ((t.next + tailPoints - 1) % tailPoints)])
            t.count = t.next = 0;   // the clock went back: start the tail again
        t.used = true;
        t.noteId = l.noteId;
        t.positions[(size_t) t.next] = l.position;
        t.times[(size_t) t.next] = nowMs;
        t.next = (t.next + 1) % tailPoints;
        t.count = std::min (t.count + 1, tailPoints);
    }
}

float WaveformView::loopLeft() const noexcept
{
    return waveWidth * juce::jlimit (0.0f, 1.0f, startPct * 0.01f);
}

float WaveformView::loopRight() const noexcept
{
    return waveWidth * juce::jlimit (0.0f, 1.0f, (startPct + lengthPct) * 0.01f);
}

float WaveformView::fadeWidth() const noexcept
{
    return (loopRight() - loopLeft()) * fadePct * 0.01f * 0.5f;
}

void WaveformView::rebuildBars (float pixelScale)
{
    barsScale = pixelScale;
    barsStart = startPct;
    barsLength = lengthPct;
    barsVoice = voice != nullptr ? voice->getId() : 0;

    const int w = juce::roundToInt (width * pixelScale), h = juce::roundToInt (height * pixelScale);
    juce::Image lit (juce::Image::ARGB, w, h, true), unlit (juce::Image::ARGB, w, h, true);
    const float barW = (waveWidth - kGap * (kBars - 1)) / kBars;
    const float left = loopLeft(), right = loopRight();

    {
        juce::Graphics gl (lit), gu (unlit);
        gl.addTransform (juce::AffineTransform::scale (pixelScale));
        gu.addTransform (juce::AffineTransform::scale (pixelScale));

        const float midY = top + waveHeight * 0.5f;
        juce::ColourGradient litFill (Colours::cream, 0.0f, midY - 55.0f, Colours::cream, 0.0f, midY + 55.0f, false);
        litFill.addColour (0.5, Colours::gold);
        gl.setGradientFill (litFill);
        gu.setColour (darkSky ? juce::Colour (0xff8a78c8).withAlpha (0.42f) : Colours::shadow.withAlpha (0.72f));

        for (int i = 0; i < kBars; ++i)
        {
            float peak = 0.0f;
            if (voice != nullptr)
            {
                const auto& peaks = voice->getPeaks();
                const int per = VoiceData::numPeaks / kBars;
                for (int k = 0; k < per; ++k)
                    peak = std::max (peak, peaks[(size_t) (i * per + k)]);
            }
            const float barH = std::max (4.0f, std::round (110.0f * peak));
            const float x = margin + (float) i * (barW + kGap);
            const juce::Rectangle<float> bar (x, midY - barH * 0.5f, barW, barH);
            const float centre = (float) i * (barW + kGap) + barW * 0.5f;
            const bool inLoop = centre >= left && centre <= right;
            (inLoop ? gl : gu).fillRoundedRectangle (bar, 2.0f);
        }
    }

    // box-shadow: 0 0 8px rgba(255,214,150,0.45) on the lit bars: a blurred copy of their
    // shape, drawn in the glow colour underneath them.
    auto glow = lit.rescaled (std::max (1, w / 2), std::max (1, h / 2));
    gaussianBlur (glow, 2.0f * pixelScale);

    bars = juce::Image (juce::Image::ARGB, w, h, true);
    juce::Graphics g (bars);
    g.drawImageAt (unlit, 0, 0);
    g.setColour (juce::Colour (0xffffd696).withAlpha (0.45f));
    g.drawImage (glow, juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), juce::RectanglePlacement::stretchToFit, true);
    g.setColour (juce::Colours::black);
    g.drawImageAt (lit, 0, 0);
}

void WaveformView::paint (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - barsScale) > 0.01f || ! bars.isValid() || std::abs (barsStart - startPct) > 1.0e-4f || std::abs (barsLength - lengthPct) > 1.0e-4f
        || barsVoice != (voice != nullptr ? voice->getId() : 0))
        rebuildBars (pixelScale);

    // Loop region: a lit band with its fade ramps (linear-gradient with fade stops).
    const float left = margin + loopLeft(), right = margin + loopRight(), loopW = right - left;
    const auto region = juce::Rectangle<float> (left, top - 6.0f, std::max (1.0f, loopW), waveHeight + 12.0f);
    if (loopW > 1.0f)
    {
        const float fade = std::min (fadeWidth(), loopW * 0.5f);
        juce::ColourGradient band (Colours::gold.withAlpha (0.0f), left, 0.0f, Colours::gold.withAlpha (0.0f), right, 0.0f, false);
        const double f = juce::jlimit (0.001, 0.499, (double) (fade / loopW));
        band.addColour (f, Colours::gold.withAlpha (0.16f));
        band.addColour (1.0 - f, Colours::gold.withAlpha (0.16f));
        g.setGradientFill (band);
        g.fillRoundedRectangle (region, 10.0f);

        g.setColour (Colours::gold.withAlpha (0.5f));
        g.fillRect (region.getX() + 6.0f, region.getY(), std::max (0.0f, region.getWidth() - 12.0f), 1.0f);
        g.fillRect (region.getX() + 6.0f, region.getBottom() - 1.0f, std::max (0.0f, region.getWidth() - 12.0f), 1.0f);
    }

    g.drawImageTransformed (bars, juce::AffineTransform::scale (1.0f / barsScale));

    if (loopW > 1.0f)
    {
        // The fades, drawn as ramps.
        const float fade = std::min (fadeWidth(), loopW * 0.5f);
        juce::Path ramps;
        ramps.startNewSubPath (left, region.getBottom() - 6.0f);
        ramps.lineTo (left + fade, region.getY() + 6.0f);
        ramps.lineTo (right - fade, region.getY() + 6.0f);
        ramps.lineTo (right, region.getBottom() - 6.0f);
        g.setColour (Colours::gold.withAlpha (0.85f));
        g.strokePath (ramps, juce::PathStrokeType (1.4f));
    }

    // Handles at each end of the loop.
    for (const float x : { left, right })
    {
        g.setColour (Colours::gold.withAlpha (grab != Grab::none ? 1.0f : 0.9f));
        g.fillRect (x - 1.0f, top - 16.0f, 2.0f, waveHeight + 32.0f);
    }

    if (showLights)
        paintLights (g, region);
}

void WaveformView::paintLights (juce::Graphics& g, juce::Rectangle<float> region)
{
    int notes = 0;
    for (const auto& l : lights)
        if (l.note >= 0)
            notes = std::max (notes, l.note + 1);
    if (notes == 0)
        return;

    // The loop, as 0 to 1 across the waveform. Read zones wrap around inside it.
    const float loopStart = juce::jlimit (0.0f, 1.0f, startPct * 0.01f);
    const float loopEnd = juce::jlimit (loopStart, 1.0f, (startPct + lengthPct) * 0.01f);
    const float loopLen = loopEnd - loopStart;
    const auto toX = [] (float p) { return margin + waveWidth * juce::jlimit (0.0f, 1.0f, p); };
    const auto fillSpan = [&] (float from, float to, auto&& fill)
    {
        if (loopLen <= 1.0e-4f)
            return;
        const float len = std::min (loopLen, to - from);
        const float a = loopStart + std::fmod (std::fmod (from - loopStart, loopLen) + loopLen, loopLen);
        if (a + len <= loopEnd)
            fill (toX (a), toX (a + len));
        else
        {
            fill (toX (a), toX (loopEnd));
            fill (toX (loopStart), toX (loopStart + a + len - loopEnd));
        }
    };

    // The granular read zone, from the same Stretch knob mapping the engine uses: grains start
    // within the jitter either side of the head and each reads a grain's length at its pitch.
    const float knob = stretchParam != nullptr ? stretchParam->load() * 0.01f : 0.6f;
    const bool held = (holdParam != nullptr && holdParam->load() > 0.5f) || knob >= StretchKnob::freezeAt;
    const float speed = held ? 0.0f : StretchKnob::toSpeed (knob);
    const auto granular = StretchKnob::toGranularSettings (knob);
    const float voiceSeconds = voice != nullptr ? (float) std::max (0.01, voice->trimSeconds()) : 4.0f;

    const auto noteAlpha = [] (const Light& l) { return juce::jlimit (0.0f, 1.0f, std::sqrt (l.level)) * (l.note == 0 ? 1.0f : 0.55f); };
    const float share = 1.0f / std::sqrt ((float) notes);

    // Halos over each copy's read zone, ghosts behind.
    for (int i = maxLights - 1; i >= 0; --i)
    {
        const auto& l = lights[(size_t) i];
        if (l.note < 0 || l.position < 0.0f || l.weight <= 0.0f)
            continue;

        float before, after;
        if (l.zone > 0.0f)
            before = after = 0.5f * l.zone;
        else
        {
            const float grain = granular.grainSeconds * l.pitchRatio;
            const float slow = 1.0f - std::min (speed * 4.0f, 1.0f);
            const float jitter = std::max (granular.jitterSeconds, slow * 0.5f * grain);
            before = jitter / voiceSeconds;
            after = (jitter + grain) / voiceSeconds;
        }

        const float alpha = l.weight * noteAlpha (l) * share * (l.copy == 0 ? 0.16f : 0.07f);
        const auto colour = panColour (l.pan);
        juce::ColourGradient halo (colour.withAlpha (0.0f), 0.0f, region.getY(), colour.withAlpha (0.0f), 0.0f, region.getBottom(), false);
        halo.addColour (0.5, colour.withAlpha (alpha));
        g.setGradientFill (halo);
        fillSpan (l.position - before, l.position + after, [&] (float x0, float x1)
        {
            g.fillRect (juce::Rectangle<float> (x0, region.getY() + 2.0f, std::max (1.0f, x1 - x0), region.getHeight() - 4.0f));
        });
    }

    // Tails: where each note's centre copy has been over the last moment.
    const double now = clockMs;
    const float midY = top + waveHeight * 0.5f;
    for (const auto& t : tails)
    {
        if (! t.used || t.count < 2)
            continue;
        const auto it = std::find_if (lights.begin(), lights.end(), [&] (const Light& l) { return l.note >= 0 && l.copy == 0 && l.noteId == t.noteId; });
        if (it == lights.end())
            continue;
        const float base = noteAlpha (*it) * 0.35f;
        for (int k = 1; k < t.count; ++k)
        {
            const int a = (t.next - t.count + k - 1 + tailPoints) % tailPoints;
            const int b = (a + 1) % tailPoints;
            const double age = now - t.times[(size_t) a];
            if (age > tailMs)
                continue;
            const float p0 = t.positions[(size_t) a], p1 = t.positions[(size_t) b];
            if (std::abs (p1 - p0) > 0.5f * loopLen)   // wrapped round the loop
                continue;
            const float fade = (float) (1.0 - age / tailMs);
            g.setColour (juce::Colour (0xfffff0d7).withAlpha (base * fade));
            const float x0 = toX (std::min (p0, p1)), x1 = toX (std::max (p0, p1));
            g.fillRect (juce::Rectangle<float> (x0, midY - 2.0f, std::max (0.0f, x1 - x0), 4.0f));
        }
    }

    // The lights: a bright line for each note's centre copy, a fine tinted ghost for the others.
    for (int i = maxLights - 1; i >= 0; --i)
    {
        const auto& l = lights[(size_t) i];
        if (l.note < 0 || l.position < 0.0f || l.weight <= 0.0f)
            continue;
        const float x = toX (l.position);
        const float alpha = noteAlpha (l);
        if (l.copy == 0)
        {
            const juce::Rectangle<float> line (x - 1.0f, region.getY(), 2.0f, region.getHeight());
            drawGlow (g, line, 1.0f, 10.0f, juce::Colour (0xffffecc8).withAlpha (0.9f * alpha));
            g.setColour (juce::Colour (0xfffff6e6).withAlpha (alpha));
            g.fillRect (line);
        }
        else
        {
            g.setColour (panColour (l.pan).withAlpha (0.85f * alpha * l.weight));
            g.fillRect (juce::Rectangle<float> (x - 0.6f, region.getY(), 1.2f, region.getHeight()));
        }
    }

    // How fast the newest note is moving, under its light.
    const auto newest = std::find_if (lights.begin(), lights.end(), [] (const Light& l) { return l.note == 0 && l.copy == 0; });
    if (newest != lights.end() && newest->position >= 0.0f)
    {
        const auto font = Fonts::sans (10.0f, true, 0.14f);
        const auto text = newest->zone > 0.0f && ! held ? juce::String() : speedText (speed);
        if (text.isNotEmpty())
        {
            const float w = trackedTextWidth (text, font) + 4.0f;
            const float x = juce::jlimit (0.0f, width - w, toX (newest->position) - w * 0.5f);
            const juce::Rectangle<float> box (x, region.getBottom() + 1.0f, w, height - region.getBottom() - 1.0f);
            g.setColour (Colours::night.withAlpha (0.75f));   // keeps it clear of the loop handles
            g.fillRoundedRectangle (box.expanded (4.0f, 0.0f), box.getHeight() * 0.5f);
            drawTrackedText (g, text, box, font, juce::Justification::centred, Colours::gold.withAlpha (0.9f));
        }
    }
}

WaveformView::Grab WaveformView::grabAt (juce::Point<float> p) const noexcept
{
    const float left = margin + loopLeft(), right = margin + loopRight();
    const float dl = std::abs (p.x - left), dr = std::abs (p.x - right);
    if (std::min (dl, dr) <= 8.0f)
        return dl <= dr ? Grab::start : Grab::end;
    if (p.x > left && p.x < right)
        return Grab::body;
    return Grab::none;
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const auto g = grabAt (e.position);
    setMouseCursor (g == Grab::start || g == Grab::end ? juce::MouseCursor::LeftRightResizeCursor
                    : g == Grab::body                  ? juce::MouseCursor::DraggingHandCursor
                                                       : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    grab = grabAt (e.position);
    grabStartPct = startPct;
    grabLengthPct = lengthPct;
    if (grab == Grab::start || grab == Grab::body)
        startAttachment.beginGesture();
    if (grab == Grab::start || grab == Grab::end)
        lengthAttachment.beginGesture();
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (grab == Grab::none)
        return;

    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    const float deltaPct = (float) e.getDistanceFromDragStartX() / waveWidth * 100.0f * fine;
    const float end = grabStartPct + grabLengthPct;

    if (grab == Grab::start)
    {
        const float s = juce::jlimit (0.0f, end - kMinLengthPct, grabStartPct + deltaPct);
        startAttachment.setValueAsPartOfGesture (s);
        lengthAttachment.setValueAsPartOfGesture (end - s);
    }
    else if (grab == Grab::end)
    {
        const float newEnd = juce::jlimit (grabStartPct + kMinLengthPct, 100.0f, end + deltaPct);
        lengthAttachment.setValueAsPartOfGesture (newEnd - grabStartPct);
    }
    else
    {
        const float s = juce::jlimit (0.0f, 100.0f - grabLengthPct, grabStartPct + deltaPct);
        startAttachment.setValueAsPartOfGesture (s);
    }
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (grab == Grab::start || grab == Grab::body)
        startAttachment.endGesture();
    if (grab == Grab::start || grab == Grab::end)
        lengthAttachment.endGesture();
    grab = Grab::none;
    repaint();
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click outside the loop: move the loop there, centred, keeping its length.
    if (grabAt (e.position) != Grab::none)
        return;
    const float centrePct = (e.position.x - margin) / waveWidth * 100.0f;
    startAttachment.setValueAsCompleteGesture (juce::jlimit (0.0f, 100.0f - lengthPct, centrePct - lengthPct * 0.5f));
}

} // namespace sing::ui
