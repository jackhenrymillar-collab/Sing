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

//==============================================================================
// The voices that move. Each is a little score: what the singer does at each moment of an
// 8 s take (vowel, pitch, vibrato, brightness, loudness), sung by one or more singers. They are
// pure harmonic tone with no breath noise: brightness comes from the upper harmonics instead.

// More vowels: "eh", "ee" and "er" from Peterson and Barney (1952), "oh" from Hillenbrand et
// al. (1995). The high "ee" uses women's values with the first formant raised to sit near the
// note, as sopranos do.
constexpr Vowel ohFormants     { { { 500, 70 }, { 900, 90 }, { 2450, 120 }, { 3350, 200 } } };
constexpr Vowel ehFormants     { { { 530, 70 }, { 1840, 100 }, { 2480, 130 }, { 3450, 200 } } };
constexpr Vowel eeFormants     { { { 270, 50 }, { 2290, 110 }, { 3010, 150 }, { 3600, 220 } } };
constexpr Vowel highEeFormants { { { 560, 90 }, { 2700, 140 }, { 3300, 180 }, { 4300, 250 } } };
constexpr Vowel highIhFormants { { { 620, 100 }, { 2400, 140 }, { 3150, 180 }, { 4200, 250 } } };

// Glides between two vowels, the frequencies on a log scale as the ear hears them.
Vowel mix (const Vowel& a, const Vowel& b, double t)
{
    Vowel v {};
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = { a[i].hz * std::pow (b[i].hz / a[i].hz, t), a[i].bandwidth + (b[i].bandwidth - a[i].bandwidth) * t };
    return v;
}

// Three vowels in a row: t from 0 to 1 goes a, b, c.
Vowel mix3 (const Vowel& a, const Vowel& b, const Vowel& c, double t)
{
    return t < 0.5 ? mix (a, b, t * 2.0) : mix (b, c, t * 2.0 - 1.0);
}

double rise (double t0, double t1, double t) { return smoothstep (t0, t1, t); }
double semis (double s) { return std::pow (2.0, s / 12.0); }

struct Frame
{
    double f0 = 220.0;           // Hz, before vibrato
    Vowel vowel = ahFormants;
    double vibratoDepth = 0.0;   // fraction of the pitch (0.006 is about 10 cents)
    double vibratoRate = 5.2;    // Hz
    double brilliance = 0.9;     // lifts the harmonics above the top formant: 0 none, 1 most
    double level = 1.0;          // loudness
    double tilt = 1.0;           // how fast the harmonics fall away: higher is darker
};

struct Score
{
    int singers = 1;
    double spreadCents = 0.0;    // how far apart the singers are tuned
    Frame (*at) (double t) = nullptr;
};

Frame openingOoh (double t)
{
    // "ooh" opens through "oh" to "ah", then closes again, louder and brighter when open.
    const double open = rise (0.8, 3.6, t) - rise (4.6, 7.4, t);
    Frame f;
    f.f0 = 220.0;
    f.vowel = mix3 (oohFormants, ohFormants, ahFormants, open);
    f.vibratoDepth = 0.007 * rise (0.3, 1.3, t);
    f.vibratoRate = 5.0;
    f.level = 0.72 + 0.28 * open;
    f.tilt = 1.15 - 0.25 * open;
    return f;
}

Frame risingEe (double t)
{
    // "ah" to "eh" to "ee", growing brighter, the vibrato quickening as it goes.
    const double p = rise (0.8, 6.8, t);
    Frame f;
    f.f0 = 246.94;
    f.vowel = mix3 (ahFormants, ehFormants, eeFormants, p);
    f.vibratoDepth = 0.006 * rise (0.4, 1.4, t) + 0.004 * p;
    f.vibratoRate = 4.8 + 0.9 * p;
    f.level = 0.85 + 0.15 * p;
    f.tilt = 1.05 - 0.2 * p;
    return f;
}

Frame swellingAlto (double t)
{
    // A straight "oh" that swells (messa di voce): louder and brighter, and the
    // vibrato grows in from nothing and quickens. Then it eases back.
    const double swell = rise (0.2, 4.8, t) - 0.6 * rise (5.0, 7.8, t);
    const double vib = rise (1.2, 4.8, t);
    Frame f;
    f.f0 = 196.0 * semis (-0.5 * (1.0 - rise (0.0, 0.3, t)));   // a small scoop up to the note
    f.vowel = mix (ohFormants, ahFormants, 0.25 * swell);
    f.vibratoDepth = 0.02 * vib;
    f.vibratoRate = 4.6 + 0.8 * vib;
    f.level = 0.35 + 0.65 * swell;
    f.tilt = 1.4 - 0.55 * swell;
    return f;
}

Frame wakingAh (double t)
{
    // An "ah" that starts nearly pure, little more than its fundamental, and wakes up: the
    // harmonics open out one after another until it is full and bright.
    const double wake = rise (0.6, 4.2, t);
    Frame f;
    f.f0 = 220.0;
    f.vowel = mix (oohFormants, ahFormants, 0.3 + 0.7 * wake);
    f.vibratoDepth = 0.006 * rise (3.0, 4.5, t);
    f.vibratoRate = 5.1;
    f.level = 0.6 + 0.4 * wake;
    f.tilt = 2.6 - 1.8 * wake;
    f.brilliance = 0.9 * wake;
    return f;
}

Frame smallChoir (double t)
{
    // Six singers on "ah", the vowel rounding towards "oh" and back.
    const double round = rise (1.5, 4.0, t) - rise (4.8, 7.2, t);
    Frame f;
    f.f0 = 196.0;
    f.vowel = mix (ahFormants, ohFormants, 0.8 * round);
    f.vibratoDepth = 0.008;
    f.vibratoRate = 5.1;
    f.level = 0.9 + 0.1 * round;
    f.tilt = 1.1;
    return f;
}

Frame lowOm (double t)
{
    // A low "oh" that closes through "ooh" to a hummed "mm".
    const double close = rise (2.5, 5.2, t);
    Frame f;
    f.f0 = 110.0;
    f.vowel = mix3 (ohFormants, oohFormants, humFormants, close);
    f.vibratoDepth = 0.004 * rise (0.6, 2.0, t);
    f.vibratoRate = 4.6;
    f.level = 1.0 - 0.25 * close;
    f.tilt = 1.1 + 0.5 * close;
    return f;
}

Frame highEe (double t)
{
    // A light soprano "ee", easing towards "ih" and back, with a quick, even vibrato.
    const double ease = 0.5 - 0.5 * std::cos (twoPi * t / 8.0);
    Frame f;
    f.f0 = 523.25;
    f.vowel = mix (highEeFormants, highIhFormants, ease);
    f.vibratoDepth = 0.011 * rise (0.3, 1.0, t);
    f.vibratoRate = 5.9 + 0.2 * std::sin (twoPi * 0.23 * t);
    f.level = 0.9 + 0.1 * ease;
    f.tilt = 0.9;
    return f;
}

Frame slowWah (double t)
{
    // "ooh" and "ah" in turn, four times across the take, louder as it opens.
    const double open = 0.5 - 0.5 * std::cos (twoPi * t / 2.0);
    Frame f;
    f.f0 = 164.81;
    f.vowel = mix (oohFormants, ahFormants, open);
    f.vibratoDepth = 0.005 * rise (0.5, 1.5, t);
    f.vibratoRate = 5.0;
    f.level = 0.62 + 0.38 * open;
    f.tilt = 1.2 - 0.3 * open;
    return f;
}

Frame overtoneDrone (double t)
{
    // Throat singing: one narrow resonance picks out a high harmonic of the drone and sweeps
    // up and down the series, twice across the take.
    const double sweep = 0.5 - 0.5 * std::cos (twoPi * t / 4.0);
    Frame f;
    f.f0 = 130.81;
    f.vowel = { { { 420, 70 }, { 800.0 * std::pow (2.0, sweep), 25 }, { 2600, 250 }, { 3400, 350 } } };
    f.vibratoDepth = 0.0015;
    f.vibratoRate = 4.5;
    f.level = 1.0;
    f.tilt = 0.9;
    return f;
}

Frame fallingSigh (double t)
{
    // An "ah" that sighs down a whole tone near the end, softening and darkening as it falls.
    const double fall = rise (4.8, 7.2, t);
    Frame f;
    f.f0 = 233.08 * semis (-2.0 * fall);
    f.vowel = mix (ahFormants, ohFormants, 0.4 * fall);
    f.vibratoDepth = 0.008 * rise (0.5, 1.5, t) * (1.0 - fall);
    f.vibratoRate = 5.3;
    f.level = 1.0 - 0.45 * fall;
    f.tilt = 1.0 + 0.5 * fall;
    f.brilliance = 0.6 * (1.0 - fall);
    return f;
}

Score scoreFor (PlaceholderVowel v)
{
    switch (v)
    {
        case PlaceholderVowel::openingOoh:    return { 1, 0.0, openingOoh };
        case PlaceholderVowel::risingEe:      return { 1, 0.0, risingEe };
        case PlaceholderVowel::swellingAlto:  return { 1, 0.0, swellingAlto };
        case PlaceholderVowel::wakingAh:      return { 1, 0.0, wakingAh };
        case PlaceholderVowel::smallChoir:    return { 6, 9.0, smallChoir };
        case PlaceholderVowel::lowOm:         return { 1, 0.0, lowOm };
        case PlaceholderVowel::highEe:        return { 1, 0.0, highEe };
        case PlaceholderVowel::slowWah:       return { 1, 0.0, slowWah };
        case PlaceholderVowel::overtoneDrone: return { 1, 0.0, overtoneDrone };
        case PlaceholderVowel::fallingSigh:   return { 1, 0.0, fallingSigh };
        default:                              return {};
    }
}

PlaceholderVoice singScore (const Score& score, double sr, int n)
{
    constexpr double designSeconds = 8.0;
    constexpr int maxHarmonics = 160;
    constexpr int block = 64;
    const double dur = (double) n / sr;
    const double toDesign = designSeconds / dur;
    const double topHz = std::min (0.45 * sr, 16000.0);

    PlaceholderVoice voice;
    voice.sampleRate = sr;
    voice.rootHz = (float) score.at (designSeconds * 0.5).f0;

    // Each singer is a little different: tuning, vibrato speed and phase, wander, loudness.
    struct Singer
    {
        double detune = 1.0, rate = 1.0, vibratoPhase = 0.0, driftPhase = 0.0, gain = 1.0, phase = 0.0;
        std::array<double, maxHarmonics + 1> amp {}, lastAmp {};
        double norm = 0.0, lastNorm = 0.0;
        int top = 1;   // the highest harmonic sounding in this block or the last
    };
    std::vector<Singer> singers ((size_t) score.singers);
    Noise rng { 0x9e3779b9u };
    for (size_t k = 0; k < singers.size(); ++k)
    {
        auto& s = singers[k];
        if (singers.size() > 1)
        {
            const double spread = (double) k / (double) (singers.size() - 1) * 2.0 - 1.0;
            s.detune = std::pow (2.0, spread * score.spreadCents / 1200.0);
            s.rate = 1.0 + 0.12 * rng.next();
            s.vibratoPhase = twoPi * (0.5 + 0.5 * rng.next());
            s.driftPhase = twoPi * (0.5 + 0.5 * rng.next());
            s.gain = 0.85 + 0.15 * rng.next();
        }
    }

    std::vector<double> out ((size_t) n);
    bool first = true;

    for (int start = 0; start < n; start += block)
    {
        const double t = (double) start / sr;
        const Frame fr = score.at (t * toDesign);

        for (auto& s : singers)
        {
            s.vibratoPhase += twoPi * fr.vibratoRate * s.rate * toDesign * (double) block / sr;
            s.driftPhase += twoPi * 0.31 * toDesign * (double) block / sr;
            const double drift = 0.0015 * std::sin (s.driftPhase) + 0.001 * std::sin (2.37 * s.driftPhase + 1.0);
            const double f = fr.f0 * s.detune * (1.0 + fr.vibratoDepth * std::sin (s.vibratoPhase) + drift);

            s.lastAmp = s.amp;
            s.lastNorm = s.norm;
            double power = 0.0;
            for (int h = 1; h <= maxHarmonics; ++h)
            {
                const double hf = f * h;
                // Above the top formant the resonances alone would roll the voice off too fast
                // to hear any shine; Brilliance lifts it back up (as a real voice's higher
                // formants do), capped at +48 dB, and the top fades out before Nyquist.
                const double lift = std::min (std::pow (std::max (1.0, hf / fr.vowel[3].hz), 8.0 * fr.brilliance), 250.0);
                const double fade = 1.0 - smoothstep (0.75 * topHz, topHz, hf);
                const double a = hf < topHz ? fade * lift * vowelGain (fr.vowel, hf) / std::pow ((double) h, fr.tilt) : 0.0;
                s.amp[(size_t) h] = a;
                power += a * a;
            }
            s.norm = 1.0 / std::sqrt (std::max (power, 1.0e-12));
            const int lastTop = s.top;
            s.top = std::clamp ((int) (topHz / f), 1, maxHarmonics);
            const int sumTo = std::max (s.top, first ? s.top : lastTop);
            if (first)
            {
                s.lastAmp = s.amp;
                s.lastNorm = s.norm;
            }
            s.phase = std::fmod (s.phase, twoPi);
            s.vibratoPhase = std::fmod (s.vibratoPhase, twoPi);

            // Sum the harmonics for this block, easing from the last block's levels.
            const int end = std::min (n, start + block);
            const double step = twoPi * f / sr;
            for (int i = start; i < end; ++i)
            {
                const double w = (double) (i - start) / (double) block;
                s.phase += step;
                const double c2 = 2.0 * std::cos (s.phase);
                double prev = 0.0, cur = std::sin (s.phase), sum = 0.0;
                for (int h = 1; h <= sumTo; ++h)
                {
                    const double a = s.lastAmp[(size_t) h] * s.lastNorm + (s.amp[(size_t) h] * s.norm - s.lastAmp[(size_t) h] * s.lastNorm) * w;
                    sum += a * cur;
                    const double next = c2 * cur - prev;
                    prev = cur;
                    cur = next;
                }
                out[(size_t) i] += s.gain * sum / std::sqrt ((double) singers.size());
            }
        }
        first = false;

        const int end = std::min (n, start + block);
        for (int i = start; i < end; ++i)
            out[(size_t) i] *= fr.level;
    }

    // Swell in, hold, fall away, with a little unevenness so it breathes like a person.
    double peak = 0.0, power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double env = smoothstep (0.0, 0.4, t) * (1.0 - smoothstep (dur - 0.8, dur, t))
                         * (0.95 + 0.03 * std::sin (twoPi * 0.37 * t * toDesign) + 0.02 * std::sin (twoPi * 0.83 * t * toDesign + 2.0));
        out[(size_t) i] *= env;
        peak = std::max (peak, std::abs (out[(size_t) i]));
        power += out[(size_t) i] * out[(size_t) i];
    }

    // Level them by loudness (about -16 dB RMS) rather than peak, so a bright, steady voice is
    // no louder than a moving one, and never let the peak pass -6 dB.
    const double rms = std::sqrt (power / (double) n);
    const double gain = peak > 0.0 ? std::min (0.5 / peak, 0.16 / std::max (rms, 1.0e-9)) : 1.0;
    voice.samples.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        voice.samples[(size_t) i] = (float) (out[(size_t) i] * gain);
    return voice;
}
} // namespace

float placeholderSeconds (PlaceholderVowel vowel)
{
    return scoreFor (vowel).at != nullptr || isPlaceholderSource (vowel) ? 8.0f : 5.0f;
}

PlaceholderVoice makePlaceholderVoice (PlaceholderVowel vowel, double sr, float seconds)
{
    if (isPlaceholderSource (vowel))
        return makePlaceholderSource (vowel, sr, seconds);

    const int n = std::max (1, (int) (seconds * sr));
    if (const auto score = scoreFor (vowel); score.at != nullptr)
        return singScore (score, sr, n);

    PlaceholderVoice voice;
    voice.sampleRate = sr;
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
    const double tilt = vowel == PlaceholderVowel::hum ? 1.6 : 1.0;   // the hum is darker
    voice.rootHz = (float) f0;

    // Pure harmonic tone, no breath noise; the harmonics above the top formant are lifted for
    // shine, as in the moving voices (see singScore).
    constexpr int maxHarmonics = 96;
    constexpr int block = 64;
    constexpr double brilliance = 0.9;
    const double topHz = std::min (0.45 * sr, 16000.0);
    std::array<double, maxHarmonics + 1> amp {};

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
            const double lift = std::min (std::pow (std::max (1.0, hf / formants[3].hz), 8.0 * brilliance), 250.0);
            const double fade = 1.0 - smoothstep (0.75 * topHz, topHz, hf);
            amp[(size_t) h] = hf < topHz ? fade * lift * vowelGain (formants, hf) / std::pow ((double) h, tilt) : 0.0;
            norm += amp[(size_t) h] * amp[(size_t) h];
        }
        norm = 1.0 / std::sqrt (std::max (norm, 1.0e-12));

        const int end = std::min (n, start + block);
        for (int i = start; i < end; ++i)
        {
            phase += twoPi * f / sr;
            if (phase > twoPi)
                phase -= twoPi;

            // sin (h x) by the recurrence sin (h x) = 2 cos x sin ((h - 1) x) - sin ((h - 2) x).
            const double c2 = 2.0 * std::cos (phase);
            double prev = 0.0, cur = std::sin (phase), s = 0.0;
            for (int h = 1; h <= maxHarmonics; ++h)
            {
                s += amp[(size_t) h] * cur;
                const double next = c2 * cur - prev;
                prev = cur;
                cur = next;
            }
            out[(size_t) i] = s * norm;
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
