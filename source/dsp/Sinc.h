#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// Windowed-sinc filters for the pitch engine.
//
// How Sing pitches a voice cleanly (the short version):
//  1. When a voice loads, it is upsampled 2x with a long, steep filter, so the stored audio
//     only fills the bottom 42.5% of its band. That leaves a wide empty gap above it.
//  2. Copies an octave, two octaves... lower in sample rate ("levels") are made offline with
//     the same kind of steep filter, so playing far up never has to squeeze too much in.
//  3. Notes are rendered at twice the host's sample rate. Each read uses a short
//     Kaiser-windowed sinc kernel that is very steep relative to the gap (about 140 dB
//     stopband), stretched when pitching up so nothing folds back into the audible band.
//  4. One steep half-band filter brings the mix back to the host rate.
// The result: no audible aliasing pitching up, no imaging pitching down, and a flat
// passband to about 20 kHz, at a modest cost per voice.
namespace sing::dsp
{

// Zeroth-order modified Bessel function of the first kind (for the Kaiser window).
inline double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double> (k) * static_cast<double> (k));
        sum += term;
        if (term < sum * 1.0e-17)
            break;
    }
    return sum;
}

// Kaiser window at x in [-1, 1].
inline double kaiser (double x, double beta) noexcept
{
    if (std::abs (x) >= 1.0)
        return 0.0;
    return besselI0 (beta * std::sqrt (1.0 - x * x)) / besselI0 (beta);
}

inline double sinc (double x) noexcept
{
    if (std::abs (x) < 1.0e-12)
        return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin (px) / px;
}

// Kaiser beta for a stopband attenuation in dB (Kaiser's formula).
inline double kaiserBeta (double attenuationDb) noexcept
{
    if (attenuationDb > 50.0)
        return 0.1102 * (attenuationDb - 8.7);
    if (attenuationDb >= 21.0)
        return 0.5842 * std::pow (attenuationDb - 21.0, 0.4) + 0.07886 * (attenuationDb - 21.0);
    return 0.0;
}

// Odd tap count for a Kaiser lowpass with a transition width (cycles per sample).
inline int kaiserLength (double attenuationDb, double transition) noexcept
{
    const int n = static_cast<int> (std::ceil ((attenuationDb - 7.95) / (2.285 * 2.0 * std::numbers::pi * transition))) + 1;
    return n | 1;
}

// A linear-phase FIR lowpass: cutoff (-6 dB point) in cycles per sample, symmetric, odd length.
inline std::vector<float> designLowpass (double cutoff, double transition, double attenuationDb, double gain = 1.0)
{
    const int n = kaiserLength (attenuationDb, transition);
    const double beta = kaiserBeta (attenuationDb);
    const int half = n / 2;
    std::vector<double> h (static_cast<std::size_t> (n));
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i - half);
        h[static_cast<std::size_t> (i)] = 2.0 * cutoff * sinc (2.0 * cutoff * t) * kaiser (t / (half + 1), beta);
        sum += h[static_cast<std::size_t> (i)];
    }
    std::vector<float> out (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] = static_cast<float> (h[static_cast<std::size_t> (i)] * gain / sum);
    return out;
}

// The interpolation kernels used at play time, as polyphase tables.
//
// Kernel k is a sinc with its cutoff scaled by s_k = 2^(-k / kSteps) (k = 0 is the
// plain Nyquist sinc, k = kSteps is half of it), windowed over kZeroCrossings zero
// crossings of that sinc. Each table holds kPhases + 1 fractional positions, with a
// second table of differences, so a read interpolates linearly between phases with one
// multiply-add per tap. Every phase is normalised to unity gain at DC.
class InterpolationKernels
{
public:
    static constexpr int kSteps = 16;          // cutoff steps per octave of stretch
    static constexpr int kZeroCrossings = 10;  // each side, at s = 1
    static constexpr int kPhases = 512;       // fractional positions per sample
    static constexpr double kAttenuation = 150.0;

    struct Kernel
    {
        int taps = 0;              // even
        std::vector<float> coef;   // (kPhases + 1) * taps
        std::vector<float> delta;  // next phase minus this phase
    };

    static const InterpolationKernels& get()
    {
        static const InterpolationKernels instance;
        return instance;
    }

    const Kernel& kernel (int k) const noexcept { return kernels[static_cast<std::size_t> (std::clamp (k, 0, kSteps))]; }

    // The step that keeps the cutoff at or below 1 / ratio (ratio = source samples per output sample).
    static int stepFor (double ratio) noexcept
    {
        if (ratio <= 1.0)
            return 0;
        return std::min (kSteps, static_cast<int> (std::ceil (kSteps * std::log2 (ratio) - 1.0e-9)));
    }

    static int maxTaps() noexcept { return 2 * ((static_cast<int> (std::ceil (kZeroCrossings * 2.0)) + 3) / 4 * 4); }

private:
    InterpolationKernels()
    {
        const double beta = kaiserBeta (kAttenuation);
        for (int k = 0; k <= kSteps; ++k)
        {
            const double s = std::exp2 (-static_cast<double> (k) / kSteps);
            const double halfWidth = kZeroCrossings / s;     // in samples
            // Rounded up to a multiple of 4 each side, so reads run in blocks of 8 (the extra
            // taps sit outside the window and are zero).
            const int halfTaps = (static_cast<int> (std::ceil (halfWidth)) + 3) / 4 * 4;
            Kernel kern;
            kern.taps = 2 * halfTaps;
            const auto taps = static_cast<std::size_t> (kern.taps);
            kern.coef.resize ((kPhases + 1) * taps);
            for (int p = 0; p <= kPhases; ++p)
            {
                // Tap j sits at sample (base - halfTaps + 1 + j); the read position is
                // base + frac, so its distance from the read position is:
                const double frac = static_cast<double> (p) / kPhases;
                double sum = 0.0;
                std::vector<double> row (taps);
                for (std::size_t j = 0; j < taps; ++j)
                {
                    const double t = frac + halfTaps - 1 - static_cast<double> (j);
                    row[j] = s * sinc (s * t) * kaiser (t / halfWidth, beta);
                    sum += row[j];
                }
                for (std::size_t j = 0; j < taps; ++j)
                    kern.coef[static_cast<std::size_t> (p) * taps + j] = static_cast<float> (row[j] / sum);
            }
            kern.delta.resize (kPhases * taps);
            for (std::size_t i = 0; i < kern.delta.size(); ++i)
                kern.delta[i] = kern.coef[i + taps] - kern.coef[i];
            kernels.push_back (std::move (kern));
        }
    }

    std::vector<Kernel> kernels;
};

} // namespace sing::dsp
