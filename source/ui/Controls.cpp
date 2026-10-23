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

bool Knob::hitTest (int x, int y)
{
    // The dial and its text both respond, but not the empty space beside them.
    const auto d = dialBounds();
    const auto p = juce::Point<float> (static_cast<float> (x), static_cast<float> (y));
    return p.getDistanceFrom (d.getCentre()) <= dialSize * 0.55f
        || (p.y > d.getBottom() && std::abs (p.x - d.getCentreX()) < dialSize * 0.7f);
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
    const auto inner = d.reduced (1.0f); // inside the 1-unit border
    const auto c = d.getCentre();

    // box-shadow: 0 10px 24px rgba(10,8,30,0.5)
    fillBlurredDisc (g, c.translated (0.0f, 10.0f), dialSize * 0.5f, 24.0f, juce::Colour (10, 8, 30).withAlpha (0.5f));

    // radial-gradient(circle at 38% 30%, #6a5aa0, #3a2f70 55%, #201a46): the circle
    // reaches the farthest corner of the 76-unit box, 71 units away.
    const juce::Point<float> light (inner.getX() + 0.38f * inner.getWidth(), inner.getY() + 0.30f * inner.getHeight());
    const float reach = light.getDistanceFrom (inner.getBottomRight());
    juce::ColourGradient grad (juce::Colour (0xff6a5aa0), light, juce::Colour (0xff201a46), light.translated (reach, 0.0f), true);
    grad.addColour (0.55, juce::Colour (0xff3a2f70));
    g.setGradientFill (grad);
    g.fillEllipse (d);

    // inset 0 1px 0 rgba(255,230,240,0.2): a thin highlight along the top edge.
    {
        juce::Path crescent;
        crescent.addEllipse (inner);
        crescent.addEllipse (inner.translated (0.0f, 1.0f));
        crescent.setUsingNonZeroWinding (false);
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addEllipse (inner);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colour (255, 230, 240).withAlpha (0.2f));
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

    // The 1px gold border, brighter while you're touching the knob.
    g.setColour (Colours::gold.withAlpha (isMouseOverOrDragging() ? 0.6f : 0.25f));
    g.drawEllipse (d.reduced (0.5f), 1.0f);

    // The ember sweeps 270 degrees, from -135 to +135, 28 units out from the centre.
    const float angle = juce::degreesToRadians (-135.0f + 270.0f * static_cast<float> (valueToProportionOfLength (getValue())));
    const juce::Point<float> dot (c.x + 28.0f * std::sin (angle), c.y - 28.0f * std::cos (angle));
    fillBlurredDisc (g, dot, 4.0f, 8.0f, juce::Colour (255, 214, 150).withAlpha (0.9f));
    g.setColour (Colours::ember);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (dot));

    // Label 8 below the dial, value 4 below that (CSS line boxes: 17.3 and 20.2 tall).
    const float w = static_cast<float> (getWidth());
    drawTrackedText (g, label.toUpperCase(), { 0.0f, d.getBottom() + 8.0f, w, 17.3f },
                     Fonts::sans (12.0f, false, 0.2f), juce::Justification::centred, Colours::label);
    const auto value = formatValue ? formatValue (getValue()) : getTextFromValue (getValue());
    drawTrackedText (g, value, { 0.0f, d.getBottom() + 21.3f, w, 20.2f },
                     Fonts::sans (14.0f), juce::Justification::centred, Colours::ivory);
}

//==============================================================================
PillToggle::PillToggle (const juce::String& caption) : PillToggle (caption, Style {}) {}

PillToggle::PillToggle (const juce::String& caption, Style s) : style (s)
{
    setButtonText (caption);
    setTitle (caption);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

float PillToggle::preferredWidth() const
{
    return style.padding * 2.0f + 2.0f + trackedTextWidth (getButtonText(), Fonts::sans (style.fontSize, false, style.tracking));
}

void PillToggle::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getHeight() * 0.5f;
    const bool on = getToggleState();

    if (on)
        g.setColour (down ? Colours::gold.darker (0.08f) : Colours::gold);
    else
        g.setColour (Colours::dusk.withAlpha (juce::jmin (1.0f, style.offFill + (down ? 0.2f : (highlighted ? 0.1f : 0.0f)))));
    g.fillRoundedRectangle (b, radius);

    g.setColour (on ? Colours::gold : Colours::gold.withAlpha (juce::jmin (1.0f, style.offBorder * (highlighted ? 1.8f : 1.0f))));
    g.drawRoundedRectangle (b, radius, 1.0f);

    drawTrackedText (g, getButtonText(), b, Fonts::sans (style.fontSize, false, style.tracking), juce::Justification::centred,
                     on ? Colours::ink : Colours::text);
}

//==============================================================================
namespace
{
    juce::Font headerFont() { return Fonts::sans (12.0f, false, 0.18f); }
}

HeaderButton::HeaderButton (const juce::String& caption) : juce::Button (caption)
{
    setButtonText (caption);
    setTitle (caption);
}

float HeaderButton::preferredWidth() const
{
    // padding: 0 16px, plus the 1px border each side.
    return 34.0f + trackedTextWidth (getButtonText(), headerFont());
}

void HeaderButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Colours::dusk.withAlpha (down ? 0.8f : (highlighted ? 0.68f : 0.55f)));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (Colours::gold.withAlpha (highlighted ? 0.55f : 0.3f));
    g.drawRoundedRectangle (b, 10.0f, 1.0f);

    const auto font = headerFont();
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
        const bool lit = i != 1 && ((i == 0) == (litSide == 0));
        drawTrackedText (g, parts[i], { x, b.getY(), widths[i] + 4.0f, b.getHeight() }, font,
                         juce::Justification::centredLeft, lit ? Colours::gold : Colours::text);
        x += widths[i];
    }
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
    chevron.startNewSubPath (c.x + 2.5f * s, c.y - 5.0f);
    chevron.lineTo (c.x - 2.5f * s, c.y);
    chevron.lineTo (c.x + 2.5f * s, c.y + 5.0f);
    g.setColour (Colours::gold);
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
namespace
{
    // Captions are tracked 0.2em, values 0.05em, with 6 between them (like the mockup).
    juce::Font captionFont() { return Fonts::sans (12.0f, false, 0.2f); }
    juce::Font valueFont()   { return Fonts::sans (12.0f, false, 0.05f); }
    constexpr float kValueGap = 6.0f;
}

TextDragger::TextDragger (const juce::String& captionText, bool darkText)
    : caption (captionText), dark (darkText)
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

juce::String TextDragger::getValueText() const
{
    // (Slider::getTextFromValue isn't const, though it doesn't change anything.)
    return formatValue ? formatValue (getValue()) : const_cast<TextDragger*> (this)->getTextFromValue (getValue());
}

float TextDragger::preferredWidth() const
{
    // The caption's tracking still trails it, as in CSS, before the value's margin.
    return juce::GlyphArrangement::getStringWidth (captionFont(), caption.toUpperCase()) + kValueGap
         + trackedTextWidth (getValueText(), valueFont()) + 2.0f;
}

void TextDragger::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto cap = caption.toUpperCase();
    const float capWidth = juce::GlyphArrangement::getStringWidth (captionFont(), cap);
    const bool active = isMouseOverOrDragging();

    const auto capColour = dark ? Colours::inkSoft : (active ? Colours::text : Colours::soft);
    const auto valueColour = dark ? (active ? juce::Colours::black : Colours::ink) : (active ? Colours::ivory : Colours::text);

    const auto value = getValueText();
    const float valueWidth = trackedTextWidth (value, valueFont());

    // The text sits against the right edge, so the value's end stays put and the gap to
    // the next caption on the right stays even, like the mockup's flex row.
    const float x = std::max (0.0f, b.getRight() - 1.0f - (capWidth + kValueGap + valueWidth));
    drawTrackedText (g, cap, b.withLeft (x), captionFont(), juce::Justification::centredLeft, capColour);
    drawTrackedText (g, value, b.withLeft (x + capWidth + kValueGap), valueFont(), juce::Justification::centredLeft, valueColour);

    // A faint underline while you're touching it, so it reads as something to drag.
    if (active)
    {
        g.setColour (valueColour.withAlpha (0.35f));
        g.fillRect (x + capWidth + kValueGap, b.getCentreY() + 9.0f, valueWidth, 1.0f);
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
    name.setColour (juce::TextEditor::backgroundColourId, Colours::dusk.withAlpha (0.8f));
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
    g.fillAll (juce::Colour (0xff0b0d2a).withAlpha (0.6f));
    const auto box = boxBounds();
    g.setColour (juce::Colour (0xff171a48).withAlpha (0.97f));
    g.fillRoundedRectangle (box, 14.0f);
    g.setColour (Colours::gold.withAlpha (0.35f));
    g.drawRoundedRectangle (box, 14.0f, 1.0f);
    drawTrackedText (g, "SAVE PRESET", box.reduced (24.0f).withHeight (16.0f),
                     Fonts::sans (12.0f, false, 0.28f), juce::Justification::centredLeft, Colours::soft);
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
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff171a48));
    setColour (juce::PopupMenu::textColourId, Colours::soft);
    setColour (juce::PopupMenu::headerTextColourId, Colours::muted);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff171a48));
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::TooltipWindow::outlineColourId, Colours::gold.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::gold.withAlpha (0.22f));
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::ivory);
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
    // The corner sits on the peach end of the sky, so the grip is dark.
    const float alpha = (isMouseOver || isMouseDragging) ? 0.6f : 0.25f;
    g.setColour (Colours::ink.withAlpha (alpha));
    const auto fw = static_cast<float> (w), fh = static_cast<float> (h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine (fw * i, fh, fw, fh * i, 1.0f);
}

} // namespace sing::ui
