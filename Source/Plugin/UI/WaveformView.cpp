#include "WaveformView.h"
#include "Parameters.h"
#include "Theme.h"

namespace sing::ui
{

namespace
{
    constexpr int kBars = 120;
    constexpr float kGap = 2.0f;
    constexpr float kMinLengthPct = 1.0f;
}

WaveformView::WaveformView (juce::AudioProcessorValueTreeState& s)
    : state (s),
      startAttachment (*s.getParameter (ParamID::loopStart), [this] (float v) { startPct = v; repaint(); }),
      lengthAttachment (*s.getParameter (ParamID::loopLength), [this] (float v) { lengthPct = v; repaint(); }),
      fadeAttachment (*s.getParameter (ParamID::loopFade), [this] (float v) { fadePct = v; repaint(); })
{
    setTitle ("Voice and loop");
    setDescription ("Drag the handles to set the loop, or drag inside it to move it");
    startAttachment.sendInitialUpdate();
    lengthAttachment.sendInitialUpdate();
    fadeAttachment.sendInitialUpdate();
}

void WaveformView::setVoice (VoiceData::Ptr v)
{
    if (v != voice)
    {
        voice = std::move (v);
        repaint();
    }
}

void WaveformView::setLights (const std::array<float, 4>& positions, const std::array<float, 4>& levels, bool show)
{
    if (positions == lightPositions && levels == lightLevels && show == showLights)
        return;
    lightPositions = positions;
    lightLevels = levels;
    showLights = show;
    repaint();
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
        gu.setColour (Colours::shadow.withAlpha (0.72f));

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

    // A light for each sounding note, brightest for the newest.
    if (showLights)
        for (int i = 3; i >= 0; --i)
        {
            const float p = lightPositions[(size_t) i];
            const float level = lightLevels[(size_t) i];
            if (p < 0.0f || level <= 0.001f)
                continue;
            const float x = margin + waveWidth * juce::jlimit (0.0f, 1.0f, p);
            const float alpha = juce::jlimit (0.0f, 1.0f, std::sqrt (level)) * (i == 0 ? 1.0f : 0.55f);
            const juce::Rectangle<float> line (x - 1.0f, region.getY(), 2.0f, region.getHeight());
            drawGlow (g, line, 1.0f, 10.0f, juce::Colour (0xffffecc8).withAlpha (0.9f * alpha));
            g.setColour (juce::Colour (0xfffff6e6).withAlpha (alpha));
            g.fillRect (line);
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
