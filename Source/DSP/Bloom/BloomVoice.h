#pragma once

#include "Bloom/BloomAnalysis.h"
#include "Common/RealFFT.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sing
{

// One note played by Bloom, the spectral smear (Paulstretch-style stretching, written from
// Paul Nasca's published description of the method).
//
// Every quarter window (about 0.17 s) the voice builds one new frame:
// 1. For each layer (a Spread copy), read the loop's analysed magnitudes at the read
//    position, moved in frequency: the fine structure by the layer's pitch, the envelope by
//    the Formant setting. Add them into one left and one right spectrum, every bin with a
//    new random phase.
// 2. Inverse FFT each channel, apply a Hann window and overlap-add four frames deep (a new
//    frame every quarter window).
//
// Phases: by default (the classic method) every bin gets a new random phase every frame,
// harmonics included, and the magnitudes are kept exactly as analysed. That is the smear,
// and it makes the level shimmer a little. With LoopSettings::bloomLockHarmonics, the
// harmonics (found in the analysis) instead keep running at their own frequency from frame
// to frame, with a small random nudge, so a clear vowel stays steady. Locked harmonics add
// in amplitude across the overlapping frames, and noise adds in power; Hann at 4x overlap
// keeps both constant (its values sum to 2 and its squares to 1.5).
// 3. Move the read position on by a hop times the speed. Hold stops it (freeze), and the
//    random phases keep the held sound alive. While an onset (a transient found in the
//    analysis) is inside the window, the position moves at the recorded speed instead.
//
// Spread copies cost a pass over the bins each, not an FFT: every note runs two inverse FFTs
// per frame however many copies there are.
//
// The work for each frame is cut into small steps (a slice of bins, or one butterfly stage of
// an FFT) spread evenly over the hop before it is needed, so the cost per audio
// block is small and steady, with no worker thread. A note's first frame is built over its
// first few milliseconds, and then fades in over the window.
//
// Threading: prepare() allocates. Everything else is real-time safe.
class BloomVoice
{
public:
    static constexpr int maxLayers = 7;
    static constexpr int overlap = 4;

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
        float blur = 0.0f;              // frequency spread, 0 to 1: smears each bin over nearby frequencies
        float tone = 0.0f;              // -1: noise and breath only; 0: as recorded; 1: harmonics only
        // Harmonics: keeps only what lies near the harmonics of the key being played (the
        // voice's root times each layer's pitch ratio), up to harmonicsCount. 0 cents is off.
        float rootHz = 261.63f;
        float harmonicsCents = 0.0f;    // how far either side of each harmonic still passes (half the level here)
        int harmonicsCount = 16;
        // Filter: keeps lowHz to highHz, with a sixth of an octave's slope at each edge. With
        // lowHz above highHz it cuts highHz to lowHz out instead.
        bool filter = false;
        float lowHz = 20.0f, highHz = 20000.0f;
        int numLayers = 1;
        std::array<Layer, maxLayers> layers {};
    };

    // True when a loop has Bloom's analysis, so Bloom can play it.
    static bool canPlay (const LoopSource& source) noexcept { return source.analysis (StretchMode::bloom) != nullptr; }

    void prepare (double hostSampleRate);

    // scheduleOffset (0 to 1) staggers when this note does its work, so a chord's notes
    // don't all run their FFTs in the same audio block.
    void start (const LoopSource& source, double bufferPosition, uint32_t seed, float scheduleOffset) noexcept;
    void setSource (const LoopSource& source) noexcept;   // ignored unless canPlay (source)
    void setParameters (const Parameters& p) noexcept { params = p; }

    // Adds numSamples to left and right.
    void render (float* left, float* right, int numSamples) noexcept;
    void stop() noexcept;

    bool isActive() const noexcept { return active; }
    bool isUsing (const LoopSource& s) const noexcept { return active && source == &s; }

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
    void buildBins (int from, int to) noexcept;
    float filterGain (int bin) const noexcept;
    float combGain (const LayerFrame&, int bin) const noexcept;   // Harmonics
    uint32_t nextRandom() noexcept;

    double hostRate = 48000.0;
    int maxSize = 0;
    int size = 0, hop = 0;
    const RealFFT* fft = nullptr;
    int spectrumUnits = 64, units = 0, unitsDone = 0;
    double firstDue = 0.0, unitSpacing = 1.0;

    const LoopSource* source = nullptr;
    Parameters params;
    std::array<LayerFrame, maxLayers> frameLayers {};
    int frameLayerCount = 0;
    float envScale = 1.0f;
    // The filter's edges in output bins: rising from riseFrom to riseTo, falling from
    // fallFrom to fallTo. Fixed for each frame.
    bool filterOn = false, filterStop = false;
    float riseFrom = 0.0f, riseTo = 0.0f, fallFrom = 0.0f, fallTo = 0.0f;
    float harmonicsWidth = 0.0f;   // harmonicsCents as a fraction of the frequency
    int harmonicsCount = 0;
    float combMakeup = 0.0f;   // Harmonics' level make-up, eased frame to frame; 0 while off
    double powerBeforeComb = 0.0, powerAfterComb = 0.0;   // summed over the frame being built

    std::vector<float> spectrumLeft, spectrumRight;
    std::vector<uint32_t> phases;    // per layer and bin; a full turn is 2^32
    std::array<std::vector<float>, overlap + 1> framesLeft, framesRight;
    int newest = 0;   // the frame started most recently; the one being built is newest + 1
    std::vector<float> synthesisWindow;   // Hann over maxSize, maxSize + 1 points

    bool active = false, priming = false, firstFrame = true;
    int pos = 0;                      // output position within the current hop
    double readPosition = 0.0;        // play buffer samples, where the next frame is read
    double currentFramePosition = 0.0, currentFrameAdvance = 0.0;   // the frame now fading in
    double buildFramePosition = 0.0, buildFrameAdvance = 0.0;       // the frame being built
    float scheduleFraction = 0.0f;
    double primeCredit = 0.0;
    std::array<bool, overlap + 1> realFrame {};   // false for the silent frames a note starts with
    bool filling = false;
    int phaseHops = 1;               // hops between the last frame's start and this one's
    uint32_t rng = 1;
    uint32_t stereoOffset = 0;       // scales the fixed left-right phase offsets (Width)
};

} // namespace sing
