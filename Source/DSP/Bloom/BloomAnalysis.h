#pragma once

#include "Stretch/LoopSource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sing
{

// Bloom's analysis of one loop, built once when the loop is built (off the audio thread).
//
// The loop is cut into overlapping frames, each a long Hann-windowed FFT (about 0.68 s),
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

    // True when harmonics were found and locked (LoopSettings::bloomLockHarmonics). False
    // for the classic method: every bin is noPeak and gets a random phase.
    bool locksHarmonics() const noexcept { return locked; }

    // Decodes a fine-structure value.
    static float decode (uint16_t code) noexcept { return decodeTable()[code]; }

    // Picks the FFT size for a loop: about 0.68 s, smaller for loops shorter than that.
    static int chooseSize (int bufferSize, double sampleRate) noexcept;

private:
    static const float* decodeTable() noexcept;

    int size = 0, bins = 0, frames = 0;
    double hopSamples = 1.0;
    float binSpacing = 1.0f, envSpacing = 1.0f;
    bool locked = false;
    std::vector<uint16_t> fineData;
    std::vector<float> envData;
    std::vector<int16_t> peakData;
};

} // namespace sing
