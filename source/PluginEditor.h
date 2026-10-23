#pragma once

#include "PluginProcessor.h"
#include "ui/BlueHourSky.h"
#include "ui/Controls.h"
#include "ui/Theme.h"
#include "ui/VoiceHorizon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace sing
{

// The blue hour front panel, laid out at 1040 x 620 units to match the mockup.
// The editor scales it as a whole when the window is resized.
//
// Audio files dropped anywhere on it replace the voice.
class BlueHourPanel final : public juce::Component,
                            public juce::FileDragAndDropTarget
{
public:
    explicit BlueHourPanel (SingProcessor&);
    ~BlueHourPanel() override;

    // Called once per display frame. `seconds` is the animation clock.
    void frame (double seconds, bool animate);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    class PresetName;
    class VenusButton;
    class DropButton;
    class Veil;

    void showPresetMenu();
    void updatePresetDisplay();
    void setBypassed (bool);
    void setLoopMode (int);
    void refreshVoice();
    void chooseVoiceFile();
    void loadVoice (const juce::File&);
    void showNote (const juce::String&, double seconds = 5.0);
    static bool isAudioFile (const juce::String& path);

    SingProcessor& processor;
    PresetManager& presets;
    ui::BlueHourSky sky;

    // Header
    ui::ArrowButton previous { "Previous preset", true }, next { "Next preset", false };
    std::unique_ptr<PresetName> presetName;
    ui::HeaderButton abButton { "A / B" }, saveButton { "SAVE" };
    juce::Rectangle<float> presetFrame;

    // The voice
    std::unique_ptr<DropButton> dropButton;
    ui::VoiceHorizon horizon;
    VoiceInfo voice;
    int lastVoiceChange = -1;
    bool loading = false, fileDragging = false;
    std::unique_ptr<juce::FileChooser> chooser;

    // The loop row
    std::array<std::unique_ptr<ui::PillToggle>, 3> loopModes;
    std::unique_ptr<juce::ParameterAttachment> loopModeAttachment;
    ui::TextDragger loopStart { "Start" }, loopLength { "Length" }, loopFade { "Fade" };

    // Knobs
    enum KnobId { attack, release, stretch, spread, formant, air, space, numKnobs };
    std::array<ui::Knob, numKnobs> knobs;

    // Bottom row
    ui::PillToggle hold { "HOLD" }, octDown { juce::CharPointer_UTF8 ("\xe2\x88\x92" "1 OCT") }, octUp { "+1 OCT" }, tape { "TAPE" };
    ui::TextDragger voices { "Voices", true }, glide { "Glide", true }, output { "Output", true };

    ui::SaveOverlay saveOverlay;
    std::unique_ptr<Veil> veil;

    // The evening star is the bypass switch.
    std::unique_ptr<VenusButton> venusButton;
    std::unique_ptr<juce::ParameterAttachment> bypassAttachment;
    bool bypassed = false;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    int lastPresetChange = -1;
    juce::String lastPresetText;
    bool lastAB = false;
    double lastPresetCheck = 0.0;
    juce::String presetNote;
    double presetNoteUntil = 0.0;

    // A short message under the voice's name, like "That file couldn't be read".
    juce::String voiceNote;
    juce::String lastRowText;
    double voiceNoteUntil = 0.0;

    bool motion = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BlueHourPanel)
};

class SingEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SingEditor (SingProcessor&);
    ~SingEditor() override;

    void resized() override;

    // Draws the panel at a given animation time. For the snapshot tool; with animate
    // false it draws the still picture shown when Reduce Motion is on.
    void renderFrame (double seconds, bool animate = true) { panel.frame (seconds, animate); }

private:
    void onFrame (double timestamp);

    SingProcessor& singProcessor;
    ui::BlueHourLookAndFeel lookAndFeel;
    BlueHourPanel panel;
    juce::TooltipWindow tooltips { this, 800 };
    juce::VBlankAttachment vblank;

    double lastFrame = -1.0, animationTime = 0.0, lastMotionCheck = -10.0;
    bool motion = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingEditor)
};

} // namespace sing
