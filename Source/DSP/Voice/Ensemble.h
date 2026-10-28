#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sing
{

// Ensemble: a few singers taking one note. Each singer is the note read through its own
// short delay whose length moves slowly (that is the singer wandering a little in pitch, the
// way nobody holds a note dead still) and trembles at their own vibrato speed; and each
// stands in their own place across the stereo field. The singers replace the note (the note
// itself was one of them), summed at equal power.
//
// Written from the string machine ensemble of the 1970s (three modulated delay lines) with
// more lines and a singer's vibrato on each. No noise, no feedback, nothing to license.
// Real-time safe after prepare().
class Ensemble
{
public:
    static constexpr int maxSingers = 8;

    void prepare (double sampleRate)
    {
        rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        // Up to 30 cents of wander and a deep vibrato need about 25 ms of travel.
        int size = 256;
        while (size < (int) (0.06 * rate) + 8)
            size *= 2;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        reset();
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        write = 0;
        age = 0.0;
        for (auto& s : singers)
            s = {};
    }

    // seed: this note's own set of singers (their vibrato speeds and phases).
    void start (uint32_t seed) noexcept
    {
        reset();
        uint32_t x = seed != 0 ? seed : 0x9e3779b9u;
        auto next = [&x]
        {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            return (float) ((double) x / 4294967296.0);
        };
        for (auto& s : singers)
        {
            s.wanderRate1 = 0.12f + 0.2f * next();
            s.wanderRate2 = 0.35f + 0.4f * next();
            s.wanderPhase1 = 6.2831853f * next();
            s.wanderPhase2 = 6.2831853f * next();
            s.vibratoRate = 4.7f + 1.6f * next();
            s.vibratoPhase = 6.2831853f * next();
            s.vibratoOnset = 0.3f + 0.6f * next();
        }
    }

    // singers 2 to 8 (below 2 passes the note through); detune in cents; vibrato and width
    // 0 to 1. The settings glide across each call.
    void set (int count, float detuneCents, float vibrato, float width) noexcept
    {
        numSingers = count < 2 ? 0 : std::min (count, maxSingers);
        detune = std::max (0.0f, detuneCents);
        vibratoDepth = std::max (0.0f, vibrato) * 30.0f;   // cents at full
        stereoWidth = std::max (0.0f, std::min (1.0f, width));
    }

    bool isOn() const noexcept { return numSingers >= 2; }

    // In place, stereo. Call with blocks of up to 64 samples: the delays move linearly
    // across each call.
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (numSingers < 2 || buffer.empty())
            return;

        // Where each singer's delay is going this block.
        const double t = age;
        age += (double) numSamples / rate;
        const float centsToSeconds = 0.0005f;   // a cent of detune at the wander's speed is about this much travel
        const float baseDelay = 0.012f;
        std::array<float, (size_t) maxSingers> target {};
        for (int k = 0; k < numSingers; ++k)
        {
            const auto& s = singers[(size_t) k];
            const float wander = 0.6f * std::sin ((float) (6.2831853 * s.wanderRate1 * t) + s.wanderPhase1)
                               + 0.4f * std::sin ((float) (6.2831853 * s.wanderRate2 * t) + s.wanderPhase2);
            const float onset = std::min (1.0f, (float) t / s.vibratoOnset);
            const float vib = std::sin ((float) (6.2831853 * s.vibratoRate * t) + s.vibratoPhase);
            // A singer's vibrato moves the delay far less per cent than the slow wander, since
            // pitch is the delay's slope: a fast cycle needs little travel.
            const float seconds = baseDelay + detune * centsToSeconds * wander
                                + vibratoDepth * onset * (0.0005f / (6.2831853f * s.vibratoRate) * 2.0f) * vib;
            target[(size_t) k] = (float) (seconds * rate);
        }

        const float norm = 1.0f / std::sqrt ((float) numSingers);
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            buffer[(size_t) write] = in;
            float outL = 0.0f, outR = 0.0f;
            const float f = (float) (i + 1) / (float) numSamples;
            for (int k = 0; k < numSingers; ++k)
            {
                auto& s = singers[(size_t) k];
                const float d = s.delay + (target[(size_t) k] - s.delay) * f;
                const float dc = std::max (2.0f, d);
                const int di = (int) dc;
                const float fr = dc - (float) di;
                // Four-point Hermite between the samples either side of the reading point.
                const int base = write - di;
                const float xm1 = buffer[(size_t) ((base + 1) & mask)];
                const float x0 = buffer[(size_t) (base & mask)];
                const float x1 = buffer[(size_t) ((base - 1) & mask)];
                const float x2 = buffer[(size_t) ((base - 2) & mask)];
                const float c1 = 0.5f * (x1 - xm1);
                const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                const float y = ((c3 * fr + c2) * fr + c1) * fr + x0;
                // Their place across the stage, equal power.
                const float place = numSingers > 1 ? ((float) k / (float) (numSingers - 1) * 2.0f - 1.0f) * stereoWidth : 0.0f;
                const float theta = (place + 1.0f) * 0.25f * 3.14159265f;
                outL += y * std::cos (theta) * 1.41421356f;
                outR += y * std::sin (theta) * 1.41421356f;
            }
            left[i] = outL * norm;
            right[i] = outR * norm;
            write = (write + 1) & mask;
        }
        for (int k = 0; k < numSingers; ++k)
            singers[(size_t) k].delay = target[(size_t) k];
    }

private:
    struct Singer
    {
        float wanderRate1 = 0.2f, wanderRate2 = 0.5f, wanderPhase1 = 0.0f, wanderPhase2 = 0.0f;
        float vibratoRate = 5.5f, vibratoPhase = 0.0f, vibratoOnset = 0.6f;
        float delay = 0.0f;   // samples, where the last block ended
    };

    double rate = 48000.0;
    std::vector<float> buffer;
    int mask = 0, write = 0;
    double age = 0.0;
    int numSingers = 0;
    float detune = 0.0f, vibratoDepth = 0.0f, stereoWidth = 0.0f;
    std::array<Singer, (size_t) maxSingers> singers {};
};

} // namespace sing
