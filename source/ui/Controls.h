#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace sing::ui
{

// A blue hour knob: the 76-unit violet dial with its glowing ember, plus its label and
// value underneath. Drag up/down (hold Shift for fine moves), scroll, or double-click
// to reset.
class Knob final : public juce::Slider
{
public:
    static constexpr float dialSize = 78.0f;  // 76 across plus its 1-unit gold border
    static constexpr float topMargin = 14.0f; // room for the dial's shadow
    static constexpr float textHeight = 52.0f; // label and value under the dial

    Knob();

    void setLabel (const juce::String&);
    std::function<juce::String (double)> formatValue;

    void paint (juce::Graphics&) override;
    bool hitTest (int x, int y) override;

private:
    juce::Rectangle<float> dialBounds() const;
    void rebuildDial (float pixelScale);

    juce::String label;
    juce::Image dial;
    float dialScale = 0.0f;
};

// The rounded switches: the loop directions and Hold, -1 Oct, +1 Oct, Tape.
// On, they fill with gold and the text turns dark.
class PillToggle final : public juce::ToggleButton
{
public:
    struct Style
    {
        float fontSize = 12.0f;
        float tracking = 0.18f;
        float padding = 16.0f;   // either side of the text
        float offFill = 0.6f;    // opacity of the dusk glass when off
        float offBorder = 0.35f; // opacity of the gold border when off
    };

    explicit PillToggle (const juce::String& text);
    PillToggle (const juce::String& text, Style);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    float preferredWidth() const;

private:
    Style style;
};

// Small framed buttons in the header (A / B and Save).
class HeaderButton final : public juce::Button
{
public:
    explicit HeaderButton (const juce::String& text);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    float preferredWidth() const;

    // For A / B: which letter is lit (0 = A, 1 = B, -1 = neither).
    void setLitSide (int side) { litSide = side; repaint(); }

private:
    int litSide = -1;
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

// Text that you drag up and down to change, like "START  1.44 s".
class TextDragger final : public juce::Slider
{
public:
    // dark = the colours for the bottom of the sky, where it turns light peach.
    TextDragger (const juce::String& caption, bool dark = false);

    std::function<juce::String (double)> formatValue;

    // The text as shown now, and the width it needs. The panel lays the row out again
    // when this changes, the way the mockup's flex row would.
    juce::String getValueText() const;
    float preferredWidth() const;

    void paint (juce::Graphics&) override;

private:
    juce::String caption;
    bool dark;
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
