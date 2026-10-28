#pragma once

#include "Voice/SingVoice.h"

#include <array>
#include <cstdint>

namespace sing
{

// The Harmoniser: extra notes in key with every key played.
struct HarmonySettings
{
    bool on = false;
    int key = 0;         // 0 C to 11 B
    int scale = 0;       // see harmonyScales
    int above = 0;       // 0 off, 1 third, 2 fourth, 3 fifth, 4 sixth, 5 octave: scale steps up
    int below = 0;       // the same, down
    float level = 0.7f;  // the harmony notes' velocity, as a share of the played note's
};

// The scales, as semitones from the key: major, natural minor, Dorian, Mixolydian, Lydian,
// major pentatonic.
constexpr int numHarmonyScales = 6;
constexpr int harmonyScaleSize[numHarmonyScales] { 7, 7, 7, 7, 7, 5 };
constexpr int harmonyScales[numHarmonyScales][7] { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 },
                                                    { 0, 2, 4, 5, 7, 9, 10 }, { 0, 2, 4, 6, 7, 9, 11 }, { 0, 2, 4, 7, 9, 0, 0 } };

// The harmony note for a played note: the note snapped to the nearest step of the scale,
// then interval steps up (or down, when up is false) that scale. interval as in
// HarmonySettings::above; 0 returns the note itself.
int harmonyNote (int note, int key, int scale, int interval, bool up) noexcept;

struct SynthSettings
{
    VoiceSettings voice;
    int polyphony = 8;   // the Voices setting, 1 to 16
    HarmonySettings harmony;
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

    // The movement's clock, in cycles of its period. It runs on by itself; set it to follow
    // the song (Sync). The effects after the notes read it too.
    void setMoveClock (double cycles) noexcept { moveCycles = cycles; }
    double getMoveClock() const noexcept { return moveCycles; }

    // The newest sounding note, or nullptr: the panel shows its orbits.
    const SingVoice* newestVoice() const noexcept;

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
    void startVoice (int note, int playedKey, float velocity, bool glide) noexcept;

    std::array<SingVoice, (size_t) voicePool> voices;
    std::array<bool, (size_t) voicePool> sustained {};
    // The key that started each voice: a harmony note's played key is not its own note.
    std::array<int, (size_t) voicePool> playedKeys {};
    SynthSettings settings;
    const LoopSource* source = nullptr;
    std::array<const LoopSource*, (size_t) maxOldSources> oldSources {};
    int numOld = 0;

    bool sustainDown = false;
    bool hasLastNote = false;
    float lastNoteSemitones = 0.0f;
    uint64_t nextOrder = 1;
    uint32_t nextSeed = 0x2545F491u;
    double moveCycles = 0.0;
};

} // namespace sing
