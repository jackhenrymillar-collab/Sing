#pragma once

#include "Bloom/BloomAnalysis.h"
#include "Bloom/BloomWorkspace.h"
#include "Common/RealFFT.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sing
{

// One note played by Bloom, the spectral smear (Paulstretch-style stretching, written from
// Paul Nasca's published description of the method).
//
// Every hop the voice builds one new frame:
// 1. For each layer (a Spread copy), read the loop's analysed magnitudes at the read
//    position, moved in frequency: the fine structure by the layer's pitch, the envelope by
//    the Formant setting.
// 2. Classic (the default): add the layers' power into one left and one right spectrum, run
//    the spectral modules on it (Octaves, Smear, Filter, Compressor), and give every bin a
//    new random phase. Inverse FFT each channel and window it.
// 3. Overlap-add: classic frames overlap two deep (a new frame every half window), as the
//    method's own implementations do, under a window whose squares sum to exactly 1 there,
//    so the noise-like frames keep an even level through every crossfade.
//
// With LoopSettings::bloomLockHarmonics the harmonics (found in the analysis) instead keep
// running at their own frequency from frame to frame, with a small random nudge, so a clear
// vowel stays steady. Those frames overlap four deep under Hann (values sum to 2, squares to
// 1.5), since locked harmonics add in amplitude and noise in power. The Octaves module is
// classic only, and Smear there jitters each bin's frequency instead.
//
// 4. Move the read position on by a hop times the speed. Hold stops it (freeze), and the
//    random phases keep the held sound alive. While an onset (a transient found in the
//    analysis) is inside the window, the position moves at the recorded speed instead.
//
// Spread copies cost a pass over the bins each, not an FFT: every note runs two inverse FFTs
// per frame however many copies there are.
//
// The work for each frame is cut into small steps of about equal cost (a slice of bins, or a
// share of one butterfly stage of an FFT) spread evenly over the hop before it is needed, so
// the cost per audio block is small and steady, with no worker thread. A note's first frame
// is built over its first moments (about 80 ms, longer for very long windows).
//
// Memory: a note's buffers come from its loop's analysis (BloomWorkspace), claimed at the
// start and handed back at the end, so long windows don't need huge buffers in every voice.
//
// Threading: prepare() allocates. Everything else is real-time safe.
class BloomVoice
{
public:
    static constexpr int maxLayers = 7;
    static constexpr int maxOverlap = BloomWorkspace::maxOverlap;
    static constexpr int numOctaves = 6;   // Octaves: -2, -1, 0, +1, +1 and a fifth, +2
    static constexpr std::array<float, numOctaves> octaveRatios { 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f };

    struct Layer
    {
        float pitchRatio = 1.0f;   // relative to the voice's root, including Spread detune
        float gainLeft = 0.0f, gainRight = 0.0f;
        double offsetSeconds = 0.0;   // reads this far ahead of the note's read position
    };

    struct Parameters
    {
        float speed = 1.0f;             // as StretchParameters::speed
        bool hold = false;
        float formantRatio = 1.0f;      // >1 moves the vowel up; 1 keeps the recorded vowel
        float width = 0.5f;             // 0: left and right share phases; 1: independent phases
        float onsets = 1.0f;            // 0: stretch transients like everything else; 1: pass them at the recorded speed
        // The spectral modules, applied to the bins before the inverse FFT:
        float shiftHz = 0.0f;           // frequency shift: moves every partial by this many Hz (inharmonic)
        float blur = 0.0f;              // Smear, 0 to 1: blurs the spectrum over up to half an octave either side
        float tone = 0.0f;              // -1: noise and breath only; 0: as recorded; 1: harmonics only
        // Harmonics: keeps only what lies near the harmonics of the key being played (the
        // voice's root times each layer's pitch ratio), up to harmonicsCount. 0 cents is off.
        float rootHz = 261.63f;
        float harmonicsCents = 0.0f;    // how far either side of each harmonic still passes (half the level here)
        int harmonicsCount = 16;
        // Filter: keeps lowHz to highHz. With lowHz above highHz it cuts highHz to lowHz out
        // instead. filterSlope 0: sharp (a sixth of an octave's edge); 1: 6 dB per octave;
        // 2: 12 dB per octave.
        bool filter = false;
        float lowHz = 20.0f, highHz = 20000.0f;
        int filterSlope = 0;
        // Compressor, 0 to 1: brings each frame toward the loop's typical level (at 1, all the
        // way, within +12 and -24 dB), so the sound holds steady instead of swelling and dipping.
        float compressor = 0.0f;
        // Octaves: the spectrum mixed with copies of itself moved by octaveRatios, at these
        // levels (amplitudes, 0 to 1). Off unless something but the 0 octave is up.
        bool octaves = false;
        std::array<float, numOctaves> octaveLevels { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
        int numLayers = 1;
        std::array<Layer, maxLayers> layers {};
    };

    // True when a loop has Bloom's analysis, so Bloom can play it.
    static bool canPlay (const LoopSource& source) noexcept { return source.analysis (StretchMode::bloom) != nullptr; }

    void prepare (double hostSampleRate);

    // scheduleOffset (0 to 1) staggers when this note does its work, so a chord's notes
    // don't all run their FFTs in the same audio block.
    // False when the note can't play Bloom (no analysis, or every note buffer is in use).
    bool start (const LoopSource& source, double bufferPosition, uint32_t seed, float scheduleOffset) noexcept;
    void setSource (const LoopSource& source) noexcept;   // ignored unless canPlay (source)
    void setParameters (const Parameters& p) noexcept { params = p; }

    // Moves the read position by this many play buffer samples (either way), for Wander.
    // The next frame reads from there, so the move blends in over the window.
    void nudge (double samples) noexcept;

    // Adds numSamples to left and right.
    void render (float* left, float* right, int numSamples) noexcept;
    void stop() noexcept;

    bool isActive() const noexcept { return active; }
    // Also true for the loop whose analysis lent this note its buffers.
    bool isUsing (const LoopSource& s) const noexcept { return active && (source == &s || workspaceOwner == &s); }

    // The read position as a sample position in the voice, for the play lights.
    // offsetSeconds: a Spread copy's head start (Layer::offsetSeconds).
    double playPosition (double offsetSeconds = 0.0) const noexcept;

    // How wide a stretch of the voice each frame hears, in voice samples.
    double windowSamples() const noexcept;

private:
    struct LayerFrame
    {
        int frameA = 0, frameB = 0;
        float frameMix = 0.0f;
        float binScale = 1.0f;   // output bin -> analysis bin, for harmonics
        float shiftBins = 0.0f, noiseShiftBins = 0.0f;   // the frequency shift, in analysis bins
        float noiseScale = 1.0f; // the same for breath and noise (no pitch on a pitched voice)
        float amplitude = 0.0f, tonalAmplitude = 0.0f;   // noise and harmonics
        float gainLeft = 0.0f, gainRight = 0.0f;
        float harmonicsPerBin = 0.0f;   // the key's harmonic number per output bin; 0: Harmonics off
        int peakBin = -1;            // the harmonic whose phase was worked out last
        uint32_t peakPhase = 0;
    };

    void beginFrame() noexcept;
    float harmonicsGain (const BloomAnalysis& analysis, int frame, double bufferPosition, float pitch) const noexcept;
    void runUnit (int unit) noexcept;
    void runDueUnits() noexcept;
    void finishFrame() noexcept;
    void rotate() noexcept;
    void renderFilling (float* left, float* right, int len) noexcept;
    int building() const noexcept { return (newest + 1) % (overlap + 1); }
    void gatherPower (int from, int to) noexcept;     // classic
    void gatherLocked (int from, int to) noexcept;    // locked harmonics
    void runPrefix (int from, int to, int slot) noexcept;
    void runOctaves (int from, int to) noexcept;
    void runSmear (int from, int to, int slot) noexcept;
    void runFinal (int from, int to, int slot) noexcept;
    void runWindow (int from, int to) noexcept;
    float windowAt (int index) const noexcept;
    float filterGain (int bin) const noexcept;
    float combGain (const LayerFrame&, int bin) const noexcept;   // Harmonics
    uint32_t nextRandom() noexcept;

    // The frame's work, in order: gather, [prefix, octaves], [prefix, smear], final (classic),
    // then both inverse FFTs and the window. Counts are fixed when a frame begins.
    enum Stage { gather, prefixA, octaveMix, prefixB, smearMix, finalPass, inverseLeft, inverseRight, windowing, numStages };
    std::array<int, numStages + 1> stageStart {};   // first unit of each stage; the last is the total
    std::array<int, numStages> stagePieces {};
    int fftChunks = 1;
    int smearSlot = 0, finalSlot = 0;   // which half of each complex bin holds the power going in

    double hostRate = 48000.0;
    int size = 0, hop = 0, overlap = 2;
    const RealFFT* fft = nullptr;
    int units = 0, unitsDone = 0;
    double firstDue = 0.0, unitSpacing = 1.0;
    double primeRate = 3.0;   // units per 128 samples while the first frame is built

    const LoopSource* source = nullptr;
    const LoopSource* workspaceOwner = nullptr;
    BloomWorkspace* work = nullptr;
    Parameters params;
    std::array<LayerFrame, maxLayers> frameLayers {};
    int frameLayerCount = 0;
    float envScale = 1.0f;
    float outBinHz = 1.0f;
    // The filter's edges in output bins: rising from riseFrom to riseTo, falling from
    // fallFrom to fallTo. Fixed for each frame.
    bool filterOn = false, filterStop = false;
    int filterSlope = 0;
    float riseFrom = 0.0f, riseTo = 0.0f, fallFrom = 0.0f, fallTo = 0.0f;
    float lowBin = 1.0f, highBin = 1.0f;
    float harmonicsWidth = 0.0f;   // harmonicsCents as a fraction of the frequency
    int harmonicsCount = 0;
    float combMakeup = 0.0f;   // Harmonics' level make-up, eased frame to frame; 0 while off
    double powerBeforeComb = 0.0, powerAfterComb = 0.0;   // summed over the frame being built
    float frameCompressorGain = 1.0f;
    float smearWidth = 0.0f;   // half-width of the smear, as a fraction of the frequency
    std::array<float, numOctaves> octaveWeights {};   // power weights, normalised
    bool frameOctaves = false, frameSmear = false, frameLocked = false;

    int newest = 0;   // the frame started most recently; the one being built is newest + 1

    bool active = false, priming = false, firstFrame = true;
    int pos = 0;                      // output position within the current hop
    double readPosition = 0.0;        // play buffer samples, where the next frame is read
    double currentFramePosition = 0.0, currentFrameAdvance = 0.0;   // the frame now fading in
    double buildFramePosition = 0.0, buildFrameAdvance = 0.0;       // the frame being built
    float scheduleFraction = 0.0f;
    double primeCredit = 0.0;
    std::array<bool, maxOverlap + 1> realFrame {};   // false for the silent frames a note starts with
    bool filling = false;
    int phaseHops = 1;               // hops between the last frame's start and this one's
    uint32_t rng = 1;
    uint32_t stereoOffset = 0;       // scales the fixed left-right phase offsets (Width)
};

} // namespace sing
