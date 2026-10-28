#pragma once

#include "Bloom/BloomVoice.h"
#include "Stretch/GranularEngine.h"
#include "Voice/Ensemble.h"
#include "Voice/FormantShifter.h"
#include "Voice/StftPitchShifter.h"
#include "Voice/Movement.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sing
{

// How a note moves to its key.
enum class PitchMode
{
    formant,   // Grain: resample, then FormantShifter. Bloom: its own spectral shift
    stft,      // Grain or Bloom at the recorded pitch, then StftPitchShifter on the mix
};

// Settings shared by every note, read from the plugin's parameters once per block.
struct VoiceSettings
{
    float attackSeconds = 1.5f;      // up to 20 s
    float releaseSeconds = 6.0f;     // up to 30 s: time to fall 60 dB
    float stretch = 0.6f;            // the Stretch knob, 0 to 1
    float spread = 0.3f;             // the Spread knob, 0 to 1
    float formantSemitones = 0.0f;   // -12 to +12; 0 keeps the recorded vowel on every key
    // On: the vowel stays as recorded on every key (formant-preserving pitch shift). Off:
    // plain pitch shifting, so the vowel moves with the key (higher keys sound smaller).
    bool keepVowel = true;
    // Which pitch shifter notes use. In STFT mode Grain and Bloom both play at the root and
    // the STFT shifter moves them to the key. A note keeps its mode until it ends.
    PitchMode pitchMode = PitchMode::formant;
    bool hold = false;
    float glideSeconds = 0.0f;
    float pitchBendSemitones = 0.0f;

    // Bloom's spectral controls (granular ignores them).
    float shiftHz = 0.0f;            // Shift: every partial moved by this many Hz, -500 to +500
    float smear = 0.0f;              // Smear: frequency spread, 0 to 1
    float tone = 0.0f;               // Tone: -1 breath and noise only, 0 as recorded, +1 harmonics only
    float onsets = 1.0f;             // Onsets: 0 smears attacks like the rest, 1 passes them at the recorded speed
    float harmonicsCents = 0.0f;     // Harmonics: 0 off; else the width either side of each of the key's harmonics
    int harmonicsCount = 16;         // ...and how many of them
    bool filter = false;             // Filter: keeps lowHz to highHz (cuts it out when lowHz is above highHz)
    float lowHz = 120.0f, highHz = 8000.0f;
    int filterSlope = 0;             // 0 sharp, 1 6 dB per octave, 2 12 dB per octave
    float compressor = 0.0f;         // Compressor: 0 off to 1, each frame toward the loop's typical level
    bool octaves = false;            // Octaves: the spectrum mixed with copies -2, -1, 0, +1, +1 and a fifth, +2 octaves away
    std::array<float, 6> octaveLevels { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };

    // Ensemble: singers on every note (0 or 1 is off, else 2 to 8), how far they wander in
    // cents, their vibrato and how wide they stand (0 to 1).
    int ensembleSingers = 0;
    float ensembleDetune = 10.0f, ensembleVibrato = 0.4f, ensembleWidth = 0.7f;
    // Halo: a frozen copy of the note under it. 0 is off; drift 0 to 1 lets the held moment
    // wander through the voice; octave -1, 0 or +1.
    float haloLevel = 0.0f, haloDrift = 0.0f;
    int haloOctave = 0;

    // The voice's root pitch. Unpitched voices (breaths) still map keys from rootHz, but the
    // formant shifter uses short frames and a fixed envelope width instead of the period.
    float rootHz = 261.63f;
    bool pitched = true;

    // Which engine new notes use. A note keeps its engine until it ends. A Bloom note needs
    // a loop with Bloom's analysis; without one it plays granular.
    StretchMode mode = StretchMode::granular;

    // Movement: the orbits of Stretch, Spread and Formant, and Wander (each note's reading
    // point moving inside the loop). Air and Space orbit after the notes, in the processor.
    MovementSettings movement;
};

// The gentle fade in and out of each note.
class Envelope
{
public:
    void prepare (double sampleRate) noexcept { rate = sampleRate; }
    void start (float attackSeconds) noexcept;
    void release (float releaseSeconds) noexcept;
    void kill() noexcept;                         // 5 ms fade, for a stolen voice
    void setTimes (float attackSeconds, float releaseSeconds) noexcept;

    // Fills gains with the next numSamples envelope values.
    void render (float* gains, int numSamples) noexcept;

    bool isIdle() const noexcept { return stage == Stage::idle; }
    bool isReleasing() const noexcept { return stage == Stage::release || stage == Stage::kill; }
    float level() const noexcept { return value; }

private:
    enum class Stage { idle, attack, sustain, release, kill };

    double rate = 48000.0;
    Stage stage = Stage::idle;
    float value = 0.0f;
    float attackStep = 1.0f, releaseCoeff = 0.0f, killStep = 1.0f;
};

// One playing key. In granular mode, up to seven Spread copies, each a granular Stretch
// engine, panned across the stereo field, then one formant shifter on their mix. In Bloom mode, one
// BloomVoice that builds all the copies in one spectrum. Either is mixed under the envelope.
class SingVoice
{
public:
    static constexpr int maxCopies = 7;
    static constexpr int subBlock = 64;   // pitch, Spread and formant update this often

    void prepare (double sampleRate, int maxBlockSize);

    // glideFromSemitones: where a glide starts, in semitones from the root; ignored when
    // Glide is 0.
    void start (const LoopSource& source, int note, float velocity, const VoiceSettings& settings,
                bool glide, float glideFromSemitones, uint32_t seed) noexcept;
    void release() noexcept;
    void kill() noexcept;
    void stop() noexcept;

    void setSource (const LoopSource& source) noexcept;
    void setSettings (const VoiceSettings& settings) noexcept;

    // Where the movement is, in cycles of its period, at the start of the next render() call.
    // The synth keeps one clock for every note (so they can follow the song's bars).
    void setMoveClock (double cycles) noexcept { moveClock = cycles; }

    // Where Stretch, Spread and Formant have moved to on this note, each 0 to 1 of its
    // control's range, and Wander's offset as a share of the loop (-0.5 to 0.5).
    struct Moved { float stretch = 0.0f, spread = 0.0f, formant = 0.5f, wander = 0.0f; };
    Moved getMoved() const noexcept;

    // Adds numSamples (any length) of the note to left and right.
    void render (float* left, float* right, int numSamples) noexcept;

    bool isActive() const noexcept { return active; }
    bool isKilled() const noexcept { return killed; }
    bool isReleased() const noexcept { return released; }
    int getNote() const noexcept { return note; }
    uint64_t getStartOrder() const noexcept { return order; }
    void setStartOrder (uint64_t o) noexcept { order = o; }
    float getLevel() const noexcept { return envelope.level(); }
    float currentSemitones() const noexcept { return semitones; }

    bool isUsing (const LoopSource& source) const noexcept;
    double playPosition() const noexcept;   // sample position in the voice, first copy

    // Where each running Spread copy is reading, for the waveform's play lights. weight is the
    // copy's place in the Spread (0 to 1, before the mix is normalised), pan is -1 to 1.
    // zoneSamples is the width of voice the copy is reading, centred on position; 0 means the
    // granular read zone, which the panel works out from the Stretch knob.
    struct CopyLight { double position; float weight, pan, pitchRatio, zoneSamples; };
    int getCopyLights (CopyLight* lights, int maxCount) const noexcept;   // centre copy first

    bool isBloom() const noexcept { return bloomNote; }
    // For a Bloom note, how wide a stretch of the voice each frame hears (voice samples),
    // centred on playPosition(). 0 for granular notes.
    double bloomWindowSamples() const noexcept { return bloomNote ? bloom.windowSamples() : 0.0; }

    // How a copy sits in the Spread: its detune and pan, and its share of the level.
    struct CopyShape { float cents, pan, weight; };
    static CopyShape copyShape (int copy, float spread) noexcept;

private:
    struct Copy
    {
        GranularEngine engine;
        bool running = false;
        float gain = 0.0f;   // smoothed share of the mix
        std::vector<float> left, right;
    };
    // One formant shifter for the whole note, after the Spread copies are mixed: the copies
    // share the note's formant setting, so they don't each need one.
    FormantShifter shifter;
    // Ensemble's singers, on the mixed note.
    Ensemble ensemble;
    // Halo: one frozen granular engine on the moment the note began. In Grain mode it joins
    // the copies before the shifter (so it keeps the vowel like they do); in Bloom's formant
    // mode it has the note's otherwise idle formant shifter to itself.
    GranularEngine haloEngine;
    bool haloRunning = false;
    float haloGain = 0.0f;
    double haloAge = 0.0;
    std::vector<float> haloLeft, haloRight;
    void startHalo() noexcept;
    void renderHalo (int numSamples, bool preShifter) noexcept;
    float haloRatio() const noexcept;
    // Ensemble on the mixed note, then the envelope.
    void finishSubBlock (float* left, float* right, int numSamples) noexcept;
    // The STFT pitch mode's shifter, used instead for notes started in that mode.
    StftPitchShifter stft;
    bool stftNote = false;
    void primeShifter() noexcept;
    void primeBloomShifter() noexcept;
    float rootPeriod() const noexcept;
    struct StftShift { float factor = 1.0f; bool envelope = false; float envelopeRatio = 1.0f; };
    StftShift stftShift() const noexcept;


    void renderSubBlock (float* left, float* right, int numSamples) noexcept;
    void applyEnvelope (float* left, float* right, int numSamples) noexcept;
    // Fills each copy's shape for a Spread setting; returns the gain that keeps their
    // total power at 1.
    static float copyShapes (float spread, std::array<CopyShape, (size_t) maxCopies>& shapes) noexcept;
    BloomVoice::Parameters bloomParameters (const std::array<CopyShape, (size_t) maxCopies>& shapes, float norm) const noexcept;
    void startCopy (int index) noexcept;
    float targetSemitones() const noexcept;
    // A Bloom note's frames are built at this pitch: the key, without glide or bend. Each
    // frame is long, so a pitch that moves inside the frames would move in steps; the rest
    // of the pitch (glide, bend) is a smooth change of playback speed on Bloom's output.
    float bloomAnchorSemitones() const noexcept;
    float bloomVarispeed() const noexcept;
    void renderBloomVarispeed (int numSamples) noexcept;
    float pitchRatio (int copy) const noexcept;
    // The rate a copy's grains play at: the whole pitch in Formant mode, only the copy's
    // Spread detune in STFT mode (the shifter does the rest).
    float grainRatio (int copy) const noexcept;
    float periodFor (const GranularEngine& engine, float ratio) const noexcept;
    float formantRatioNow() const noexcept;
    // Works out this sub-block's settings (voiceSettings) from the knobs (baseSettings) and
    // the movement, and moves the reading point for Wander. jump: no easing (note start).
    void updateMovement (int numSamples, bool jump) noexcept;
    void applyWander (bool jump) noexcept;
    void refreshMoved() noexcept;   // voiceSettings from baseSettings and the eased offsets

    double hostRate = 48000.0;
    std::array<Copy, (size_t) maxCopies> copies;
    BloomVoice bloom;
    bool bloomNote = false;
    std::vector<float> envelopeGains, mixLeft, mixRight;
    // Bloom's output waiting to be read at the varispeed rate, and where the reading is.
    static constexpr float maxVarispeed = 8.0f;
    std::vector<float> varispeedLeft, varispeedRight;
    int varispeedFill = 0;
    double varispeedRead = 1.0;
    float varispeedRate = 1.0f;
    Envelope envelope;
    VoiceSettings baseSettings;    // as the knobs are set
    VoiceSettings voiceSettings;   // as this sub-block plays them, moved by the orbits
    const LoopSource* source = nullptr;

    // Movement.
    double moveClock = 0.0;        // cycles, at the next sub-block
    double movePhase = 0.0;        // this note's own place in the wave (Each note)
    uint32_t moveSeed = 1;         // this note's own drift (Each note)
    std::array<float, numMoveTargets> moveOffset {};   // eased: moved value minus the knob
    double wanderApplied = 0.0;    // how far Wander has moved the reading point, loop samples

    bool active = false, killed = false, released = false;
    int note = 60;
    float velocityGain = 1.0f;
    float semitones = 0.0f;     // current pitch, semitones from the root
    float glideRate = 0.0f;     // semitones per sample while gliding
    bool gliding = false;
    uint64_t order = 0;
    uint32_t seedBase = 1;
};

} // namespace sing
