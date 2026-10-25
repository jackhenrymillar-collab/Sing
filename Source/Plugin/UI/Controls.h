#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace sing::ui
{

// A blue hour knob: the 76-unit dial with its glowing dot, plus its label and value
// underneath. Drag up/down (hold Shift for fine moves), scroll, or double-click to reset.
//
// A knob that can move has an orbit ring around its dial (Movement). Drag the ring up or
// down (or Option-drag the dial) to set how far the knob wanders; double-click the ring to
// stop it. The gold arc shows where it wanders and a small light circles to where it is now.
class Knob final : public juce::Slider
{
public:
    static constexpr float dialSize = 76.0f;
    static constexpr float topMargin = 14.0f;   // room above the dial for the orbit ring
    static constexpr float height = topMargin + dialSize + 52.0f;
    static constexpr float ringGap = 9.0f;   // from the dial's edge to the orbit ring

    Knob();
    ~Knob() override;

    void setLabel (const juce::String&);
    std::function<juce::String (double)> formatValue;

    // Gives the knob an orbit ring for this parameter (0 to 100%).
    void setOrbit (juce::RangedAudioParameter&);
    // Where the knob has wandered to now, 0 to 1 of its range; -1 hides the light.
    void setOrbitLive (float position);
    float getOrbitAmount() const noexcept { return orbitAmount; }

    void paint (juce::Graphics&) override;
    bool hitTest (int x, int y) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> dialBounds() const;
    void rebuildDial (float pixelScale);
    bool onRing (juce::Point<float>) const;
    void setRingHover (bool);

    juce::String label;
    juce::Image dial;
    float dialScale = 0.0f;

    std::unique_ptr<juce::ParameterAttachment> orbit;
    float orbitAmount = 0.0f;          // 0 to 1
    float orbitLive = -1.0f;
    float orbitDragStart = 0.0f;
    bool orbitDragging = false, ringHover = false;
};

// Movement's shape switch in the bottom row: a pill with a small drawing of the shape and
// its name. Click for the next shape.
class ShapeButton final : public juce::Button
{
public:
    ShapeButton();
    void setShape (int index, const juce::String& name);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    float preferredWidth() const;

private:
    int shape = 0;
};

// The rounded switches: Hold along the bottom, and the loop direction choices.
class PillToggle final : public juce::ToggleButton
{
public:
    PillToggle (const juce::String& text, float height = 40.0f, float fontSize = 12.0f);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    float preferredWidth() const;

private:
    float pillHeight, fontPx;
};

// Small framed buttons in the header (A / B and Save).
class HeaderButton final : public juce::Button
{
public:
    explicit HeaderButton (const juce::String& text);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    // For A / B: which letter is lit (0 = A, 1 = B, -1 = neither).
    void setLitSide (int side) { litSide = side; repaint(); }

private:
    int litSide = -1;
};

// "DROP A VOICE": a dashed pill with a download arrow.
class DropButton final : public juce::Button
{
public:
    DropButton();
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    float preferredWidth() const;
    void setHighlighted (bool dragOver) { dragging = dragOver; repaint(); }

private:
    bool dragging = false;
};

// The preset arrows: a gold chevron.
class ArrowButton final : public juce::Button
{
public:
    ArrowButton (const juce::String& name, bool pointsLeft);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    bool left;
};

// Text that you drag up and down to change, like "LENGTH  1.92 s".
class TextDragger final : public juce::Slider
{
public:
    // onSunset: drawn in dark ink, for the bottom row where the sky is peach.
    TextDragger (const juce::String& caption, bool onSunset = false);
    std::function<juce::String (double)> formatValue;
    void paint (juce::Graphics&) override;
    float preferredWidth (const juce::String& widestValue) const;
    void setOnSunset (bool s) { if (s != sunset) { sunset = s; repaint(); } }

    // Row style, for the module tiles: the caption on the left, the value on the right and a
    // thin bar underneath showing where the value sits in its range.
    void setRowStyle (bool r) { row = r; repaint(); }

private:
    juce::String caption;
    bool sunset;
    bool row = false;
};

// A small upright fader with its value above and its caption below (the Octaves levels).
// Drag up or down (Shift for fine moves), or double-click to reset.
class VerticalFader final : public juce::Slider
{
public:
    explicit VerticalFader (const juce::String& caption);
    std::function<juce::String (double)> formatValue;
    void paint (juce::Graphics&) override;

private:
    juce::String caption;
};

// "Name this preset" box shown over the panel when Save is clicked.
class SaveOverlay final : public juce::Component
{
public:
    SaveOverlay();
    void show (const juce::String& suggestedName);
    std::function<void (const juce::String&)> onSave;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void commit();
    juce::Rectangle<float> boxBounds() const;

    juce::TextEditor name;
    HeaderButton save { "SAVE" }, cancel { "CANCEL" };
};

// Look and feel for the bits JUCE draws itself: popup menus, text editor, resizer.
class BlueHourLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    BlueHourLookAndFeel();
    juce::Font getPopupMenuFont() override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
};

} // namespace sing::ui
