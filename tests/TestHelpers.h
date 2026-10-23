#pragma once

#include "dsp/Halfband.h"
#include "dsp/VoiceData.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace sing::test
{

inline std::vector<float> sine (double hz, double fs, double seconds, float amp = 0.5f)
{
    std::vector<float> s (static_cast<std::size_t> (seconds * fs));
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * static_cast<double> (i) / fs));
    return s;
}

// Plays a voice straight through at a pitch ratio the way the engine does: reads at twice
// the output rate, then the half-band filter. Returns output-rate samples.
inline std::vector<float> playAtPitch (const dsp::VoiceData& v, double pitchRatio, double outRate, double startSeconds, int numOut)
{
    const double ratio = v.getSourceRate() / outRate * pitchRatio; // level-0 samples per internal sample
    double pos = startSeconds * 2.0 * v.getSourceRate();
    std::vector<float> inter (static_cast<std::size_t> (2 * numOut));
    for (auto& x : inter)
    {
        x = v.read (pos, ratio);
        pos += ratio;
    }
    dsp::HalfbandDecimator dec;
    dec.prepare();
    std::vector<float> out (static_cast<std::size_t> (numOut));
    dec.process (inter.data(), out.data(), numOut);
    return out;
}

// Fits a sine of a known frequency (plus DC) over the middle of a signal and returns the
// fitted amplitude and the RMS of what's left, both relative to full scale.
struct Fit
{
    double amplitude = 0.0;
    double residualRms = 0.0;
    double residualDb (double reference) const { return 20.0 * std::log10 (std::max (1.0e-12, residualRms) / reference); }
};

inline Fit fitSine (const std::vector<float>& x, double hz, double fs, std::size_t skip)
{
    const std::size_t n = x.size() - 2 * skip;
    // Least squares for a cos + b sin + c.
    double scc = 0, sss = 0, ssc = 0, sc = 0, ss = 0, s1 = 0, xc = 0, xs = 0, x1 = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * std::numbers::pi * hz * static_cast<double> (i + skip) / fs;
        const double c = std::cos (t), s = std::sin (t), v = x[i + skip];
        scc += c * c; sss += s * s; ssc += s * c; sc += c; ss += s; s1 += 1;
        xc += v * c; xs += v * s; x1 += v;
    }
    // Solve the 3x3 normal equations.
    const double m[3][4] = { { scc, ssc, sc, xc }, { ssc, sss, ss, xs }, { sc, ss, s1, x1 } };
    double a[3][4];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) a[r][c] = m[r][c];
    for (int i = 0; i < 3; ++i)
    {
        for (int r = i + 1; r < 3; ++r)
        {
            const double f = a[r][i] / a[i][i];
            for (int c = i; c < 4; ++c) a[r][c] -= f * a[i][c];
        }
    }
    double sol[3];
    for (int i = 2; i >= 0; --i)
    {
        double v = a[i][3];
        for (int c = i + 1; c < 3; ++c) v -= a[i][c] * sol[c];
        sol[i] = v / a[i][i];
    }
    Fit fit;
    fit.amplitude = std::hypot (sol[0], sol[1]);
    double res = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * std::numbers::pi * hz * static_cast<double> (i + skip) / fs;
        const double e = x[i + skip] - (sol[0] * std::cos (t) + sol[1] * std::sin (t) + sol[2]);
        res += e * e;
    }
    fit.residualRms = std::sqrt (res / static_cast<double> (n));
    return fit;
}

// Keeps what a listener could hear: a steep lowpass at 20 kHz (150 dB down by 21 kHz).
inline std::vector<float> lowpass20k (const std::vector<float>& x, double fs)
{
    const auto h = dsp::designLowpass (20500.0 / fs, 1000.0 / fs, 150.0);
    std::vector<float> y (x.size(), 0.0f);
    const std::size_t half = h.size() / 2;
    for (std::size_t i = half; i + half < x.size(); ++i)
    {
        double acc = 0.0;
        for (std::size_t k = 0; k < h.size(); ++k)
            acc += static_cast<double> (h[k]) * x[i + half - k];
        y[i] = static_cast<float> (acc);
    }
    return y;
}

inline double rms (const std::vector<float>& x, std::size_t skip = 0)
{
    double s = 0.0;
    for (std::size_t i = skip; i + skip < x.size(); ++i) s += static_cast<double> (x[i]) * x[i];
    return std::sqrt (s / static_cast<double> (std::max<std::size_t> (1, x.size() - 2 * skip)));
}

} // namespace sing::test
