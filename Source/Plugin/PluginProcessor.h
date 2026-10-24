#pragma once

#include "Parameters.h"
#include "PresetManager.h"
#include "VoiceData.h"

#include "Voice/SingSynth.h"
#include "Voice/SourceExchange.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace sing
{

class SingProcessor final : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener,
                            private juce::Timer
{
public:
    SingProcessor();
    ~SingProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    // Hosts see the factory presets as programs.
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    PresetManager& getPresets() noexcept { return presets; }

    // The voice. Message thread (the getter is safe anywhere).
    VoiceData::Ptr getVoice() const;
    void setVoice (VoiceData::Ptr);
    bool loadVoiceFile (const juce::File&, juce::String& error);

    // Where the sounding notes are in the voice, for the play lights: one light per Spread copy
    // of the newest maxLightNotes notes, newest note first and its centre copy first. position
    // is 0 to 1 across the trimmed waveform; note is -1 for an unused light.
    static constexpr int maxLightNotes = 4;
    static constexpr int maxLights = maxLightNotes * SingVoice::maxCopies;
    struct Light
    {
        float position = -1.0f, level = 0.0f, weight = 0.0f, pan = 0.0f, pitchRatio = 1.0f;
        float zone = 0.0f;   // read zone width, 0 to 1 of the waveform; 0 for the granular zone
        int note = -1, copy = 0;
        uint32_t noteId = 0;
    };
    std::array<Light, maxLights> getLights() const noexcept;

    // The panel's size, remembered with the session.
    float getUiScale() const noexcept { return uiScale.load(); }
    void setUiScale (float s) noexcept { uiScale = s; }

private:
    void parameterChanged (const juce::String& id, float) override;
    void timerCallback() override;
    void rebuildLoop();
    SynthSettings readSettings() const noexcept;
    void handleMidi (const juce::MidiMessage&) noexcept;

    juce::AudioProcessorValueTreeState state;
    VoiceData::Ptr voice;
    mutable juce::CriticalSection voiceLock;
    PresetManager presets;
    std::atomic<float> uiScale { 1.0f };

    SourceExchange exchange;
    SingSynth synth;
    std::atomic<bool> loopDirty { true };
    std::atomic<int> voiceTrimStart { 0 }, voiceTrimLength { 1 };
    std::atomic<float> rootHz { 261.63f };
    std::atomic<bool> rootPitched { true };
    std::atomic<bool> panicPending { false };

    // Written at the end of each block, read by the panel at 30 fps. A light can tear between
    // fields for one frame, which can't be seen.
    struct SharedLight
    {
        std::atomic<float> position { -1.0f }, level { 0.0f }, weight { 0.0f }, pan { 0.0f }, pitchRatio { 1.0f }, zone { 0.0f };
        std::atomic<int> note { -1 }, copy { 0 };
        std::atomic<uint32_t> noteId { 0 };
    };
    std::array<SharedLight, maxLights> lights;

    // Cached raw parameter pointers: lock-free reads on the audio thread.
    std::atomic<float>* attack = nullptr;
    std::atomic<float>* release = nullptr;
    std::atomic<float>* stretch = nullptr;
    std::atomic<float>* spread = nullptr;
    std::atomic<float>* formant = nullptr;
    std::atomic<float>* hold = nullptr;
    std::atomic<float>* voices = nullptr;
    std::atomic<float>* glide = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* bypass = nullptr;
    juce::AudioProcessorParameter* bypassParam = nullptr;

    // Output level and bypass ramp smoothly.
    juce::SmoothedValue<float> outputGain;
    float bypassMix = 1.0f, bypassStep = 0.0f;   // 1 = playing, 0 = bypassed (silent)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingProcessor)
};

} // namespace sing
