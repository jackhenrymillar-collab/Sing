#pragma once

// Shared building blocks used by several machines: a feedback delay network,
// diffusers, early reflections, antialiased saturation, envelope following and
// pitch shifting. Nothing here allocates after prepare().

#include "Blocks.h"

#include <array>

namespace sing::dsp
{

inline float msToSamples (float ms, double fs) noexcept
{
    return ms * 0.001f * static_cast<float> (fs);
}

// Decay gain for one pass through a delay of `delaySamples` so the loop reaches -60 dB in rt60.
inline float decayGain (float delaySamples, float rt60Seconds, double fs) noexcept
{
    return std::pow (10.0f, -3.0f * delaySamples / (static_cast<float> (fs) * std::max (0.05f, rt60Seconds)));
}

// In-place fast Walsh-Hadamard transform, normalised so it is orthogonal (lossless).
template <std::size_t N>
inline void hadamard (std::array<float, N>& x) noexcept
{
    static_assert ((N & (N - 1)) == 0, "N must be a power of two");
    for (std::size_t h = 1; h < N; h <<= 1)
        for (std::size_t i = 0; i < N; i += h << 1)
            for (std::size_t j = i; j < i + h; ++j)
            {
                const float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
    const float norm = 1.0f / std::sqrt (static_cast<float> (N));
    for (auto& v : x)
        v *= norm;
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

struct DecaySettings
{
    float rtLow = 3.0f, rtMid = 3.0f, rtHigh = 1.5f; // seconds
    float lowHz = 250.0f, highHz = 4000.0f;          // crossover points
};

// N-line feedback delay network with a Hadamard mixing matrix, three-band decay,
// smoothed (glide-free) delay changes, gentle delay modulation and a lossless freeze.
//
// Modulation is split between two methods. Half of it wobbles the delay lengths as
// before. The other half slowly rotates the mixing matrix (Schlecht and Habets, "Time-
// varying feedback matrices in feedback delay networks", 2015): after the Hadamard mix,
// each pair of lines (i, i + N/2) is turned by an angle that swings slowly around zero,
// each pair at its own rate. A rotation is orthogonal at every angle, so the network
// stays lossless and stable, and the echo pattern keeps changing without bending the
// pitch of what rings. With Mod at zero and a short decay the angles are zero: the plain
// Hadamard network.
//
// Long tails need more movement. Past about 3 s of decay a static network's tail splits
// into separate pitches that ring on (worst below 500 Hz, where delay modulation barely
// reaches), so from 3 s to 8 s of decay, and whenever frozen, the rotation grows to a
// 90 degree swing, whatever Mod is set to. Jack picked the middle setting by ear: the
// rotation turns up to twice its slow rates (0.09 to 0.4 Hz, so at most 0.8 Hz) and the
// delays drift by up to 1 ms. Much faster turning makes held notes swirl (phasing);
// slower lets low notes ring.
template <std::size_t N>
class Fdn
{
public:
    // maxLoopAllpassMs: the longest in-loop allpass this network will be given.
    void prepare (double sampleRate, float maxDelayMs, float maxLoopAllpassMs = 8.0f)
    {
        fs = sampleRate;
        maxLoopAllpass = maxLoopAllpassMs;
        maxDelay = msToSamples (maxDelayMs, fs) + msToSamples (kMaxModMs, fs) + 8.0f;
        for (std::size_t i = 0; i < N; ++i)
        {
            lines[i].prepare (static_cast<std::size_t> (maxDelay) + 8);
            delays[i].prepare (fs, 60.0f);
            gLow[i].prepare (fs, 30.0f);
            gMid[i].prepare (fs, 30.0f);
            gHigh[i].prepare (fs, 30.0f);
            smear[i].prepare (fs, 324.0f, 0.07f + 0.023f * static_cast<float> (i) * (16.0f / static_cast<float> (N)),
                              static_cast<float> ((i * 5) % N) / static_cast<float> (N));
            lfos[i].prepare (fs);
            lfos[i].setPhase (static_cast<float> (i) / static_cast<float> (N));
            lfos[i].setFrequency (0.11f + 0.07f * static_cast<float> (i));
            drifts[i].prepare (fs);
            drifts[i].setPhase (static_cast<float> (i) / static_cast<float> (N));
            drifts[i].setFrequency (0.11f + 0.07f * static_cast<float> (i));
            // Alternating input/output signs decorrelate the channels.
            signs[i] = ((i * 7 + i / 2) % 3 == 0) ? -1.0f : 1.0f;
        }
        for (std::size_t i = 0; i < N; ++i)
        {
            loopAllpass[i].prepare (static_cast<std::size_t> (msToSamples (maxLoopAllpass, fs)) + 2);
            loopAllpassDelay[i].prepare (fs, 60.0f);
        }
        inputGain.prepare (fs, 50.0f);
        modDepth.prepare (fs, 50.0f);
        rotation.prepare (fs, 50.0f);
        tailDepth.prepare (fs, 50.0f);
        // A delay swinging D seconds peak to peak at f Hz bends pitch by about
        // 1731 * pi * f * D cents; solve for D at the fastest line's rate.
        {
            const float fastestHz = 0.11f + 0.07f * static_cast<float> (N - 1);
            maxTailDriftMs = 1000.0f * kMaxTailCents / (1731.0f * std::numbers::pi_v<float> * fastestHz);
        }
        for (std::size_t k = 0; k < N / 2; ++k)
        {
            // 0.09 to about 0.4 Hz, spread so no two pairs line up.
            const float hz = 0.09f + 0.043f * static_cast<float> (k) * (8.0f / static_cast<float> (N));
            rotStep[k] = static_cast<float> (kRotationBlock) * hz / static_cast<float> (fs);
        }
        reset();
    }

    void reset()
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            lines[i].reset();
            loopAllpass[i].reset();
            loopAllpassDelay[i].snapTo (loopAllpassDelay[i].getTarget());
            decay[i].reset();
            smear[i].reset();
            delays[i].snapTo (delays[i].getTarget());
            gLow[i].snapTo (gLow[i].getTarget());
            gMid[i].snapTo (gMid[i].getTarget());
            gHigh[i].snapTo (gHigh[i].getTarget());
        }
        inputGain.snapTo (inputGain.getTarget());
        modDepth.snapTo (modDepth.getTarget());
        rotation.snapTo (rotation.getTarget());
        tailDepth.snapTo (tailDepth.getTarget());
        for (std::size_t k = 0; k < N / 2; ++k)
        {
            rotPhase[k] = 0.37f * static_cast<float> (k);
            rotCos[k] = 1.0f; rotSin[k] = 0.0f;
            rotCosStep[k] = rotSinStep[k] = 0.0f;
        }
        rotCounter = 0;
    }

    // Delay lengths in milliseconds; changes glide smoothly.
    void setDelays (const std::array<float, N>& ms)
    {
        for (std::size_t i = 0; i < N; ++i)
            delays[i].setTarget (std::clamp (msToSamples (ms[i], fs), 4.0f, maxDelay - msToSamples (kMaxModMs, fs) - 4.0f));
        updateGains();
    }

    void setDecay (const DecaySettings& d)
    {
        decaySettings = d;
        aLow = OnePoleLowpass::coefficientFor (d.lowHz, fs);
        aHigh = OnePoleLowpass::coefficientFor (d.highHz, fs);
        updateGains();
        updateRotation();
    }

    // Optional allpass inside each feedback path. Short networks (small rooms) otherwise
    // have too few echo paths and ring at individual pitches; an allpass per line multiplies
    // the echo density and spreads the modes without changing the tone or the decay.
    // Gain 0 turns it off. Delays are in milliseconds, up to the maximum given to prepare.
    void setLoopAllpasses (const std::array<float, N>& ms, float gain)
    {
        const float maxSamples = msToSamples (maxLoopAllpass, fs);
        for (std::size_t i = 0; i < N; ++i)
        {
            const float target = std::clamp (std::round (msToSamples (ms[i], fs)), 1.0f, maxSamples);
            if (loopAllpassDelay[i].getCurrent() < 1.0f) loopAllpassDelay[i].snapTo (target); // first use
            else                                         loopAllpassDelay[i].setTarget (target);
        }
        loopAllpassGain = std::clamp (gain, 0.0f, 0.75f);
        updateGains();
    }

    // depth 0..1 sets both the matrix rotation and the delay excursion. delayShare is the
    // part of kMaxModMs the delays may swing: half by default, so the rotation does the
    // rest of the work and sustained notes bend less in pitch.
    void setModulation (float depth, float delayShare = 0.5f)
    {
        const float d = std::clamp (depth, 0.0f, 1.0f);
        modDepth.setTarget (std::clamp (delayShare, 0.0f, 1.0f) * d * msToSamples (kMaxModMs, fs));
        modAmount = d;
        updateRotation();
    }

    // Smooth random drift in place of the sine wobble on each line (see DriftLfo). By ear
    // and by ringing score it only helped the old Space Map, so it is off unless a machine asks (none does now).
    void setRandomDrift (bool on) noexcept { useDrift = on; }

    // Scales the extra movement long tails get (the faster rotation and the delay drift
    // from 3 s to 8 s of decay). 1 is the "Middle" setting; machines whose long tails
    // still ring ask for a little more. capDrift false keeps the full delay drift instead of
    // capping it at kMaxTailCents (the old Arena: Jack preferred it uncapped by ear).
    void setTailMovement (float amount, bool capDrift = true)
    {
        tailMovement = std::max (0.0f, amount);
        driftCapped = capDrift;
        updateRotation();
    }

    // Low-mid smear: a slowly swept allpass in every line, centred on 150 to 700 Hz, whose
    // sweep (in octaves either side) is reached in long tails. 0 leaves it out.
    void setLowMidSmear (float octaves)
    {
        smearOctaves = std::max (0.0f, octaves);
        updateSmear();
    }

    void setFreeze (bool f)
    {
        frozen = f;
        inputGain.setTarget (f ? 0.0f : 1.0f);
        updateGains();
        updateRotation();
    }

    // Optional per-line hook inside the loop (used by Halo for pitch shifting).
    template <typename LineHook>
    void process (float inL, float inR, float& outL, float& outR, LineHook&& hook) noexcept
    {
        float sumL = 0.0f, sumR = 0.0f;
        step ([&] (std::size_t i) { return signs[i] * (i % 2 == 0 ? inL : inR); },
              [&] (std::size_t i, float y)
              {
                  if (i % 2 == 0) sumL += signs[i] * y;
                  else            sumR += signs[i] * y;
              },
              hook);
        constexpr float outNorm = 1.0f / static_cast<float> (N / 2);
        outL = sumL * std::sqrt (outNorm);
        outR = sumR * std::sqrt (outNorm);
    }

    void process (float inL, float inR, float& outL, float& outR) noexcept
    {
        process (inL, inR, outL, outR, [] (std::size_t, float v) { return v; });
    }

    // Per-line input and output, for machines that feed and tap each line through their
    // own filters (Nebula's velvet-noise filters). in[i] is added to line i; out[i] is
    // what line i delivers this sample.
    void processLines (const std::array<float, N>& in, std::array<float, N>& out) noexcept
    {
        step ([&] (std::size_t i) { return in[i]; },
              [&] (std::size_t i, float y) { out[i] = y; },
              [] (std::size_t, float v) { return v; });
    }

private:
    template <typename Inject, typename Tap, typename LineHook>
    void step (Inject&& inject, Tap&& tap, LineHook&& hook) noexcept
    {
        const float depth = std::max (modDepth.next(), tailDepth.next());
        const float in = inputGain.next();
        std::array<float, N> y {};
        for (std::size_t i = 0; i < N; ++i)
        {
            const float wobble = useDrift ? drifts[i].next() : lfos[i].next();
            const float d = delays[i].next() + depth * (0.5f + 0.5f * wobble) + 2.0f;
            y[i] = lines[i].readFractional (d);
            tap (i, y[i]);
            float v = decay[i].process (y[i], aLow, aHigh, gLow[i].next(), gMid[i].next(), gHigh[i].next());
            if (smearOn)
                v = smear[i].process (v);
            if (loopAllpassGain > 0.0f)
            {
                // The length glides when the size changes (a jump would click); once
                // settled it is a whole number of samples and needs no interpolation.
                auto& apd = loopAllpassDelay[i];
                if (apd.isSettled())
                    v = loopAllpass[i].process (v, static_cast<std::size_t> (apd.getTarget()), loopAllpassGain);
                else
                    v = loopAllpass[i].processModulated (v, apd.next(), loopAllpassGain);
            }
            y[i] = hook (i, v);
        }
        if (matrix != nullptr)
            mix (y);
        else
            hadamard (y);
        rotate (y);
        for (std::size_t i = 0; i < N; ++i)
            lines[i].push (loopGuard (y[i] + in * inject (i)));
    }

public:
    // Energy loss of the loop is bounded by the decay gains, so the network is stable for
    // any setting where every gain is <= 1 (the mixing matrix is orthogonal).
    static constexpr float kMaxModMs = 3.0f;
    float maxLoopAllpass = 8.0f;

    // Replace the Hadamard mix with another orthogonal matrix (row-major, N * N), or
    // nullptr to go back. The caller keeps the array alive. Clear Night uses this for its
    // optimised colourless matrix.
    void setMatrix (const std::array<float, N * N>* m) noexcept { matrix = m; }

private:
    void mix (std::array<float, N>& y) const noexcept
    {
        std::array<float, N> out {};
        const auto& a = *matrix;
        for (std::size_t r = 0; r < N; ++r)
        {
            float sum = 0.0f;
            for (std::size_t c = 0; c < N; ++c)
                sum += a[r * N + c] * y[c];
            out[r] = sum;
        }
        y = out;
    }

    void rotate (std::array<float, N>& y) noexcept
    {
        constexpr std::size_t H = N / 2;
        if (rotCounter == 0)
        {
            // Every few samples, work out where each angle will be at the end of the next
            // block and glide (cos, sin) there in straight steps. A straight step between
            // two points on the unit circle stays inside it, so the mix can only lose a
            // hair of energy, never gain it.
            rotCounter = kRotationBlock;
            const float amount = rotation.skip (static_cast<int> (kRotationBlock)) * kMaxRotation;
            for (std::size_t k = 0; k < H; ++k)
            {
                rotPhase[k] += rotStep[k] * rotRate;
                if (rotPhase[k] >= 1.0f) rotPhase[k] -= 1.0f;
                const float angle = amount * std::sin (6.28318531f * rotPhase[k]);
                const float inv = 1.0f / static_cast<float> (kRotationBlock);
                rotCosStep[k] = (std::cos (angle) - rotCos[k]) * inv;
                rotSinStep[k] = (std::sin (angle) - rotSin[k]) * inv;
            }
        }
        --rotCounter;
        for (std::size_t k = 0; k < H; ++k)
        {
            float c = rotCos[k] += rotCosStep[k];
            float s = rotSin[k] += rotSinStep[k];
            // Pull the straight-line step back towards the unit circle. (3 - r^2) / 2 is one
            // Newton step for 1 / r, which never overshoots: the pair stays at or inside the
            // circle, so faster rotations don't shorten the tail and a freeze never grows.
            const float g = 1.5f - 0.5f * (c * c + s * s);
            c *= g;
            s *= g;
            const float u = y[k], v = y[k + H];
            y[k] = c * u - s * v;
            y[k + H] = s * u + c * v;
        }
    }

    void updateRotation()
    {
        const float rt = decaySettings.rtMid;
        const float t = frozen ? 1.0f : std::clamp ((rt - kTailStart) / (kTailFull - kTailStart), 0.0f, 1.0f);
        const float tail = t * t * (3.0f - 2.0f * t);
        rotation.setTarget (std::max (modAmount, tail) * (1.0f + tail));
        // Delay drift bends pitch, so it is capped at kMaxTailCents on the fastest line; any
        // movement asked for beyond that goes into the rotation instead, which doesn't.
        const float wantMs = tail * kTailDriftMs * tailMovement;
        const float driftMs = std::min ({ wantMs, driftCapped ? maxTailDriftMs : kMaxModMs, kMaxModMs });
        const float extra = wantMs > driftMs ? wantMs / driftMs : 1.0f;
        rotRate = (1.0f + (kTailRate - 1.0f) * tail * tailMovement) * extra;
        tailDepth.setTarget (msToSamples (driftMs, fs));
        updateSmear();
    }

    void updateGains()
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            // An allpass delays the signal by its length on average, so it counts toward the loop time.
            const float d = delays[i].getTarget()
                          + (loopAllpassGain > 0.0f ? loopAllpassDelay[i].getTarget() : 0.0f);
            if (frozen)
            {
                gLow[i].setTarget (1.0f);
                gMid[i].setTarget (1.0f);
                gHigh[i].setTarget (1.0f);
            }
            else
            {
                gLow[i].setTarget (decayGain (d, decaySettings.rtLow, fs));
                gMid[i].setTarget (decayGain (d, decaySettings.rtMid, fs));
                gHigh[i].setTarget (decayGain (d, decaySettings.rtHigh, fs));
            }
        }
    }

    double fs = 48000.0;
    float maxDelay = 0.0f;
    std::array<DelayLine, N> lines;
    std::array<Allpass, N> loopAllpass;
    std::array<Smoother, N> loopAllpassDelay;
    float loopAllpassGain = 0.0f;
    std::array<ThreeBandDecay, N> decay;
    std::array<SweptAllpass, N> smear;
    float smearOctaves = 0.0f;
    bool smearOn = false;
    void updateSmear() noexcept
    {
        const float rt = decaySettings.rtMid;
        const float t = frozen ? 1.0f : std::clamp ((rt - kTailStart) / (kTailFull - kTailStart), 0.0f, 1.0f);
        const float oct = smearOctaves;
        smearOn = oct > 0.0f;
        for (auto& a : smear) a.setDepth (oct * t * t * (3.0f - 2.0f * t));
    }
    std::array<Smoother, N> delays, gLow, gMid, gHigh;
    std::array<Lfo, N> lfos;
    std::array<DriftLfo, N> drifts;
    bool useDrift = false;
    float tailMovement = 1.0f;
    std::array<float, N> signs {};
    Smoother inputGain, modDepth, rotation, tailDepth;
    float modAmount = 0.0f, rotRate = 1.0f;
    static constexpr float kTailRate = 2.0f, kTailDriftMs = 1.0f; // "Middle" long-tail movement
    // Long-tail pitch drift stays within the "lush but not wobbly" range (3 to 8 cents is
    // the usual target; 10 to 30 cents reads as chorus). Capping it and turning the
    // rotation faster by the same factor kept ringing scores level on average (3.06 vs
    // 3.08 over the old Chamber, Arena and Rise, and Halo), with less pitch bend.
    static constexpr float kMaxTailCents = 8.0f;
    float maxTailDriftMs = 1.27f;
    bool driftCapped = true;
    static constexpr float kTailStart = 3.0f, kTailFull = 8.0f; // seconds of decay
    const std::array<float, N * N>* matrix = nullptr;
    std::array<float, N / 2> rotCos {}, rotSin {}, rotCosStep {}, rotSinStep {}, rotPhase {}, rotStep {};
    std::size_t rotCounter = 0;
    static constexpr std::size_t kRotationBlock = 16;
    static constexpr float kMaxRotation = 0.785398f; // 45 degrees at full Mod
    DecaySettings decaySettings;
    float aLow = 0.0f, aHigh = 0.0f;
    bool frozen = false;
};

// Chain of allpass diffusers for one channel, optionally modulated ("smear").
template <std::size_t N>
class Diffuser
{
public:
    void prepare (double sampleRate, const std::array<float, N>& delaysMs, float maxStretch = 1.0f)
    {
        fs = sampleRate;
        for (std::size_t i = 0; i < N; ++i)
        {
            baseDelays[i] = msToSamples (delaysMs[i], fs);
            allpasses[i].prepare (static_cast<std::size_t> (baseDelays[i] * maxStretch + msToSamples (2.0f, fs)) + 8);
            lfos[i].prepare (fs);
            lfos[i].setFrequency (0.3f + 0.13f * static_cast<float> (i));
            lfos[i].setPhase (0.37f * static_cast<float> (i));
        }
        maxScale = maxStretch;
    }

    void reset() { for (auto& a : allpasses) a.reset(); }

    // scale stretches every delay (0.25..maxScale), smearMs adds modulation.
    float process (float x, float coefficient, float scale = 1.0f, float smearMs = 0.0f) noexcept
    {
        const float smear = msToSamples (smearMs, fs);
        const float s = std::clamp (scale, 0.25f, maxScale);
        for (std::size_t i = 0; i < N; ++i)
        {
            const float d = std::max (3.0f, baseDelays[i] * s + (smear > 0.0f ? smear * (0.5f + 0.5f * lfos[i].next()) : 0.0f));
            x = allpasses[i].processModulated (x, d, coefficient);
        }
        return x;
    }

private:
    double fs = 48000.0;
    float maxScale = 1.0f;
    std::array<float, N> baseDelays {};
    std::array<Allpass, N> allpasses;
    std::array<Lfo, N> lfos;
};

// Multi-tap early reflections read from one delay line. Tap times glide smoothly when
// they change; once settled, taps sit on whole samples and use a cheap direct read.
template <std::size_t MaxTaps>
class TapDelay
{
public:
    void prepare (double sampleRate, float maxMs)
    {
        fs = sampleRate;
        maxSamples = msToSamples (maxMs, fs);
        line.prepare (static_cast<std::size_t> (maxSamples) + 8);
        for (auto& t : times) t.prepare (fs, 40.0f);
        for (auto& g : gains) g.prepare (fs, 40.0f);
    }

    void reset()
    {
        line.reset();
        snap();
    }

    void setTap (std::size_t i, float ms, float gain)
    {
        // Whole-sample targets: identical sound, and a settled tap needs no interpolation.
        times[i].setTarget (std::round (std::clamp (msToSamples (ms, fs), 2.0f, std::max (2.0f, maxSamples))));
        gains[i].setTarget (gain);
        moving = true;
    }

    void snap()
    {
        for (auto& t : times) t.snapTo (t.getTarget());
        for (auto& g : gains) g.snapTo (g.getTarget());
        settle();
    }

    void setNumTaps (std::size_t n) { numTaps = std::min (n, MaxTaps); }

    // Write the input without reading any taps (keeps an idle bank ready to use).
    void push (float x) noexcept { line.push (x); }

    float process (float x) noexcept
    {
        line.push (x);
        float y = 0.0f;
        if (! moving)
        {
            for (std::size_t i = 0; i < numTaps; ++i)
                y += fixedGains[i] * line.read (fixedTimes[i]);
            return y;
        }

        bool stillMoving = false;
        for (std::size_t i = 0; i < numTaps; ++i)
        {
            const float t = times[i].next();
            const float g = gains[i].next();
            stillMoving = stillMoving || ! times[i].isSettled() || ! gains[i].isSettled();
            y += g * line.readFractional (t);
        }
        if (! stillMoving)
            settle();
        return y;
    }

private:
    void settle()
    {
        for (std::size_t i = 0; i < MaxTaps; ++i)
        {
            fixedTimes[i] = static_cast<std::size_t> (std::max (2.0f, times[i].getTarget()));
            fixedGains[i] = gains[i].getTarget();
        }
        moving = false;
    }

    double fs = 48000.0;
    float maxSamples = 0.0f;
    DelayLine line;
    std::array<Smoother, MaxTaps> times, gains;
    std::array<std::size_t, MaxTaps> fixedTimes {};
    std::array<float, MaxTaps> fixedGains {};
    std::size_t numTaps = MaxTaps;
    bool moving = false;
};

// tanh saturation with first-order antiderivative antialiasing (Parker, Zavalishin,
// Le Bivic, "Reducing the aliasing of nonlinear waveshaping using continuous-time
// convolution", DAFx 2016). Much less aliasing than plain tanh at no oversampling cost.
class AdaaTanh
{
public:
    void reset() { x1 = 0.0f; f1 = 0.0f; }

    float process (float x) noexcept
    {
        const float f = antiderivative (x);
        const float dx = x - x1;
        float y;
        if (std::abs (dx) > 1.0e-4f)
            y = (f - f1) / dx;
        else
            y = std::tanh (0.5f * (x + x1));
        x1 = x;
        f1 = f;
        return y;
    }

private:
    // log(cosh(x)), computed without overflow.
    static float antiderivative (float x) noexcept
    {
        const float ax = std::abs (x);
        return ax + std::log1p (std::exp (-2.0f * ax)) - std::numbers::ln2_v<float>;
    }

    float x1 = 0.0f, f1 = 0.0f;
};

// Peak envelope follower with separate attack and release.
class EnvelopeFollower
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs)
    {
        att = std::exp (-1.0f / (attackMs * 0.001f * static_cast<float> (sampleRate)));
        rel = std::exp (-1.0f / (releaseMs * 0.001f * static_cast<float> (sampleRate)));
    }

    void reset() { env = 0.0f; }

    float process (float x) noexcept
    {
        const float a = std::abs (x);
        env = a > env ? a + att * (env - a) : a + rel * (env - a);
        return env;
    }

private:
    float att = 0.0f, rel = 0.0f, env = 0.0f;
};

// Two-head granular pitch shifter. Each head reads the delay line through a grain window,
// and the two heads are half a grain apart, so one fades in as the other fades out.
//
// Window: the Vorbis power-complementary window, w = sin(pi/2 * sin^2(pi * phase)). It
// starts and ends with zero slope (no corners, so no clicks or buzz at grain edges), and
// the two heads' windows satisfy w1^2 + w2^2 = 1 exactly.
//
// Overlap-add gain: what the heads read is usually unrelated (a reverb tail half a grain
// apart), and then w1^2 + w2^2 = 1 keeps the level steady. When they read the same thing
// (a held sine, a frozen chord) the amplitudes add instead, which would bulge by up to
// 3 dB mid-crossfade. So the shifter tracks how alike the two heads are (their
// correlation, 0 to 1) and divides by the true output level, sqrt(1 + 2 r w1 w2). Either
// way the output level matches the input with no flutter at the grain rate, and inside a
// feedback loop it never adds gain.
class PitchShifter
{
public:
    void prepare (double sampleRate, float windowMs)
    {
        window = msToSamples (windowMs, sampleRate);
        line.prepare (static_cast<std::size_t> (window) + 16);
        // Correlation is averaged over about one grain.
        corrCoeff = 1.0f - std::exp (-1.0f / window);
    }

    void reset()
    {
        line.reset();
        phase = 0.0f;
        eAA = eBB = eAB = 0.0f;
    }

    void setRatio (float r) noexcept { step = (1.0f - r) / window; }

    float process (float x) noexcept
    {
        line.push (x);
        phase += step;
        phase -= std::floor (phase);
        const float p2 = phase + 0.5f >= 1.0f ? phase - 0.5f : phase + 0.5f;
        const float a = line.readFractional (3.0f + phase * window);
        const float b = line.readFractional (3.0f + p2 * window);

        const float s = std::sin (std::numbers::pi_v<float> * phase);
        const float u = 0.5f * std::numbers::pi_v<float> * s * s;
        const float w1 = std::sin (u), w2 = std::cos (u);

        eAA += corrCoeff * (a * a - eAA);
        eBB += corrCoeff * (b * b - eBB);
        eAB += corrCoeff * (a * b - eAB);
        const float r = std::clamp (eAB / std::sqrt (eAA * eBB + 1.0e-20f), 0.0f, 1.0f);
        return (w1 * a + w2 * b) / std::sqrt (1.0f + 2.0f * r * w1 * w2);
    }

private:
    DelayLine line;
    float window = 2400.0f;
    float phase = 0.0f;
    float step = 0.0f;
    float corrCoeff = 0.001f;
    float eAA = 0.0f, eBB = 0.0f, eAB = 0.0f;
};

} // namespace sing::dsp
