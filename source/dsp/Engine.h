#pragma once

#include "Loop.h"

#include <array>
#include <memory>

namespace sing::dsp
{

class VoiceData;

struct EngineParams
{
    float loopStart = 0.3f, loopLength = 0.4f;  // fractions of the voice
    float loopFade = 0.3f;                      // 0..1: each fade is this share of half the loop
    LoopMode loopMode = LoopMode::forward;
    float attack = 1.0f, release = 5.0f;        // seconds
    float stretch = 0.0f, spread = 0.3f;        // 0..1
    float formant = 0.0f;                       // semitones
    float air = 2.0f;                           // dB
    float space = 0.5f;                         // 0..1
    bool hold = false, octDown = false, octUp = false, tape = false;
    int voices = 8;
    float glideMs = 0.0f;
    float outputDb = 0.0f;
    bool bypass = false;
};

// The whole instrument: notes, pad tools and the effects after them. Plain C++, no JUCE.
//
// Notes render at twice the host rate (see Sinc.h for why), then a steep half-band filter
// brings them down, then Formant, Air and Space run at the host rate.
//
// Threading: prepare, process and the note calls run on the audio thread. submitVoice
// and collectGarbage run on the message thread; voices are handed over lock-free.
class Engine
{
public:
    Engine();
    ~Engine();

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    int getLatencySamples() const noexcept;
    void setParams (const EngineParams&) noexcept;

    void noteOn (int note, float velocity) noexcept;  // velocity 0..1
    void noteOff (int note) noexcept;
    void sustainPedal (bool down) noexcept;
    void pitchBend (float semitones) noexcept;
    void allNotesOff() noexcept;

    // Renders (replaces) numSamples of output.
    void process (float* left, float* right, int numSamples) noexcept;

    // Message thread: hand over a new voice (the engine owns it from here). Notes playing
    // the old voice fade out quickly; new notes use the new one.
    void submitVoice (std::unique_ptr<VoiceData>);
    // Message thread: frees voices the audio thread has finished with.
    void collectGarbage();
    // The voice new notes will use (or the one waiting to be picked up). Message thread.
    bool hasVoice() const noexcept;

    // Positions of sounding notes as fractions of the voice (-1 = none). Any thread.
    static constexpr int kMaxPlayheads = 16;
    void getPlayheads (std::array<float, kMaxPlayheads>&) const noexcept;

    // Number of notes sounding (for tests).
    int getActiveNotes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace sing::dsp
