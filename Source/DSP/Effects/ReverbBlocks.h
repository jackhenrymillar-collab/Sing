#pragma once

// The small building blocks Space's reverb (Aurora) needs, copied from Overcast, the reverb
// in the same range (source/dsp/Blocks.h and Shared.h), so the two sound the same. Plain
// C++ with no framework dependencies. Nothing here allocates after prepare().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace sing::reverb
{

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

inline std::size_t nextPowerOfTwo (std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// Circular delay buffer. read(d) returns the sample pushed d samples ago (d >= 1).
class DelayLine
{
public:
    void prepare (std::size_t maxDelaySamples)
    {
        buffer.assign (nextPowerOfTwo (maxDelaySamples + 4), 0.0f);
        mask = buffer.size() - 1;
        writePos = 0;
    }

    void reset() { std::fill (buffer.begin(), buffer.end(), 0.0f); writePos = 0; }

    void push (float x) noexcept
    {
        buffer[writePos] = x;
        writePos = (writePos + 1) & mask;
    }

    float read (std::size_t delay) const noexcept
    {
        return buffer[(writePos - delay) & mask];
    }

    // Third-order Lagrange interpolation for fractional (modulated) delays.
    // Needs delay >= 2.
    float readFractional (float delay) const noexcept
    {
        const auto whole = static_cast<std::size_t> (delay);
        const float f = delay - static_cast<float> (whole);
        const float ym1 = read (whole - 1);
        const float y0  = read (whole);
        const float y1  = read (whole + 1);
        const float y2  = read (whole + 2);
        const float cm1 = -f * (f - 1.0f) * (f - 2.0f) / 6.0f;
        const float c0  = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) / 2.0f;
        const float c1  = -(f + 1.0f) * f * (f - 2.0f) / 2.0f;
        const float c2  = (f + 1.0f) * f * (f - 1.0f) / 6.0f;
        return cm1 * ym1 + c0 * y0 + c1 * y1 + c2 * y2;
    }

private:
    std::vector<float> buffer;
    std::size_t mask = 0;
    std::size_t writePos = 0;
};

// Schroeder allpass: H(z) = (z^-D - g) / (1 - g z^-D). Lossless for any |g| < 1.
class Allpass
{
public:
    void prepare (std::size_t maxDelaySamples) { line.prepare (maxDelaySamples + 8); }
    void reset() { line.reset(); }

    float process (float x, std::size_t delay, float g) noexcept
    {
        const float delayed = line.read (delay);
        const float w = x + g * delayed;
        line.push (w);
        return delayed - g * w;
    }

    float processModulated (float x, float delay, float g) noexcept
    {
        const float delayed = line.readFractional (delay);
        const float w = x + g * delayed;
        line.push (w);
        return delayed - g * w;
    }

    // Read the allpass's internal delay line (used for output taps).
    float tap (std::size_t delay) const noexcept { return line.read (delay); }

private:
    DelayLine line;
};

// One-pole lowpass: y = (1 - a) x + a y[n-1], a = exp(-2 pi fc / fs).
class OnePoleLowpass
{
public:
    static float coefficientFor (float cutoffHz, double sampleRate) noexcept
    {
        return std::exp (-kTwoPi * cutoffHz / static_cast<float> (sampleRate));
    }

    void reset() { state = 0.0f; }

    float process (float x, float a) noexcept
    {
        state = x + a * (state - x);
        return state;
    }

private:
    float state = 0.0f;
};

// Exponential parameter smoother. Converges to the target with time constant tau.
// State is kept in double so it reaches the target exactly instead of stalling a few
// float steps short (which would break bit-exact dry gain).
class Smoother
{
public:
    void prepare (double sampleRate, float timeMs)
    {
        coeff = std::exp (-1.0 / (static_cast<double> (timeMs) * 0.001 * sampleRate));
    }

    void setTarget (float t) noexcept { target = t; }
    void snapTo (float t) noexcept { target = t; current = t; }
    float getTarget() const noexcept { return static_cast<float> (target); }
    float getCurrent() const noexcept { return static_cast<float> (current); }
    bool isSettled() const noexcept { return ! (current < target) && ! (current > target); }

    float next() noexcept
    {
        current = target + coeff * (current - target);
        if (std::abs (current - target) < 1.0e-9)
            current = target;
        return static_cast<float> (current);
    }

    // Advance several samples at once (for values updated per block).
    float skip (int numSamples) noexcept
    {
        current = target + std::pow (coeff, numSamples) * (current - target);
        if (std::abs (current - target) < 1.0e-9)
            current = target;
        return static_cast<float> (current);
    }

private:
    double coeff = 0.0;
    double current = 0.0;
    double target = 0.0;
};

// Safety guard for signals inside a feedback loop. The loops are lossless at most, so
// this never acts in normal use: the loudest loop signal measured anywhere (+12 dBFS
// noise held into a frozen 60 s tail at 96 kHz) is about 24, well under the threshold
// of 64. Above that it saturates smoothly towards 128, so a runaway can never blow up.
// A value that is not a finite number is dropped to silence instead of spreading.
inline float loopGuard (float x) noexcept
{
    constexpr float kThreshold = 64.0f, kRange = 64.0f;
    const float ax = std::abs (x);
    if (ax <= kThreshold)
        return x;
    if (! (ax < 1.0e30f))
        return 0.0f;
    return std::copysign (kThreshold + kRange * std::tanh ((ax - kThreshold) / kRange), x);
}

inline float msToSamples (float ms, double fs) noexcept
{
    return ms * 0.001f * static_cast<float> (fs);
}

// Decay gain for one pass through a delay of `delaySamples` so the loop reaches -60 dB in rt60.
inline float decayGain (float delaySamples, float rt60Seconds, double fs) noexcept
{
    return std::pow (10.0f, -3.0f * delaySamples / (static_cast<float> (fs) * std::max (0.05f, rt60Seconds)));
}

// Frequency-dependent decay inside a feedback loop: separate gains for low, mid and high
// bands, split by two complementary one-pole crossovers (the bands sum back to the input).
class ThreeBandDecay
{
public:
    void reset() { lpLow.reset(); lpHigh.reset(); }

    float process (float x, float aLow, float aHigh, float gLow, float gMid, float gHigh) noexcept
    {
        const float low = lpLow.process (x, aLow);
        const float belowHigh = lpHigh.process (x, aHigh);
        return gLow * low + gMid * (belowHigh - low) + gHigh * (x - belowHigh);
    }

private:
    OnePoleLowpass lpLow, lpHigh;
};

} // namespace sing::reverb
