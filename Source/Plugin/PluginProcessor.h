#pragma once

#include "Parameters.h"
#include "PresetManager.h"
#include "VoiceData.h"

#include "Effects/Air.h"
#include "Effects/Binaural.h"
#include "Effects/Space.h"
#include "Voice/SingSynth.h"
#include "Voice/SourceExchange.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <optional>
#include <vector>

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

    // Where each orbit has moved its knob to right now (0 to 1 of the knob's range), for the
    // lights that circle the knobs: the newest note's for Stretch, Spread and Formant. -1
    // when that knob isn't moving (no orbit, or no note sounding).
    std::array<float, numMoveTargets> getOrbitPositions() const noexcept;

    // The panel's size, remembered with the session.
    float getUiScale() const noexcept { return uiScale.load(); }
    void setUiScale (float s) noexcept { uiScale = s; }

private:
    void parameterChanged (const juce::String& id, float) override;
    void timerCallback() override;
    // Message thread: asks the loop builder for a loop with the current settings.
    void rebuildLoop();
    SynthSettings readSettings() const noexcept;
    void handleMidi (const juce::MidiMessage&) noexcept;

    juce::AudioProcessorValueTreeState state;
    VoiceData::Ptr voice;
    mutable juce::CriticalSection voiceLock;
    PresetManager presets;
    std::atomic<float> uiScale { 1.0f };

    SourceExchange exchange;

    // Builds loops off the message thread (Bloom's analysis of a long loop takes a moment)
    // and publishes them to the exchange. Only this thread publishes or collects garbage.
    class LoopBuilder final : public juce::Thread
    {
    public:
        explicit LoopBuilder (SingProcessor& p) : juce::Thread ("Sing loop builder"), owner (p) {}
        ~LoopBuilder() override { stopThread (4000); }

        struct Request
        {
            VoiceData::Ptr voice;
            LoopSettings loop;
            int trimStart = 0, trimLength = 1;
        };
        void request (Request r);
        void run() override;

    private:
        SingProcessor& owner;
        juce::CriticalSection lock;
        std::optional<Request> pending;
        juce::WaitableEvent wake;
    };
    LoopBuilder builder { *this };

    SingSynth synth;
    Air air;
    Space space;
    Binaural binaural;
    std::vector<float> monoRight;   // a right channel for the effects when the output is mono
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
    std::array<std::atomic<float>, numMoveTargets> orbitPositions {};

    MovementSettings readMovement (double sampleRate) const noexcept;
    void followSong (const MovementSettings&) noexcept;
    std::array<float, numMoveTargets> fxOffset {};

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
    std::atomic<float>* engine = nullptr;
    std::atomic<float>* airAmount = nullptr;
    std::atomic<float>* spaceAmount = nullptr;
    std::atomic<float>* shiftHz = nullptr;
    std::atomic<float>* smear = nullptr;
    std::atomic<float>* tone = nullptr;
    std::atomic<float>* keepVowel = nullptr;
    std::atomic<float>* pitchMode = nullptr;
    std::atomic<float>* onsetsOn = nullptr;
    std::atomic<float>* onsets = nullptr;
    std::atomic<float>* shiftOn = nullptr;
    std::atomic<float>* smearOn = nullptr;
    std::atomic<float>* toneOn = nullptr;
    std::atomic<float>* harmonicsOn = nullptr;
    std::atomic<float>* harmonicsWidth = nullptr;
    std::atomic<float>* harmonicsCount = nullptr;
    std::atomic<float>* filterOn = nullptr;
    std::atomic<float>* filterLow = nullptr;
    std::atomic<float>* filterHigh = nullptr;
    std::atomic<float>* filterSlope = nullptr;
    std::atomic<float>* compressorOn = nullptr;
    std::atomic<float>* compressorAmount = nullptr;
    std::atomic<float>* octavesOn = nullptr;
    std::atomic<float>* octaveDown2 = nullptr;
    std::atomic<float>* octaveDown1 = nullptr;
    std::atomic<float>* octaveRoot = nullptr;
    std::atomic<float>* octaveUp1 = nullptr;
    std::atomic<float>* octaveUp15 = nullptr;
    std::atomic<float>* octaveUp2 = nullptr;
    std::atomic<float>* binauralOn = nullptr;
    std::atomic<float>* binauralRate = nullptr;
    std::atomic<float>* binauralAmount = nullptr;
    std::atomic<float>* spaceFilterOn = nullptr;
    std::atomic<float>* spaceLow = nullptr;
    std::atomic<float>* spaceHigh = nullptr;
    std::atomic<float>* spaceDecay = nullptr;
    std::atomic<float>* spaceSize = nullptr;
    std::array<std::atomic<float>*, numMoveTargets> orbitAmounts {};
    std::atomic<float>* moveShape = nullptr;
    std::atomic<float>* moveSpeed = nullptr;
    std::atomic<float>* moveSync = nullptr;
    std::atomic<float>* moveBars = nullptr;
    std::atomic<float>* moveEach = nullptr;
    juce::AudioProcessorParameter* bypassParam = nullptr;
    double preparedRate = 0.0;

    // Output level and bypass ramp smoothly.
    juce::SmoothedValue<float> outputGain;
    float bypassMix = 1.0f, bypassStep = 0.0f;   // 1 = playing, 0 = bypassed (silent)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingProcessor)
};

} // namespace sing
