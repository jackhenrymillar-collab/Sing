#pragma once

#include "Voice/PlaceholderVoices.h"
#include "Voice/VoiceAnalysis.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <memory>
#include <optional>

namespace sing
{

// One voice: the recording a preset plays, with what Sing worked out about it.
// Immutable once made; editing it (a new root, trim on or off) makes a new one.
class VoiceData
{
public:
    using Ptr = std::shared_ptr<const VoiceData>;

    static constexpr double maxSeconds = 60.0;
    static constexpr int numPeaks = 240;

    // Reads a WAV, AIFF or FLAC file. Returns nullptr and an error message if it can't.
    static Ptr fromFile (const juce::File&, juce::String& error);

    // A generated stand-in for a factory voice.
    static Ptr fromPlaceholder (PlaceholderVowel);

    // Builds a voice from audio and analyses it (root pitch, silence trim, waveform).
    static Ptr fromAudio (juce::AudioBuffer<float> audio, double sampleRate, const juce::String& name,
                          const juce::String& description);

    // Saved with sessions and user presets. Factory placeholders store only their id.
    juce::ValueTree toValueTree() const;
    static Ptr fromValueTree (const juce::ValueTree&);

    // Copies with a setting overridden.
    Ptr withRootOverride (std::optional<int> midiNote) const;
    Ptr withTrim (bool trimSilence) const;

    const juce::AudioBuffer<float>& getAudio() const noexcept { return audio; }
    double getSampleRate() const noexcept { return sampleRate; }
    const juce::String& getName() const noexcept { return name; }
    const juce::String& getDescription() const noexcept { return description; }

    // The part of the recording that plays: the whole of it, or with silence trimmed.
    int trimStart() const noexcept { return trimSilence ? audibleStart : 0; }
    int trimEnd() const noexcept { return trimSilence ? audibleEnd : audio.getNumSamples(); }
    int trimLength() const noexcept { return trimEnd() - trimStart(); }
    double trimSeconds() const noexcept { return (double) trimLength() / sampleRate; }
    bool isTrimmed() const noexcept { return trimSilence; }

    // The root: the override if one is set, else the detected pitch, else middle C.
    float rootHz() const noexcept;
    bool isPitched() const noexcept { return rootOverride.has_value() || detectedHz > 0.0f; }
    float getDetectedHz() const noexcept { return detectedHz; }

    // The voice's pitch over time (for voices that move between notes).
    std::shared_ptr<const PitchTrack> getPitchTrack() const noexcept { return pitchTrack; }
    std::optional<int> getRootOverride() const noexcept { return rootOverride; }

    // Peak levels across the trimmed voice, 0 to 1, for drawing.
    const std::array<float, numPeaks>& getPeaks() const noexcept { return peaks; }

    // Identity for "has the voice changed" checks.
    juce::int64 getId() const noexcept { return id; }

private:
    VoiceData() = default;
    static Ptr makePlaceholder (PlaceholderVowel);
    void analyse();
    void computePeaks();

    juce::AudioBuffer<float> audio;
    double sampleRate = 48000.0;
    juce::String name, description;
    std::optional<PlaceholderVowel> placeholder;

    float detectedHz = 0.0f;
    std::shared_ptr<const PitchTrack> pitchTrack;
    std::optional<int> rootOverride;
    bool trimSilence = true;
    int audibleStart = 0, audibleEnd = 0;
    std::array<float, numPeaks> peaks {};
    juce::int64 id = 0;
};

// Note name for the panel, like "A3".
juce::String noteName (int midiNote);

} // namespace sing
