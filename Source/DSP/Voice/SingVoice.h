#pragma once

#include "Stretch/GranularEngine.h"
#include "Voice/FormantShifter.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sing
{

// Settings shared by every note, read from the plugin's parameters once per block.
struct VoiceSettings
{
    float attackSeconds = 1.5f;      // up to 20 s
    float releaseSeconds = 6.0f;     // up to 30 s: time to fall 60 dB
    float stretch = 0.6f;            // the Stretch knob, 0 to 1
    float spread = 0.3f;             // the Spread knob, 0 to 1
    float formantSemitones = 0.0f;   // -12 to +12; 0 keeps the recorded vowel on every key
    bool hold = false;
    float glideSeconds = 0.0f;
    float pitchBendSemitones = 0.0f;

    // The voice's root pitch. Unpitched voices (breaths) still map keys from rootHz, but the
    // formant shifter uses short fixed grains instead of following the period.
    float rootHz = 261.63f;
    bool pitched = true;
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

// One playing key. Up to seven Spread copies, each a granular Stretch engine followed by a
// formant shifter, panned across the stereo field and mixed under the envelope.
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
    void setSettings (const VoiceSettings& settings) noexcept { voiceSettings = settings; }

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

    // How a copy sits in the Spread: its detune and pan, and its share of the level.
    struct CopyShape { float cents, pan, weight; };
    static CopyShape copyShape (int copy, float spread) noexcept;

private:
    struct Copy
    {
        GranularEngine engine;
        FormantShifter shifter;
        bool running = false;
        float gain = 0.0f;   // smoothed share of the mix
        std::vector<float> left, right;
    };

    void renderSubBlock (float* left, float* right, int numSamples) noexcept;
    void startCopy (int index) noexcept;
    float targetSemitones() const noexcept;
    float pitchRatio (int copy) const noexcept;
    float periodFor (const GranularEngine& engine, float ratio) const noexcept;
    float formantRatioNow() const noexcept;

    double hostRate = 48000.0;
    std::array<Copy, (size_t) maxCopies> copies;
    std::vector<float> envelopeGains, mixLeft, mixRight;
    Envelope envelope;
    VoiceSettings voiceSettings;
    const LoopSource* source = nullptr;

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
