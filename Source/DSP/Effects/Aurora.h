#pragma once

#include "ReverbBlocks.h"

#include <cstdint>

namespace sing::reverb
{

// Aurora's settings, in physical units (Overcast's MachineParams).
struct MachineParams
{
    float decaySeconds = 3.0f;   // RT60, 0.1 to 60 s
    float mod = 0.5f;            // 0 to 1, tail modulation depth
    float param1 = 0.0f;         // 0 to 1: size
    float param2 = 0.5f;         // 0 to 1: diffusion
    bool freeze = false;         // hold the tail indefinitely
};

// Copied from Overcast (source/dsp/machines/Aurora.h), unchanged but for the namespace and
// dropping Overcast's machine interface.
//
// Aurora: a huge, lush wash for pads and ambient sound. The core follows Cloud Seed by
// Ghost Note Audio (Valdemar Erlingsson), MIT licensed: https://github.com/GhostNoteAudio/
// CloudSeedCore. Each channel runs eight independent feedback delay lines in parallel,
// and every line has its own chain of six short, slowly modulated allpass diffusers in its loop,
// so each pass round a line smears the sound further. The left and right channels use
// different line lengths, so the tail is wide even from a mono source.
//
// Changes from Cloud Seed:
// - Per-sample feedback (Cloud Seed feeds back one host block late, so its sound
//   depended on the buffer size).
// - Line lengths are a fixed, hand-checked set: no two lines in the machine are near a
//   2:1, 3:2 or 4:3 ratio, so their resonances don't stack.
// - Decay is frequency dependent (three bands, gains worked out per line so every line
//   decays at the same rate in each band) instead of fixed shelf cuts per pass.
// - Every diffuser is under 10 ms (more, shorter stages instead of a few long ones).
// - Delay lengths glide when Size moves, and every loop has the safety guard.
// - Long decays keep a little modulation even at Mod 0 so the tail never rings, always
//   at Cloud Seed's slow rates (well under 1 Hz) so held notes don't swirl.
//
// param1 = size (line length, about 30 to 300 ms), param2 = diffusion (how much each
// pass smears: from distinct echoes to a soft cloud).
class Aurora final
{
public:
    static constexpr std::size_t kLines = 8;    // per channel
    static constexpr std::size_t kStages = 6;   // diffusers inside each line
    static constexpr std::size_t kEarly = 8;    // input diffusers per channel
    static constexpr float kMinSizeMs = 30.0f, kMaxSizeMs = 300.0f;
    // Every diffuser is under 10 ms: Cloud Seed's Dark Plate uses up to 32 ms in its loops,
    // but many short stages give the same density without a metallic colour.
    static constexpr float kStageMaxMs = 9.5f;
    static constexpr float kEarlyMaxMs = 9.5f;
    static constexpr float kLineModMs = 1.0f, kStageModMs = 0.5f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const auto lineMax = static_cast<std::size_t> (msToSamples (kMaxSizeMs * 1.6f + kLineModMs, fs)) + 8;
        const auto stageMax = static_cast<std::size_t> (msToSamples (kStageMaxMs + kStageModMs, fs)) + 8;
        const auto earlyMax = static_cast<std::size_t> (msToSamples (kEarlyMaxMs + kStageModMs, fs)) + 8;
        for (std::size_t c = 0; c < 2; ++c)
        {
            auto& ch = channels[c];
            const auto& mult = c == 0 ? kMultL : kMultR;
            // Cloud Seed seeds each line's diffusers from (seed * (line + 1)); left and right
            // take the two halves of its cross-seeded series.
            for (std::size_t i = 0; i < kLines; ++i)
            {
                auto& line = ch.lines[i];
                line.delay.prepare (lineMax);
                line.size.prepare (fs, 120.0f);
                line.multiplier = mult[i];
                const auto r = seriesFor (kPostDiffusionSeed * static_cast<std::uint64_t> (i + 1), 3 * kStages, c);
                for (std::size_t s = 0; s < kStages; ++s)
                {
                    line.stages[s].prepare (stageMax);
                    line.stageMs[s] = kStageMaxMs * 0.1f * std::pow (10.0f, r[s]); // 0.1 to 1 of the longest
                    line.stageBase[s] = msToSamples (line.stageMs[s], fs);
                    line.stageDepth[s] = 0.85f + 0.3f * r[kStages + s];
                    line.stageRate[s] = 0.85f + 0.3f * r[2 * kStages + s];
                    line.stagePhase[s] = r[(s * 7 + 3) % (3 * kStages)];
                }
                const auto m = seriesFor (kDelaySeed + i, 3, c);
                line.modDepth = 0.7f + 0.3f * m[0];
                line.modRate = 0.7f + 0.3f * m[1];
                line.phase = m[2];
                line.gLow.prepare (fs, 30.0f);
                line.gMid.prepare (fs, 30.0f);
                line.gHigh.prepare (fs, 30.0f);
            }
            const auto e = seriesFor (kDiffusionSeed, 3 * kEarly, c);
            for (std::size_t s = 0; s < kEarly; ++s)
            {
                ch.early[s].prepare (earlyMax);
                ch.earlyMs[s] = kEarlyMaxMs * 0.1f * std::pow (10.0f, e[s]);
                ch.earlyBase[s] = msToSamples (ch.earlyMs[s], fs);
                ch.earlyPhase[s] = e[kEarly + s];
                ch.earlyRate[s] = 0.85f + 0.3f * e[2 * kEarly + s];
            }
        }
        modAmount.prepare (fs, 50.0f);
        lateDiffusion.prepare (fs, 30.0f);
        earlyDiffusion.prepare (fs, 30.0f);
        inputGain.prepare (fs, 50.0f);
        aLow = OnePoleLowpass::coefficientFor (kLowHz, fs);
        aHigh = OnePoleLowpass::coefficientFor (kHighHz, fs);
        setParams (params);
        reset();
    }

    void reset()
    {
        for (auto& ch : channels)
        {
            for (auto& line : ch.lines)
            {
                line.delay.reset();
                for (auto& st : line.stages) st.reset();
                line.decay.reset();
                line.size.snapTo (line.size.getTarget());
                line.gLow.snapTo (line.gLow.getTarget());
                line.gMid.snapTo (line.gMid.getTarget());
                line.gHigh.snapTo (line.gHigh.getTarget());
            }
            for (auto& st : ch.early) st.reset();
        }
        for (auto* sm : { &modAmount, &lateDiffusion, &earlyDiffusion, &inputGain })
            sm->snapTo (sm->getTarget());
        modCounter = 0;
    }

    void setParams (const MachineParams& p)
    {
        params = p;
        const float sizeMs = kMinSizeMs * std::pow (kMaxSizeMs / kMinSizeMs, std::clamp (p.param1, 0.0f, 1.0f));
        const float diff = std::clamp (p.param2, 0.0f, 1.0f);
        // Dark Plate runs its loop diffusers at 0.85; below about 0.4 the echoes stand apart.
        lateDiffusion.setTarget (0.35f + 0.5f * diff);
        earlyDiffusion.setTarget (0.7f * diff);
        inputGain.setTarget (p.freeze ? 0.0f : 1.0f);

        const float t = p.freeze ? 1.0f : std::clamp ((p.decaySeconds - kTailStart) / (kTailFull - kTailStart), 0.0f, 1.0f);
        const float tail = t * t * (3.0f - 2.0f * t);
        modAmount.setTarget (std::max (std::clamp (p.mod, 0.0f, 1.0f), kTailMod * tail));

        const float rt = std::max (0.1f, p.decaySeconds);
        for (auto& ch : channels)
        {
            for (auto& line : ch.lines)
            {
                const float d = msToSamples (sizeMs * line.multiplier, fs);
                line.size.setTarget (d);
                // The diffusers delay the sound by their lengths on average, so they count
                // toward the loop time.
                float loop = d;
                for (const float ms : line.stageMs) loop += msToSamples (ms, fs);
                if (p.freeze)
                {
                    line.gLow.setTarget (1.0f);
                    line.gMid.setTarget (1.0f);
                    line.gHigh.setTarget (1.0f);
                }
                else
                {
                    line.gLow.setTarget (decayGain (loop, rt * kLowRatio, fs));
                    line.gMid.setTarget (decayGain (loop, rt, fs));
                    line.gHigh.setTarget (decayGain (loop, rt * kHighRatio, fs));
                }
            }
        }
    }

    void process (const float* inL, const float* inR, float* wetL, float* wetR, int n)
    {
        const float outGain = kOutputScale / std::sqrt (static_cast<float> (kLines));
        for (int i = 0; i < n; ++i)
        {
            if (modCounter == 0)
                updateModulation();
            --modCounter;
            const float g = inputGain.next();
            const float late = lateDiffusion.next(), early = earlyDiffusion.next();
            // Cloud Seed's input mix: each channel takes a quarter of the other side.
            const float l = inL[i] * g, r = inR[i] * g;
            const float in[2] = { 0.75f * l + 0.25f * r, 0.75f * r + 0.25f * l };
            float out[2] = { 0.0f, 0.0f };

            for (std::size_t c = 0; c < 2; ++c)
            {
                auto& ch = channels[c];
                float x = in[c];
                for (std::size_t s = 0; s < kEarly; ++s)
                    x = ch.early[s].processModulated (x, ch.earlyDelay[s], early);

                for (auto& line : ch.lines)
                {
                    // Size glides every sample so moving it never zips; modulation steps.
                    float v = line.delay.readFractional (std::max (4.0f, line.size.next() + line.modOffset));
                    for (std::size_t s = 0; s < kStages; ++s)
                        v = line.stages[s].processModulated (v, line.stageDelay[s], late);
                    out[c] += v; // Cloud Seed's "post diffuser" tap
                    v = line.decay.process (v, aLow, aHigh, line.gLow.next(), line.gMid.next(), line.gHigh.next());
                    line.delay.push (loopGuard (x + v));
                }
            }
            wetL[i] = out[0] * outGain;
            wetR[i] = out[1] * outGain;
        }
    }

private:
    static float wrap (float p) noexcept { return p >= 1.0f ? p - 1.0f : p; }

    // Like Cloud Seed, work out the modulated delay times every few samples and hold them
    // in between. The rates are well under 1 Hz, so each step is a few hundredths of a
    // sample: inaudible, and it saves most of the cost.
    void updateModulation() noexcept
    {
        modCounter = kModBlock;
        const float mod = modAmount.skip (kModBlock);
        const float lineModSamples = msToSamples (kLineModMs, fs) * mod;
        const float stageModSamples = msToSamples (kStageModMs, fs) * mod;
        const float step = (kMinRateHz + kRateSpanHz * mod) * static_cast<float> (kModBlock) / static_cast<float> (fs);
        for (auto& ch : channels)
        {
            for (std::size_t s = 0; s < kEarly; ++s)
            {
                ch.earlyPhase[s] = wrap (ch.earlyPhase[s] + step * ch.earlyRate[s]);
                ch.earlyDelay[s] = std::max (3.0f, ch.earlyBase[s] + stageModSamples * std::sin (kTwoPi * ch.earlyPhase[s]));
            }
            for (auto& line : ch.lines)
            {
                line.phase = wrap (line.phase + step * line.modRate);
                line.modOffset = lineModSamples * line.modDepth * std::sin (kTwoPi * line.phase);
                for (std::size_t s = 0; s < kStages; ++s)
                {
                    line.stagePhase[s] = wrap (line.stagePhase[s] + step * line.stageRate[s]);
                    line.stageDelay[s] = std::max (3.0f, line.stageBase[s] + stageModSamples * line.stageDepth[s] * std::sin (kTwoPi * line.stagePhase[s]));
                }
            }
        }
    }

    // Cloud Seed's random series: a 32-bit LCG, with the left channel reading the series
    // from ~seed and the right from seed (its default cross-seed of 0).
    static std::array<float, 24> seriesFor (std::uint64_t seed, std::size_t count, std::size_t channel)
    {
        std::array<float, 24> out {};
        std::uint64_t x = channel == 0 ? ~seed : seed;
        for (std::size_t k = 0; k < count && k < out.size(); ++k)
        {
            x = (22695477ull * x + 1ull) & 0xFFFFFFFFull;
            out[k] = static_cast<float> (static_cast<double> (x) / 4294967295.0);
        }
        return out;
    }

    struct Line
    {
        DelayLine delay;
        std::array<Allpass, kStages> stages;
        std::array<float, kStages> stageMs {}, stageBase {}, stageDelay {}, stageDepth {}, stageRate {}, stagePhase {};
        ThreeBandDecay decay;
        Smoother size, gLow, gMid, gHigh;
        float multiplier = 1.0f, modDepth = 1.0f, modRate = 1.0f, phase = 0.0f, modOffset = 0.0f;
    };

    struct Channel
    {
        std::array<Line, kLines> lines;
        std::array<Allpass, kEarly> early;
        std::array<float, kEarly> earlyMs {}, earlyBase {}, earlyDelay {}, earlyPhase {}, earlyRate {};
    };

    // Line lengths as multiples of Size: geometric from 0.49 to 1.5, alternating between
    // the channels, nudged so no two of the sixteen sit within 1% of 2:1, 3:2 or 4:3.
    static constexpr std::array<float, kLines> kMultL { 0.4885f, 0.5656f, 0.6581f, 0.762f, 0.8866f, 1.0265f, 1.2041f, 1.3941f };
    static constexpr std::array<float, kLines> kMultR { 0.5256f, 0.6085f, 0.7081f, 0.8199f, 0.954f, 1.1045f, 1.2956f, 1.5f };
    // Seeds from Cloud Seed's Dark Plate program.
    static constexpr std::uint64_t kDelaySeed = 218, kPostDiffusionSeed = 365, kDiffusionSeed = 185;
    static constexpr float kTailStart = 3.0f, kTailFull = 8.0f;     // seconds of decay
    // Modulation kept at long decays. Raised from 0.35 after Aurora scored 12.9 for ringing
    // at 12 s (the highest of the delay-network machines); 0.65 brings it to about 5.7.
    static constexpr float kTailMod = 0.65f;
    static constexpr float kMinRateHz = 0.06f, kRateSpanHz = 0.35f; // Cloud Seed's slow rates
    static constexpr float kLowHz = 250.0f, kHighHz = 3500.0f;
    static constexpr float kLowRatio = 1.1f, kHighRatio = 0.55f;    // band decay times vs Decay
    static constexpr float kOutputScale = 1.0f;
    static constexpr int kModBlock = 8;

    double fs = 48000.0;
    std::array<Channel, 2> channels;
    Smoother modAmount, lateDiffusion, earlyDiffusion, inputGain;
    float aLow = 0.0f, aHigh = 0.0f;
    int modCounter = 0;
    MachineParams params;
};

} // namespace sing::reverb
