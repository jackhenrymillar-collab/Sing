#pragma once

#include "Effects/Biquad.h"

#include <array>

namespace sing
{

// Vowel: a vowel's shape laid over whatever is playing. Three resonances (the first three
// formants of ooh, oh, ah, eh and ee, from Peterson and Barney's 1952 measurements and
// Hillenbrand's for "oh") in parallel over the sound, their centres sliding on a log scale as
// the Vowel setting moves between neighbours. Breathe swings the vowel slowly towards its
// neighbours and back, like a mouth opening and closing. Works on any source: a synth or an
// instrument comes out singing.
//
// Real-time safe after prepare(). Stereo, in place.
class Vowel
{
public:
    void prepare (double sampleRate) noexcept
    {
        rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : bands)
            for (auto& b : ch)
                b.reset();
        age = 0.0;
        mixDry = mixWet = 0.0f;
    }

    // on; vowel 0 to 1 (ooh, oh, ah, eh, ee at 0, 0.25, 0.5, 0.75, 1); depth and breathe 0 to 1.
    void set (bool on, float vowel, float depth, float breathe) noexcept
    {
        enabled = on;
        position = std::max (0.0f, std::min (1.0f, vowel)) * 4.0f;
        amount = std::max (0.0f, std::min (1.0f, depth));
        swing = std::max (0.0f, std::min (1.0f, breathe));
    }

    bool isOn() const noexcept { return enabled; }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const float wantWet = enabled ? amount : 0.0f;
        if (wantWet <= 0.0f && mixWet <= 1.0e-4f)
        {
            mixWet = 0.0f;
            mixDry = 1.0f;
            return;
        }

        for (int pos = 0; pos < numSamples;)
        {
            const int n = std::min (64, numSamples - pos);
            // Where the vowel is now: the setting, swung by Breathe.
            const double t = age;
            age += (double) n / rate;
            const float rateHz = 0.08f + 0.35f * swing;
            const float p = std::max (0.0f, std::min (4.0f, position + 0.7f * swing * std::sin ((float) (6.2831853 * rateHz * t))));
            const int a = std::min (3, (int) p), b = a + 1;
            const float f = p - (float) a;
            for (int k = 0; k < 3; ++k)
            {
                const float hz = formants[(size_t) a][(size_t) k] * std::pow (formants[(size_t) b][(size_t) k] / formants[(size_t) a][(size_t) k], f);
                for (auto& ch : bands)
                    setBandPass (ch[(size_t) k], hz, 7.0);
            }
            // The mix glides, so switching on or off never clicks.
            const float targetWet = wantWet * 3.2f, targetDry = 1.0f - 0.7f * wantWet;
            const float dw = (targetWet - mixWet) / (float) n, dd = (targetDry - mixDry) / (float) n;
            for (int i = pos; i < pos + n; ++i)
            {
                mixWet += dw;
                mixDry += dd;
                const float l = left[i], r = right[i];
                float wl = 0.0f, wr = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    wl += gains[(size_t) k] * bands[0][(size_t) k].process (l);
                    wr += gains[(size_t) k] * bands[1][(size_t) k].process (r);
                }
                left[i] = l * mixDry + wl * mixWet;
                right[i] = r * mixDry + wr * mixWet;
            }
            pos += n;
        }
    }

private:
    // RBJ band-pass with 0 dB peak gain.
    void setBandPass (Biquad& b, double hz, double q) const noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * rate) / rate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b.b0 = (float) (alpha / a0);
        b.b1 = 0.0f;
        b.b2 = (float) (-alpha / a0);
        b.a1 = (float) (-2.0 * cw / a0);
        b.a2 = (float) ((1.0 - alpha) / a0);
    }

    static constexpr float formants[5][3] { { 300.0f, 870.0f, 2240.0f }, { 500.0f, 900.0f, 2450.0f }, { 730.0f, 1090.0f, 2440.0f },
                                            { 530.0f, 1840.0f, 2480.0f }, { 270.0f, 2290.0f, 3010.0f } };
    static constexpr float gains[3] { 1.0f, 0.8f, 0.5f };

    double rate = 48000.0;
    std::array<std::array<Biquad, 3>, 2> bands {};
    bool enabled = false;
    float position = 2.0f, amount = 0.0f, swing = 0.0f;
    float mixDry = 1.0f, mixWet = 0.0f;
    double age = 0.0;
};

} // namespace sing
