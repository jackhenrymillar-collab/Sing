#pragma once

#include "Common/RealFFT.h"

#include <vector>

namespace sing
{

// Shifts a voice's pitch in the spectrum, without resampling it: the STFT pitch shift of
// Jürgen Hock's stftPitchShift (github.com/jurihock/stftPitchShift, MIT licence, see
// THIRD_PARTY_NOTICES.md). Adapted from that library's Vocoder, Pitcher and Cepster:
//
// 1. Analysis: Hann frames (at least eight pitch periods and 85 ms, a new one every eighth
//    of a frame: the long frames and high overlap keep the partials clean),
//    FFT'd, and each bin turned into a magnitude and an exact frequency (from how far its
//    phase moved since the last frame, the phase vocoder).
// 2. Vowel (optional): the spectral envelope is found by smoothing the log magnitudes in
//    the cepstrum (a low-pass "lifter" below the pitch period) and divided out.
// 3. Shift: the bins are read at bin / factor and their frequencies multiplied by factor,
//    so every partial moves to factor times its frequency.
// 4. The envelope goes back on, read at bin / envelopeRatio: 1 keeps the recorded vowel,
//    other values move it (the Formant knob).
// 5. Synthesis: each bin's phase runs on at its new frequency; inverse FFT, Hann again,
//    overlap-add.
//
// At factor 1 with the vowel untouched the output is the input, delayed by latency()
// samples. Real-time safe after prepare().
class StftPitchShifter
{
public:
    static constexpr float minFactor = 0.25f;
    static constexpr float maxFactor = 4.0f;

    void prepare (double sampleRate, float maxPeriodSeconds = 0.025f);

    // Starts a note. periodSamples: the input's pitch period (0 if unpitched), which sets
    // the frame length for the note and the lifter. Returns the latency in samples.
    int start (float periodSamples, float factor, bool envelope, float envelopeRatio) noexcept;

    void setPeriod (float periodSamples) noexcept;
    // factor: the pitch change (2 is an octave up). envelope: take the spectral envelope out
    // before shifting and put it back after, moved by envelopeRatio; off, the envelope moves
    // with the pitch, like plain resampling.
    void setShift (float factor, bool envelope, float envelopeRatio) noexcept;

    int latency() const noexcept { return size; }

    // In and out may be the same buffers. Pass inRight == nullptr for mono.
    void process (const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples) noexcept;

private:
    struct Channel
    {
        std::vector<float> in, out, acc, spec;
        std::vector<float> lastPhase, phase, mag, freq;
    };

    void processFrame() noexcept;
    void analyse (Channel& c) noexcept;
    void shiftAndSynthesise (Channel& c) noexcept;
    void findEnvelope() noexcept;

    double rate = 48000.0;
    int minSize = 1024, maxSize = 4096;
    int size = 1024, hop = 256, rover = 0;
    float period = 0.0f, factor = 1.0f, envelopeRatio = 1.0f;
    bool envelopeOn = false, stereo = false, running = false;

    std::vector<RealFFT> ffts, cepstrumFfts;
    RealFFT* fft = nullptr;
    RealFFT* cepstrumFft = nullptr;
    std::vector<float> window, frame;
    Channel left, right;
    std::vector<float> envelope, movedEnvelope, cepstrum, logSpec, shiftedMag, shiftedFreq;
};

} // namespace sing
