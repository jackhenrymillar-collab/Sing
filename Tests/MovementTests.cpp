// Tests for Movement: the shapes, the orbits on the notes, Each note, and Wander.

#include "TestHelpers.h"

#include "Voice/Movement.h"
#include "Voice/PlaceholderVoices.h"
#include "Voice/SingSynth.h"

#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace sing::test;
using Catch::Matchers::WithinAbs;

namespace
{
std::shared_ptr<const LoopSource> ahSource (bool bloom = false)
{
    const auto v = makePlaceholderVoice (PlaceholderVowel::ah, sr, 4.0f);
    LoopSettings s;
    s.start = (int) (0.5 * sr);
    s.length = (int) (3.0 * sr);
    s.fade = (int) (0.3 * sr);
    s.pitchPeriod = (float) (sr / v.rootHz);
    s.analyseForBloom = bloom;
    const float* ch[] = { v.samples.data() };
    return LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
}

SynthSettings settingsFor (float rootHz)
{
    SynthSettings s;
    s.voice.attackSeconds = 0.0f;
    s.voice.releaseSeconds = 0.5f;
    s.voice.stretch = 0.6f;
    s.voice.spread = 0.3f;
    s.voice.rootHz = rootHz;
    s.polyphony = 8;
    return s;
}

double periodSamples (double seconds) { return 1.0 / (seconds * sr); }

Render renderSynth (SingSynth& synth, int n, int block = 256)
{
    Render r { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; i += block)
        synth.render (r.left.data() + i, r.right.data() + i, std::min (block, n - i));
    return r;
}
} // namespace

//==============================================================================
TEST_CASE ("Movement: every shape stays in range and moves smoothly")
{
    for (auto shape : { MoveShape::drift, MoveShape::wave, MoveShape::breath })
    {
        float lo = 1.0f, hi = -1.0f, biggestStep = 0.0f;
        float last = movement::value (shape, 0.0, 99u);
        // 40 periods, 2000 steps each.
        for (int i = 1; i <= 80000; ++i)
        {
            const float v = movement::value (shape, i / 2000.0, 99u);
            lo = std::min (lo, v);
            hi = std::max (hi, v);
            biggestStep = std::max (biggestStep, std::abs (v - last));
            last = v;
        }
        INFO ("shape " << (int) shape << ": " << lo << " to " << hi << ", biggest step " << biggestStep);
        CHECK (lo >= -1.0f);
        CHECK (hi <= 1.0f);
        CHECK (hi - lo > 1.2f);            // uses most of its range
        CHECK (biggestStep < 0.02f);       // no jumps: 2000 steps a period
    }
}

TEST_CASE ("Movement: Drift never repeats, and each seed takes its own path")
{
    // Compare one period with every later one: none should match closely.
    auto segment = [] (double from, uint32_t seed)
    {
        std::vector<float> v (200);
        for (int i = 0; i < 200; ++i)
            v[(size_t) i] = movement::value (MoveShape::drift, from + i / 200.0, seed);
        return v;
    };
    auto distance = [] (const std::vector<float>& a, const std::vector<float>& b)
    {
        double d = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            d += std::abs (a[i] - b[i]);
        return d / (double) a.size();
    };
    const auto first = segment (0.0, 7u);
    double closest = 1.0e9;
    for (int k = 1; k < 200; ++k)
        closest = std::min (closest, distance (first, segment (k, 7u)));
    INFO ("closest repeat " << closest);
    CHECK (closest > 0.05);
    CHECK (distance (first, segment (0.0, 8u)) > 0.1);
}

TEST_CASE ("Movement: Breath rises faster than it falls")
{
    // The peak comes 40% of the way through each cycle.
    float best = -2.0f;
    double at = 0.0;
    for (int i = 0; i < 1000; ++i)
    {
        const float v = movement::value (MoveShape::breath, i / 1000.0, 1u);
        if (v > best)
        {
            best = v;
            at = i / 1000.0;
        }
    }
    CHECK_THAT (at, WithinAbs (0.4, 0.01));
    CHECK_THAT (movement::value (MoveShape::breath, 0.0, 1u), WithinAbs (-1.0, 1.0e-4));
}

TEST_CASE ("Movement: an orbit keeps its width and stays inside the knob's range")
{
    float lo = 0.0f, hi = 0.0f;
    movement::range (0.5f, 0.4f, lo, hi);
    CHECK_THAT (lo, WithinAbs (0.3, 1.0e-6));
    CHECK_THAT (hi, WithinAbs (0.7, 1.0e-6));
    // Near the top it moves down rather than being cut short.
    movement::range (0.95f, 0.4f, lo, hi);
    CHECK_THAT (lo, WithinAbs (0.6, 1.0e-6));
    CHECK_THAT (hi, WithinAbs (1.0, 1.0e-6));
    movement::range (0.2f, 1.0f, lo, hi);
    CHECK_THAT (lo, WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (hi, WithinAbs (1.0, 1.0e-6));
    CHECK (movement::orbit (0.3f, 0.0f, 1.0f) == 0.3f);
    CHECK_THAT (movement::orbit (0.5f, 0.4f, -1.0f), WithinAbs (0.3, 1.0e-6));
    CHECK_THAT (movement::orbit (0.5f, 0.4f, 1.0f), WithinAbs (0.7, 1.0e-6));
}

TEST_CASE ("Synth: with no orbits set, Movement changes nothing")
{
    // A running clock and a shape, but every orbit at 0: exactly the sound without Movement.
    // (Keep vowel off and Formant 0 leave the formant shifter only windowing, but it staggers
    // its frames from note to note across the whole plugin, so two runs differ by rounding.)
    auto src = ahSource();
    const auto root = 220.0f;
    auto play = [&] (bool clockRunning)
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = settingsFor (root);
        s.voice.keepVowel = false;
        if (clockRunning)
            s.voice.movement.cyclesPerSample = periodSamples (2.0);
        synth.setSettings (s);
        synth.noteOn (57, 0.9f);
        synth.noteOn (69, 0.9f);
        return renderSynth (synth, 48000);
    };
    const auto a = play (false), b = play (true);
    float worst = 0.0f;
    for (size_t i = 0; i < a.left.size(); ++i)
        worst = std::max ({ worst, std::abs (a.left[i] - b.left[i]), std::abs (a.right[i] - b.right[i]) });
    INFO ("largest difference " << worst);
    CHECK (worst < 1.0e-5f);
}

TEST_CASE ("Synth: orbits move Stretch, Spread and Formant within their rings")
{
    auto src = ahSource();
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    auto s = settingsFor (220.0f);
    s.voice.formantSemitones = 0.0f;
    s.voice.movement.shape = MoveShape::wave;
    s.voice.movement.cyclesPerSample = periodSamples (1.0);
    s.voice.movement.amounts[moveStretch] = 0.2f;
    s.voice.movement.amounts[moveSpread] = 0.4f;
    s.voice.movement.amounts[moveFormant] = 0.25f;   // +-3 st
    synth.setSettings (s);
    synth.noteOn (57, 0.9f);

    float stretchLo = 2.0f, stretchHi = -1.0f, spreadLo = 2.0f, spreadHi = -1.0f, formantLo = 2.0f, formantHi = -1.0f;
    std::vector<float> left (256), right (256);
    for (int block = 0; block < 2 * 48000 / 256; ++block)
    {
        synth.render (left.data(), right.data(), 256);
        const auto m = synth.newestVoice()->getMoved();
        stretchLo = std::min (stretchLo, m.stretch); stretchHi = std::max (stretchHi, m.stretch);
        spreadLo = std::min (spreadLo, m.spread);    spreadHi = std::max (spreadHi, m.spread);
        formantLo = std::min (formantLo, m.formant); formantHi = std::max (formantHi, m.formant);
    }
    INFO ("stretch " << stretchLo << " to " << stretchHi << ", spread " << spreadLo << " to " << spreadHi
                     << ", formant " << formantLo << " to " << formantHi);
    CHECK_THAT (stretchLo, WithinAbs (0.5, 0.01));
    CHECK_THAT (stretchHi, WithinAbs (0.7, 0.01));
    CHECK_THAT (spreadLo, WithinAbs (0.1, 0.01));
    CHECK_THAT (spreadHi, WithinAbs (0.5, 0.01));
    CHECK_THAT (formantLo, WithinAbs (0.5 - 0.125, 0.01));
    CHECK_THAT (formantHi, WithinAbs (0.5 + 0.125, 0.01));
}

TEST_CASE ("Synth: Each note gives every note its own path; off, they move together")
{
    auto src = ahSource();
    for (bool each : { true, false })
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = settingsFor (220.0f);
        s.voice.movement.cyclesPerSample = periodSamples (2.0);
        s.voice.movement.amounts[moveFormant] = 0.5f;
        s.voice.movement.eachNote = each;
        synth.setSettings (s);
        synth.noteOn (57, 0.9f);
        synth.noteOn (64, 0.9f);
        renderSynth (synth, 48000);

        std::vector<float> moved;
        for (int i = 0; i < SingSynth::voicePool; ++i)
            if (synth.getVoice (i).isActive())
                moved.push_back (synth.getVoice (i).getMoved().formant);
        REQUIRE (moved.size() == 2);
        INFO ("each note " << each << ": " << moved[0] << " and " << moved[1]);
        if (each)
            CHECK (std::abs (moved[0] - moved[1]) > 1.0e-3f);
        else
            CHECK_THAT (moved[0], WithinAbs (moved[1], 1.0e-6));
    }
}

TEST_CASE ("Synth: Wander moves a held note's reading point through the loop")
{
    // Hold freezes the reading point; Wander still moves it, and only within its range.
    for (bool bloom : { false, true })
    {
        auto src = ahSource (bloom);
        for (float wander : { 0.0f, 0.6f })
        {
            SingSynth synth;
            synth.prepare (sr, 256);
            synth.setSource (src.get());
            auto s = settingsFor (220.0f);
            s.voice.hold = true;
            s.voice.mode = bloom ? StretchMode::bloom : StretchMode::granular;
            s.voice.movement.shape = MoveShape::wave;
            s.voice.movement.cyclesPerSample = periodSamples (2.0);
            s.voice.movement.amounts[moveWander] = wander;
            synth.setSettings (s);
            synth.noteOn (57, 0.9f);

            // How far from where the note started (the middle of its wander), the short way
            // round the loop.
            double lo = 1.0e18, hi = -1.0e18;
            const auto length = (double) src->settings().length;
            const double first = src->toVoicePosition (0.0);
            std::vector<float> left (256), right (256);
            bool finite = true;
            for (int block = 0; block < 4 * 48000 / 256; ++block)
            {
                synth.render (left.data(), right.data(), 256);
                for (int i = 0; i < 256; ++i)
                    finite = finite && std::isfinite (left[(size_t) i]) && std::isfinite (right[(size_t) i]);
                if (block * 256 > 24000)   // after the first Bloom frame
                {
                    const double p = synth.newestVoice()->playPosition();
                    double d = std::fmod (p - first, length);
                    if (d > 0.5 * length) d -= length;
                    if (d < -0.5 * length) d += length;
                    lo = std::min (lo, d);
                    hi = std::max (hi, d);
                }
            }
            const double travelled = (hi - lo) / length;
            INFO ((bloom ? "Bloom" : "Grain") << ", Wander " << wander << ": travelled " << travelled << " of the loop");
            CHECK (finite);
            if (wander == 0.0f)
                CHECK (travelled < 0.01);
            else
            {
                CHECK (travelled > 0.4);   // a 60% orbit, two full swings
                CHECK (travelled < 0.65);
            }
        }
    }
}
