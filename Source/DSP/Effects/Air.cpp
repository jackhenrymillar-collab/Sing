#include "Air.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr float shelfHz = 8000.0f;
constexpr float maxShelfDb = 8.0f;
constexpr float breathLowHz = 3000.0f, breathHighHz = 10000.0f;

// Band-passed white noise (uniform, -1 to 1) has an RMS of about 0.31; the follower reads
// about 1.2 times a voice's RMS. This puts the breath 22 dB under the voice at full Air.
constexpr float breathGain = 0.21f;

void designPass (float hz, double rate, bool high, float& b0, float& b1, float& b2, float& a1, float& a2) noexcept
{
    // RBJ low- or high-pass, Q = 1/sqrt(2).
    const double w0 = 2.0 * pi * std::min<double> (hz, 0.45 * rate) / rate;
    const double c = std::cos (w0), alpha = std::sin (w0) / std::sqrt (2.0);
    const double a0 = 1.0 + alpha;
    const double n0 = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    const double n1 = high ? -(1.0 + c) : 1.0 - c;
    b0 = (float) (n0 / a0);
    b1 = (float) (n1 / a0);
    b2 = (float) (n0 / a0);
    a1 = (float) (-2.0 * c / a0);
    a2 = (float) ((1.0 - alpha) / a0);
}
} // namespace

void Air::prepare (double sampleRate) noexcept
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    smoothCoeff = (float) std::exp (-1.0 / (0.05 * rate));
    attack = (float) std::exp (-1.0 / (0.005 * rate));
    release = (float) std::exp (-1.0 / (0.12 * rate));
    for (auto* f : { &breathHighL, &breathHighR })
        designPass (breathLowHz, rate, true, f->b0, f->b1, f->b2, f->a1, f->a2);
    for (auto* f : { &breathLowL, &breathLowR })
        designPass (breathHighHz, rate, false, f->b0, f->b1, f->b2, f->a1, f->a2);
    shelfDbDesigned = -1.0f;
    reset();
}

void Air::reset() noexcept
{
    smoothedAmount = amount;
    envelope = 0.0f;
    for (auto* f : { &shelfL, &shelfR, &breathHighL, &breathHighR, &breathLowL, &breathLowR })
        f->clear();
}

void Air::setAmount (float a) noexcept
{
    amount = std::clamp (a, 0.0f, 1.0f);
}

void Air::designShelf (float gainDb) noexcept
{
    // RBJ high shelf, slope 1.
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * pi * std::min<double> (shelfHz, 0.45 * rate) / rate;
    const double c = std::cos (w0), s = std::sin (w0);
    const double alpha = s / 2.0 * std::sqrt (2.0);
    const double sq = 2.0 * std::sqrt (A) * alpha;
    const double a0 = (A + 1.0) - (A - 1.0) * c + sq;
    const auto b0 = (float) (A * ((A + 1.0) + (A - 1.0) * c + sq) / a0);
    const auto b1 = (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0);
    const auto b2 = (float) (A * ((A + 1.0) + (A - 1.0) * c - sq) / a0);
    const auto a1 = (float) (2.0 * ((A - 1.0) - (A + 1.0) * c) / a0);
    const auto a2 = (float) (((A + 1.0) - (A - 1.0) * c - sq) / a0);
    for (auto* f : { &shelfL, &shelfR })
    {
        f->b0 = b0; f->b1 = b1; f->b2 = b2; f->a1 = a1; f->a2 = a2;
    }
    shelfDbDesigned = gainDb;
}

float Air::noise (uint32_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (float) state * (2.0f / 4294967296.0f) - 1.0f;
}

void Air::process (float* left, float* right, int numSamples) noexcept
{
    // Air glides over about 50 ms; the shelf is redrawn once per block as it moves.
    smoothedAmount = amount + std::pow (smoothCoeff, (float) numSamples) * (smoothedAmount - amount);
    if (std::abs (smoothedAmount - amount) < 1.0e-4f)
        smoothedAmount = amount;
    if (smoothedAmount <= 0.0f)
    {
        // Off: untouched. Keep following the voice so the breath is ready when Air returns.
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = 0.5f * std::abs (left[i] + right[i]);
            envelope = x + (x > envelope ? attack : release) * (envelope - x);
        }
        shelfL.clear();
        shelfR.clear();
        return;
    }

    const float shelfDb = maxShelfDb * smoothedAmount;
    if (std::abs (shelfDb - shelfDbDesigned) > 0.02f)
        designShelf (shelfDb);

    const float breath = breathGain * smoothedAmount;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = 0.5f * std::abs (left[i] + right[i]);
        envelope = x + (x > envelope ? attack : release) * (envelope - x);
        const float g = breath * envelope;
        const float nl = breathLowL.process (breathHighL.process (noise (seedL)));
        const float nr = breathLowR.process (breathHighR.process (noise (seedR)));
        left[i] = shelfL.process (left[i]) + g * nl;
        right[i] = shelfR.process (right[i]) + g * nr;
    }
}

} // namespace sing
