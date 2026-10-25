#include "Tape.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double twoPi = 6.28318530717958647692;
constexpr float centreDelayMs = 12.0f;
constexpr float maxWowMs = 4.0f;        // with the two components, about 30 cents at the top
constexpr float maxFlutterMs = 0.1f;    // about 12 cents at the top
constexpr float referenceLevel = 0.25f; // a voice at this level comes out at the same level
constexpr int chunk = 32;               // Age's filter is redrawn this often

} // namespace

float Tape::driveGainFor (float drive) noexcept
{
    return 0.6f * std::pow (10.0f, 0.9f * std::clamp (drive, 0.0f, 1.0f));
}

float Tape::ageHzFor (float age) noexcept
{
    // On a log scale: 22 kHz (open, at 44.1 and 48 kHz) down to 2.5 kHz.
    return 22000.0f * std::pow (2500.0f / 22000.0f, std::clamp (age, 0.0f, 1.0f));
}

void Tape::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto maxDelay = (std::size_t) reverb::msToSamples (centreDelayMs + 1.4f * maxWowMs + 1.4f * maxFlutterMs + 2.0f, rate) + 8;
    lineL.prepare (maxDelay);
    lineR.prepare (maxDelay);
    mix.prepare (rate, 10.0f);
    wowDepth.prepare (rate, 50.0f);
    flutterDepth.prepare (rate, 50.0f);
    driveGain.prepare (rate, 30.0f);
    reset();
}

void Tape::reset() noexcept
{
    lineL.reset();
    lineR.reset();
    ageL.reset();
    ageR.reset();
    mix.snapTo (on ? 1.0f : 0.0f);
    wowDepth.snapTo (wowDepth.getTarget());
    flutterDepth.snapTo (flutterDepth.getTarget());
    driveGain.snapTo (driveGain.getTarget() > 0.0f ? driveGain.getTarget() : driveGainFor (0.0f));
    ageNow = ageTarget;
    updateAge (0);
    running = on;
}

void Tape::set (bool isOn, float wow, float flutter, float drive, float age) noexcept
{
    if (isOn && ! running)
    {
        // Start clean: nothing old left in the delay or the filters.
        lineL.reset();
        lineR.reset();
        ageL.reset();
        ageR.reset();
        ageNow = ageHzFor (age);
        updateAge (0);
        running = true;
    }
    on = isOn;
    mix.setTarget (on ? 1.0f : 0.0f);
    wowDepth.setTarget (reverb::msToSamples (maxWowMs, rate) * std::clamp (wow, 0.0f, 1.0f));
    flutterDepth.setTarget (reverb::msToSamples (maxFlutterMs, rate) * std::clamp (flutter, 0.0f, 1.0f));
    driveGain.setTarget (driveGainFor (drive));
    ageTarget = ageHzFor (age);
}

void Tape::updateAge (int samples) noexcept
{
    const float k = samples <= 0 ? 1.0f : 1.0f - std::exp (-(float) samples / (0.015f * (float) rate));
    ageNow *= std::pow (ageTarget / ageNow, k);
    const double hz = std::min ((double) ageNow, 0.45 * rate);
    ageL.setLowPass (hz, rate);
    ageR.setLowPass (hz, rate);
}

float Tape::nextRandom() noexcept
{
    seed = seed * 1664525u + 1013904223u;
    return (float) (seed >> 8) / 8388608.0f - 1.0f;   // -1 to 1
}

void Tape::process (float* left, float* right, int numSamples) noexcept
{
    if (! running)
        return;

    const float centre = reverb::msToSamples (centreDelayMs, rate);
    const double invRate = 1.0 / rate;
    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, chunk);
        updateAge (n);

        // Wow's speed wanders a little, so the sway never settles into a plain wave.
        driftCountdown -= n;
        if (driftCountdown <= 0)
        {
            driftTarget = nextRandom();
            driftCountdown = (int) (0.8 * rate);
        }
        drift += (driftTarget - drift) * (1.0f - std::exp (-(float) n / (0.6f * (float) rate)));
        const double wowHz1 = 0.48 * (1.0 + 0.25 * (double) drift), wowHz2 = 0.83;
        const double flutterHz1 = 7.1, flutterHz2 = 10.9;

        for (int i = done; i < done + n; ++i)
        {
            wowPhase1 += wowHz1 * invRate;
            wowPhase2 += wowHz2 * invRate;
            flutterPhase1 += flutterHz1 * invRate;
            flutterPhase2 += flutterHz2 * invRate;
            const float wow = (float) (std::sin (twoPi * wowPhase1) + 0.3 * std::sin (twoPi * wowPhase2));
            const float flutter = (float) (std::sin (twoPi * flutterPhase1) + 0.35 * std::sin (twoPi * flutterPhase2));
            const float delay = centre + wowDepth.next() * wow + flutterDepth.next() * flutter;

            const float g = driveGain.next();
            const float makeUp = referenceLevel / std::tanh (referenceLevel * g);
            lineL.push (std::tanh (g * left[i]) * makeUp);
            lineR.push (std::tanh (g * right[i]) * makeUp);
            const float wetL = ageL.process (lineL.readFractional (delay));
            const float wetR = ageR.process (lineR.readFractional (delay));

            const float m = mix.next();
            left[i] += m * (wetL - left[i]);
            right[i] += m * (wetR - right[i]);
        }
        done += n;

        for (double* p : { &wowPhase1, &wowPhase2, &flutterPhase1, &flutterPhase2 })
            *p -= std::floor (*p);
    }

    if (! on && mix.getCurrent() <= 0.0f)
        running = false;
}

} // namespace sing
