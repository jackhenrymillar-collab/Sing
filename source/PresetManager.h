#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>

namespace sing
{

class VoiceLibrary;

// Factory and user presets, plus the A/B compare slots. Each preset stores its own voice.
//
// Presets store real parameter values by parameter ID, so they keep working if a range
// changes. User presets are files in ~/Library/Audio/Presets/Overcast Audio/Sing, with
// the voice's audio inside.
//
// Loading, saving and A/B run on the message thread.
class PresetManager final
{
public:
    PresetManager (juce::AudioProcessorValueTreeState&, VoiceLibrary&);

    int getNumPresets() const;
    int getNumFactoryPresets() const noexcept;
    juce::String getPresetName (int index) const;

    // -1 when no preset is loaded (the plugin's defaults).
    int getCurrentIndex() const noexcept { return current.load(); }
    juce::String getCurrentName() const;
    // True when the knobs or the voice no longer match the loaded preset. Message thread only.
    bool isModified() const;

    void loadPreset (int index);
    void loadNext();
    void loadPrevious();

    // Returns false if the file couldn't be written.
    bool saveUserPreset (const juce::String& name);
    void refreshUserPresets();
    static juce::File getUserPresetFolder();

    // A/B compare. The first switch copies A into B, so you can tweak B and compare.
    bool isOnB() const noexcept { return onB; }
    void toggleAB();

    // Session save and restore: remembers which preset was loaded.
    void writeToState (juce::ValueTree& stateCopy) const;
    void readFromState (const juce::ValueTree& restored);

    // A new voice was dropped in: the preset now counts as modified.
    void voiceReplaced() { voiceChanged = true; ++changeCount; }

    // Bumped whenever the preset list, the current preset or the A/B slot changes.
    int getChangeCount() const noexcept { return changeCount.load(); }

private:
    using Values = std::map<juce::String, float>;

    struct Preset
    {
        juce::String name;
        Values values;
        juce::ValueTree voice; // factory presets: the factory voice ID; user presets: the audio
        juce::File file;       // empty for factory presets
    };

    struct Slot
    {
        Values values;
        juce::ValueTree voice;
    };

    Values captureValues() const;
    void applyValues (const Values&);
    void setLoaded (Values);
    static bool readPresetFile (const juce::File&, Preset&);

    juce::AudioProcessorValueTreeState& state;
    VoiceLibrary& voices;
    std::vector<Preset> factory;
    std::vector<Preset> user;
    mutable juce::CriticalSection listLock;

    std::atomic<int> current { -1 };
    std::atomic<int> changeCount { 0 };
    bool voiceChanged = false;

    bool onB = false;
    Slot slotA, slotB;
    Values loaded; // what the current preset set, to tell when it's been changed
};

} // namespace sing
