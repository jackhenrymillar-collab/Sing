#pragma once

// Small DSP building blocks shared by every reverb machine.
// Plain C++ with no framework dependencies, so they can be unit tested anywhere.
// Nothing here allocates after prepare().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace sing::dsp
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

// Sine LFO.
class Lfo
{
public:
    void prepare (double sampleRate) { fs = static_cast<float> (sampleRate); }
    void setFrequency (float hz) noexcept { increment = hz / fs; }
    void setPhase (float p) noexcept { phase = p; }

    float next() noexcept
    {
        const float out = std::sin (kTwoPi * phase);
        phase += increment;
        if (phase >= 1.0f)
            phase -= 1.0f;
        return out;
    }

private:
    float fs = 48000.0f;
    float phase = 0.0f;
    float increment = 0.0f;
};

// Smooth random drift, used in place of a sine LFO for delay modulation. A sine repeats
// exactly, and in a long tail that repetition can be heard as a metallic or chorus-like
// wobble. This draws a new random point every half cycle and joins the points with a
// Catmull-Rom curve, which keeps the slope continuous, so the delay (and the pitch it
// bends) never jumps. The output stays within +-1, like a sine.
// The points alternate up and down; the size and the timing of each swing vary.
// setFrequency sets the average speed, like a sine of that frequency; setPhase seeds it.
class DriftLfo
{
public:
    void prepare (double sampleRate) { fs = static_cast<float> (sampleRate); }
    void setFrequency (float hz) noexcept { increment = 2.0f * hz / fs; }

    void setPhase (float p) noexcept
    {
        state = 0x9E3779B9u ^ static_cast<std::uint32_t> (p * 1000003.0f + 17.0f);
        for (int k = 0; k < 8; ++k) random();
        for (auto& v : points) v = random();
        t = p - std::floor (p);
    }

    float next() noexcept
    {
        const float p0 = points[0], p1 = points[1], p2 = points[2], p3 = points[3];
        const float out = 0.5f * (2.0f * p1 + (p2 - p0) * t
                                  + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                                  + (3.0f * (p1 - p2) + p3 - p0) * t * t * t);
        t += increment * pace;
        if (t >= 1.0f)
        {
            t -= 1.0f;
            points = { p1, p2, p3, random() };
            // Each swing takes 0.7x to 1.3x the average time. Changing pace at a point bends
            // the pitch by well under a cent, which is inaudible.
            pace = 0.7f + 0.6f * unit();
        }
        return std::clamp (out, -1.0f, 1.0f);
    }

private:
    float unit() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> (state >> 8) * (1.0f / 16777216.0f);
    }

    float random() noexcept
    {
        // Alternate sides with a random size from 0.85 to 1, so the drift swings as far as
        // the sine it replaces (a uniform spread would mostly hover near the middle, and the
        // shallower wobble let long tails ring more).
        state = state * 1664525u + 1013904223u;
        side = -side;
        return side * (0.85f + 0.15f * static_cast<float> (state >> 8) * (1.0f / 16777216.0f));
    }

    float fs = 48000.0f;
    float increment = 0.0f;
    float t = 0.0f;
    std::uint32_t state = 1u;
    float side = 1.0f;
    float pace = 1.0f;
    std::array<float, 4> points {};
};

// RBJ biquad, transposed direct form II. Used for the wet low/high cut.
class Biquad
{
public:
    void reset() { z1 = z2 = 0.0f; }

    void setHighpass (float hz, double sampleRate) { design (hz, sampleRate, true); }
    void setLowpass (float hz, double sampleRate) { design (hz, sampleRate, false); }

    // Constant 0 dB peak-gain bandpass (RBJ), used for formants.
    void setBandpass (float hz, float q, double sampleRate)
    {
        const double w0 = 2.0 * std::numbers::pi * std::min<double> (hz, 0.45 * sampleRate) / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> (alpha / a0);
        b1 = 0.0f;
        b2 = static_cast<float> (-alpha / a0);
        a1 = static_cast<float> (-2.0 * std::cos (w0) / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    // Peaking EQ (RBJ), used for tape head bump.
    void setPeak (float hz, float q, float gainDb, double sampleRate)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * std::min<double> (hz, 0.45 * sampleRate) / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double cosw = std::cos (w0);
        const double a0 = 1.0 + alpha / A;
        b0 = static_cast<float> ((1.0 + alpha * A) / a0);
        b1 = static_cast<float> (-2.0 * cosw / a0);
        b2 = static_cast<float> ((1.0 - alpha * A) / a0);
        a1 = static_cast<float> (-2.0 * cosw / a0);
        a2 = static_cast<float> ((1.0 - alpha / A) / a0);
    }

    // High shelf (RBJ, shelf slope 1), used for Air.
    void setHighShelf (float hz, float gainDb, double sampleRate)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * std::min<double> (hz, 0.45 * sampleRate) / sampleRate;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / std::numbers::sqrt2;
        const double sq = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cosw + sq;
        b0 = static_cast<float> (A * ((A + 1.0) + (A - 1.0) * cosw + sq) / a0);
        b1 = static_cast<float> (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw) / a0);
        b2 = static_cast<float> (A * ((A + 1.0) + (A - 1.0) * cosw - sq) / a0);
        a1 = static_cast<float> (2.0 * ((A - 1.0) - (A + 1.0) * cosw) / a0);
        a2 = static_cast<float> (((A + 1.0) - (A - 1.0) * cosw - sq) / a0);
    }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

private:
    void design (float hz, double sampleRate, bool highpass)
    {
        const double w0 = 2.0 * std::numbers::pi * std::min<double> (hz, 0.45 * sampleRate) / sampleRate;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / std::numbers::sqrt2; // sin(w0) / (2Q), Q = 1/sqrt(2)
        const double a0 = 1.0 + alpha;
        double nb0, nb1, nb2;
        if (highpass)
        {
            nb0 = (1.0 + cosw) / 2.0;
            nb1 = -(1.0 + cosw);
            nb2 = nb0;
        }
        else
        {
            nb0 = (1.0 - cosw) / 2.0;
            nb1 = 1.0 - cosw;
            nb2 = nb0;
        }
        b0 = static_cast<float> (nb0 / a0);
        b1 = static_cast<float> (nb1 / a0);
        b2 = static_cast<float> (nb2 / a0);
        a1 = static_cast<float> (-2.0 * cosw / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

// Transparent below the threshold, smoothly saturating above it. Never exceeds 1.0.
inline float softLimit (float x, float threshold = 0.9f) noexcept
{
    const float ax = std::abs (x);
    if (ax <= threshold)
        return x;
    const float range = 1.0f - threshold;
    const float y = threshold + range * std::tanh ((ax - threshold) / range);
    return std::copysign (y, x);
}

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

// Allpass whose centre slowly sweeps up and down around a band (second order, RBJ). Inside
// a feedback loop it moves that band's resonant modes back and forth without changing the
// level or touching the pitch of anything outside the band, which smears low-mid ringing
// that delay modulation is too slow to reach. Depth is in octaves either side.
class SweptAllpass
{
public:
    void prepare (double sampleRate, float centreHz, float rateHz, float startPhase, float q = 0.7f)
    {
        fs = static_cast<float> (sampleRate);
        centre = centreHz;
        qInv = 0.5f / q;
        step = static_cast<float> (kBlock) * rateHz / fs;
        phase = startPhase - std::floor (startPhase);
        counter = 0;
        reset();
    }
    void reset() noexcept { z1 = z2 = 0.0f; }
    void setDepth (float octaves) noexcept { depth = std::max (0.0f, octaves); }

    float process (float x) noexcept
    {
        if (counter == 0)
        {
            counter = kBlock;
            phase += step;
            if (phase >= 1.0f) phase -= 1.0f;
            const float f0 = centre * std::exp2 (depth * std::sin (2.0f * std::numbers::pi_v<float> * phase));
            const float w0 = 2.0f * std::numbers::pi_v<float> * f0 / fs;
            const float alpha = std::sin (w0) * qInv;
            b0 = (1.0f - alpha) / (1.0f + alpha);
            b1 = -2.0f * std::cos (w0) / (1.0f + alpha);
        }
        --counter;
        // Transposed direct form II; an allpass has b2 = 1, a1 = b1 and a2 = b0.
        const float y = b0 * x + z1;
        z1 = b1 * (x - y) + z2;
        z2 = x - b0 * y;
        return y;
    }

private:
    static constexpr int kBlock = 16;
    float fs = 48000.0f, centre = 324.0f, qInv = 0.714f, step = 0.0f, phase = 0.0f, depth = 0.0f;
    float b0 = 0.0f, b1 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    int counter = 0;
};

// Stereo-linked lookahead peak limiter. Below the ceiling it passes the signal unchanged
// (just delayed by the lookahead). Above it, the gain dips smoothly before the peak
// arrives and recovers over the release time, so loud input is turned down rather than
// clipped or distorted.
//
// How it guarantees the ceiling: the gain each sample needs is held at its minimum for the
// lookahead window, eased back up with the release, then averaged over the window. Every
// value in that average is at or below what the delayed sample needs, so the average is too.
class PeakLimiter
{
public:
    void prepare (double sampleRate, float ceiling, float lookaheadMs = 1.5f, float releaseMs = 150.0f)
    {
        look = std::max<std::size_t> (1, static_cast<std::size_t> (std::lround (lookaheadMs * 0.001 * sampleRate)));
        limit = ceiling;
        release = 1.0f - std::exp (-1.0f / (releaseMs * 0.001f * static_cast<float> (sampleRate)));
        cap = look + 2;
        delayL.assign (look + 1, 0.0f);
        delayR.assign (look + 1, 0.0f);
        box.assign (look, 1.0f);
        dqVal.assign (cap, 1.0f);
        dqIdx.assign (cap, 0);
        reset();
    }

    void reset()
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        std::fill (box.begin(), box.end(), 1.0f);
        boxSum = static_cast<double> (look);
        env = 1.0f;
        head = tail = 0;
        n = 0;
        pos = boxPos = 0;
    }

    std::size_t latency() const noexcept { return look; }
    float gain() const noexcept { return lastGain; }

    void process (float& l, float& r) noexcept
    {
        const float peak = std::max (std::abs (l), std::abs (r));
        const float need = peak > limit ? limit / peak : 1.0f;

        // Running minimum over the last look + 1 samples (a monotonic queue).
        while (tail != head && dqVal[(tail + cap - 1) % cap] >= need)
            tail = (tail + cap - 1) % cap;
        dqVal[tail] = need;
        dqIdx[tail] = n;
        tail = (tail + 1) % cap;
        while (dqIdx[head] + static_cast<std::int64_t> (look) < n)
            head = (head + 1) % cap;
        const float held = dqVal[head];

        env = held < env ? held : env + release * (held - env);

        boxSum += static_cast<double> (env) - static_cast<double> (box[boxPos]);
        box[boxPos] = env;
        boxPos = (boxPos + 1) % look;
        if (boxPos == 0) // re-add from scratch once per window so rounding never builds up
        {
            boxSum = 0.0;
            for (const float v : box) boxSum += static_cast<double> (v);
        }
        lastGain = static_cast<float> (std::min (1.0, boxSum / static_cast<double> (look)));

        delayL[pos] = l;
        delayR[pos] = r;
        pos = (pos + 1) % (look + 1);
        l = delayL[pos] * lastGain; // the oldest sample: exactly look samples ago
        r = delayR[pos] * lastGain;
        ++n;
    }

private:
    std::size_t look = 64, cap = 66, head = 0, tail = 0, pos = 0, boxPos = 0;
    std::int64_t n = 0;
    float limit = 1.0f, release = 0.0001f, env = 1.0f, lastGain = 1.0f;
    double boxSum = 64.0;
    std::vector<float> delayL, delayR, box, dqVal;
    std::vector<std::int64_t> dqIdx;
};

} // namespace sing::dsp
