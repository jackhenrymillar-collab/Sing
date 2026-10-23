#pragma once

#include "Shared.h"

namespace sing::dsp
{

// Space: a long, diffuse wash after the voice, built like Overcast's Haze machine (twelve
// short diffusers per channel feeding a 16-line delay network). One amount sets the decay,
// the send and how much of the dry voice stays.
class Space
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        diffL.prepare (sampleRate, { 1.3f, 1.9f, 2.7f, 3.4f, 4.1f, 4.9f, 5.6f, 6.3f, 7.1f, 7.9f, 8.6f, 9.4f });
        diffR.prepare (sampleRate, { 1.5f, 2.1f, 2.5f, 3.2f, 4.3f, 4.7f, 5.3f, 6.7f, 7.3f, 8.3f, 8.9f, 9.7f });
        fdn.prepare (sampleRate, kDelays.back());
        fdn.setDelays (kDelays);
        fdn.setModulation (0.45f, 1.0f);
        wet.prepare (sampleRate, 60.0f);
        dry.prepare (sampleRate, 60.0f);
        lowCutL.setHighpass (90.0f, sampleRate);
        lowCutR.setHighpass (90.0f, sampleRate);
        setAmount (amount);
        reset();
    }

    void reset()
    {
        diffL.reset(); diffR.reset(); fdn.reset();
        lowCutL.reset(); lowCutR.reset();
        wet.snapTo (wet.getTarget());
        dry.snapTo (dry.getTarget());
    }

    // 0..1.
    void setAmount (float a)
    {
        a = std::clamp (a, 0.0f, 1.0f);
        if (std::abs (a - amount) < 1.0e-5f && wet.getTarget() > 0.0f)
            return;
        amount = a;
        DecaySettings d;
        d.rtMid = 2.5f + 15.5f * a;         // 2.5 s to 18 s
        d.rtLow = d.rtMid * 1.1f;
        d.rtHigh = d.rtMid * 0.55f;
        d.highHz = 3800.0f;
        fdn.setDecay (d);
        // Equal-power style: the wash comes up faster than the dry goes down.
        wet.setTarget (kWetTrim * std::sqrt (a));
        dry.setTarget (1.0f - 0.45f * a * a);
    }

    void process (float& l, float& r) noexcept
    {
        const float w = wet.next(), d = dry.next();
        if (w < 1.0e-6f && wet.isSettled())
        {
            l *= d; r *= d;
            return;
        }
        const float a = diffL.process (lowCutL.process (l), 0.62f, 1.0f, 1.2f);
        const float b = diffR.process (lowCutR.process (r), 0.62f, 1.0f, 1.2f);
        float wl, wr;
        fdn.process (a, b, wl, wr);
        l = d * l + w * wl;
        r = d * r + w * wr;
    }

private:
    static constexpr float kWetTrim = 0.9f;
    static constexpr std::array<float, 16> kDelays { 61.2292f, 67.8958f, 71.1042f, 77.8958f, 83.3542f, 90.1458f, 94.1042f, 101.2708f,
                                                     107.8958f, 113.2708f, 119.6458f, 127.1042f, 133.7708f, 139.1458f, 146.2292f, 153.5208f };
    double fs = 48000.0;
    float amount = -1.0f;
    Diffuser<12> diffL, diffR;
    Fdn<16> fdn;
    Biquad lowCutL, lowCutR;
    Smoother wet, dry;
};

} // namespace sing::dsp
