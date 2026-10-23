#pragma once

#include "VoiceData.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>

namespace sing
{

// Factory and user presets, plus the A/B compare slots. Adapted from Overcast's.
//
// A preset is its knob settings (real values by parameter ID, so they survive a range change)
// plus its voice, stored inside the preset so it sounds the same on any machine. User presets
// are XML files in ~/Library/Audio/Presets/Overcast Audio/Sing.
//
// Loading, saving and A/B run on the message thread.
class PresetManager final
{
public:
    // getVoice / setVoice reach the processor's current voice.
    PresetManager (juce::AudioProcessorValueTreeState&,
                   std::function<VoiceData::Ptr()> getVoice,
                   std::function<void (VoiceData::Ptr)> setVoice);

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

    // The voice the first factory preset uses: what a fresh instance plays.
    static VoiceData::Ptr defaultVoice();

    // Bumped whenever the preset list, the current preset or the A/B slot changes.
    int getChangeCount() const noexcept { return changeCount.load(); }

private:
    using Values = std::map<juce::String, float>;

    struct Preset
    {
        juce::String name;
        Values values;
        juce::File file;                       // empty for factory presets
        std::optional<PlaceholderVowel> vowel; // factory presets' stand-in voice
    };

    struct Snapshot
    {
        Values values;
        VoiceData::Ptr voice;
    };

    Values captureValues() const;
    void applyValues (const Values&);
    void setLoaded (Values, juce::int64 voiceId);
    static bool readPresetFile (const juce::File&, Preset&, bool withVoice, VoiceData::Ptr* voiceOut);

    juce::AudioProcessorValueTreeState& state;
    std::function<VoiceData::Ptr()> getVoice;
    std::function<void (VoiceData::Ptr)> setVoice;
    std::vector<Preset> factory;
    std::vector<Preset> user;
    mutable juce::CriticalSection listLock;

    std::atomic<int> current { -1 };
    std::atomic<int> changeCount { 0 };

    bool onB = false;
    Snapshot slotA, slotB;
    Values loaded;              // what the current preset set, to tell when it's been changed
    juce::int64 loadedVoice = 0;
};

} // namespace sing
