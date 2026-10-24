#pragma once

#include "Voice/SingVoice.h"

#include <array>
#include <cstdint>

namespace sing
{

struct SynthSettings
{
    VoiceSettings voice;
    int polyphony = 8;   // the Voices setting, 1 to 16
};

// The polyphonic part of Sing: MIDI notes in, mixed voices out.
//
// Voices beyond the Voices setting are stolen with a 5 ms fade (a released note first, else
// the oldest), so a stolen note never clicks. Sustain pedal holds released notes. Glide
// slides each new note from the last one played.
//
// Everything here runs on the audio thread and is real-time safe after prepare().
class SingSynth
{
public:
    static constexpr int maxPolyphony = 16;
    static constexpr int voicePool = maxPolyphony + 4;   // spare voices for stolen ones fading out
    static constexpr int maxOldSources = 32;

    void prepare (double sampleRate, int maxBlockSize);

    // Switches to a rebuilt loop. Notes playing move over to it smoothly.
    void setSource (const LoopSource* source) noexcept;
    const LoopSource* getSource() const noexcept { return source; }

    // Hands back loops no note uses any more, so they can be freed. Call once per block.
    template <typename RetireFn>
    void releaseUnusedSources (RetireFn&& retire) noexcept
    {
        for (int i = numOld; --i >= 0;)
            if (! isUsing (*oldSources[(size_t) i]))
            {
                retire (oldSources[(size_t) i]);
                oldSources[(size_t) i] = oldSources[(size_t) --numOld];
            }
    }

    // Keeps the current pitch bend: that comes from setPitchBend.
    void setSettings (const SynthSettings& settings) noexcept;

    void noteOn (int note, float velocity) noexcept;
    void noteOff (int note) noexcept;
    void setSustain (bool down) noexcept;
    void setPitchBend (float semitones) noexcept;
    void allNotesOff() noexcept;     // release everything
    void allSoundOff() noexcept;     // silence at once

    // Adds numSamples of all voices to left and right.
    void render (float* left, float* right, int numSamples) noexcept;

    int activeVoiceCount() const noexcept;
    bool isUsing (const LoopSource& source) const noexcept;

    // Sample positions in the voice of the sounding notes, newest first, for the waveform.
    int getPlayPositions (double* positions, float* levels, int maxCount) const noexcept;

    // Every Spread copy of the newest maxNotes sounding notes, newest note first and its centre
    // copy first. note counts the notes from 0 (newest); noteId tells notes apart across calls.
    struct PlayLight
    {
        double position;   // sample position in the voice
        float level;       // the note's envelope, 0 to 1
        float weight;      // the copy's place in the Spread, 0 to 1
        float pan;         // -1 to 1
        float pitchRatio;
        float zoneSamples; // width of voice being read, centred; 0 for the granular zone
        int note, copy;
        uint32_t noteId;
    };
    int getPlayLights (PlayLight* lights, int maxNotes, int maxCount) const noexcept;

    const SingVoice& getVoice (int index) const noexcept { return voices[(size_t) index]; }

private:
    SingVoice* findVoiceToStart() noexcept;

    std::array<SingVoice, (size_t) voicePool> voices;
    std::array<bool, (size_t) voicePool> sustained {};
    SynthSettings settings;
    const LoopSource* source = nullptr;
    std::array<const LoopSource*, (size_t) maxOldSources> oldSources {};
    int numOld = 0;

    bool sustainDown = false;
    bool hasLastNote = false;
    float lastNoteSemitones = 0.0f;
    uint64_t nextOrder = 1;
    uint32_t nextSeed = 0x2545F491u;
};

} // namespace sing
