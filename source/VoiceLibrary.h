#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace sing
{

namespace dsp { class VoiceData; }

// What the panel shows about the voice.
struct VoiceInfo
{
    juce::String name;          // "Hollow Choir", or the dropped file's name
    juce::String description;   // one line, empty for dropped voices
    double lengthSeconds = 0.0;
    float rootNote = 60.0f;     // MIDI note the voice was sung at (fractional = cents)
    bool factory = true;
};

// Owns the current voice: where it came from (a factory voice made in code, or audio
// someone dropped in), and its saved form inside sessions and presets.
//
// Preparing a voice (making or decoding it, finding its pitch, building the pitch
// engine's tables) runs on a background thread; onReady fires on the message thread
// with the result.
class VoiceLibrary final
{
public:
    VoiceLibrary();
    ~VoiceLibrary();

    // The factory voices, by ID.
    static juce::StringArray factoryIds();
    static juce::String factoryName (const juce::String& id);

    // The saved form of a voice: a "VOICE" tree with either a factory ID or the audio.
    static juce::ValueTree factoryTree (const juce::String& id);
    juce::ValueTree getTree() const;

    // Starts preparing a voice. With wait = true it is ready when this returns
    // (used when a session is restored, so offline renders never start silent).
    void setTree (const juce::ValueTree&, bool wait = false);

    // Decodes an audio file into a user voice. Returns false with a message on failure.
    bool loadFile (const juce::File&, juce::String& error);

    VoiceInfo getInfo() const;
    std::vector<float> getWaveform (int points) const;
    bool isLoading() const noexcept { return jobsRunning.load() > 0; }

    // Called on the message thread (or inside setTree when wait = true) with the new
    // voice, ready to hand to the engine.
    std::function<void (std::unique_ptr<dsp::VoiceData>)> onReady;

    // The audio of a voice as mono floats, for tests and tools.
    struct Audio
    {
        std::vector<float> samples;
        double sampleRate = 48000.0;
    };
    static Audio renderFactory (const juce::String& id);

private:
    struct Prepared;
    void prepare (juce::ValueTree, int generation, bool sync);
    void finish (std::shared_ptr<Prepared>);

    mutable juce::CriticalSection lock;
    juce::ValueTree tree;            // the saved form of the current voice
    VoiceInfo info;
    std::vector<float> peaks;        // fine peak outline, resampled for the panel
    juce::ThreadPool pool { 1 };
    std::atomic<int> generation { 0 }, jobsRunning { 0 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace sing
