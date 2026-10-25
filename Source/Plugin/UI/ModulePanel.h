#pragma once

#include "Controls.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace sing::ui
{

// The modules on the main panel: one tile per module, each with its on/off light, its controls
// and a small moving picture of what it does. Most shape Bloom only and rest while Grain plays;
// Binaural and Space work in both engines. (The Space tile is ModuleKind::spaceFilter, its
// name from when it held only the reverb's cuts.)
enum class ModuleKind { window, onsets, shift, smear, tone, harmonics, filter, compressor, octaves, binaural, spaceFilter };

// Every module's settings, in parameter units, for the pictures.
struct ModuleSettings
{
    static constexpr int numOctaves = 6;

    float windowSeconds = 0.68f;
    float onsets = 100.0f;           // percent
    float shiftHz = 0.0f;
    float smear = 0.0f;              // percent
    float tone = 0.0f;               // -100 to 100
    float harmonicsCents = 25.0f;
    int harmonicsCount = 16;
    float lowHz = 120.0f, highHz = 8000.0f;
    int filterSlope = 0;             // 0 sharp, 1 6 dB per octave, 2 12 dB per octave
    float compressor = 50.0f;        // percent
    std::array<float, numOctaves> octaveLevels { 0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f };   // percent
    float beatHz = 4.0f, binauralAmount = 100.0f;
    float spaceLowHz = 150.0f, spaceHighHz = 9000.0f;
    float spaceLevel = 0.0f;           // percent: the Space knob
    float spaceDecaySeconds = 6.3f, spaceSize = 67.5f;
    bool onsetsOn = true, shiftOn = true, smearOn = true, toneOn = true, harmonicsOn = false, filterOn = false;
    bool compressorOn = false, octavesOn = false, binauralOn = false, spaceFilterOn = false;

    bool isOn (ModuleKind) const noexcept;
};

// True for the modules that work in both engines.
bool worksInBothEngines (ModuleKind) noexcept;

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
    PowerButton* power() noexcept { return powerButton.get(); }   // null for Window (always on) and Space
    // A small switch in the header, where the power light would be (Space's CUTS).
    PillToggle& addHeaderToggle (const juce::String& text);
    TextDragger& addControl (const juce::String& caption);
    VerticalFader& addFader (const juce::String& caption);
    PillToggle& addChoice (const juce::String& text);

    // resting: Grain is playing and this module only shapes Bloom.
    void update (const ModuleSettings&, float time, bool resting);
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr float height = 188.0f;

private:
    juce::Rectangle<float> visualArea() const;

    ModuleKind moduleKind;
    juce::String name;
    bool isNew;
    std::unique_ptr<PowerButton> powerButton;
    std::unique_ptr<PillToggle> headerToggle;
    std::vector<std::unique_ptr<TextDragger>> controls;
    std::vector<std::unique_ptr<VerticalFader>> faders;
    std::vector<std::unique_ptr<PillToggle>> choices;
    ModuleSettings settings;
    float t = 0.0f;
    bool rest = false;
};

// Draws one module's picture into area. Exposed for the snapshot tool and tests of layout.
void drawModuleVisual (juce::Graphics&, ModuleKind, juce::Rectangle<float> area, const ModuleSettings&, float time);

class ModulesPanel final : public juce::Component
{
public:
    explicit ModulesPanel (juce::AudioProcessorValueTreeState&);
    ~ModulesPanel() override;

    // Once per display frame. activity (0 to 1) is how loud the notes are: the pictures move
    // faster while something plays.
    void frame (double seconds, bool animate, float activity);
    void setBloomEngine (bool);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr float headingHeight = 28.0f;
    static constexpr int columns = 6;
    static constexpr float gap = 12.0f;
    static float preferredHeight() noexcept { return headingHeight + 8.0f + 2.0f * ModuleTile::height + gap; }

private:
    ModuleSettings readSettings() const;
    void resetAll();

    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<ModuleTile>> tiles;
    HeaderButton resetButton { "RESET ALL" };
    std::array<PillToggle*, 3> slopeButtons {};
    std::unique_ptr<juce::ParameterAttachment> slopeAttachment;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    double lastSeconds = -1.0;
    float visualTime = 0.0f;
    bool bloom = true;
};

} // namespace sing::ui
