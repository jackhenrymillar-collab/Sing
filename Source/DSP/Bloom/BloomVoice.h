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
// Every half window (about 0.34 s) the voice builds one new frame:
// 1. For each layer (a Spread copy), read the loop's analysed magnitudes at the read
//    position, moved in frequency: the fine structure by the layer's pitch, the envelope by
//    the Formant setting. Add them into one left and one right spectrum, every bin with a
//    new random phase.
// 2. Inverse FFT each channel, apply a sine window and overlap-add at 50%. The sine window's
//    squares sum to one at that overlap, and frames with random phases add in power, so the
//    level stays steady.
// 3. Move the read position on by a half window times the speed. Hold stops it, and the
//    random phases keep the held sound alive.
//
// Spread copies cost a pass over the bins each, not an FFT: every note runs two inverse FFTs
// per frame however many copies there are.
//
// The work for each frame is cut into small steps (a slice of bins, or one butterfly stage of
// an FFT) spread evenly over the half window before it is needed, so the cost per audio
// block is small and steady, with no worker thread. A note's first frame is built over its
// first few milliseconds, and then fades in over half a window.
//
// Threading: prepare() allocates. Everything else is real-time safe.
class BloomVoice
{
public:
    static constexpr int maxLayers = 7;

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
        float width = 0.5f;             // 0: left and right share phases; 1: independent
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
        float binScale = 1.0f;   // output bin -> analysis bin
        float amplitude = 0.0f;
        float gainLeft = 0.0f, gainRight = 0.0f;
    };

    void beginFrame() noexcept;
    void runUnit (int unit) noexcept;
    void runDueUnits() noexcept;
    void finishFrame() noexcept;
    void rotate() noexcept;
    void buildBins (int from, int to) noexcept;
    uint32_t nextRandom() noexcept;

    double hostRate = 48000.0;
    int maxSize = 0;
    int size = 0, half = 0;
    const RealFFT* fft = nullptr;
    int spectrumUnits = 16, units = 0, unitsDone = 0;
    double firstDue = 0.0, unitSpacing = 1.0;

    const LoopSource* source = nullptr;
    Parameters params;
    std::array<LayerFrame, maxLayers> frameLayers {};
    int frameLayerCount = 0;
    float envScale = 1.0f, phaseMixSame = 1.0f, phaseMixOwn = 0.0f;

    std::vector<float> spectrumLeft, spectrumRight;
    std::array<std::vector<float>, 3> framesLeft, framesRight;
    int prev = 0, cur = 1, next = 2;
    std::vector<float> sineWindow;   // sin(pi i / maxSize), maxSize + 1 points

    bool active = false, priming = false;
    int pos = 0;                      // output position within the current half window
    double readPosition = 0.0;        // play buffer samples, where the next frame is read
    double currentFramePosition = 0.0, currentFrameAdvance = 0.0;   // the frame now fading in
    double buildFramePosition = 0.0, buildFrameAdvance = 0.0;       // the frame being built
    float scheduleFraction = 0.0f;
    uint32_t rng = 1;
};

} // namespace sing
