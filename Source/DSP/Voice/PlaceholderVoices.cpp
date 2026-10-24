#include "PlaceholderVoices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace sing
{

namespace
{
constexpr double twoPi = 6.28318530717958647692;

struct Formant { double hz, bandwidth; };
using Vowel = std::array<Formant, 4>;

// Formant frequencies from Peterson and Barney (1952); bandwidths are typical values.
constexpr Vowel oohFormants { { { 300, 60 }, { 870, 90 }, { 2240, 120 }, { 3300, 200 } } };
constexpr Vowel ahFormants  { { { 730, 80 }, { 1090, 90 }, { 2440, 120 }, { 3400, 200 } } };
constexpr Vowel humFormants { { { 250, 50 }, { 1100, 300 }, { 2300, 400 }, { 3300, 500 } } };
constexpr Vowel haFormants  { { { 750, 200 }, { 1200, 250 }, { 2500, 300 }, { 3500, 400 } } };

// Magnitude of a cascade of second-order resonators (unity gain at 0 Hz) at frequency f.
double vowelGain (const Vowel& v, double f)
{
    double g = 1.0;
    for (const auto& fm : v)
    {
        const double r = f / fm.hz;
        const double re = 1.0 - r * r;
        const double im = f * fm.bandwidth / (fm.hz * fm.hz);
        g /= std::sqrt (re * re + im * im);
    }
    return g;
}

struct Noise
{
    uint32_t state = 0x1234567u;
    float next()
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (float) ((double) state * (2.0 / 4294967296.0) - 1.0);
    }
};

// A two-pole resonator for shaping noise (constant peak gain bandpass, RBJ cookbook form).
struct Resonator
{
    double b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Resonator (double hz, double bandwidth, double sr)
    {
        const double w = twoPi * hz / sr;
        const double q = hz / bandwidth;
        const double alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = alpha / a0; b2 = -alpha / a0;
        a1 = -2.0 * std::cos (w) / a0; a2 = (1.0 - alpha) / a0;
    }
    double process (double x)
    {
        const double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

double smoothstep (double edge0, double edge1, double x)
{
    const double t = std::clamp ((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
} // namespace

PlaceholderVoice makePlaceholderVoice (PlaceholderVowel vowel, double sr, float seconds)
{
    PlaceholderVoice voice;
    voice.sampleRate = sr;
    const int n = std::max (1, (int) (seconds * sr));
    voice.samples.assign ((size_t) n, 0.0f);

    Noise noise;
    const double dur = (double) n / sr;

    if (vowel == PlaceholderVowel::breath)
    {
        // Two slow breaths through an open "ha" shape.
        std::array<Resonator, 3> res { Resonator (750, 200, sr), Resonator (1200, 250, sr), Resonator (2500, 300, sr) };
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double half = dur * 0.5;
            const double local = std::fmod (t, half) / half;
            const double env = std::pow (std::sin (3.14159265358979 * local), 1.5);
            const double x = noise.next();
            const double y = res[0].process (x) + 0.7 * res[1].process (x) + 0.4 * res[2].process (x);
            voice.samples[(size_t) i] = (float) (env * y);
        }
        float breathPeak = 0.0f;
        for (float v : voice.samples)
            breathPeak = std::max (breathPeak, std::abs (v));
        if (breathPeak > 0.0f)
            for (float& v : voice.samples)
                v *= 0.4f / breathPeak;
        voice.rootHz = 0.0f;
        return voice;
    }

    const Vowel& formants = vowel == PlaceholderVowel::ooh ? oohFormants
                          : vowel == PlaceholderVowel::ah  ? ahFormants
                                                           : humFormants;
    const double f0 = vowel == PlaceholderVowel::ooh ? 220.0 : vowel == PlaceholderVowel::ah ? 196.0 : 261.63;
    const double breathiness = vowel == PlaceholderVowel::ooh ? 0.06 : vowel == PlaceholderVowel::ah ? 0.035 : 0.015;
    const double tilt = vowel == PlaceholderVowel::hum ? 1.6 : 1.0;   // the hum is darker
    voice.rootHz = (float) f0;

    constexpr int maxHarmonics = 48;
    constexpr int block = 64;
    std::array<double, maxHarmonics + 1> amp {};
    std::array<Resonator, 2> breathRes { Resonator (formants[1].hz, 400, sr), Resonator (formants[2].hz, 600, sr) };

    double phase = 0.0;
    double peak = 0.0;
    std::vector<double> out ((size_t) n);

    for (int start = 0; start < n; start += block)
    {
        const double t = (double) start / sr;
        // Vibrato fades in after half a second, like a held note.
        const double vib = 0.006 * smoothstep (0.4, 1.4, t) * std::sin (twoPi * 5.2 * t);
        const double drift = 0.002 * std::sin (twoPi * 0.31 * t + 1.0);
        const double f = f0 * (1.0 + vib + drift);

        double norm = 0.0;
        for (int h = 1; h <= maxHarmonics; ++h)
        {
            const double hf = f * h;
            amp[(size_t) h] = hf < 0.45 * sr ? vowelGain (formants, hf) / std::pow ((double) h, tilt) : 0.0;
            norm += amp[(size_t) h] * amp[(size_t) h];
        }
        norm = 1.0 / std::sqrt (std::max (norm, 1.0e-12));

        const int end = std::min (n, start + block);
        for (int i = start; i < end; ++i)
        {
            phase += twoPi * f / sr;
            if (phase > twoPi * 1000.0)
                phase -= twoPi * 1000.0;

            double s = 0.0;
            for (int h = 1; h <= maxHarmonics; ++h)
                if (amp[(size_t) h] > 0.0)
                    s += amp[(size_t) h] * std::sin (phase * h);
            s *= norm;

            // Breath noise, louder on the glottal pulse's open phase.
            const double pulse = 0.5 + 0.5 * std::cos (phase);
            const double b = noise.next();
            s += breathiness * pulse * (breathRes[0].process (b) + breathRes[1].process (b)) * 4.0;
            out[(size_t) i] = s;
        }
    }

    // A sung note: swell in, hold, fall away.
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double env = smoothstep (0.0, 0.35, t) * (1.0 - smoothstep (dur - 0.6, dur, t)) * (0.92 + 0.08 * std::sin (twoPi * 0.4 * t));
        out[(size_t) i] *= env;
        peak = std::max (peak, std::abs (out[(size_t) i]));
    }

    const double gain = peak > 0.0 ? 0.5 / peak : 1.0;
    for (int i = 0; i < n; ++i)
        voice.samples[(size_t) i] = (float) (out[(size_t) i] * gain);
    return voice;
}

} // namespace sing
