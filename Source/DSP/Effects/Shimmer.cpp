#include "Shimmer.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr float bandLowHz = 200.0f, bandHighHz = 6000.0f;
constexpr float referenceDecaySeconds = 4.0f;
} // namespace

void Shimmer::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    shifter.prepare (rate);
    for (auto* b : { &lowL, &lowR })
        b->setHighPass (bandLowHz, rate);
    for (auto* b : { &highL, &highR })
        b->setLowPass (bandHighHz, rate);
    amountNow.prepare (rate, 30.0f);
    riseNow.prepare (rate, 30.0f);
    reset();
}

void Shimmer::reset() noexcept
{
    shifter.start (0.0f, ratio, false, 1.0f);
    for (auto* b : { &lowL, &lowR, &highL, &highR })
        b->reset();
    amountNow.snapTo (amountNow.getTarget());
    riseNow.snapTo (riseNow.getTarget());
}

void Shimmer::set (bool isOn, float amount, float rise, int interval) noexcept
{
    on = isOn;
    // Squared, so the first part of the control stays subtle.
    const float a = std::clamp (amount, 0.0f, 1.0f);
    amountNow.setTarget (on ? a * a : 0.0f);
    riseSetting = on ? std::clamp (rise, 0.0f, 1.0f) : 0.0f;
    updateRise();
    const float r = ratioFor (interval);
    if (r != ratio)
    {
        ratio = r;
        shifter.setShift (ratio, false, 1.0f);
        updateRise();
    }
}

void Shimmer::setDecay (float seconds) noexcept
{
    decaySeconds = std::max (0.1f, seconds);
    updateRise();
}

void Shimmer::updateRise() noexcept
{
    // A tail twice as long sends about twice the power round: take the gain down by its
    // square root past a few seconds.
    const float scale = std::sqrt (std::min (1.0f, referenceDecaySeconds / decaySeconds));
    // Fifths take more passes to climb out of the band, so each one gives a little less.
    const float steps = ratio < 1.9f ? 0.8f : 1.0f;
    riseNow.setTarget (maxRise * riseSetting * scale * steps);
}

void Shimmer::process (const float* inL, const float* inR, const float* tailL, const float* tailR,
                       float* outL, float* outR, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float a = amountNow.next(), r = riseNow.next();
        float xl = a * inL[i] + r * tailL[i];
        float xr = a * inR[i] + r * tailR[i];
        xl = highL.process (lowL.process (xl));
        xr = highR.process (lowR.process (xr));
        // A soft limit on the loop: transparent at normal levels, never runs away.
        outL[i] = std::tanh (xl);
        outR[i] = std::tanh (xr);
    }
    shifter.process (outL, outR, outL, outR, numSamples);
}

} // namespace sing
