#pragma once

#include "Parameters.h"
#include "PresetManager.h"
#include "VoiceLibrary.h"
#include "dsp/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

namespace sing
{

class SingProcessor final : public juce::AudioProcessor
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
    double getTailLengthSeconds() const override { return 30.0; }
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

    // ---- The voice (message thread) ----------------------------------------------------
    // What the panel shows about the current voice.
    VoiceInfo getVoiceInfo() const;
    // A peak outline of the whole voice for the waveform, values 0..1 (kWaveformPoints long).
    std::vector<float> getWaveform() const;
    static constexpr int kWaveformPoints = 480;
    // Bumped every time a new voice is ready, so the panel knows to redraw.
    int getVoiceChangeCount() const noexcept { return voiceChanges.load(); }
    // True while a new voice is being prepared in the background.
    bool isVoiceLoading() const noexcept { return library.isLoading(); }

    // Replaces the voice with an audio file (WAV, AIFF, FLAC). Sets the loop to a sensible
    // stretch. Returns false with a message if the file can't be used.
    bool loadVoiceFile (const juce::File&, juce::String& error);

    // Where each sounding note is in the voice right now, as fractions of the voice's
    // length (-1 = unused slot). Safe to read from the message thread.
    static constexpr int kMaxPlayheads = 16;
    std::array<float, kMaxPlayheads> getPlayheads() const noexcept;

    // The panel's size, remembered with the session.
    float getUiScale() const noexcept { return uiScale.load(); }
    void setUiScale (float s) noexcept { uiScale = s; }

private:
    dsp::EngineParams readParams() const noexcept;

    juce::AudioProcessorValueTreeState state;
    VoiceLibrary library;
    PresetManager presets { state, library };
    std::atomic<float> uiScale { 1.0f };
    std::atomic<int> voiceChanges { 0 };
    dsp::Engine engine;
    // Frees voices the audio thread has finished with (message thread).
    juce::TimedCallback garbageTimer { [this] { engine.collectGarbage(); } };

    // Cached raw parameter pointers: lock-free reads on the audio thread.
    std::atomic<float>* loopStart = nullptr;
    std::atomic<float>* loopLength = nullptr;
    std::atomic<float>* loopFade = nullptr;
    std::atomic<float>* loopMode = nullptr;
    std::atomic<float>* attack = nullptr;
    std::atomic<float>* release = nullptr;
    std::atomic<float>* stretch = nullptr;
    std::atomic<float>* spread = nullptr;
    std::atomic<float>* formant = nullptr;
    std::atomic<float>* air = nullptr;
    std::atomic<float>* space = nullptr;
    std::atomic<float>* hold = nullptr;
    std::atomic<float>* octDown = nullptr;
    std::atomic<float>* octUp = nullptr;
    std::atomic<float>* tape = nullptr;
    std::atomic<float>* voices = nullptr;
    std::atomic<float>* glide = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* bypass = nullptr;
    juce::AudioProcessorParameter* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingProcessor)
};

} // namespace sing
