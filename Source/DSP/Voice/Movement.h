#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace sing
{

// Movement: slow change while a note is held, so a long pad keeps breathing.
//
// Each control that can move has an orbit: how far it wanders, as a share of the control's
// whole range (0 to 1), centred on where the control is set. One shape and one speed drive
// every orbit:
// - Drift: smooth random wandering that never repeats. Each control drifts on its own path.
// - Wave: a sine. Every control moves in step.
// - Breath: a slow rise and a longer fall, like breathing. Every control moves in step.
// With Each note on, every note follows its own path (its own drift, or its own place in the
// wave), so a chord shimmers instead of moving as one. Off, every note moves together.
//
// Wander is the orbit of each note's reading point inside the loop: 100% lets it travel the
// whole loop.
enum class MoveShape
{
    drift,
    wave,
    breath
};

enum MoveTarget
{
    moveStretch,
    moveSpread,
    moveFormant,
    moveAir,
    moveSpace,
    moveWander,
    numMoveTargets
};

struct MovementSettings
{
    MoveShape shape = MoveShape::drift;
    std::array<float, numMoveTargets> amounts {};   // orbit widths, 0 to 1
    bool eachNote = true;
    double cyclesPerSample = 0.0;                    // 1 / the period, in host samples

    bool any() const noexcept
    {
        return std::any_of (amounts.begin(), amounts.end(), [] (float a) { return a > 0.0f; });
    }
};

namespace movement
{
    inline uint32_t hash (uint32_t x) noexcept
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    // A random value in -1 to 1 for each whole step, the same every time for the same seed.
    // Pushed out toward the ends, so the drift uses more of its range.
    inline float knot (uint32_t seed, int64_t index) noexcept
    {
        const uint32_t h = hash (seed ^ hash ((uint32_t) index ^ (uint32_t) (index >> 32) * 0x9e3779b9u));
        const float u = (float) h * (2.0f / 4294967296.0f) - 1.0f;
        return std::copysign (std::sqrt (std::abs (u)), u);
    }

    // Smooth value noise: random knots joined by S-curves (no corners, so nothing clicks).
    inline float smoothNoise (uint32_t seed, double t) noexcept
    {
        const double floorT = std::floor (t);
        const auto i = (int64_t) floorT;
        const auto f = (float) (t - floorT);
        const float u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);   // smootherstep
        const float a = knot (seed, i), b = knot (seed, i + 1);
        return a + (b - a) * u;
    }

    // The shape's value, -1 to 1, at `cycles` periods into the movement.
    inline float value (MoveShape shape, double cycles, uint32_t seed) noexcept
    {
        constexpr double twoPi = 6.283185307179586;
        constexpr float pi = 3.14159265f;
        switch (shape)
        {
            case MoveShape::wave:
                return (float) std::sin (twoPi * cycles);
            case MoveShape::breath:
            {
                // In over the first 40% of each cycle, out over the rest; flat at the turns.
                const auto p = (float) (cycles - std::floor (cycles));
                return p < 0.4f ? -std::cos (pi * p / 0.4f) : std::cos (pi * (p - 0.4f) / 0.6f);
            }
            case MoveShape::drift:
            default:
            {
                // About two turns per period, with a faster, quieter layer so it never settles
                // into a pattern.
                const float slow = smoothNoise (seed, cycles * 2.0);
                const float fast = smoothNoise (seed ^ 0x5bd1e995u, cycles * 5.3 + 17.0);
                return std::clamp (0.8f * slow + 0.35f * fast, -1.0f, 1.0f);
            }
        }
    }

    // Where an orbit runs, in the control's 0 to 1 range. The orbit keeps its full width:
    // near either end of the range it is moved inward rather than cut short.
    inline void range (float base, float width, float& lo, float& hi) noexcept
    {
        width = std::clamp (width, 0.0f, 1.0f);
        const float half = 0.5f * width;
        const float centre = std::clamp (std::clamp (base, 0.0f, 1.0f), half, 1.0f - half);
        lo = centre - half;
        hi = centre + half;
    }

    // Where a control sits now: its orbit, at shape value v (-1 to 1).
    inline float orbit (float base, float width, float v) noexcept
    {
        if (width <= 0.0f)
            return base;
        float lo = 0.0f, hi = 1.0f;
        range (base, width, lo, hi);
        return lo + (hi - lo) * 0.5f * (std::clamp (v, -1.0f, 1.0f) + 1.0f);
    }

    // The seed every note shares when Each note is off (and the effects after the notes use).
    inline constexpr uint32_t sharedSeed = 0x51a6c0deu;

    // Drift gives each control its own path; Wave and Breath move them all in step.
    inline uint32_t targetSeed (uint32_t seed, int target) noexcept
    {
        return hash (seed + (uint32_t) target * 0x632be5abu);
    }
}

} // namespace sing
