// The whole instrument.
#include "TestHelpers.h"
#include "dsp/Engine.h"
#include "dsp/Formant.h"
#include "dsp/VoiceSynth.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>

using namespace sing;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    dsp::EngineParams plain()
    {
        dsp::EngineParams p;
        p.loopStart = 0.2f; p.loopLength = 0.5f; p.loopFade = 0.3f;
        p.attack = 0.005f; p.release = 0.1f;
        p.stretch = 0.0f; p.spread = 0.0f; p.formant = 0.0f; p.air = 0.0f; p.space = 0.0f;
        p.voices = 8;
        return p;
    }

    std::unique_ptr<dsp::VoiceData> toneVoice (double hz, float root)
    {
        const auto s = test::sine (hz, kFs, 3.0, 0.5f);
        return dsp::VoiceData::build (s.data(), s.size(), kFs, root);
    }

    std::unique_ptr<dsp::VoiceData> factoryVoice (dsp::FactoryVoice which)
    {
        const auto v = dsp::synthesise (which);
        return dsp::VoiceData::build (v.samples.data(), v.samples.size(), v.sampleRate, v.rootNote);
    }

    struct Render
    {
        std::vector<float> left, right;
    };

    Render render (dsp::Engine& e, double seconds)
    {
        Render r;
        const auto n = static_cast<std::size_t> (seconds * kFs);
        r.left.resize (n);
        r.right.resize (n);
        for (std::size_t i = 0; i < n; i += kBlock)
        {
            const int len = static_cast<int> (std::min<std::size_t> (kBlock, n - i));
            e.process (r.left.data() + i, r.right.data() + i, len);
        }
        return r;
    }
}

TEST_CASE ("A note plays the voice at the note's pitch", "[engine]")
{
    const int note = GENERATE (45, 57, 64, 69, 81);
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    e.setParams (plain());
    e.submitVoice (toneVoice (220.0, 57.0f)); // a 220 Hz tone, sung "at" A3
    e.noteOn (note, 1.0f);
    const auto out = render (e, 1.0);
    std::vector<float> tail (out.left.begin() + 12000, out.left.end());
    const float detected = dsp::detectPitch (tail.data(), tail.size(), kFs);
    INFO ("note " << note << " came out as " << detected);
    CHECK (std::abs (detected - static_cast<float> (note)) < 0.05f); // within 5 cents (the drift is 3)
    CHECK (test::rms (tail) > 0.05);
}

TEST_CASE ("Released notes fade out and free their slot", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    auto p = plain();
    p.release = 0.2f;
    e.setParams (p);
    e.submitVoice (toneVoice (220.0, 57.0f));
    e.noteOn (60, 0.8f);
    render (e, 0.3);
    CHECK (e.getActiveNotes() == 1);
    e.noteOff (60);
    render (e, 0.5);
    CHECK (e.getActiveNotes() == 0);
}

TEST_CASE ("The sustain pedal holds released notes", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    e.setParams (plain());
    e.submitVoice (toneVoice (220.0, 57.0f));
    e.sustainPedal (true);
    e.noteOn (60, 0.8f);
    render (e, 0.1);
    e.noteOff (60);
    render (e, 0.5);
    CHECK (e.getActiveNotes() == 1);
    e.sustainPedal (false);
    render (e, 0.5);
    CHECK (e.getActiveNotes() == 0);
}

TEST_CASE ("Polyphony is limited by Voices", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    auto p = plain();
    p.voices = 3;
    e.setParams (p);
    e.submitVoice (toneVoice (220.0, 57.0f));
    for (int k = 0; k < 6; ++k)
    {
        e.noteOn (60 + k, 0.8f);
        render (e, 0.05);
    }
    render (e, 0.2); // stolen notes fade in 15 ms
    CHECK (e.getActiveNotes() == 3);
}

TEST_CASE ("Everything stays finite and under full scale with every tool on", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    auto p = plain();
    p.stretch = 0.7f; p.spread = 1.0f; p.formant = -5.0f; p.air = 8.0f; p.space = 1.0f;
    p.octDown = p.octUp = p.tape = true;
    p.loopMode = dsp::LoopMode::backAndForth;
    p.voices = 16;
    p.outputDb = 12.0f;
    e.setParams (p);
    e.submitVoice (factoryVoice (dsp::FactoryVoice::hollowChoir));
    for (int k = 0; k < 16; ++k)
        e.noteOn (36 + 5 * k, 1.0f);
    const auto out = render (e, 2.0);
    float peak = 0.0f;
    for (std::size_t i = 0; i < out.left.size(); ++i)
    {
        REQUIRE (std::isfinite (out.left[i]));
        REQUIRE (std::isfinite (out.right[i]));
        peak = std::max ({ peak, std::abs (out.left[i]), std::abs (out.right[i]) });
    }
    CHECK (peak <= 1.0f);
    CHECK (peak > 0.1f);
}

TEST_CASE ("Changing the voice mid-note is safe", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    e.setParams (plain());
    e.submitVoice (toneVoice (220.0, 57.0f));
    e.noteOn (60, 0.8f);
    render (e, 0.2);
    for (int i = 0; i < 5; ++i)
    {
        e.submitVoice (toneVoice (330.0 + 10.0 * i, 64.0f));
        render (e, 0.05);
        e.collectGarbage();
    }
    e.noteOn (62, 0.8f);
    const auto out = render (e, 0.3);
    CHECK (test::rms (out.left) > 0.01);
    CHECK (e.getActiveNotes() == 1);
}

TEST_CASE ("Without a voice, notes are silent", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    e.setParams (plain());
    e.noteOn (60, 1.0f);
    const auto out = render (e, 0.2);
    CHECK (test::rms (out.left) == 0.0);
}

TEST_CASE ("Formant at zero passes the sound through untouched", "[formant]")
{
    dsp::FormantShifter f;
    f.prepare (kFs);
    f.setShift (0.0f);
    dsp::Random rng (3u);
    std::vector<float> in (20000), outL (in.size()), outR (in.size());
    for (auto& x : in) x = 0.5f * rng.bipolar();
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        float l = in[i], r = -in[i];
        f.process (l, r);
        outL[i] = l; outR[i] = r;
    }
    const auto lat = static_cast<std::size_t> (f.latency());
    double err = 0.0;
    for (std::size_t i = lat + 2048; i < in.size(); ++i)
        err = std::max (err, static_cast<double> (std::abs (outL[i] - in[i - lat])) + std::abs (outR[i] + in[i - lat]));
    CHECK (err < 1.0e-5);
}

TEST_CASE ("Formant moves the vowel up and down", "[formant]")
{
    // Spectral centroid of a sung vowel, shifted each way.
    const auto voice = dsp::synthesise (dsp::FactoryVoice::distantAh);
    auto centroid = [&] (float semis)
    {
        dsp::FormantShifter f;
        f.prepare (voice.sampleRate);
        f.setShift (semis);
        std::vector<float> out (voice.samples.size());
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            float l = voice.samples[i], r = voice.samples[i];
            f.process (l, r);
            out[i] = l;
        }
        // Centroid via first differences (a crude but monotonic brightness measure).
        double d = 0.0, e = 0.0;
        for (std::size_t i = 1; i < out.size(); ++i)
        {
            d += std::pow (out[i] - out[i - 1], 2.0);
            e += std::pow (out[i], 2.0);
        }
        return d / e;
    };
    const double down = centroid (-5.0f), none = centroid (0.0f), up = centroid (5.0f);
    INFO ("brightness down " << down << ", none " << none << ", up " << up);
    CHECK (down < none * 0.8);
    CHECK (up > none * 1.2);
}

TEST_CASE ("Latency never changes", "[engine]")
{
    dsp::Engine e;
    e.prepare (kFs, kBlock);
    const int a = e.getLatencySamples();
    auto p = plain();
    p.formant = 7.0f;
    e.setParams (p);
    render (e, 0.1);
    CHECK (e.getLatencySamples() == a);
    CHECK (a > 0);
    CHECK (a < 2000);
}

TEST_CASE ("Benchmark: a big pad", "[.bench]")
{
    dsp::Engine e;
    e.prepare (kFs, 512);
    auto p = plain();
    p.stretch = GENERATE (0.0f, 0.6f);
    p.spread = 1.0f; p.formant = -3.0f; p.air = 3.0f; p.space = 0.6f;
    p.octDown = true; p.octUp = true; p.tape = true;
    e.setParams (p);
    e.submitVoice (factoryVoice (dsp::FactoryVoice::hollowChoir));
    for (int k = 0; k < 8; ++k)
        e.noteOn (48 + 3 * k, 0.8f);
    const auto t0 = std::chrono::steady_clock::now();
    render (e, 10.0);
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("8 notes, 13 reads each, stretch %.1f: %.1f%% of one core\n", static_cast<double> (p.stretch), 100.0 * secs / 10.0);
}
