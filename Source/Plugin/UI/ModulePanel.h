#pragma once

#include "Controls.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace sing::ui
{

// Bloom's Advanced section: one square per module, each with its on/off light, its controls
// and a small moving picture of what it does, plus an eighth square that draws what they all
// add up to. It covers the waveform and knobs while it is open (layout B of the mockups).
enum class ModuleKind { window, onsets, shift, smear, tone, harmonics, filter };

// Every module's settings, in parameter units, for the pictures.
struct ModuleSettings
{
    float windowSeconds = 0.68f;
    float onsets = 100.0f;           // percent
    float shiftHz = 0.0f;
    float smear = 0.0f;              // percent
    float tone = 0.0f;               // -100 to 100
    float harmonicsCents = 25.0f;
    int harmonicsCount = 16;
    float lowHz = 120.0f, highHz = 8000.0f;
    bool onsetsOn = true, shiftOn = true, smearOn = true, toneOn = true, harmonicsOn = false, filterOn = false;

    bool isOn (ModuleKind) const noexcept;
};

// A round on/off light with a power symbol.
class PowerButton final : public juce::ToggleButton
{
public:
    explicit PowerButton (const juce::String& moduleName);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

class ModuleTile final : public juce::Component
{
public:
    ModuleTile (ModuleKind, const juce::String& name, bool isNew);

    ModuleKind kind() const noexcept { return moduleKind; }
    PowerButton* power() noexcept { return powerButton.get(); }   // null for Window, which is always on
    TextDragger& addControl (const juce::String& caption, const juce::String& widestValue);

    void update (const ModuleSettings&, float time);
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr float height = 160.0f;

private:
    juce::Rectangle<float> visualArea() const;

    ModuleKind moduleKind;
    juce::String name;
    bool isNew;
    std::unique_ptr<PowerButton> powerButton;
    std::vector<std::unique_ptr<TextDragger>> controls;
    std::vector<juce::String> widest;
    ModuleSettings settings;
    float t = 0.0f;
};

// The eighth square: the spectrum after every module that is on.
class ResultTile final : public juce::Component
{
public:
    ResultTile();
    void update (const ModuleSettings&, float time);
    void paint (juce::Graphics&) override;

private:
    ModuleSettings settings;
    float t = 0.0f;
};

// Draws one module's picture into area. Exposed for the snapshot tool and tests of layout.
void drawModuleVisual (juce::Graphics&, ModuleKind, juce::Rectangle<float> area, const ModuleSettings&, float time);

class AdvancedPanel final : public juce::Component
{
public:
    explicit AdvancedPanel (juce::AudioProcessorValueTreeState&);
    ~AdvancedPanel() override;

    // Once per display frame while open. activity (0 to 1) is how loud the notes are: the
    // pictures move faster while something plays.
    void frame (double seconds, bool animate, float activity);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr float headingHeight = 32.0f;

private:
    ModuleSettings readSettings() const;
    void resetAll();

    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<ModuleTile>> tiles;
    ResultTile result;
    HeaderButton resetButton { "RESET ALL" };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    double lastSeconds = -1.0;
    float visualTime = 0.0f;
};

} // namespace sing::ui
