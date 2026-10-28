#pragma once

#include "Controls.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace sing::ui
{

// The modules on the main panel: one tile per module, each with its on/off light, its controls
// and a small moving picture of what it does. Most shape Bloom only and rest while Grain plays;
// Binaural, Tape, Shimmer, Ensemble, Halo, Vowel, Harmoniser and Space work in both engines. (The
// Space tile is ModuleKind::spaceFilter, its name from when it held only the reverb's cuts.)
enum class ModuleKind { window, onsets, shift, smear, tone, harmonics, filter, compressor, octaves, binaural, tape, shimmer,
                        ensemble, halo, vowel, harmoniser, spaceFilter };

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
    float tapeWow = 30.0f, tapeFlutter = 20.0f, tapeDrive = 35.0f, tapeAge = 40.0f;   // percent
    float shimmerAmount = 40.0f, shimmerRise = 50.0f;                                  // percent
    int shimmerInterval = 0;         // 0 an octave up, 1 a fifth up
    int ensembleSingers = 4;
    float ensembleDetune = 10.0f;    // cents
    float ensembleVibrato = 40.0f, ensembleWidth = 70.0f;   // percent
    float haloLevel = 50.0f, haloDrift = 20.0f;             // percent
    int haloOctave = 1;              // 0 down, 1 same, 2 up
    float vowel = 50.0f;             // 0 ooh, 25 oh, 50 ah, 75 eh, 100 ee
    float vowelDepth = 60.0f, vowelBreathe = 25.0f;        // percent
    int harmonyKey = 0, harmonyScale = 0;                   // C; major
    int harmonyAbove = 1, harmonyBelow = 0;                 // 0 off, 1 3rd, 2 4th, 3 5th, 4 6th, 5 octave
    bool onsetsOn = true, shiftOn = true, smearOn = true, toneOn = true, harmonicsOn = false, filterOn = false;
    bool compressorOn = false, octavesOn = false, binauralOn = false, spaceFilterOn = false;
    bool tapeOn = false, shimmerOn = false;
    bool ensembleOn = false, haloOn = false, vowelOn = false, harmonyOn = false;

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
    bool isCompact() const noexcept
    {
        return moduleKind == ModuleKind::spaceFilter || moduleKind == ModuleKind::tape
            || moduleKind == ModuleKind::ensemble || moduleKind == ModuleKind::harmoniser;
    }

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
    static constexpr int columns = 6, rows = 3;
    static constexpr float gap = 12.0f;
    static float preferredHeight() noexcept { return headingHeight + 8.0f + (float) rows * ModuleTile::height + (float) (rows - 1) * gap; }

private:
    ModuleSettings readSettings() const;
    void resetAll();

    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<ModuleTile>> tiles;
    HeaderButton resetButton { "RESET ALL" };
    std::array<PillToggle*, 3> slopeButtons {};
    std::unique_ptr<juce::ParameterAttachment> slopeAttachment;
    std::array<PillToggle*, 2> intervalButtons {};
    std::unique_ptr<juce::ParameterAttachment> intervalAttachment;
    std::array<PillToggle*, 3> haloOctaveButtons {};
    std::unique_ptr<juce::ParameterAttachment> haloOctaveAttachment;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    double lastSeconds = -1.0;
    float visualTime = 0.0f;
    bool bloom = true;
};

} // namespace sing::ui
