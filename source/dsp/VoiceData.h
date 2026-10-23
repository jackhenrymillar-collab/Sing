#pragma once

#include "Sinc.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace sing::dsp
{

// A voice, prepared for clean pitching (see Sinc.h for the method).
//
// Positions are in "level 0" samples: the voice upsampled 2x, so position 2n is the
// voice's original sample n. Ratios are level-0 samples per sample at the engine's
// internal rate (twice the host rate).
class VoiceData
{
public:
    // Builds the tables from mono audio. Slow (around a second for a long voice): call it
    // off the audio thread.
    static std::unique_ptr<VoiceData> build (const float* mono, std::size_t numSamples, double sampleRate, float rootNote);

    double getSourceRate() const noexcept { return sourceRate; }
    float getRootNote() const noexcept { return rootNote; }
    double getLength() const noexcept { return static_cast<double> (levels[0].length); } // level-0 samples
    double getSeconds() const noexcept { return static_cast<double> (sourceLength) / sourceRate; }
    int getNumLevels() const noexcept { return static_cast<int> (levels.size()); }

    // One band-limited sample at pos (level-0 samples), read at the given ratio.
    // Positions outside the voice read silence.
    float read (double pos, double ratio) const noexcept
    {
        int k = 0;
        const int last = static_cast<int> (levels.size()) - 1;
        while (ratio > 2.0 && k < last)
        {
            ratio *= 0.5;
            pos *= 0.5;
            ++k;
        }
        const auto& level = levels[static_cast<std::size_t> (k)];
        const auto& kern = InterpolationKernels::get().kernel (InterpolationKernels::stepFor (ratio));

        const double limit = static_cast<double> (level.length) + 8.0;
        pos = pos < -8.0 ? -8.0 : (pos > limit ? limit : pos);
        const double whole = std::floor (pos);
        const double phase = (pos - whole) * InterpolationKernels::kPhases;
        const int p = std::min (static_cast<int> (phase), InterpolationKernels::kPhases - 1);
        const float a = static_cast<float> (phase - p);
        const int taps = kern.taps;
        const float* c = kern.coef.data() + static_cast<std::ptrdiff_t> (p) * taps;
        const float* d = kern.delta.data() + static_cast<std::ptrdiff_t> (p) * taps;
        const float* x = level.data.data() + kPad + static_cast<std::ptrdiff_t> (whole) - taps / 2 + 1;

        // Eight running sums, so the compiler can use vector instructions.
        float acc[8] {};
        for (int j = 0; j < taps; j += 8)
            for (int q = 0; q < 8; ++q)
                acc[q] += x[j + q] * (c[j + q] + a * d[j + q]);
        return ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
    }

    // Zero padding at each end of every level, enough for the widest kernel plus the
    // positions read() allows outside the voice.
    static constexpr std::ptrdiff_t kPad = 40;

    struct Level
    {
        std::vector<float> data; // kPad zeros, the samples, kPad zeros
        std::size_t length = 0;
    };
    const Level& getLevel (int k) const noexcept { return levels[static_cast<std::size_t> (k)]; }

    // The filters used to build the levels, exposed for tests.
    static const std::vector<float>& upsampleFilter();
    static const std::vector<float>& decimateFilter();

private:
    VoiceData() = default;

    std::vector<Level> levels;
    double sourceRate = 48000.0;
    std::size_t sourceLength = 0;
    float rootNote = 60.0f;
};

} // namespace sing::dsp
