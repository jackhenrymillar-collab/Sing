#pragma once

#include "VoiceData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace sing::dsp
{

enum class LoopMode { forward = 0, backward = 1, backAndForth = 2 };

// The loop in level-0 samples (see VoiceData).
struct Loop
{
    double start = 0.0, end = 1.0, fade = 0.0;
    LoopMode mode = LoopMode::forward;

    double length() const noexcept { return end - start; }
};

// Small, fast random numbers for grains and start offsets.
class Random
{
public:
    explicit Random (std::uint32_t seed = 1u) noexcept : state (seed * 2654435761u + 1u) {}
    void seed (std::uint32_t s) noexcept { state = s * 2654435761u + 1u; }

    float unit() noexcept // 0..1
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float> (state >> 8) * (1.0f / 16777216.0f);
    }

    float bipolar() noexcept { return 2.0f * unit() - 1.0f; }

private:
    std::uint32_t state;
};

// sin(x * pi / 2) for x in 0..1, from a table (error below 1e-7). Fades and grain windows
// need one per sample per read, where std::sin would cost more than the read itself.
inline float quarterSine (float x) noexcept
{
    static constexpr int kSize = 4096;
    static const auto table = []
    {
        std::array<float, kSize + 2> t {};
        for (int i = 0; i <= kSize + 1; ++i)
            t[static_cast<std::size_t> (i)] = static_cast<float> (std::sin (0.5 * std::numbers::pi * std::min (i, kSize) / kSize));
        return t;
    }();
    const float p = std::clamp (x, 0.0f, 1.0f) * kSize;
    const int i = static_cast<int> (p);
    const float f = p - static_cast<float> (i);
    const float a = table[static_cast<std::size_t> (i)], b = table[static_cast<std::size_t> (i + 1)];
    return a + f * (b - a);
}

inline void equalPowerGains (double t, float& out, float& in) noexcept
{
    const float x = static_cast<float> (std::clamp (t, 0.0, 1.0));
    out = quarterSine (1.0f - x);
    in = quarterSine (x);
}

// Hann window at phase 0..1: sin^2(pi * phase).
inline float hann (float phase) noexcept
{
    const float s = quarterSine (phase < 0.5f ? 2.0f * phase : 2.0f - 2.0f * phase);
    return s * s;
}

// Plays the loop straight through, the plain sampler way.
//
// The fade overlaps the loop's two ends: as the playhead nears the end it fades out while
// a second read, starting from the loop's start (or, for back and forth, from the end
// heading back), fades in. When the fade finishes the playhead carries on from where that
// second read had got to, so nothing ever jumps. Fades are equal power, the right law for
// two different stretches of a voice.
//
// If the loop is moved so the playhead ends up outside it, the playhead moves inside and
// the old position fades out over a few milliseconds, so knob moves never click.
class LoopPlayer
{
public:
    void start (const Loop& loop, double position, int direction) noexcept
    {
        pos = std::clamp (position, loop.start, loop.end);
        dir = direction >= 0 ? 1 : -1;
        ghostGain = 0.0f;
    }

    double position() const noexcept { return pos; }
    int direction() const noexcept { return dir; }

    // ghostStep: how much the declick fade moves per sample (1 / its length in samples).
    // Reader is VoiceData (anything with read (position, ratio) works, for comparisons).
    template <typename Reader>
    float next (const Reader& v, const Loop& loop, double ratio, float ghostStep) noexcept
    {
        if (loop.mode == LoopMode::forward) dir = 1;
        else if (loop.mode == LoopMode::backward) dir = -1;

        if (pos < loop.start - 0.5 || pos > loop.end + 0.5)
        {
            startGhost();
            pos = dir > 0 ? loop.start : loop.end;
        }

        const double fade = std::max (1.0, loop.fade);
        float out;
        if (dir > 0)
        {
            const double zone = loop.end - fade;
            if (pos >= zone)
            {
                const double into = pos - zone;
                const double partner = loop.mode == LoopMode::forward ? loop.start + into : loop.end - into;
                float gOut, gIn;
                equalPowerGains (into / fade, gOut, gIn);
                out = gOut * v.read (pos, ratio) + gIn * v.read (partner, ratio);
            }
            else
            {
                out = v.read (pos, ratio);
            }

            pos += ratio;
            if (pos >= loop.end)
            {
                const double over = std::min (pos - loop.end, loop.length() - fade);
                if (loop.mode == LoopMode::forward)
                    pos = loop.start + fade + over;
                else
                {
                    pos = loop.end - fade - over;
                    dir = -1;
                }
            }
        }
        else
        {
            const double zone = loop.start + fade;
            if (pos <= zone)
            {
                const double into = zone - pos;
                const double partner = loop.mode == LoopMode::backward ? loop.end - into : loop.start + into;
                float gOut, gIn;
                equalPowerGains (into / fade, gOut, gIn);
                out = gOut * v.read (pos, ratio) + gIn * v.read (partner, ratio);
            }
            else
            {
                out = v.read (pos, ratio);
            }

            pos -= ratio;
            if (pos <= loop.start)
            {
                const double over = std::min (loop.start - pos, loop.length() - fade);
                if (loop.mode == LoopMode::backward)
                    pos = loop.end - fade - over;
                else
                {
                    pos = loop.start + fade + over;
                    dir = 1;
                }
            }
        }

        if (ghostGain > 0.0f)
        {
            float gOut, gIn;
            equalPowerGains (1.0 - ghostGain, gOut, gIn);
            out = gIn * out + gOut * v.read (ghostPos, ratio);
            ghostPos += ghostDir * ratio;
            ghostGain -= ghostStep;
        }
        return out;
    }

private:
    void startGhost() noexcept
    {
        ghostPos = pos;
        ghostDir = dir;
        ghostGain = 1.0f;
    }

    double pos = 0.0;
    int dir = 1;
    double ghostPos = 0.0;
    double ghostDir = 1.0;
    float ghostGain = 0.0f;
};

// Plays the loop as a cloud of overlapping grains, for Stretch and Hold.
//
// A scan point moves through the loop (in the loop's direction) slower than normal
// playback, down to standing still. Every quarter of a grain length a new grain starts
// near the scan point, with a little random offset so held sounds don't buzz. Each grain
// reads at the note's pitch through the same band-limited reader as normal playback and is
// shaped with a Hann window. Four Hann windows a quarter apart add up to a constant, so the
// level stays steady whether neighbouring grains are alike or not.
class GrainCloud
{
public:
    static constexpr int kGrains = 4;

    void start (const Loop& loop, double position, int direction, std::uint32_t seed) noexcept
    {
        scan = std::clamp (position, loop.start, loop.end);
        scanDir = direction >= 0 ? 1 : -1;
        rng.seed (seed);
        for (auto& g : grains) g.active = false;
        countdown = 0.0f;
        next = 0;
    }

    double position() const noexcept { return scan; }
    int direction() const noexcept { return scanDir; }

    // grainSamples: grain length in output samples. scanRate: level-0 samples the scan point
    // moves per output sample. jitter: random start offset range in level-0 samples.
    float process (const VoiceData& v, const Loop& loop, double ratio, double scanRate, float grainSamples, double jitter) noexcept
    {
        if (loop.mode == LoopMode::forward) scanDir = 1;
        else if (loop.mode == LoopMode::backward) scanDir = -1;
        scan = std::clamp (scan, loop.start, loop.end);

        if (countdown <= 0.0f)
        {
            spawn (loop, ratio, grainSamples, jitter);
            countdown += grainSamples * (1.0f / kGrains);
        }
        countdown -= 1.0f;

        float out = 0.0f;
        for (auto& g : grains)
        {
            if (! g.active)
                continue;
            const float w = hann (g.phase);
            out += w * v.read (g.pos, ratio);
            g.pos += g.dir * ratio;
            g.phase += g.step;
            if (g.phase >= 1.0f)
                g.active = false;
        }

        advanceScan (loop, scanRate);
        return 0.5f * out; // four Hann windows a quarter apart sum to 2
    }

private:
    struct Grain
    {
        double pos = 0.0;
        double dir = 1.0;
        float phase = 0.0f, step = 0.0f;
        bool active = false;
    };

    void spawn (const Loop& loop, double ratio, float grainSamples, double jitter) noexcept
    {
        auto& g = grains[static_cast<std::size_t> (next)];
        next = (next + 1) % kGrains;

        // Where the grain starts, kept so the whole grain stays inside the loop when it fits.
        const double travel = grainSamples * ratio;
        double p = scan + jitter * rng.bipolar();
        if (scanDir > 0)
            p = std::clamp (p, loop.start, std::max (loop.start, loop.end - travel));
        else
            p = std::clamp (p, std::min (loop.end, loop.start + travel), loop.end);

        g.pos = p;
        g.dir = scanDir;
        g.phase = 0.0f;
        g.step = 1.0f / std::max (8.0f, grainSamples);
        g.active = true;
    }

    void advanceScan (const Loop& loop, double rate) noexcept
    {
        scan += scanDir * rate;
        const double len = std::max (1.0, loop.length());
        if (scan > loop.end)
        {
            const double over = std::fmod (scan - loop.end, len);
            if (loop.mode == LoopMode::backAndForth) { scan = loop.end - over; scanDir = -1; }
            else scan = loop.start + over;
        }
        else if (scan < loop.start)
        {
            const double over = std::fmod (loop.start - scan, len);
            if (loop.mode == LoopMode::backAndForth) { scan = loop.start + over; scanDir = 1; }
            else scan = loop.end - over;
        }
    }

    std::array<Grain, kGrains> grains {};
    double scan = 0.0;
    int scanDir = 1;
    float countdown = 0.0f;
    int next = 0;
    Random rng;
};

} // namespace sing::dsp
