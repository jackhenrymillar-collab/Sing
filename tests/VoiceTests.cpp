// The loop, the grains and the factory voices.
#include "TestHelpers.h"
#include "dsp/Loop.h"
#include "dsp/VoiceSynth.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>

using namespace sing;

namespace
{
    constexpr double kFs = 48000.0;

    std::vector<float> noise (double seconds, double fs = kFs)
    {
        dsp::Random rng (7u);
        std::vector<float> s (static_cast<std::size_t> (seconds * fs));
        for (auto& x : s) x = 0.3f * rng.bipolar();
        return s;
    }

    dsp::Loop makeLoop (const dsp::VoiceData& v, double start, double length, double fade, dsp::LoopMode mode)
    {
        dsp::Loop l;
        l.start = start * v.getLength();
        l.end = (start + length) * v.getLength();
        l.fade = std::max (0.005 * 2.0 * v.getSourceRate(), fade * 0.5 * (l.end - l.start));
        l.mode = mode;
        return l;
    }

    // RMS in short windows, as dB relative to the mean of all windows: min and max.
    std::pair<double, double> levelSpread (const std::vector<float>& x, std::size_t window)
    {
        std::vector<double> levels;
        for (std::size_t i = 0; i + window <= x.size(); i += window)
        {
            double s = 0.0;
            for (std::size_t k = 0; k < window; ++k) s += static_cast<double> (x[i + k]) * x[i + k];
            levels.push_back (s / static_cast<double> (window));
        }
        double mean = 0.0;
        for (double l : levels) mean += l;
        mean /= static_cast<double> (levels.size());
        double lo = 1e9, hi = -1e9;
        for (double l : levels)
        {
            const double db = 10.0 * std::log10 (l / mean);
            lo = std::min (lo, db);
            hi = std::max (hi, db);
        }
        return { lo, hi };
    }
}

TEST_CASE ("The loop plays on without dips or bumps at the seam", "[loop]")
{
    // With noise (every stretch different, like a voice), equal-power fades keep the level steady.
    const auto mode = GENERATE (dsp::LoopMode::forward, dsp::LoopMode::backward, dsp::LoopMode::backAndForth);
    const double fade = GENERATE (0.0, 0.3, 1.0);
    const auto s = noise (3.0);
    const auto v = dsp::VoiceData::build (s.data(), s.size(), kFs, 60.0f);
    const auto loop = makeLoop (*v, 0.3, 0.1, fade, mode); // 0.3 s loop: many trips in 4 s
    dsp::LoopPlayer player;
    player.start (loop, mode == dsp::LoopMode::backward ? loop.end : loop.start, mode == dsp::LoopMode::backward ? -1 : 1);
    std::vector<float> out (static_cast<std::size_t> (4.0 * 2.0 * kFs));
    for (auto& x : out)
    {
        x = player.next (*v, loop, 1.0, 0.001f);
        REQUIRE (player.position() >= loop.start - 1.0);
        REQUIRE (player.position() <= loop.end + 1.0);
    }
    const auto [lo, hi] = levelSpread (out, 4800); // 25 ms windows
    INFO ("level range " << lo << " to " << hi << " dB");
    CHECK (lo > -1.5);
    CHECK (hi < 1.5);
}

TEST_CASE ("The loop never clicks, even with the shortest fade", "[loop]")
{
    // A tone whose loop doesn't hold a whole number of cycles: without a fade it would
    // jump every trip. The steepest step must stay close to the tone's own.
    const auto mode = GENERATE (dsp::LoopMode::forward, dsp::LoopMode::backward, dsp::LoopMode::backAndForth);
    const auto s = test::sine (220.0, kFs, 2.0, 0.5f);
    const auto v = dsp::VoiceData::build (s.data(), s.size(), kFs, 57.0f);
    const auto loop = makeLoop (*v, 0.2137, 0.1013, 0.0, mode);
    dsp::LoopPlayer player;
    player.start (loop, mode == dsp::LoopMode::backward ? loop.end : loop.start, mode == dsp::LoopMode::backward ? -1 : 1);
    float maxStep = 0.0f, last = 0.0f;
    for (int i = 0; i < static_cast<int> (3.0 * 2.0 * kFs); ++i)
    {
        const float x = player.next (*v, loop, 1.0, 0.001f);
        if (i > 0) maxStep = std::max (maxStep, std::abs (x - last));
        last = x;
    }
    const float toneStep = static_cast<float> (0.5 * 2.0 * std::numbers::pi * 220.0 / (2.0 * kFs));
    INFO ("steepest step " << maxStep << ", the tone's own " << toneStep);
    CHECK (maxStep < 1.5f * toneStep);
}

TEST_CASE ("Moving the loop under the playhead doesn't click", "[loop]")
{
    const auto s = test::sine (220.0, kFs, 2.0, 0.5f);
    const auto v = dsp::VoiceData::build (s.data(), s.size(), kFs, 57.0f);
    auto loop = makeLoop (*v, 0.5, 0.2, 0.3, dsp::LoopMode::forward);
    dsp::LoopPlayer player;
    player.start (loop, loop.start + 1000.0, 1);
    float maxStep = 0.0f, last = player.next (*v, loop, 1.0, 1.0f / 576.0f);
    for (int i = 0; i < 40000; ++i)
    {
        if (i == 20000)
            loop = makeLoop (*v, 0.1, 0.1, 0.3, dsp::LoopMode::forward); // playhead now far outside
        const float x = player.next (*v, loop, 1.0, 1.0f / 576.0f);
        maxStep = std::max (maxStep, std::abs (x - last));
        last = x;
    }
    const float toneStep = static_cast<float> (0.5 * 2.0 * std::numbers::pi * 220.0 / (2.0 * kFs));
    CHECK (maxStep < 1.5f * toneStep);
    CHECK (player.position() >= loop.start);
    CHECK (player.position() <= loop.end);
}

TEST_CASE ("Back and forth turns round at both ends", "[loop]")
{
    const auto s = noise (2.0);
    const auto v = dsp::VoiceData::build (s.data(), s.size(), kFs, 60.0f);
    const auto loop = makeLoop (*v, 0.25, 0.25, 0.3, dsp::LoopMode::backAndForth);
    dsp::LoopPlayer player;
    player.start (loop, loop.start, 1);
    int turns = 0, dir = 1;
    for (int i = 0; i < static_cast<int> (3.0 * 2.0 * kFs); ++i)
    {
        player.next (*v, loop, 1.0, 0.001f);
        if (player.direction() != dir) { ++turns; dir = player.direction(); }
    }
    // A 0.5 s loop (0.35 s per leg once the fades overlap), for 3 s: about eight turns.
    CHECK (turns >= 6);
    CHECK (turns <= 10);
}

TEST_CASE ("Grains hold a steady level, moving or frozen", "[grains]")
{
    const double rate = GENERATE (0.0, 0.3, 1.0);
    const auto mode = GENERATE (dsp::LoopMode::forward, dsp::LoopMode::backAndForth);
    const auto s = noise (3.0);
    const auto v = dsp::VoiceData::build (s.data(), s.size(), kFs, 60.0f);
    const auto loop = makeLoop (*v, 0.3, 0.3, 0.3, mode);
    dsp::GrainCloud cloud;
    cloud.start (loop, loop.start, 1, 99u);
    const float grain = static_cast<float> (0.15 * 2.0 * kFs);
    std::vector<float> out (static_cast<std::size_t> (3.0 * 2.0 * kFs));
    for (auto& x : out)
        x = cloud.process (*v, loop, 1.0, rate, grain, 0.03 * 2.0 * 2.0 * kFs);
    // Skip the first grain length while the cloud fills.
    std::vector<float> settled (out.begin() + static_cast<std::ptrdiff_t> (grain), out.end());
    const auto [lo, hi] = levelSpread (settled, 9600); // 50 ms windows
    INFO ("level range " << lo << " to " << hi << " dB");
    CHECK (lo > -2.0);
    CHECK (hi < 2.0);
    CHECK (test::rms (settled) > 0.1);
}

TEST_CASE ("Factory voices are sung at their root note", "[voices]")
{
    const auto which = GENERATE (dsp::FactoryVoice::hollowChoir, dsp::FactoryVoice::distantAh, dsp::FactoryVoice::glassHum);
    const auto voice = dsp::synthesise (which);
    REQUIRE (voice.samples.size() > 48000);
    float peak = 0.0f;
    for (float x : voice.samples) { REQUIRE (std::isfinite (x)); peak = std::max (peak, std::abs (x)); }
    CHECK (peak < 0.71f);
    CHECK (peak > 0.1f);
    const float detected = dsp::detectPitch (voice.samples.data(), voice.samples.size(), voice.sampleRate);
    INFO ("detected " << detected << ", sung " << voice.rootNote);
    CHECK (std::abs (detected - voice.rootNote) < 0.2f); // within 20 cents (vibrato and drift move it about)
}

TEST_CASE ("The breathy voice is still made, even without a clear pitch", "[voices]")
{
    const auto voice = dsp::synthesise (dsp::FactoryVoice::lastLight);
    REQUIRE (voice.samples.size() > 48000);
    for (float x : voice.samples) REQUIRE (std::isfinite (x));
}

TEST_CASE ("Pitch detection finds a plain tone", "[voices]")
{
    const double hz = GENERATE (82.41, 220.0, 311.13, 659.26);
    const auto s = test::sine (hz, kFs, 1.0);
    const float note = dsp::detectPitch (s.data(), s.size(), kFs);
    CHECK (std::abs (note - static_cast<float> (69.0 + 12.0 * std::log2 (hz / 440.0))) < 0.02f);
}

TEST_CASE ("Silence is trimmed from the ends", "[voices]")
{
    std::vector<float> s (48000, 0.0f);
    for (std::size_t i = 12000; i < 36000; ++i) s[i] = 0.5f * std::sin (0.05f * static_cast<float> (i));
    dsp::trimSilence (s, kFs);
    CHECK (s.size() < 24000 + 2 * 480 + 10);
    CHECK (s.size() > 23000);
}
