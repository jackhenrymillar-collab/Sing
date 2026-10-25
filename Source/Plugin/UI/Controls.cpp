#include "Controls.h"
#include "Theme.h"

#include <cmath>

namespace sing::ui
{

//==============================================================================
Knob::Knob()
{
    setSliderStyle (juce::Slider::RotaryVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setMouseDragSensitivity (220);
    // Holding Shift switches to velocity mode with a low sensitivity, for fine moves.
    setVelocityModeParameters (0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setScrollWheelEnabled (true);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setRepaintsOnMouseActivity (true);
    onValueChange = [this] { repaint(); };
}

void Knob::setLabel (const juce::String& newLabel)
{
    if (label != newLabel)
    {
        label = newLabel;
        setTitle (newLabel);
        repaint();
    }
}

juce::Rectangle<float> Knob::dialBounds() const
{
    return { (static_cast<float> (getWidth()) - dialSize) * 0.5f, topMargin, dialSize, dialSize };
}

Knob::~Knob() = default;

bool Knob::hitTest (int x, int y)
{
    const auto d = dialBounds();
    const auto p = juce::Point<float> (static_cast<float> (x), static_cast<float> (y));
    return p.getDistanceFrom (d.getCentre()) <= dialSize * 0.55f
        || onRing (p)
        || (p.y > d.getBottom() && std::abs (p.x - d.getCentreX()) < dialSize * 0.7f);
}

void Knob::setOrbit (juce::RangedAudioParameter& parameter)
{
    orbit = std::make_unique<juce::ParameterAttachment> (parameter, [this] (float value)
    {
        orbitAmount = juce::jlimit (0.0f, 1.0f, value * 0.01f);
        repaint();
    });
    orbit->sendInitialUpdate();
}

void Knob::setOrbitLive (float position)
{
    if (std::abs (position - orbitLive) > 1.0e-4f)
    {
        orbitLive = position;
        repaint();
    }
}

bool Knob::onRing (juce::Point<float> p) const
{
    if (orbit == nullptr)
        return false;
    // The ring and a little either side of it, so it is easy to catch. The gap at the bottom
    // belongs to the label.
    const auto d = dialBounds();
    const float r = p.getDistanceFrom (d.getCentre());
    const float ring = dialSize * 0.5f + ringGap;
    return r > dialSize * 0.5f + 1.0f && r < ring + 8.0f && p.y < d.getBottom() + 4.0f;
}

void Knob::setRingHover (bool h)
{
    if (h != ringHover)
    {
        ringHover = h;
        repaint();
    }
}

void Knob::mouseMove (const juce::MouseEvent& e)
{
    setRingHover (onRing (e.position));
    Slider::mouseMove (e);
}

void Knob::mouseExit (const juce::MouseEvent& e)
{
    setRingHover (false);
    Slider::mouseExit (e);
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (orbit != nullptr && (onRing (e.position) || e.mods.isAltDown()) && ! e.mods.isPopupMenu())
    {
        orbitDragging = true;
        orbitDragStart = orbitAmount;
        orbit->beginGesture();
        repaint();
        return;
    }
    Slider::mouseDown (e);
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (! orbitDragging)
    {
        Slider::mouseDrag (e);
        return;
    }
    // Up widens the orbit. 200 units of travel cover it all; Shift for fine moves.
    const float travel = e.mods.isShiftDown() ? 800.0f : 200.0f;
    const float amount = juce::jlimit (0.0f, 1.0f, orbitDragStart - (float) e.getDistanceFromDragStartY() / travel);
    orbit->setValueAsPartOfGesture (amount * 100.0f);
}

void Knob::mouseUp (const juce::MouseEvent& e)
{
    if (! orbitDragging)
    {
        Slider::mouseUp (e);
        return;
    }
    orbitDragging = false;
    orbit->endGesture();
    setRingHover (onRing (e.position));
    repaint();
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (orbit != nullptr && onRing (e.position))
    {
        orbit->setValueAsCompleteGesture (0.0f);
        return;
    }
    Slider::mouseDoubleClick (e);
}

void Knob::rebuildDial (float pixelScale)
{
    dialScale = pixelScale;
    dial = juce::Image (juce::Image::ARGB,
                        std::max (1, juce::roundToInt (static_cast<float> (getWidth()) * pixelScale)),
                        std::max (1, juce::roundToInt ((topMargin + dialSize + 40.0f) * pixelScale)), true);
    juce::Graphics g (dial);
    g.addTransform (juce::AffineTransform::scale (pixelScale));

    const auto d = dialBounds();
    const auto c = d.getCentre();
    const float r = dialSize * 0.5f;

    // box-shadow: 0 10px 24px rgba(10,8,30,0.5)
    fillBlurredDisc (g, c.translated (0.0f, 10.0f), r, 24.0f, juce::Colour (0xff0a081e).withAlpha (0.5f));

    // radial-gradient(circle at 38% 30%, #6a5aa0 0%, #3a2f70 55%, #201a46 100%)
    const juce::Point<float> light (d.getX() + 0.38f * dialSize, d.getY() + 0.30f * dialSize);
    juce::ColourGradient body (juce::Colour (0xff6a5aa0), light, juce::Colour (0xff201a46), light.translated (70.0f, 0.0f), true);
    body.addColour (0.55, juce::Colour (0xff3a2f70));
    g.setGradientFill (body);
    g.fillEllipse (d);

    // inset 0 1px 0 rgba(255,230,240,0.2): a thin highlight along the top edge.
    {
        juce::Path crescent;
        crescent.addEllipse (d);
        crescent.addEllipse (d.translated (0.0f, 1.0f));
        crescent.setUsingNonZeroWinding (false);
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addEllipse (d);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colour (0xffffe6f0).withAlpha (0.2f));
        g.fillPath (crescent);
    }
}

void Knob::paint (juce::Graphics& g)
{
    const float pixelScale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (std::abs (pixelScale - dialScale) > 0.01f || ! dial.isValid())
        rebuildDial (pixelScale);
    g.drawImageTransformed (dial, juce::AffineTransform::scale (1.0f / dialScale));

    const auto d = dialBounds();
    const auto c = d.getCentre();

    // The 1px gold ring, brighter while you're touching the knob.
    g.setColour (Colours::gold.withAlpha (isMouseOverOrDragging() ? 0.6f : 0.25f));
    g.drawEllipse (d.expanded (0.5f), 1.0f);

    // The orbit: a faint track while you can set it, the gold arc where the knob wanders, and
    // a light where it is now.
    if (orbit != nullptr && (orbitAmount > 0.0f || ringHover || orbitDragging))
    {
        const float ring = dialSize * 0.5f + ringGap;
        const auto arc = [&] (float from, float to)
        {
            juce::Path p;
            p.addCentredArc (c.x, c.y, ring, ring, 0.0f,
                             juce::degreesToRadians (-135.0f + 270.0f * from), juce::degreesToRadians (-135.0f + 270.0f * to), true);
            return p;
        };
        const juce::PathStrokeType stroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour (Colours::gold.withAlpha (ringHover || orbitDragging ? 0.22f : 0.12f));
        g.strokePath (arc (0.0f, 1.0f), stroke);
        if (orbitAmount > 0.0f)
        {
            const float base = static_cast<float> (valueToProportionOfLength (getValue()));
            const float half = 0.5f * orbitAmount;
            const float centre = juce::jlimit (half, 1.0f - half, base);
            g.setColour (Colours::gold.withAlpha (ringHover || orbitDragging ? 0.9f : 0.65f));
            g.strokePath (arc (centre - half, std::max (centre + half, centre - half + 0.004f)), stroke);
        }
        if (orbitAmount > 0.0f && orbitLive >= 0.0f)
        {
            const float a = juce::degreesToRadians (-135.0f + 270.0f * juce::jlimit (0.0f, 1.0f, orbitLive));
            const juce::Point<float> light (c.x + ring * std::sin (a), c.y - ring * std::cos (a));
            fillBlurredDisc (g, light, 4.0f, 10.0f, juce::Colour (0xffffd696).withAlpha (0.9f));
            g.setColour (Colours::goldLight);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (light));
        }
    }

    // The glowing dot sweeps 270 degrees, from -135 to +135.
    const float angle = juce::degreesToRadians (-135.0f + 270.0f * static_cast<float> (valueToProportionOfLength (getValue())));
    const juce::Point<float> dot (c.x + 28.0f * std::sin (angle), c.y - 28.0f * std::cos (angle));
    fillBlurredDisc (g, dot, 4.0f, 8.0f, juce::Colour (0xffffd696).withAlpha (0.9f));
    g.setColour (Colours::goldLight);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (dot));

    const float w = static_cast<float> (getWidth());
    drawTrackedText (g, label.toUpperCase(), { 0.0f, d.getBottom() + 8.0f, w, 16.0f },
                     Fonts::sans (12.0f, false, 0.2f), juce::Justification::centred, juce::Colour (0xfff1eaf6));
    // While you're on the ring, the value line shows the orbit instead.
    if (orbit != nullptr && (ringHover || orbitDragging))
    {
        const auto text = orbitAmount > 0.0f ? "Orbit " + juce::String (juce::roundToInt (orbitAmount * 100.0f)) + "%"
                                             : juce::String ("Drag to move");
        drawTrackedText (g, text, { 0.0f, d.getBottom() + 28.0f, w, 20.0f }, Fonts::sans (14.0f), juce::Justification::centred, Colours::gold);
        return;
    }
    const auto value = formatValue ? formatValue (getValue()) : getTextFromValue (getValue());
    drawTrackedText (g, value, { 0.0f, d.getBottom() + 28.0f, w, 20.0f },
                     Fonts::sans (14.0f), juce::Justification::centred, Colours::ivory);
}

//==============================================================================
ShapeButton::ShapeButton() : juce::Button ("Movement shape")
{
    setTitle ("Movement shape");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void ShapeButton::setShape (int index, const juce::String& name)
{
    shape = index;
    setButtonText (name.toUpperCase());
    setDescription (name);
    repaint();
}

float ShapeButton::preferredWidth() const
{
    // Wide enough for the longest name, so the row doesn't shift as the shape changes.
    return 14.0f + 26.0f + 8.0f + trackedTextWidth ("BREATH", Fonts::sans (11.0f, false, 0.18f)) + 16.0f;
}

void ShapeButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getHeight() * 0.5f;
    g.setColour (Colours::night.withAlpha (down ? 0.8f : (highlighted ? 0.75f : 0.6f)));
    g.fillRoundedRectangle (b, radius);
    g.setColour (Colours::gold.withAlpha (highlighted ? 0.7f : 0.45f));
    g.drawRoundedRectangle (b, radius, 1.0f);

    // A small drawing of the shape, two periods long.
    const juce::Rectangle<float> glyph (b.getX() + 14.0f, b.getCentreY() - 6.0f, 26.0f, 12.0f);
    juce::Path p;
    for (int i = 0; i <= 40; ++i)
    {
        const float t = (float) i / 40.0f;
        float v = 0.0f;
        if (shape == 1)
            v = std::sin (t * 4.0f * juce::MathConstants<float>::pi);
        else if (shape == 2)
        {
            const float ph = std::fmod (t * 2.0f, 1.0f);
            v = ph < 0.4f ? -std::cos (juce::MathConstants<float>::pi * ph / 0.4f)
                          : std::cos (juce::MathConstants<float>::pi * (ph - 0.4f) / 0.6f);
        }
        else
            v = 0.55f * std::sin (t * 7.1f + 0.4f) + 0.35f * std::sin (t * 17.3f + 1.1f) + 0.1f * std::sin (t * 31.0f);
        const juce::Point<float> pt (glyph.getX() + t * glyph.getWidth(), glyph.getCentreY() - v * glyph.getHeight() * 0.5f);
        if (i == 0)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
    }
    g.setColour (Colours::gold);
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    drawTrackedText (g, getButtonText(), b.withTrimmedLeft (glyph.getRight() - b.getX() + 8.0f), Fonts::sans (11.0f, false, 0.18f),
                     juce::Justification::centredLeft, Colours::text);
}

//==============================================================================
PillToggle::PillToggle (const juce::String& caption, float h, float fontSize)
    : pillHeight (h), fontPx (fontSize)
{
    setButtonText (caption);
    setTitle (caption);
}

float PillToggle::preferredWidth() const
{
    return (pillHeight < 38.0f ? 28.0f : 32.0f) + trackedTextWidth (getButtonText(), Fonts::sans (fontPx, false, 0.18f)) + 2.0f;
}

void PillToggle::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getHeight() * 0.5f;
    const bool on = getToggleState();

    if (on)
        g.setColour (Colours::gold.withMultipliedBrightness (down ? 0.92f : 1.0f));
    else
        g.setColour (highlighted ? Colours::night.withAlpha (0.75f) : Colours::night.withAlpha (0.6f));
    g.fillRoundedRectangle (b, radius);

    g.setColour (on ? Colours::gold : Colours::gold.withAlpha (highlighted ? 0.6f : 0.35f));
    g.drawRoundedRectangle (b, radius, 1.0f);

    drawTrackedText (g, getButtonText(), b, Fonts::sans (fontPx, false, 0.18f), juce::Justification::centred,
                     on ? Colours::ink : Colours::text);
}

//==============================================================================
HeaderButton::HeaderButton (const juce::String& caption) : juce::Button (caption)
{
    setButtonText (caption);
    setTitle (caption);
}

void HeaderButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Colours::night.withAlpha (down ? 0.7f : (highlighted ? 0.65f : 0.55f)));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (Colours::gold.withAlpha (highlighted ? 0.5f : 0.3f));
    g.drawRoundedRectangle (b, 10.0f, 1.0f);

    const auto font = Fonts::sans (12.0f, false, 0.18f);
    if (litSide < 0)
    {
        drawTrackedText (g, getButtonText(), b, font, juce::Justification::centred, Colours::text);
        return;
    }

    // "A / B" with the active side lit in gold.
    const juce::String parts[] = { "A", " / ", "B" };
    float total = 0.0f;
    float widths[3];
    for (int i = 0; i < 3; ++i)
        total += widths[i] = juce::GlyphArrangement::getStringWidth (font, parts[i]);
    total -= font.getHeight() * font.getExtraKerningFactor();

    float x = b.getCentreX() - total * 0.5f;
    for (int i = 0; i < 3; ++i)
    {
        const auto colour = i == 1 ? Colours::text
                          : ((i == 0) == (litSide == 0) ? Colours::gold : Colours::text);
        drawTrackedText (g, parts[i], { x, b.getY(), widths[i] + 4.0f, b.getHeight() }, font,
                         juce::Justification::centredLeft, colour);
        x += widths[i];
    }
}

//==============================================================================
DropButton::DropButton() : juce::Button ("Drop a voice")
{
    setTitle ("Drop a voice");
    setTooltip ("Drag a WAV or AIFF file onto Sing, or click to choose one");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

float DropButton::preferredWidth() const
{
    return 32.0f + 14.0f + 8.0f + trackedTextWidth ("DROP A VOICE", Fonts::sans (12.0f, false, 0.18f)) + 2.0f;
}

void DropButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getHeight() * 0.5f;
    const bool lit = highlighted || dragging;

    g.setColour (Colours::night.withAlpha (down ? 0.65f : (lit ? 0.6f : 0.45f)));
    g.fillRoundedRectangle (b, radius);

    juce::Path outline, dashed;
    outline.addRoundedRectangle (b, radius);
    const float dashes[] = { 4.0f, 3.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (Colours::gold.withAlpha (dragging ? 0.95f : (lit ? 0.75f : 0.55f)));
    g.fillPath (dashed);

    // Download arrow, as in the mockup.
    const float x = b.getX() + 16.0f, y = b.getCentreY() - 7.0f;
    juce::Path arrow;
    arrow.startNewSubPath (x + 7.0f, y + 1.0f);
    arrow.lineTo (x + 7.0f, y + 10.0f);
    arrow.startNewSubPath (x + 3.0f, y + 6.0f);
    arrow.lineTo (x + 7.0f, y + 10.0f);
    arrow.lineTo (x + 11.0f, y + 6.0f);
    arrow.startNewSubPath (x + 2.0f, y + 13.0f);
    arrow.lineTo (x + 12.0f, y + 13.0f);
    g.setColour (Colours::gold);
    g.strokePath (arrow, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    drawTrackedText (g, dragging ? "LET GO TO LOAD" : "DROP A VOICE", b.withTrimmedLeft (16.0f + 14.0f + 8.0f),
                     Fonts::sans (12.0f, false, 0.18f), juce::Justification::centredLeft, Colours::text);
}

//==============================================================================
ArrowButton::ArrowButton (const juce::String& name, bool pointsLeft)
    : juce::Button (name), left (pointsLeft)
{
    setTitle (name);
}

void ArrowButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat();
    if (highlighted || down)
    {
        g.setColour (Colours::gold.withAlpha (down ? 0.18f : 0.1f));
        g.fillRoundedRectangle (b, 8.0f);
    }

    const auto c = b.getCentre();
    const float s = left ? 1.0f : -1.0f;
    juce::Path chevron;
    chevron.startNewSubPath (c.x + 3.0f * s, c.y - 6.0f);
    chevron.lineTo (c.x - 3.0f * s, c.y);
    chevron.lineTo (c.x + 3.0f * s, c.y + 6.0f);
    g.setColour (Colours::gold);
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
TextDragger::TextDragger (const juce::String& captionText, bool onSunset)
    : caption (captionText), sunset (onSunset)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setMouseDragSensitivity (200);
    setVelocityModeParameters (0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setRepaintsOnMouseActivity (true);
    setTitle (caption);
    onValueChange = [this] { repaint(); };
}

float TextDragger::preferredWidth (const juce::String& widestValue) const
{
    // Wide enough for the longest value, so the text doesn't shift as it changes.
    const auto capFont = Fonts::sans (12.0f, false, 0.2f);
    const auto valueFont = Fonts::sans (12.0f, false, 0.05f);
    return trackedTextWidth (caption.toUpperCase(), capFont) + 8.0f + trackedTextWidth (widestValue, valueFont) + 4.0f;
}

void TextDragger::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto capFont = Fonts::sans (12.0f, false, 0.2f);
    const auto valueFont = Fonts::sans (12.0f, false, 0.05f);
    const auto cap = caption.toUpperCase();
    const float capWidth = juce::GlyphArrangement::getStringWidth (capFont, cap) + 6.0f;
    const bool active = isMouseOverOrDragging();

    const auto capColour = sunset ? Colours::dusk : (active ? Colours::text : Colours::soft);
    const auto valueColour = sunset ? Colours::ink : (active ? Colours::ivory : Colours::text);
    if (row)
    {
        const auto line = b.withTrimmedBottom (6.0f);
        drawTrackedText (g, cap, line, Fonts::sans (10.0f, false, 0.16f), juce::Justification::centredLeft, capColour);
        const auto value = formatValue ? formatValue (getValue()) : getTextFromValue (getValue());
        drawTrackedText (g, value, line, Fonts::sans (12.5f, false, 0.03f), juce::Justification::centredRight, valueColour);
        const juce::Rectangle<float> track (b.getX(), b.getBottom() - 4.0f, b.getWidth(), 3.0f);
        g.setColour (Colours::muted.withAlpha (0.18f));
        g.fillRoundedRectangle (track, 1.5f);
        const auto proportion = (float) valueToProportionOfLength (getValue());
        g.setColour (Colours::gold.withAlpha (active ? 1.0f : 0.85f));
        g.fillRoundedRectangle (track.withWidth (std::max (3.0f, track.getWidth() * proportion)), 1.5f);
        return;
    }
    drawTrackedText (g, cap, b, capFont, juce::Justification::centredLeft, capColour);
    const auto value = formatValue ? formatValue (getValue()) : getTextFromValue (getValue());
    drawTrackedText (g, value, b.withTrimmedLeft (capWidth), valueFont, juce::Justification::centredLeft, valueColour);
    if (active)
    {
        g.setColour (valueColour.withAlpha (0.35f));
        g.fillRect (b.getX() + capWidth, b.getBottom() - 9.0f, trackedTextWidth (value, valueFont), 1.0f);
    }
}

//==============================================================================
SaveOverlay::SaveOverlay()
{
    setInterceptsMouseClicks (true, true);
    addAndMakeVisible (name);
    addAndMakeVisible (save);
    addAndMakeVisible (cancel);

    name.setFont (Fonts::sans (15.0f));
    name.setJustification (juce::Justification::centredLeft);
    name.setIndents (12, 0);
    name.setColour (juce::TextEditor::backgroundColourId, Colours::night.withAlpha (0.7f));
    name.setColour (juce::TextEditor::textColourId, Colours::ivory);
    name.setColour (juce::TextEditor::outlineColourId, Colours::gold.withAlpha (0.35f));
    name.setColour (juce::TextEditor::focusedOutlineColourId, Colours::gold.withAlpha (0.8f));
    name.setColour (juce::TextEditor::highlightColourId, Colours::gold.withAlpha (0.3f));
    name.setColour (juce::CaretComponent::caretColourId, Colours::gold);
    name.setInputRestrictions (60);
    name.setTitle ("Preset name");
    name.onReturnKey = [this] { commit(); };
    name.onEscapeKey = [this] { setVisible (false); };

    save.onClick = [this] { commit(); };
    cancel.onClick = [this] { setVisible (false); };
    setVisible (false);
}

void SaveOverlay::show (const juce::String& suggestedName)
{
    name.setText (suggestedName, false);
    name.selectAll();
    setVisible (true);
    toFront (true);
    name.grabKeyboardFocus();
}

void SaveOverlay::commit()
{
    const auto text = name.getText().trim();
    if (text.isEmpty())
        return;
    setVisible (false);
    if (onSave)
        onSave (text);
}

juce::Rectangle<float> SaveOverlay::boxBounds() const
{
    return getLocalBounds().toFloat().withSizeKeepingCentre (380.0f, 176.0f);
}

void SaveOverlay::paint (juce::Graphics& g)
{
    g.fillAll (Colours::night.withAlpha (0.6f));
    const auto box = boxBounds();
    g.setColour (juce::Colour (0xff1d2360).withAlpha (0.97f));
    g.fillRoundedRectangle (box, 14.0f);
    g.setColour (Colours::gold.withAlpha (0.35f));
    g.drawRoundedRectangle (box, 14.0f, 1.0f);
    drawTrackedText (g, "SAVE PRESET (THE VOICE IS SAVED WITH IT)", box.reduced (24.0f).withHeight (16.0f),
                     Fonts::sans (12.0f, false, 0.2f), juce::Justification::centredLeft, Colours::muted);
}

void SaveOverlay::resized()
{
    const auto box = boxBounds().reduced (24.0f).toNearestInt();
    name.setBounds (box.getX(), box.getY() + 28, box.getWidth(), 40);
    save.setBounds (box.getRight() - 90, box.getBottom() - 36, 90, 36);
    cancel.setBounds (box.getRight() - 90 - 10 - 100, box.getBottom() - 36, 100, 36);
}

void SaveOverlay::mouseDown (const juce::MouseEvent& e)
{
    // Clicking outside the box cancels.
    if (! boxBounds().contains (e.position))
        setVisible (false);
}

//==============================================================================
BlueHourLookAndFeel::BlueHourLookAndFeel()
{
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1d2360));
    setColour (juce::PopupMenu::textColourId, Colours::soft);
    setColour (juce::PopupMenu::headerTextColourId, Colours::muted);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::gold.withAlpha (0.22f));
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::ivory);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff1d2360));
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::TooltipWindow::outlineColourId, Colours::gold.withAlpha (0.3f));
}

juce::Font BlueHourLookAndFeel::getPopupMenuFont() { return Fonts::sans (15.0f); }

void BlueHourLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (Colours::gold.withAlpha (0.3f));
    g.drawRect (0, 0, width, height, 1);
}

void BlueHourLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    const float alpha = (isMouseOver || isMouseDragging) ? 0.6f : 0.3f;
    g.setColour (Colours::ink.withAlpha (alpha));
    const auto fw = static_cast<float> (w), fh = static_cast<float> (h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine (fw * i, fh, fw, fh * i, 1.0f);
}

} // namespace sing::ui

namespace sing::ui
{

//==============================================================================
VerticalFader::VerticalFader (const juce::String& captionText) : caption (captionText)
{
    setSliderStyle (juce::Slider::LinearVertical);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setSliderSnapsToMousePosition (false);
    setVelocityModeParameters (0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setRepaintsOnMouseActivity (true);
    setTitle (caption);
    onValueChange = [this] { repaint(); };
}

void VerticalFader::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool active = isMouseOverOrDragging();
    const auto value = formatValue ? formatValue (getValue()) : getTextFromValue (getValue());
    drawTrackedText (g, value, b.withHeight (14.0f), Fonts::sans (11.5f, false, 0.02f), juce::Justification::centred,
                     active ? Colours::ivory : Colours::text);
    drawTrackedText (g, caption.toUpperCase(), b.withTrimmedTop (b.getHeight() - 14.0f), Fonts::sans (9.5f, false, 0.1f),
                     juce::Justification::centred, active ? Colours::text : Colours::muted);

    const juce::Rectangle<float> track (b.getCentreX() - 3.0f, 20.0f, 6.0f, b.getHeight() - 40.0f);
    g.setColour (Colours::muted.withAlpha (0.18f));
    g.fillRoundedRectangle (track, 3.0f);
    const auto proportion = (float) valueToProportionOfLength (getValue());
    const float y = track.getBottom() - track.getHeight() * proportion;
    g.setColour (Colours::gold.withAlpha (0.85f));
    g.fillRoundedRectangle (track.withTop (y), 3.0f);
    const auto thumb = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ track.getCentreX(), y });
    if (proportion > 0.0f)
        fillBlurredDisc (g, thumb.getCentre(), 6.0f, 6.0f, Colours::gold.withAlpha (active ? 0.6f : 0.4f));
    g.setColour (Colours::goldLight);
    g.fillEllipse (thumb);
}

} // namespace sing::ui
