#include "VoiceSynth.h"

#include "Blocks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sing::dsp
{

namespace
{
    struct Formant
    {
        float hz, bandwidth;
    };

    struct Recipe
    {
        float note;               // MIDI
        float seconds;
        std::array<Formant, 5> formants;
        float zeroHz = 0.0f, zeroBandwidth = 0.0f; // nasal antiresonance (0 = none)
        float tilt;               // source roll-off: harmonic amplitude ~ 1 / h^tilt
        float breath;             // 0..1 noise share
        float vibratoCents, vibratoHz, vibratoDelay;
        float onset, offset;      // seconds
        int breaths;              // 0 = one sustained note; otherwise this many swells
    };

    Recipe recipeFor (FactoryVoice v)
    {
        switch (v)
        {
            case FactoryVoice::hollowChoir: // a breathy alto "ooh"
                return { 57.0f, 4.8f, { { { 330, 70 }, { 760, 90 }, { 2600, 140 }, { 3300, 220 }, { 4200, 280 } } },
                         0, 0, 1.35f, 0.32f, 16.0f, 5.1f, 0.7f, 0.45f, 0.5f, 0 };
            case FactoryVoice::distantAh: // a soft tenor "ah"
                return { 50.0f, 3.1f, { { { 700, 90 }, { 1150, 110 }, { 2500, 140 }, { 3400, 220 }, { 4300, 280 } } },
                         0, 0, 1.2f, 0.18f, 13.0f, 4.8f, 0.6f, 0.35f, 0.45f, 0 };
            case FactoryVoice::glassHum: // a hummed "mm"
                return { 64.0f, 6.2f, { { { 270, 55 }, { 1100, 180 }, { 2300, 250 }, { 3200, 320 }, { 4300, 400 } } },
                         1350, 140, 1.5f, 0.08f, 9.0f, 5.4f, 1.0f, 0.5f, 0.6f, 0 };
            case FactoryVoice::lastLight: // two breaths with a faint "ha"
                return { 55.0f, 2.4f, { { { 800, 160 }, { 1250, 170 }, { 2600, 220 }, { 3500, 300 }, { 4500, 380 } } },
                         0, 0, 1.6f, 0.78f, 6.0f, 4.6f, 0.4f, 0.12f, 0.12f, 2 };
        }
        return recipeFor (FactoryVoice::hollowChoir);
    }

    // Magnitude of the vocal tract at f: a cascade of resonances, unity at DC.
    float tract (const Recipe& r, float f) noexcept
    {
        double g = 1.0;
        for (const auto& fm : r.formants)
        {
            const double F2 = static_cast<double> (fm.hz) * fm.hz;
            const double d = F2 - static_cast<double> (f) * f;
            g *= F2 / std::sqrt (d * d + std::pow (static_cast<double> (fm.bandwidth) * f, 2.0));
        }
        if (r.zeroHz > 0.0f)
        {
            const double Z2 = static_cast<double> (r.zeroHz) * r.zeroHz;
            const double d = Z2 - static_cast<double> (f) * f;
            g *= std::sqrt (d * d + std::pow (static_cast<double> (r.zeroBandwidth) * f, 2.0)) / Z2;
        }
        return static_cast<float> (g);
    }

    // Smooth random wobble, about +-1.
    struct Wobble
    {
        DriftLfo lfo;
        Wobble (double rate, float hz, float seed) { lfo.prepare (rate); lfo.setFrequency (hz); lfo.setPhase (seed); }
        float next() noexcept { return lfo.next(); }
    };
}

SynthesisedVoice synthesise (FactoryVoice which, double fs)
{
    const Recipe r = recipeFor (which);
    SynthesisedVoice out;
    out.sampleRate = fs;
    out.rootNote = r.note;
    const auto n = static_cast<std::size_t> (r.seconds * fs);
    out.samples.assign (n, 0.0f);

    const float f0 = 440.0f * std::exp2 ((r.note - 69.0f) / 12.0f);
    const int maxHarmonics = static_cast<int> (0.45 * fs / (f0 * 0.97f));
    std::vector<float> amp (static_cast<std::size_t> (maxHarmonics + 1), 0.0f);
    std::vector<double> phase (static_cast<std::size_t> (maxHarmonics + 1), 0.0);

    constexpr int kBlock = 32;
    Wobble jitter (fs / kBlock, 7.0f, 0.13f + static_cast<float> (which));
    Wobble drift (fs / kBlock, 0.35f, 0.57f + static_cast<float> (which));
    Wobble shimmer (fs / kBlock, 2.3f, 0.91f + static_cast<float> (which));
    Wobble rate (fs / kBlock, 0.4f, 0.29f + static_cast<float> (which));

    // Breath: noise through the same resonances, in parallel, weighted by the tract.
    std::array<Biquad, 5> noiseBands;
    std::array<float, 5> noiseGain {};
    for (std::size_t k = 0; k < noiseBands.size(); ++k)
    {
        const auto& fm = r.formants[k];
        noiseBands[k].setBandpass (fm.hz, fm.hz / fm.bandwidth, fs);
        noiseGain[k] = tract (r, fm.hz) / std::pow (fm.hz / 300.0f, 0.6f);
    }
    Biquad noiseHp;
    noiseHp.setHighpass (400.0f, fs);
    std::uint32_t seed = 0x2545F491u + static_cast<std::uint32_t> (which) * 977u;
    auto noise = [&seed]
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float> (seed >> 8) * (2.0f / 16777216.0f) - 1.0f;
    };

    double vibPhase = 0.0;
    float f = f0;
    for (std::size_t start = 0; start < n; start += kBlock)
    {
        const float t = static_cast<float> (static_cast<double> (start) / fs);

        // Pitch: delayed vibrato, slow drift and a little jitter, in cents.
        const float vibAmount = std::clamp ((t - r.vibratoDelay) / 0.8f, 0.0f, 1.0f);
        const float cents = vibAmount * r.vibratoCents * static_cast<float> (std::sin (2.0 * std::numbers::pi * vibPhase))
                          + 6.0f * drift.next() + 2.5f * jitter.next();
        f = f0 * std::exp2 (cents / 1200.0f);
        vibPhase += kBlock * (r.vibratoHz * (1.0f + 0.06f * rate.next())) / fs;

        // Loudness envelope.
        float env;
        if (r.breaths == 0)
        {
            env = std::min (1.0f, t / r.onset);
            env = env * env * (3.0f - 2.0f * env);
            const float tail = std::clamp ((r.seconds - t) / r.offset, 0.0f, 1.0f);
            env *= tail * tail * (3.0f - 2.0f * tail);
            env *= 1.0f + 0.05f * shimmer.next();
        }
        else
        {
            const float each = r.seconds / static_cast<float> (r.breaths);
            const float local = std::fmod (t, each) / each;
            env = std::pow (std::max (0.0f, std::sin (std::numbers::pi_v<float> * std::clamp ((local - 0.06f) / 0.88f, 0.0f, 1.0f))), 1.6f);
            env *= 1.0f + 0.06f * shimmer.next();
        }

        // Harmonic amplitudes for this block (below 0.45 of the sample rate only).
        const int harmonics = std::min (maxHarmonics, static_cast<int> (0.45 * fs / f));
        for (int h = 1; h <= maxHarmonics; ++h)
            amp[static_cast<std::size_t> (h)] = h <= harmonics
                ? env * tract (r, f * static_cast<float> (h)) / std::pow (static_cast<float> (h), r.tilt)
                : 0.0f;

        const auto end = std::min (n, start + kBlock);
        const double inc = f / fs;
        for (std::size_t i = start; i < end; ++i)
        {
            double voiced = 0.0;
            for (int h = 1; h <= harmonics; ++h)
            {
                auto& ph = phase[static_cast<std::size_t> (h)];
                ph += inc * h;
                ph -= std::floor (ph);
                voiced += amp[static_cast<std::size_t> (h)] * std::sin (2.0 * std::numbers::pi * ph);
            }
            // Breath, pulsing a little with the glottal cycle.
            const float pulse = 0.6f + 0.4f * static_cast<float> (std::cos (2.0 * std::numbers::pi * phase[1]));
            const float w = noiseHp.process (noise());
            float breath = 0.0f;
            for (std::size_t k = 0; k < noiseBands.size(); ++k)
                breath += noiseGain[k] * noiseBands[k].process (w);
            const float voicedShare = 1.0f - r.breath;
            out.samples[i] = static_cast<float> (voicedShare * voiced) + r.breath * 2.5f * env * pulse * breath;
        }
    }

    // Even loudness across the factory voices: the sung part (anything within 30 dB of
    // the peak) at -18 dBFS RMS, with the peak kept under -3 dBFS.
    float peak = 1.0e-9f;
    for (const float s : out.samples)
        peak = std::max (peak, std::abs (s));
    double sum = 0.0;
    std::size_t count = 0;
    constexpr std::size_t kWindow = 1024;
    for (std::size_t i = 0; i + kWindow <= out.samples.size(); i += kWindow)
    {
        double w = 0.0;
        for (std::size_t k = 0; k < kWindow; ++k)
            w += static_cast<double> (out.samples[i + k]) * out.samples[i + k];
        if (std::sqrt (w / kWindow) > 0.0316 * peak)
        {
            sum += w;
            count += kWindow;
        }
    }
    const double rms = count > 0 ? std::sqrt (sum / static_cast<double> (count)) : peak;
    const float g = static_cast<float> (std::min (0.1259 / rms, 0.7079 / peak));
    for (auto& s : out.samples)
        s *= g;
    return out;
}

float detectPitch (const float* x, std::size_t numSamples, double fs)
{
    const int window = static_cast<int> (std::max (512.0, 0.025 * fs));
    const int tauMin = static_cast<int> (fs / 1000.0);
    const int tauMax = static_cast<int> (fs / 65.0);
    const auto needed = static_cast<std::size_t> (window + tauMax + 2);
    if (numSamples < needed)
        return -1.0f;

    float peak = 0.0f;
    for (std::size_t i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (x[i]));

    // Up to 48 frames spread through the middle 80%.
    std::vector<float> found;
    std::vector<double> d (static_cast<std::size_t> (tauMax + 2));
    const std::size_t first = numSamples / 10;
    const std::size_t last = numSamples - numSamples / 10 > needed ? numSamples - numSamples / 10 - needed : 0;
    const int frames = 48;
    for (int fr = 0; fr < frames; ++fr)
    {
        const std::size_t pos = first + (last > first ? (last - first) * static_cast<std::size_t> (fr) / (frames - 1) : 0);
        if (pos + needed > numSamples)
            break;
        const float* s = x + pos;

        double energy = 0.0;
        for (int i = 0; i < window; ++i)
            energy += static_cast<double> (s[i]) * s[i];
        if (std::sqrt (energy / window) < 0.03 * peak)
            continue; // too quiet to trust

        // Difference function, then the cumulative mean normalised version.
        d[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double sum = 0.0;
            for (int i = 0; i < window; ++i)
            {
                const double diff = static_cast<double> (s[i]) - s[i + tau];
                sum += diff * diff;
            }
            running += sum;
            d[static_cast<std::size_t> (tau)] = running > 0.0 ? sum * tau / running : 1.0;
        }

        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (d[static_cast<std::size_t> (tau)] < 0.15)
            {
                while (tau + 1 < tauMax && d[static_cast<std::size_t> (tau + 1)] < d[static_cast<std::size_t> (tau)])
                    ++tau;
                best = tau;
                break;
            }
        if (best < 0)
            continue;

        // Parabolic interpolation around the dip.
        const double a = d[static_cast<std::size_t> (best - 1)], b = d[static_cast<std::size_t> (best)], c = d[static_cast<std::size_t> (best + 1)];
        const double denom = a - 2.0 * b + c;
        const double refined = best + (std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0);
        found.push_back (static_cast<float> (fs / refined));
    }

    if (found.size() < 3)
        return -1.0f;
    std::nth_element (found.begin(), found.begin() + static_cast<std::ptrdiff_t> (found.size() / 2), found.end());
    const float hz = found[found.size() / 2];
    return 69.0f + 12.0f * std::log2 (hz / 440.0f);
}

void trimSilence (std::vector<float>& s, double fs)
{
    float peak = 0.0f;
    for (const float v : s)
        peak = std::max (peak, std::abs (v));
    if (peak <= 0.0f)
        return;
    const float threshold = peak * 0.00316f; // -50 dB
    std::size_t a = 0, b = s.size();
    while (a < b && std::abs (s[a]) < threshold) ++a;
    while (b > a && std::abs (s[b - 1]) < threshold) --b;
    const auto margin = static_cast<std::size_t> (0.01 * fs);
    a = a > margin ? a - margin : 0;
    b = std::min (s.size(), b + margin);
    s = std::vector<float> (s.begin() + static_cast<std::ptrdiff_t> (a), s.begin() + static_cast<std::ptrdiff_t> (b));
}

} // namespace sing::dsp
