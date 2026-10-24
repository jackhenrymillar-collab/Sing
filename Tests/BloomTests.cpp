// Tests for Bloom, the spectral smear: its analysis and its voice, through the synth.

#include "TestHelpers.h"

#include "Bloom/BloomAnalysis.h"
#include "Bloom/BloomVoice.h"
#include "Common/RealFFT.h"
#include "Voice/PlaceholderVoices.h"
#include "Voice/SingSynth.h"
#include "Voice/VoiceAnalysis.h"

#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace sing::test;
using Catch::Matchers::WithinAbs;

namespace
{
std::shared_ptr<const LoopSource> bloomSource (PlaceholderVowel vowel, bool analyse = true, bool lockHarmonics = false)
{
    const auto v = makePlaceholderVoice (vowel, sr, 4.0f);
    LoopSettings s;
    s.start = (int) (1.0 * sr);
    s.length = (int) (2.0 * sr);
    s.fade = (int) (0.3 * sr);
    s.pitchPeriod = v.rootHz > 0.0f ? (float) (sr / v.rootHz) : 0.0f;
    s.analyseForBloom = analyse;
    s.bloomLockHarmonics = lockHarmonics;
    const float* ch[] = { v.samples.data() };
    return LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
}

struct Stereo
{
    std::vector<float> left, right;
};

Stereo render (SingSynth& synth, int n, int block = 256)
{
    Stereo r { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; i += block)
        synth.render (r.left.data() + i, r.right.data() + i, std::min (block, n - i));
    return r;
}

std::vector<float> mono (const Stereo& r)
{
    std::vector<float> m (r.left.size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (r.left[i] + r.right[i]);
    return m;
}

bool allFinite (const Stereo& r)
{
    for (size_t i = 0; i < r.left.size(); ++i)
        if (! std::isfinite (r.left[i]) || ! std::isfinite (r.right[i]))
            return false;
    return true;
}

// The level of a stereo render: the average power of left and right.
double level (const Stereo& r, int from, int n)
{
    const double l = rms (r.left.data() + from, n), q = rms (r.right.data() + from, n);
    return db (std::sqrt (0.5 * (l * l + q * q)));
}

SynthSettings bloomSynth (float stretch = 0.5f, float spread = 0.0f)
{
    SynthSettings s;
    s.voice.attackSeconds = 0.0f;
    s.voice.releaseSeconds = 0.5f;
    s.voice.stretch = stretch;
    s.voice.spread = spread;
    s.voice.rootHz = 220.0f;
    s.voice.mode = StretchMode::bloom;
    s.polyphony = 8;
    return s;
}

// Biggest minus smallest level of 50 ms slices, in dB.
double wobble (const std::vector<float>& x, int from, int to)
{
    double lo = 1.0e9, hi = -1.0e9;
    for (int b = from; b + 2400 <= to; b += 2400)
    {
        const double d = db (rms (x.data() + b, 2400));
        lo = std::min (lo, d);
        hi = std::max (hi, d);
    }
    return hi - lo;
}

double correlation (const Stereo& r, int from, int n)
{
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = from; i < from + n; ++i)
    {
        lr += (double) r.left[(size_t) i] * r.right[(size_t) i];
        ll += (double) r.left[(size_t) i] * r.left[(size_t) i];
        rr += (double) r.right[(size_t) i] * r.right[(size_t) i];
    }
    return lr / std::sqrt (ll * rr + 1.0e-30);
}
// Power in octave bands from 220 Hz up, in dB relative to the total over those bands.
std::vector<double> bandProfile (const float* x, int n)
{
    constexpr int size = 16384;
    sing::RealFFT fft (size);
    std::vector<float> frame ((size_t) size), spectrum ((size_t) size + 2);
    std::vector<double> power ((size_t) size / 2 + 1);
    for (int start = 0; start + size <= n; start += size / 2)
    {
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (6.283185307179586 * i / size));
        fft.forward (frame.data(), spectrum.data());
        for (int k = 0; k <= size / 2; ++k)
            power[(size_t) k] += (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)]
                               + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
    }
    std::vector<double> bands;
    double total = 0.0;
    for (double lo = 220.0; lo < 7040.0; lo *= 2.0)
    {
        double sum = 0.0;
        for (int k = (int) (lo * size / sr); k < (int) (2.0 * lo * size / sr); ++k)
            sum += power[(size_t) k];
        bands.push_back (sum);
        total += sum;
    }
    for (auto& b : bands)
        b = 10.0 * std::log10 (b / total + 1.0e-30);
    return bands;
}
} // namespace

TEST_CASE ("Bloom: analysis is only built when asked for")
{
    auto with = bloomSource (PlaceholderVowel::ooh);
    auto without = bloomSource (PlaceholderVowel::ooh, false);
    REQUIRE (BloomVoice::canPlay (*with));
    CHECK_FALSE (BloomVoice::canPlay (*without));

    const auto* a = static_cast<const BloomAnalysis*> (with->analysis (StretchMode::bloom));
    CHECK (a->fftSize() == 32768);   // about 0.68 s at 48 kHz
    CHECK (a->numFrames() > 0);
    // The frames tile the loop exactly, so the read position wraps without a jump.
    CHECK_THAT (a->hop() * a->numFrames(), WithinAbs ((double) with->size(), 1.0e-6));
}

TEST_CASE ("Bloom: pitch follows the key and the level stays near the loop's")
{
    auto src = bloomSource (PlaceholderVowel::ooh);
    const double dry = db (rms (src->channel (0), src->size()));

    for (const float stretch : { 0.0f, 0.5f, 1.0f })
        for (const int semis : { -12, 0, 12 })
        {
            SingSynth synth;
            synth.prepare (sr, 256);
            synth.setSource (src.get());
            synth.setSettings (bloomSynth (stretch));
            synth.noteOn (57 + semis, 1.0f);
            const auto r = render (synth, 4 * 48000);
            const auto m = mono (r);
            const double hz = 220.0 * std::exp2 (semis / 12.0);
            const double lv = level (r, 48000, 3 * 48000);
            std::printf ("  Bloom stretch %.1f, %+3d semitones: %6.1f Hz, level %+.1f dB against the loop\n",
                         stretch, semis, detectPitch (m.data() + 48000, 96000, sr).hz, lv - dry);

            INFO ("stretch " << stretch << ", semitones " << semis);
            CHECK_THAT (detectPitch (m.data() + 48000, 96000, sr).hz, WithinAbs (hz, hz * 0.01));
            CHECK (std::abs (lv - dry) < 3.0);   // granular stays within about 3 dB too
            CHECK (allFinite (r));
        }
}

TEST_CASE ("Bloom (locked harmonics): a note sounds within 100 ms and starts near full level")
{
    auto src = bloomSource (PlaceholderVowel::ooh, true, true);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (bloomSynth (0.8f));
    synth.noteOn (57, 1.0f);
    const auto m = mono (render (synth, 3 * 48000));

    int onset = 0;
    while (onset < (int) m.size() && std::abs (m[(size_t) onset]) < 0.01f)
        ++onset;
    std::printf ("  Bloom note sounds after %.0f ms\n", onset / 48.0);
    CHECK (onset < 4800);

    // From 20 ms after it sounds, no 50 ms slice is far below the held level: no swell or
    // dip while the overlapping frames fill in.
    const double held = db (rms (m.data() + 96000, 48000));
    for (int b = onset + 960; b + 2400 <= 96000; b += 2400)
    {
        INFO ("slice at " << b / 48 << " ms");
        CHECK (db (rms (m.data() + b, 2400)) > held - 6.0);
    }
}

TEST_CASE ("Bloom (locked harmonics): Hold keeps a vowel sounding, steady")
{
    // The "ah" voice has vibrato, which used to make a held Bloom note tremble at the
    // vibrato rate (its sidebands beating) by about 6 dB.
    auto src = bloomSource (PlaceholderVowel::ah, true, true);
    SingSynth synth;
    synth.prepare (sr, 256);
    auto settings = bloomSynth (0.5f);
    settings.voice.hold = true;
    settings.voice.rootHz = 196.0f;   // the "ah" voice's root
    synth.setSource (src.get());
    synth.setSettings (settings);
    synth.noteOn (55, 1.0f);          // G3, 196 Hz
    const auto r = render (synth, 8 * 48000);
    const auto m = mono (r);

    const double w = wobble (m, 48000, 8 * 48000);
    std::printf ("  Bloom held vowel: level moves by %.1f dB over 50 ms slices\n", w);
    CHECK (w < 3.0);
    CHECK (rms (m.data() + 7 * 48000, 48000) > 0.05);
    CHECK_THAT (detectPitch (m.data() + 6 * 48000, 96000, sr).hz, WithinAbs (196.0, 2.0));
}

TEST_CASE ("Bloom: Spread widens the note and keeps its level")
{
    auto src = bloomSource (PlaceholderVowel::ooh);
    double level[2] {}, corr[2] {};
    for (int i = 0; i < 2; ++i)
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        synth.setSettings (bloomSynth (0.5f, i == 0 ? 0.0f : 1.0f));
        synth.noteOn (57, 1.0f);
        const auto r = render (synth, 4 * 48000);
        level[i] = ::level (r, 48000, 3 * 48000);
        corr[i] = correlation (r, 48000, 3 * 48000);
        CHECK (allFinite (r));
    }
    std::printf ("  Bloom Spread 0 -> 1: level %+.1f dB, left-right correlation %.2f -> %.2f\n",
                 level[1] - level[0], corr[0], corr[1]);
    CHECK (corr[0] > 0.5);   // holds up in mono
    CHECK (corr[1] < corr[0] - 0.2);
    CHECK (std::abs (level[1] - level[0]) < 3.0);
}

TEST_CASE ("Bloom: a loop without its analysis plays granular instead")
{
    auto src = bloomSource (PlaceholderVowel::ooh, false);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (bloomSynth());
    synth.noteOn (57, 1.0f);
    const auto m = mono (render (synth, 48000));
    CHECK (rms (m.data() + 9600, 38400) > 0.05);
    CHECK_THAT (detectPitch (m.data() + 9600, 38400, sr).hz, WithinAbs (220.0, 2.2));
}

TEST_CASE ("Bloom: swapping the loop keeps the note playing and releases the old one")
{
    auto a = bloomSource (PlaceholderVowel::ooh);
    auto b = bloomSource (PlaceholderVowel::ah);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (a.get());
    synth.setSettings (bloomSynth());
    synth.noteOn (57, 1.0f);
    render (synth, 24000);

    synth.setSource (b.get());
    std::vector<const LoopSource*> retired;
    const auto r = render (synth, 48000);
    synth.releaseUnusedSources ([&] (const LoopSource* s) { retired.push_back (s); });
    REQUIRE (retired.size() == 1);
    CHECK (retired[0] == a.get());
    CHECK (rms (r.left.data() + 24000, 24000) > 0.02);
    CHECK (allFinite (r));
}

TEST_CASE ("Bloom (classic): every bin gets a random phase and the spectrum is kept")
{
    auto src = bloomSource (PlaceholderVowel::ooh);
    const auto* analysis = static_cast<const BloomAnalysis*> (src->analysis (StretchMode::bloom));
    REQUIRE (analysis != nullptr);
    CHECK_FALSE (analysis->locksHarmonics());
    int marked = 0;
    for (int f = 0; f < analysis->numFrames(); ++f)
        for (int k = 0; k < analysis->numBins(); ++k)
            marked += analysis->peaks (f)[k] != BloomAnalysis::noPeak ? 1 : 0;
    CHECK (marked == 0);

    // Played at the root, the smear keeps the loop's balance of lows and highs.
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (bloomSynth (0.5f));
    synth.noteOn (57, 1.0f);
    const auto m = mono (render (synth, 6 * 48000));
    const auto played = bandProfile (m.data() + 48000, 5 * 48000);
    const auto loop = bandProfile (src->channel (0), src->size());
    REQUIRE (played.size() == loop.size());
    for (size_t b = 0; b < loop.size(); ++b)
    {
        std::printf ("  Bloom classic band %4.0f Hz: loop %+5.1f dB, played %+5.1f dB\n", 220.0 * std::exp2 ((double) b), loop[b], played[b]);
        if (loop[b] > -30.0)   // bands that hold some of the sound
            CHECK (std::abs (played[b] - loop[b]) < 3.0);
    }
}

TEST_CASE ("Bloom (classic): a held note sounds at once and keeps sounding")
{
    auto src = bloomSource (PlaceholderVowel::ah);
    SingSynth synth;
    synth.prepare (sr, 256);
    auto settings = bloomSynth (0.5f);
    settings.voice.hold = true;
    settings.voice.rootHz = 196.0f;
    synth.setSource (src.get());
    synth.setSettings (settings);
    synth.noteOn (55, 1.0f);
    const auto m = mono (render (synth, 8 * 48000));

    int onset = 0;
    while (onset < (int) m.size() && std::abs (m[(size_t) onset]) < 0.01f)
        ++onset;
    CHECK (onset < 4800);

    // Random phases make the level shimmer (the method's sound), but it never drops out.
    const double held = db (rms (m.data() + 4 * 48000, 4 * 48000));
    double lowest = 0.0;
    for (int b = onset + 960; b + 2400 <= 8 * 48000; b += 2400)
        lowest = std::min (lowest, db (rms (m.data() + b, 2400)) - held);
    std::printf ("  Bloom classic held vowel: shimmer %.1f dB over 50 ms slices, lowest slice %.1f dB under the held level\n",
                 wobble (m, 48000, 8 * 48000), -lowest);
    CHECK (lowest > -12.0);
    CHECK_THAT (detectPitch (m.data() + 6 * 48000, 96000, sr).hz, WithinAbs (196.0, 2.0));
}
