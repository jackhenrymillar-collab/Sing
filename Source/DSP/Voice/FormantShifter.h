#pragma once

#include "Common/RealFFT.h"

#include <vector>

namespace sing
{

// Moves a voice's formants (its vowel colour) up or down without changing its pitch.
//
// A spectral envelope shifter, in the style of Röbel and Rodet ("Efficient spectral envelope
// estimation and its application to pitch shifting and envelope preservation", DAFx 2005):
//
// 1. Analysis: the input is cut into Hann-windowed frames (at least four pitch periods long,
//    a new one every quarter frame) and FFT'd.
// 2. Separation: each frame's spectral envelope (the vowel: formants and timbre) is traced
//    through the tops of the harmonics (a running maximum over about one and a half
//    harmonics, then smoothed). What lies under it, the harmonics and breath, is the
//    excitation.
// 3. Reshaping: the envelope is read at frequency / ratio, so it moves up (ratio > 1) or down,
//    and each bin is multiplied by new envelope / old envelope. Only magnitudes change: the
//    harmonics keep their frequencies and phases, so no new partials appear and the pitch
//    stays exactly where it was.
// 4. Reconstruction: inverse FFT, Hann window again, overlap-add.
//
// The voice does the pitch shifting itself (by reading the loop faster or slower), which
// moves the envelope along with the harmonics; this shifter then puts the envelope back where
// the Formant setting wants it.
//
// At ratio 1 the output is the input, delayed by latency() samples. Real-time safe after
// prepare().
class FormantShifter
{
public:
    static constexpr float minRatio = 0.5f;
    static constexpr float maxRatio = 2.0f;

    FormantShifter() = default;

    // maxPeriodSeconds: the longest pitch period it will be asked to follow (25 ms is 40 Hz).
    void prepare (double sampleRate, float maxPeriodSeconds = 0.025f);

    // Starts a note. periodSamples is the pitch period at the note's pitch (0 if unpitched);
    // it sets the frame length, which then stays for the note. Returns the latency in samples.
    int start (float periodSamples, float ratio) noexcept;

    // Follows a changing pitch (glide, bend) and formant setting.
    void setPeriod (float periodSamples) noexcept;
    void setRatio (float ratio) noexcept;

    int latency() const noexcept { return size; }

    // Reads numSamples from inLeft/inRight and writes the shifted audio to outLeft/outRight.
    // In and out may be the same buffers. Pass inRight == nullptr for mono.
    void process (const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples) noexcept;

private:
    void processFrame() noexcept;
    void traceEnvelope() noexcept;

    double rate = 48000.0;
    int minSize = 1024, maxSize = 4096;
    int size = 1024, hop = 256, rover = 0;
    float period = 0.0f, ratio = 1.0f;
    bool stereo = false;

    std::vector<RealFFT> ffts;   // one per size from minSize to maxSize
    RealFFT* fft = nullptr;
    std::vector<float> window;   // Hann for the current size, scaled for 4x overlap-add
    std::vector<float> inL, inR, outL, outR, accL, accR;
    std::vector<float> specL, specR, frame;
    std::vector<float> power, envelope, logUpper, gains;
    std::vector<double> prefix;
    std::vector<int> queue;
};

} // namespace sing
