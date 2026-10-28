#pragma once

#include "PluginProcessor.h"
#include "UI/BlueHourSky.h"
#include "UI/NightSky.h"
#include "UI/Controls.h"
#include "UI/ModulePanel.h"
#include "UI/Theme.h"
#include "UI/WaveformView.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace sing
{

// The blue hour front panel, laid out at ui::panelWidth x ui::panelHeight units: the header,
// the voice's waveform, the loop row, the knobs, every module, and the bottom row.
// The editor scales it as a whole when the window is resized.
class SingPanel final : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit SingPanel (SingProcessor&);
    ~SingPanel() override;

    // Called once per display frame. `seconds` is the animation clock.
    void frame (double seconds, bool animate);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    class PresetName;
    class StarButton;
    class VoiceInfo;

    void showPresetMenu();
    void showVoiceMenu();
    void chooseVoiceFile();
    void loadVoice (const juce::File&);
    void updatePresetDisplay();
    void updateVoiceDisplay();
    void setBypassed (bool);
    void showNote (const juce::String&);
    juce::String formatLoopSeconds (double percent) const;

    SingProcessor& processor;
    PresetManager& presets;
    ui::NightSky sky;   // the firefly night, in both engines

    // Header
    ui::ArrowButton previous { "Previous preset", true }, next { "Next preset", false };
    std::unique_ptr<PresetName> presetName;
    ui::HeaderButton abButton { "A / B" }, saveButton { "SAVE" };
    juce::Rectangle<float> presetFrame;

    // The voice
    std::unique_ptr<VoiceInfo> voiceInfo;
    ui::DropButton dropButton;
    ui::WaveformView waveform;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::int64 shownVoice = -1;

    // Loop settings
    std::array<std::unique_ptr<ui::PillToggle>, 4> directionButtons;
    std::unique_ptr<juce::ParameterAttachment> directionAttachment;
    ui::TextDragger loopStart { "Start" }, loopLength { "Length" }, loopFade { "Fade" };
    ui::TextDragger wanderDragger { "Wander" };

    // How keys are pitched (Grain only): Formant or STFT.
    std::array<std::unique_ptr<ui::PillToggle>, 2> pitchModeButtons;
    std::unique_ptr<juce::ParameterAttachment> pitchModeAttachment;
    void updatePitchModeButtons();
    float keysCaptionX = 0.0f;

    // Knobs
    enum KnobId { attack, release, stretch, spread, formant, air, space, numKnobs };
    std::array<ui::Knob, numKnobs> knobs;

    // Bottom row
    ui::PillToggle holdButton { "HOLD" };
    ui::PillToggle keepVowelButton { "KEEP VOWEL" };
    ui::ModulesPanel modules;
    std::array<std::unique_ptr<ui::PillToggle>, 2> engineButtons;
    std::unique_ptr<juce::ParameterAttachment> engineAttachment;
    bool bloomEngine = false;
    ui::TextDragger voicesDragger { "Voices", true }, glideDragger { "Glide", true }, outputDragger { "Output", true };

    // Movement: the shape, the speed (seconds, or bars when synced), Sync and Each note.
    ui::ShapeButton shapeButton;
    std::unique_ptr<juce::ParameterAttachment> shapeAttachment, syncAttachment;
    ui::TextDragger speedDragger { "Speed", true }, barsDragger { "Every", true };
    ui::PillToggle syncButton { "SYNC", 32.0f, 11.0f }, eachNoteButton { "EACH NOTE", 32.0f, 11.0f };
    void updateMovementStrip();
    void parameterChanged (const juce::String&, float) override;
    std::atomic<bool> movementDirty { true };

    ui::SaveOverlay saveOverlay;

    // The evening star is the bypass switch.
    std::unique_ptr<StarButton> starButton;
    std::unique_ptr<juce::ParameterAttachment> bypassAttachment;
    bool bypassed = false;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    int lastPresetChange = -1;
    juce::String lastPresetText;
    bool lastAB = false;
    double lastPresetCheck = 0.0, lastFrameTime = -1.0;
    juce::String note;
    double noteUntil = 0.0;
    bool motion = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingPanel)
};

class SingEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SingEditor (SingProcessor&);
    ~SingEditor() override;

    void resized() override;

    // Draws the panel at a given animation time. For the snapshot tool.
    void renderFrame (double seconds, bool animate) { panel.frame (seconds, animate); }

private:
    void onFrame (double timestamp);

    SingProcessor& processor;
    ui::BlueHourLookAndFeel lookAndFeel;
    SingPanel panel;
    juce::TooltipWindow tooltips { this, 600 };
    juce::VBlankAttachment vblank;

    double lastFrame = -1.0, animationTime = 0.0, lastMotionCheck = -10.0;
    bool motion = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingEditor)
};

} // namespace sing
