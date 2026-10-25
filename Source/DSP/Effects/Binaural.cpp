#include "Binaural.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double twoPi = 6.283185307179586476925;
// The two allpass chains (each coefficient a 2-sample-delay section, a^2 below).
constexpr std::array<float, 4> pathA { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
constexpr std::array<float, 4> pathB { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
} // namespace

float Binaural::Allpass4::process (float x) noexcept
{
    // Each section: y[n] = c (x[n] + y[n-2]) - x[n-2], with c = a^2.
    for (size_t i = 0; i < 4; ++i)
    {
        const float y = coeff[i] * (x + y2[i]) - x2[i];
        x2[i] = x1[i];
        x1[i] = x;
        y2[i] = y1[i];
        y1[i] = y;
        x = y;
    }
    return x;
}

void Binaural::prepare (double sampleRate) noexcept
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (auto* c : { &left, &right })
        for (size_t i = 0; i < 4; ++i)
        {
            c->a.coeff[i] = pathA[i] * pathA[i];
            c->b.coeff[i] = pathB[i] * pathB[i];
        }
    reset();
}

void Binaural::reset() noexcept
{
    left.reset();
    right.reset();
    phase = 0.0;
    mix = mixTarget;
}

void Binaural::set (bool on, float rateHz, float amount) noexcept
{
    beatHz = std::clamp (rateHz, 0.01f, 40.0f);
    mixTarget = on ? std::clamp (amount, 0.0f, 1.0f) : 0.0f;
}

void Binaural::process (float* l, float* r, int numSamples) noexcept
{
    if (mix == 0.0f && mixTarget == 0.0f)
    {
        // Keep the allpasses running, so switching on is clean.
        for (int i = 0; i < numSamples; ++i)
        {
            left.delayed = left.a.process (l[i]);
            (void) left.b.process (l[i]);
            right.delayed = right.a.process (r[i]);
            (void) right.b.process (r[i]);
        }
        return;
    }

    // Each ear moves by half the beat, in opposite directions.
    const double step = twoPi * 0.5 * (double) beatHz / rate;
    const float glide = 1.0f / (0.05f * (float) rate);   // amount changes over 50 ms
    for (int i = 0; i < numSamples; ++i)
    {
        mix += std::clamp (mixTarget - mix, -glide, glide);

        // Analytic signal: path a (one sample late) as the real part, path b as the
        // imaginary part, 90 degrees behind it.
        const float reL = left.delayed, imL = left.b.process (l[i]);
        left.delayed = left.a.process (l[i]);
        const float reR = right.delayed, imR = right.b.process (r[i]);
        right.delayed = right.a.process (r[i]);

        const auto c = (float) std::cos (phase), s = (float) std::sin (phase);
        const float upL = reL * c + imL * s;      // left up by half the beat
        const float downR = reR * c - imR * s;    // right down by half
        // From the plain sound to the moved one (in between, each ear beats a little too).
        l[i] += mix * (upL - l[i]);
        r[i] += mix * (downR - r[i]);

        phase += step;
        if (phase > twoPi)
            phase -= twoPi;
    }
}

} // namespace sing
