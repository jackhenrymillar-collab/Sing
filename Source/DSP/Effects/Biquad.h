#pragma once

#include <cmath>

namespace sing
{

// A second-order filter section (transposed direct form II), with the high- and low-pass
// designs from Robert Bristow-Johnson's "Audio EQ Cookbook": 12 dB per octave, Butterworth
// (Q = 1/sqrt 2) unless told otherwise.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setHighPass (double hz, double rate, double q = 0.70710678) noexcept { set (hz, rate, q, true); }
    void setLowPass (double hz, double rate, double q = 0.70710678) noexcept { set (hz, rate, q, false); }

private:
    void set (double hz, double rate, double q, bool high) noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * std::fmin (hz, 0.49 * rate) / rate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        const double side = high ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0;
        b0 = (float) (side / a0);
        b1 = (float) ((high ? -(1.0 + cw) : (1.0 - cw)) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * cw / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }
};

} // namespace sing
