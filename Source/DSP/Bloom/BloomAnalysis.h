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

    // Decodes a fine-structure value.
    static float decode (uint16_t code) noexcept { return decodeTable()[code]; }

    // Picks the FFT size for a loop: about 0.68 s, smaller for loops shorter than that.
    static int chooseSize (int bufferSize, double sampleRate) noexcept;

private:
    static const float* decodeTable() noexcept;

    int size = 0, bins = 0, frames = 0;
    double hopSamples = 1.0;
    float binSpacing = 1.0f, envSpacing = 1.0f;
    std::vector<uint16_t> fineData;
    std::vector<float> envData;
};

} // namespace sing
