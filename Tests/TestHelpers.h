#pragma once

// Shared helpers for Sing's DSP tests: test signals, measurements and a render loop.

#include "Stretch/GranularEngine.h"
#include "Stretch/LoopSource.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// CHECK with a note that is printed when it fails.
#define check(cond, what) do { INFO (what); CHECK ((cond)); } while (0)

using namespace sing;

namespace sing::test
{
inline constexpr double sr = 48000.0;
inline constexpr double twoPi = 6.28318530717958647692;

inline std::vector<float> sine (double hz, int n, float amp = 0.5f)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = amp * (float) std::sin (twoPi * hz * i / sr);
    return x;
}

// A vowel-ish test voice: harmonics of 220 Hz with a slow vibrato, plus a little breath noise.
inline std::vector<float> voice (int n)
{
    std::vector<float> x ((size_t) n);
    uint32_t r = 12345;
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f0 = 220.0 * (1.0 + 0.01 * std::sin (twoPi * 5.0 * i / sr));
        phase += twoPi * f0 / sr;
        double s = 0.0;
        for (int h = 1; h <= 12; ++h)
            s += std::sin (h * phase) / (double) (h * h > 9 ? h : 3);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        s += 0.02 * ((double) r / 4294967296.0 - 0.5);
        x[(size_t) i] = (float) (0.2 * s);
    }
    return x;
}

inline std::vector<float> noise (int n, uint32_t seed = 777)
{
    std::vector<float> x ((size_t) n);
    for (auto& v : x)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        v = (float) ((double) seed / 4294967296.0 - 0.5);
    }
    return x;
}

inline double rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt (s / std::max (1, n));
}

inline double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

inline std::shared_ptr<const LoopSource> makeSource (const std::vector<float>& x, LoopSettings s)
{
    const float* ch[] = { x.data() };
    return LoopSource::build (ch, 1, (int) x.size(), sr, s);
}

struct Render
{
    std::vector<float> left, right;
};

inline Render render (GranularEngine& e, int n, int block = 256)
{
    Render r { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; i += block)
        e.process (r.left.data() + i, r.right.data() + i, std::min (block, n - i));
    return r;
}

inline bool allFinite (const Render& r)
{
    for (size_t i = 0; i < r.left.size(); ++i)
        if (! std::isfinite (r.left[i]) || ! std::isfinite (r.right[i]))
            return false;
    return true;
}

inline double zeroCrossingHz (const std::vector<float>& x, int from, int n)
{
    int crossings = 0;
    for (int i = from + 1; i < from + n; ++i)
        if (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f)
            ++crossings;
    return crossings * sr / n;
}

// Standard deviation, in dB, of the level measured in 20 ms windows. Low means steady.
inline double levelWobbleDb (const std::vector<float>& x, int from, int n)
{
    const int w = (int) (0.02 * sr);
    std::vector<double> levels;
    for (int i = from; i + w <= from + n; i += w)
        levels.push_back (db (rms (x.data() + i, w)));
    double mean = 0.0;
    for (double l : levels) mean += l;
    mean /= (double) levels.size();
    double var = 0.0;
    for (double l : levels) var += (l - mean) * (l - mean);
    return std::sqrt (var / (double) levels.size());
}

inline GranularEngine::Settings plainSettings()
{
    GranularEngine::Settings s;
    s.jitterSeconds = 0.0f;
    s.timingJitter = 0.0f;
    s.stereoWidth = 0.0f;
    return s;
}
} // namespace sing::test

