#pragma once

#include "LoopSource.h"

#include <memory>

namespace sing
{

// Parameters every Stretch engine understands. Engine-specific settings live with the engine
// (see GranularEngine::Settings).
struct StretchParameters
{
    // How fast the read position moves through the loop. 1 plays at the recorded speed,
    // 0.25 is four times slower, 0 is the frozen "held breath".
    float speed = 1.0f;

    // Pitch of the note relative to the voice's root (2 is an octave up). The engine adds the
    // sample rate conversion itself.
    float pitchRatio = 1.0f;

    // Hold: freezes the read position, so the pad sustains for as long as the key is held,
    // whatever Stretch is set to.
    bool hold = false;
};

// One note's Stretch engine. Each playing key owns one; the synth voice adds the envelope,
// Spread, Formant and the rest around it.
//
// Threading: prepare() allocates and runs off the audio thread. Everything else is real-time
// safe: no allocation, no locks.
class StretchEngine
{
public:
    virtual ~StretchEngine() = default;

    virtual StretchMode mode() const noexcept = 0;

    virtual void prepare (double hostSampleRate, int maxBlockSize) = 0;

    // Starts a note at a play buffer position (0 is the loop start).
    virtual void start (const LoopSource& source, double bufferPosition = 0.0) noexcept = 0;

    // Swaps in a rebuilt loop while the note plays (new Start, Length, Fade or Direction).
    // The engine may keep reading the old source for a moment to avoid a click, so keep it
    // alive until isUsing (oldSource) returns false.
    virtual void setSource (const LoopSource& source) noexcept = 0;

    virtual void setParameters (const StretchParameters& parameters) noexcept = 0;

    // Adds numSamples of output to left and right.
    virtual void process (float* left, float* right, int numSamples) noexcept = 0;

    // Silences the engine at once. Fade-outs belong to the voice's Release envelope.
    virtual void stop() noexcept = 0;

    virtual bool isActive() const noexcept = 0;
    virtual bool isUsing (const LoopSource& source) const noexcept = 0;

    // Read position as a sample position in the original voice, for the waveform's play light.
    virtual double playPosition() const noexcept = 0;
};

std::unique_ptr<StretchEngine> createStretchEngine (StretchMode mode);

// Builds the analysis an engine needs from a freshly built loop, off the audio thread.
// Returns nullptr when the engine needs none (granular).
std::shared_ptr<const SourceAnalysis> analyseSource (StretchMode mode, const LoopSource& source);

} // namespace sing
