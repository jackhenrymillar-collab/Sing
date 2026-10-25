#include "Space.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr float lowCutHz = 150.0f;
constexpr float maxPreDelayMs = 40.0f;
constexpr float diffusion = 0.85f;
constexpr float modulation = 0.5f;
constexpr float maxWet = 1.5f;
} // namespace

float Space::wetGainFor (float l) noexcept
{
    l = std::clamp (l, 0.0f, 1.0f);
    return maxWet * l * l;
}

float Space::preDelayMsFor (float sz) noexcept
{
    // The old knob's pre-delay ran 10 to 40 ms as its size ran 0.35 to 1.
    return 10.0f + (maxPreDelayMs - 10.0f) * std::clamp ((sz - 0.35f) / 0.65f, 0.0f, 1.0f);
}

Space::Legacy Space::legacy (float a) noexcept
{
    a = std::clamp (a, 0.0f, 1.0f);
    constexpr float halfPi = 1.57079632679f;
    Legacy m {};
    m.size = 0.35f + 0.65f * a;
    m.decaySeconds = 2.0f * std::pow (10.0f, a);           // 2 s to 20 s
    m.preDelayMs = 10.0f + 30.0f * a;
    m.wet = 0.8f * std::sin (a * halfPi);                  // rose fast, then levelled off
    m.dry = 1.0f - 0.4f * a * a;                           // and turned the voice down a little
    m.level = std::min (1.0f, std::sqrt (m.wet / m.dry / maxWet));
    return m;
}

void Space::prepare (double sampleRate, int maxBlockSize)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    aurora.prepare (rate);
    updateReverb();
    const auto maxDelay = (std::size_t) reverb::msToSamples (maxPreDelayMs + 2.0f, rate) + 4;
    preDelayL.prepare (maxDelay);
    preDelayR.prepare (maxDelay);
    wetGain.prepare (rate, 50.0f);
    preDelay.prepare (rate, 200.0f);
    lowCoeff = reverb::OnePoleLowpass::coefficientFor (lowCutHz, rate);
    const auto n = (size_t) std::max (1, maxBlockSize);
    sendL.assign (n, 0.0f);
    sendR.assign (n, 0.0f);
    wetL.assign (n, 0.0f);
    wetR.assign (n, 0.0f);
    reset();
}

void Space::reset()
{
    aurora.reset();
    preDelayL.reset();
    preDelayR.reset();
    lowL.reset();
    lowR.reset();
    for (auto* b : { &wetLowL, &wetLowR, &wetHighL, &wetHighR })
        b->reset();
    lowNow = lowTarget;
    highNow = highTarget;
    updateFilter (0);
    wetGain.snapTo (wetGainFor (level));
    preDelay.snapTo (reverb::msToSamples (preDelayMsFor (size), rate));
    running = level > 0.0f;
    silentSamples = 0;
}

void Space::setFilter (bool on, float lowHz, float highHz) noexcept
{
    lowTarget = on ? std::clamp (lowHz, 10.0f, 20000.0f) : 10.0f;
    highTarget = on ? std::clamp (highHz, 100.0f, 22000.0f) : 22000.0f;
}

void Space::updateFilter (int samples) noexcept
{
    // Glide on a log scale, about 50 ms to settle.
    const float k = samples <= 0 ? 1.0f : 1.0f - std::exp (-(float) samples / (0.015f * (float) rate));
    lowNow *= std::pow (lowTarget / lowNow, k);
    highNow *= std::pow (highTarget / highNow, k);
    const double top = 0.45 * rate;
    wetLowL.setHighPass (lowNow, rate);
    wetLowR.setHighPass (lowNow, rate);
    wetHighL.setLowPass (std::min ((double) highNow, top), rate);
    wetHighR.setLowPass (std::min ((double) highNow, top), rate);
}

void Space::setLevel (float l) noexcept
{
    l = std::clamp (l, 0.0f, 1.0f);
    if (l == level)
        return;
    level = l;
    wetGain.setTarget (wetGainFor (l));

    if (l > 0.0f && ! running)
    {
        // Start clean: the reverb was stopped and its old tail is long gone.
        aurora.reset();
        preDelayL.reset();
        preDelayR.reset();
        running = true;
        silentSamples = 0;
    }
}

void Space::setDecay (float seconds) noexcept
{
    seconds = std::clamp (seconds, minDecaySeconds, maxDecaySeconds);
    if (seconds == decaySeconds)
        return;
    decaySeconds = seconds;
    updateReverb();
}

void Space::setSize (float s) noexcept
{
    s = std::clamp (s, 0.0f, 1.0f);
    if (s == size)
        return;
    size = s;
    preDelay.setTarget (reverb::msToSamples (preDelayMsFor (s), rate));
    updateReverb();
}

void Space::updateReverb() noexcept
{
    reverb::MachineParams p;
    p.param1 = size;
    p.param2 = diffusion;
    p.mod = modulation;
    p.decaySeconds = decaySeconds;
    aurora.setParams (p);
}

void Space::process (float* left, float* right, int numSamples) noexcept
{
    if (! running)
        return;

    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, (int) sendL.size());
        float* l = left + done;
        float* r = right + done;

        // The send: low cut, then pre-delay.
        for (int i = 0; i < n; ++i)
        {
            const float d = std::max (2.0f, preDelay.next());
            preDelayL.push (l[i] - lowL.process (l[i], lowCoeff));
            preDelayR.push (r[i] - lowR.process (r[i], lowCoeff));
            sendL[(size_t) i] = preDelayL.readFractional (d);
            sendR[(size_t) i] = preDelayR.readFractional (d);
        }

        aurora.process (sendL.data(), sendR.data(), wetL.data(), wetR.data(), n);
        updateFilter (n);
        for (int i = 0; i < n; ++i)
        {
            wetL[(size_t) i] = wetHighL.process (wetLowL.process (wetL[(size_t) i]));
            wetR[(size_t) i] = wetHighR.process (wetLowR.process (wetR[(size_t) i]));
        }

        float loudest = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float w = wetGain.next();
            const float wl = wetL[(size_t) i] * w, wr = wetR[(size_t) i] * w;
            l[i] += wl;
            r[i] += wr;
            loudest = std::max (loudest, std::max (std::abs (wl), std::abs (wr)));
        }
        done += n;

        // Turned off: once the wet has faded to nothing (-80 dB), stop running.
        if (level == 0.0f && wetGain.getCurrent() < 1.0e-4f && loudest < 1.0e-5f)
        {
            silentSamples += n;
            if (silentSamples > (int) (0.02 * rate))
            {
                running = false;
                wetGain.snapTo (0.0f);
                return;
            }
        }
        else
            silentSamples = 0;
    }
}

} // namespace sing
