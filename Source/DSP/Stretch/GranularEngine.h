#pragma once

#include "StretchEngine.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sing
{

// Sing's granular Stretch engine.
//
// A read head moves through the loop at Stretch speed (down to a full stop). Grains are
// started at a steady rate; each one reads a short stretch of the loop near the head, at the
// note's pitch, shaped by a Hann window, and the grains are overlap-added. A random offset on
// each grain's start ("jitter") turns the overlapping copies into a soft cloud instead of a
// comb, which is what lets a frozen read head still breathe.
//
// Written from the published granular synthesis literature (Gabor, Roads' "Microsound",
// Truax). No third-party code.
class GranularEngine final : public StretchEngine
{
public:
    struct Settings
    {
        float grainSeconds = 0.14f;   // length of each grain
        int overlap = 4;              // grains sounding at once, 2 to 8
        float jitterSeconds = 0.02f;  // random offset of each grain's start around the read head
        float timingJitter = 0.15f;   // 0 to 1: randomness in when grains start, against flutter
        float stereoWidth = 0.35f;    // 0 to 1: random pan per grain
        bool phaseAlign = true;       // start grains in phase when the loop's pitch is known
    };

    static constexpr int maxGrains = 64;

    GranularEngine();

    StretchMode mode() const noexcept override { return StretchMode::granular; }

    void prepare (double hostSampleRate, int maxBlockSize) override;
    void start (const LoopSource& source, double bufferPosition = 0.0) noexcept override;
    void setSource (const LoopSource& source) noexcept override;
    void setParameters (const StretchParameters& parameters) noexcept override;
    void process (float* left, float* right, int numSamples) noexcept override;
    void stop() noexcept override;

    bool isActive() const noexcept override { return current != nullptr; }
    bool isUsing (const LoopSource& source) const noexcept override;
    double playPosition() const noexcept override;

    void setSettings (const Settings& newSettings) noexcept;
    const Settings& getSettings() const noexcept { return settings; }

    // Play buffer position of the read head, for tests and debugging.
    double headPosition() const noexcept { return head; }

    // The voice's pitch period (voice samples) where the read head is; 0 when unpitched there.
    float currentPeriod() const noexcept { return current != nullptr ? current->periodAt (head) : 0.0f; }
    int activeGrainCount() const noexcept;

    // Seeds the random generator, so renders and tests are repeatable.
    void setSeed (uint32_t seed) noexcept { rngState = seed != 0 ? seed : 0x9e3779b9u; }

private:
    struct Grain
    {
        const LoopSource* source = nullptr;
        double position = 0.0;   // play buffer position, in source samples
        int age = 0;             // output samples since the grain started
        int length = 0;          // output samples
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
    };

    void spawnGrain (int age) noexcept;
    void renderGrain (Grain& grain, float* left, float* right, int numSamples) noexcept;
    double alignToPhase (double target) const noexcept;
    float effectiveJitterSamples() const noexcept;   // in source samples
    float levelCompensation (float jitterSamples) const noexcept;
    float nextRandom() noexcept;                     // uniform in [-1, 1)

    Settings settings;
    StretchParameters params;

    double hostRate = 48000.0;
    const LoopSource* current = nullptr;
    double sourceScale = 1.0;    // source samples per output sample at pitch 1

    double head = 0.0;           // read position, play buffer samples
    double phaseReference = 0.0; // where an in-phase grain would read now
    float speed = 1.0f;          // smoothed read speed
    float rate = 1.0f;           // smoothed grain read rate, source samples per output sample
    float smoothingCoeff = 0.0f; // one-pole coefficient per control step

    int grainLength = 1;         // output samples
    double samplesToNextGrain = 0.0;

    std::array<Grain, (size_t) maxGrains> grains {};
    std::vector<float> window;   // Hann table, windowSize + 1 points
    uint32_t rngState = 0x9e3779b9u;
};

// The Stretch knob, 0 to 1, as the panel shows it. Low settings play near the recorded speed
// with small, tight grains. Turning it up slows the voice, lengthens the grains and widens the
// jitter, until the top of the range is the frozen "held breath".
struct StretchKnob
{
    static constexpr float freezeAt = 0.97f;   // the top 3% of travel is the held breath
    static constexpr float maxSlowdown = 256.0f;

    static float toSpeed (float knob) noexcept;
    static GranularEngine::Settings toGranularSettings (float knob,
                                                         GranularEngine::Settings base = {}) noexcept;
};

} // namespace sing
