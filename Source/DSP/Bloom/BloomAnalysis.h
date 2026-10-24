#pragma once

#include "Bloom/BloomWorkspace.h"
#include "Stretch/LoopSource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sing
{

// Bloom's analysis of one loop, built once when the loop is built (off the audio thread).
//
// The loop is cut into overlapping frames, each a long windowed FFT (0.68 s unless the
// Window control says otherwise; Hamming for the classic method, Hann with locked harmonics),
// read with wrap-around so the seam is already blended. For every frame Sing keeps only how
// loud each frequency is, split into two parts:
// - the spectral envelope: the vowel's overall shape, smoothed over a few hundred Hz;
// - the fine structure: the harmonics and breath, with the envelope divided out.
// Keeping them apart lets Bloom move the pitch (the fine structure) and the vowel (the
// envelope) separately, just by reading them at different frequencies.
//
// Magnitudes are stored as amplitude densities (|X| / sqrt(sum of window squared)), so they
// don't depend on the FFT size and a voice can synthesise at a different size.
class BloomAnalysis final : public SourceAnalysis
{
public:
    static std::shared_ptr<const BloomAnalysis> build (const LoopSource& source);

    static constexpr int envelopePoints = 1024;

    int fftSize() const noexcept { return size; }
    int numBins() const noexcept { return bins; }
    int numFrames() const noexcept { return frames; }
    double hop() const noexcept { return hopSamples; }     // play buffer samples per frame
    float binHz() const noexcept { return binSpacing; }
    float envelopeHz() const noexcept { return envSpacing; }

    // The fine structure of a frame, one value per bin, coded as 16-bit log values.
    const uint16_t* fine (int frame) const noexcept { return fineData.data() + (size_t) frame * (size_t) bins; }
    const float* envelope (int frame) const noexcept { return envData.data() + (size_t) frame * envelopePoints; }

    // With locked harmonics: for each bin, where the harmonic it belongs to really is: the harmonic's centre minus
    // the bin, in 1/64ths of a bin, or noPeak for bins of breath and noise. Bloom keeps the
    // phases of harmonics running smoothly from frame to frame (so a held vowel doesn't
    // tremble) and gives everything else random phases. Each harmonic is stored as one clean
    // main lobe (vibrato sidebands folded in), so only its 3 or 4 nearest bins have a peak.
    static constexpr int16_t noPeak = -32768;
    static constexpr float peakScale = 64.0f;
    const int16_t* peaks (int frame) const noexcept { return peakData.data() + (size_t) frame * (size_t) bins; }

    // True when harmonics are locked (LoopSettings::bloomLockHarmonics). False for the
    // classic method: harmonics are still marked in peaks() (for the Tone control), but every
    // bin gets a random phase and the magnitudes are as analysed.
    bool locksHarmonics() const noexcept { return locked; }

    // Onsets: sudden rises in the sound (a consonant, a breath in, a hit), found with short
    // frames, as positions in the play buffer and strengths from 0 to 1. Bloom moves through
    // these at the recorded speed instead of stretching them, so a transient stays a moment
    // rather than smearing into everything around it. Sorted by position.
    struct Onset
    {
        double position = 0.0;
        float strength = 0.0f;
    };
    const std::vector<Onset>& onsets() const noexcept { return onsetList; }

    // Finds onsets in a buffer by spectral flux: how much louder each of 24 bands has become
    // since the last short frame, peaks of that standing well over its local median. Reads
    // round the buffer, as a loop.
    static std::vector<Onset> findOnsets (const LoopSource& source);

    // Decodes a fine-structure value.
    static float decode (uint16_t code) noexcept { return decodeTable()[code]; }

    // The Window control's range, in seconds.
    static constexpr double minWindowSeconds = 0.1, maxWindowSeconds = 40.0;

    // Picks the FFT size for a loop: the power of two nearest the window length, smaller for
    // loops shorter than that.
    static int chooseSize (int bufferSize, double sampleRate, double windowSeconds = 0.68) noexcept;

    // How loud each frame is (its power, summed over the bins), and the loop's typical frame
    // power (the geometric mean over the frames), for the Compressor.
    float framePower (int frame) const noexcept { return powerData[(size_t) frame]; }
    float typicalPower() const noexcept { return typical; }

    // The per-note buffers made for this loop's window (see BloomWorkspace). claim() is
    // real-time safe and returns nullptr when every one is in use.
    BloomWorkspace* claimWorkspace() const noexcept;
    int numWorkspaces() const noexcept { return (int) workspaces.size(); }

    // The synthesis FFT size for this analysis at a host sample rate.
    static int synthesisSize (int analysisSize, double sourceRate, double hostRate) noexcept;

    // Memory the per-note buffers may take for one loop, in total.
    static constexpr size_t workspaceBudget = (size_t) 640 << 20;

private:
    static const float* decodeTable() noexcept;

    int size = 0, bins = 0, frames = 0;
    double hopSamples = 1.0;
    float binSpacing = 1.0f, envSpacing = 1.0f;
    bool locked = false;
    std::vector<uint16_t> fineData;
    std::vector<float> envData;
    std::vector<int16_t> peakData;
    std::vector<Onset> onsetList;
    std::vector<float> powerData;
    float typical = 0.0f;
    std::vector<std::unique_ptr<BloomWorkspace>> workspaces;
};

} // namespace sing
