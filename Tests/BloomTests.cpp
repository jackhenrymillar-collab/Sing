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
    CHECK (marked > 0);   // harmonics are marked for Tone, but nothing is redrawn or locked

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

namespace
{
// A soft held tone with one sudden loud burst 1.5 s into a 4 s voice; the loop is 1 s to 3 s.
std::shared_ptr<const LoopSource> burstSource()
{
    std::vector<float> v ((size_t) (4 * 48000));
    uint32_t noise = 1;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = (double) i / sr;
        float x = 0.05f * (float) std::sin (6.283185307179586 * 220.0 * t);
        if (t >= 1.5)
        {
            noise = noise * 1664525u + 1013904223u;
            const auto burst = (float) std::exp (-(t - 1.5) / 0.08);
            x += burst * (0.5f * (float) std::sin (6.283185307179586 * 660.0 * t) + 0.3f * ((float) (noise >> 8) / 8388608.0f - 1.0f));
        }
        v[i] = x;
    }
    LoopSettings s;
    s.start = 48000;
    s.length = 2 * 48000;
    s.fade = 4800;
    s.pitchPeriod = (float) (sr / 220.0);
    s.analyseForBloom = true;
    const float* ch[] = { v.data() };
    return LoopSource::build (ch, 1, (int) v.size(), sr, s);
}
} // namespace

TEST_CASE ("Bloom: onsets are found where the sound jumps, and not in a steady vowel")
{
    auto src = burstSource();
    const auto* analysis = static_cast<const BloomAnalysis*> (src->analysis (StretchMode::bloom));
    REQUIRE (analysis != nullptr);
    const auto& onsets = analysis->onsets();
    REQUIRE (onsets.size() >= 1);
    double best = 1.0e9;
    for (const auto& o : onsets)
    {
        std::printf ("  onset at %.3f s in the voice, strength %.2f\n", src->toVoicePosition (o.position) / sr, o.strength);
        best = std::min (best, std::abs (src->toVoicePosition (o.position) - 1.5 * sr));
    }
    CHECK (best < 0.02 * sr);

    for (const auto vowel : { PlaceholderVowel::ah, PlaceholderVowel::ooh })
    {
        auto steady = bloomSource (vowel);
        CHECK (static_cast<const BloomAnalysis*> (steady->analysis (StretchMode::bloom))->onsets().empty());
    }
}

TEST_CASE ("Bloom: a stretched note passes an onset at the recorded speed")
{
    auto src = burstSource();
    const auto* analysis = static_cast<const BloomAnalysis*> (src->analysis (StretchMode::bloom));
    REQUIRE (! analysis->onsets().empty());
    const double onset = analysis->onsets().front().position;

    double seconds[2] {};
    for (int i = 0; i < 2; ++i)
    {
        BloomVoice voice;
        voice.prepare (sr);
        BloomVoice::Parameters p;
        p.speed = 0.05f;
        p.onsets = i == 0 ? 0.0f : 1.0f;
        p.numLayers = 1;
        p.layers[0].gainLeft = p.layers[0].gainRight = 0.5f;
        voice.setParameters (p);
        const double half = 0.5 * (double) analysis->fftSize();
        double startAt = onset - half - 0.05 * sr;
        if (startAt < 0.0)
            startAt += src->size();
        voice.start (*src, startAt, 7u, 0.0f);

        // Time from the window first reaching the onset to it leaving the window.
        std::vector<float> l (256), r (256);
        const double target = src->toVoicePosition (onset) + half;
        int samples = 0;
        while (samples < 30 * 48000 && voice.playPosition() < target)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            voice.render (l.data(), r.data(), 256);
            samples += 256;
        }
        seconds[i] = samples / sr;
    }
    std::printf ("  Bloom passing an onset at Stretch speed 0.05: %.1f s stretched, %.1f s with onsets\n", seconds[0], seconds[1]);
    CHECK (seconds[0] > 8.0);
    CHECK (seconds[1] < 2.0);
}

namespace
{
// The share of a render's power within 8 Hz of the partials n * f0 + offsetHz (n = 1 to 20).
double partialShare (const std::vector<float>& x, int from, int n, double f0, double offsetHz)
{
    constexpr int size = 32768;
    sing::RealFFT fft (size);
    std::vector<float> frame ((size_t) size), spectrum ((size_t) size + 2);
    std::vector<double> power ((size_t) size / 2 + 1);
    for (int start = from; start + size <= from + n; start += size / 2)
    {
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = x[(size_t) (start + i)] * (float) (0.5 - 0.5 * std::cos (6.283185307179586 * i / size));
        fft.forward (frame.data(), spectrum.data());
        for (int k = 0; k <= size / 2; ++k)
            power[(size_t) k] += (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)]
                               + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
    }
    double total = 0.0, near = 0.0;
    for (int k = 1; k <= size / 2; ++k)
    {
        const double hz = k * sr / size;
        if (hz < 50.0 || hz > 20.0 * f0 + offsetHz + 100.0)
            continue;
        total += power[(size_t) k];
        const double m = std::round ((hz - offsetHz) / f0);
        if (m >= 1.0 && m <= 20.0 && std::abs (hz - (m * f0 + offsetHz)) < 8.0)
            near += power[(size_t) k];
    }
    return near / (total + 1.0e-30);
}

std::vector<float> playBloom (const std::shared_ptr<const LoopSource>& src, float shiftHz, float blur, float tone)
{
    BloomVoice voice;
    voice.prepare (sr);
    BloomVoice::Parameters p;
    p.speed = 0.3f;
    p.shiftHz = shiftHz;
    p.blur = blur;
    p.tone = tone;
    p.numLayers = 1;
    p.layers[0].gainLeft = p.layers[0].gainRight = 0.5f;
    voice.setParameters (p);
    voice.start (*src, 0.0, 11u, 0.0f);
    std::vector<float> l ((size_t) 5 * 48000), r ((size_t) 5 * 48000);
    for (size_t i = 0; i < l.size(); i += 256)
        voice.render (l.data() + i, r.data() + i, (int) std::min<size_t> (256, l.size() - i));
    return l;
}
} // namespace

TEST_CASE ("Bloom: frequency shift moves every partial by the same Hz")
{
    auto src = bloomSource (PlaceholderVowel::ooh);
    const auto plain = playBloom (src, 0.0f, 0.0f, 0.0f);
    const auto shifted = playBloom (src, 100.0f, 0.0f, 0.0f);
    const double before = partialShare (plain, 48000, 4 * 48000, 220.0, 0.0);
    const double onOld = partialShare (shifted, 48000, 4 * 48000, 220.0, 0.0);
    const double onNew = partialShare (shifted, 48000, 4 * 48000, 220.0, 100.0);
    std::printf ("  Bloom shift +100 Hz: power on 220 Hz harmonics %.2f before, %.2f after; on harmonics + 100 Hz %.2f\n", before, onOld, onNew);
    CHECK (onNew > 0.5 * before);
    CHECK (onOld < 0.2 * before);
}

TEST_CASE ("Bloom: Tone moves between harmonics only and breath only, and blur smears them")
{
    auto src = bloomSource (PlaceholderVowel::ah);
    const double f0 = 196.0;   // the "ah" voice's root, played at the root
    const double noise = partialShare (playBloom (src, 0.0f, 0.0f, -1.0f), 48000, 4 * 48000, f0, 0.0);
    const double both = partialShare (playBloom (src, 0.0f, 0.0f, 0.0f), 48000, 4 * 48000, f0, 0.0);
    const double tonal = partialShare (playBloom (src, 0.0f, 0.0f, 1.0f), 48000, 4 * 48000, f0, 0.0);
    const double blurred = partialShare (playBloom (src, 0.0f, 1.0f, 0.0f), 48000, 4 * 48000, f0, 0.0);
    std::printf ("  Bloom power on the harmonics: Tone -1 %.2f, 0 %.2f, +1 %.2f; blur 1 %.2f\n", noise, both, tonal, blurred);
    CHECK (noise < both);
    CHECK (tonal > both);
    CHECK (blurred < 0.7 * both);
}

TEST_CASE ("Bloom: the Window setting sets the analysis length")
{
    const auto v = makePlaceholderVoice (PlaceholderVowel::ooh, sr, 4.0f);
    const float* ch[] = { v.samples.data() };
    int sizes[3] {};
    const float seconds[3] { 0.1f, 0.68f, 1.4f };
    for (int i = 0; i < 3; ++i)
    {
        LoopSettings s;
        s.start = 0;
        s.length = (int) v.samples.size();
        s.fade = 4800;
        s.analyseForBloom = true;
        s.bloomWindowSeconds = seconds[i];
        auto src = LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
        sizes[i] = static_cast<const BloomAnalysis*> (src->analysis (StretchMode::bloom))->fftSize();
    }
    CHECK (sizes[0] == 4096);
    CHECK (sizes[1] == 32768);
    CHECK (sizes[2] == 65536);

    // A long window on a short loop is held to half the loop, so the frames still differ and
    // Stretch still has something to move through.
    CHECK (BloomAnalysis::chooseSize (72000, sr, 1.4) == 32768);
    CHECK (BloomAnalysis::chooseSize (24000, sr, 1.4) == 8192);
}

TEST_CASE ("Bloom: swapping to a loop with fewer analysis frames mid-note stays clean")
{
    // A new Window (or loop length) gives the new loop a different number of frames. A
    // frame being built when the loop is swapped used to read past the end of the new
    // analysis, which could put garbage (and NaN) into Air and Space.
    const auto v = makePlaceholderVoice (PlaceholderVowel::ah, sr, 4.0f);
    auto build = [&v] (float window)
    {
        LoopSettings s;
        s.start = 48000;
        s.length = 2 * 48000;
        s.fade = 14400;
        s.pitchPeriod = (float) (sr / v.rootHz);
        s.analyseForBloom = true;
        s.bloomWindowSeconds = window;
        const float* ch[] = { v.samples.data() };
        return LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
    };
    auto many = build (0.1f), few = build (1.4f);
    REQUIRE (static_cast<const BloomAnalysis*> (many->analysis (StretchMode::bloom))->numFrames()
             > static_cast<const BloomAnalysis*> (few->analysis (StretchMode::bloom))->numFrames());

    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (many.get());
    auto settings = bloomSynth (0.3f);
    settings.voice.rootHz = v.rootHz;
    synth.setSettings (settings);
    synth.noteOn (55, 1.0f);
    const auto before = render (synth, 3 * 48000 + 77);   // somewhere late in the loop
    synth.setSource (few.get());
    const auto after = render (synth, 2 * 48000);
    CHECK (allFinite (before));
    CHECK (allFinite (after));
    float peak = 0.0f;
    for (const float x : after.left)
        peak = std::max (peak, std::abs (x));
    CHECK (peak < 4.0f);
    CHECK (rms (after.left.data() + 48000, 48000) > 0.01);
}

namespace
{
// A loop of white noise, like a breath with no pitch.
std::shared_ptr<const LoopSource> noiseSource()
{
    std::vector<float> v ((size_t) (4 * 48000));
    uint32_t noise = 7;
    for (auto& x : v)
    {
        noise = noise * 1664525u + 1013904223u;
        x = 0.2f * ((float) (noise >> 8) / 8388608.0f - 1.0f);
    }
    LoopSettings s;
    s.start = 48000;
    s.length = 2 * 48000;
    s.fade = 4800;
    s.analyseForBloom = true;
    const float* ch[] = { v.data() };
    return LoopSource::build (ch, 1, (int) v.size(), sr, s);
}

std::vector<float> playBloomWith (const std::shared_ptr<const LoopSource>& src, BloomVoice::Parameters p)
{
    BloomVoice voice;
    voice.prepare (sr);
    p.speed = 0.3f;
    p.numLayers = 1;
    p.layers[0].gainLeft = p.layers[0].gainRight = 0.5f;
    voice.setParameters (p);
    voice.start (*src, 0.0, 11u, 0.0f);
    std::vector<float> l ((size_t) 5 * 48000), r ((size_t) 5 * 48000);
    for (size_t i = 0; i < l.size(); i += 256)
        voice.render (l.data() + i, r.data() + i, (int) std::min<size_t> (256, l.size() - i));
    return l;
}

// The share of a render's power (50 Hz to 16 kHz) that lies between lo and hi Hz.
double bandShare (const std::vector<float>& x, int from, int n, double lo, double hi)
{
    constexpr int size = 8192;
    sing::RealFFT fft (size);
    std::vector<float> frame ((size_t) size), spectrum ((size_t) size + 2);
    double total = 0.0, inside = 0.0;
    for (int start = from; start + size <= from + n; start += size / 2)
    {
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = x[(size_t) (start + i)] * (float) (0.5 - 0.5 * std::cos (6.283185307179586 * i / size));
        fft.forward (frame.data(), spectrum.data());
        for (int k = 1; k <= size / 2; ++k)
        {
            const double hz = k * sr / size;
            if (hz < 50.0 || hz > 16000.0)
                continue;
            const double pw = (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)]
                            + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
            total += pw;
            if (hz >= lo && hz <= hi)
                inside += pw;
        }
    }
    return inside / (total + 1.0e-30);
}
} // namespace

TEST_CASE ("Bloom: Harmonics keeps only the key's harmonics, up to Count")
{
    auto src = noiseSource();
    BloomVoice::Parameters p;
    p.rootHz = 200.0f;
    const auto plain = playBloomWith (src, p);
    p.harmonicsCents = 10.0f;
    p.harmonicsCount = 10;
    const auto combed = playBloomWith (src, p);
    const double before = partialShare (plain, 48000, 4 * 48000, 200.0, 0.0);
    const double after = partialShare (combed, 48000, 4 * 48000, 200.0, 0.0);
    const double above = bandShare (combed, 48000, 4 * 48000, 2200.0, 16000.0);
    std::printf ("  Bloom Harmonics on noise: power on 200 Hz harmonics %.2f before, %.2f after; above harmonic 10 %.4f\n", before, after, above);
    CHECK (before < 0.15);
    CHECK (after > 0.6);
    CHECK (above < 0.01);
    // The comb keeps a sliver of the noise, and the make-up brings it back to level.
    const double levelBefore = level (Stereo { plain, plain }, 48000, 4 * 48000);
    const double levelAfter = level (Stereo { combed, combed }, 48000, 4 * 48000);
    std::printf ("  Bloom Harmonics level: %.1f dB before, %.1f dB after\n", levelBefore, levelAfter);
    CHECK (std::abs (levelAfter - levelBefore) < 3.0);
}

TEST_CASE ("Bloom: Filter keeps its band, or cuts it out with Low above High")
{
    auto src = noiseSource();
    BloomVoice::Parameters p;
    p.filter = true;
    p.lowHz = 500.0f;
    p.highHz = 2000.0f;
    const auto pass = playBloomWith (src, p);
    std::swap (p.lowHz, p.highHz);
    const auto stop = playBloomWith (src, p);
    const double kept = bandShare (pass, 48000, 4 * 48000, 420.0, 2400.0);
    const double cut = bandShare (stop, 48000, 4 * 48000, 600.0, 1650.0);
    std::printf ("  Bloom Filter 500 Hz to 2 kHz: %.4f of the power inside; reversed: %.4f inside\n", kept, cut);
    CHECK (kept > 0.98);
    CHECK (cut < 0.01);
    CHECK (level (Stereo { pass, pass }, 48000, 4 * 48000) > -60.0);
}

TEST_CASE ("Bloom (classic): a note starts at its held level, with no burst while the frames fill in")
{
    // Noise is analysed without locked harmonics, so its frames take random phases and add
    // up in power. The make-up while the first frames overlap has to match that.
    auto src = noiseSource();
    BloomVoice::Parameters p;
    p.hold = true;
    const auto x = playBloomWith (src, p);

    int onset = 0;
    while (onset < (int) x.size() && std::abs (x[(size_t) onset]) < 1.0e-4f)
        ++onset;
    REQUIRE (onset < 9600);

    const double held = db (rms (x.data() + 3 * 48000, 2 * 48000));
    double loudest = -100.0;
    for (int b = onset; b + 2400 <= onset + 48000; b += 2400)
        loudest = std::max (loudest, db (rms (x.data() + b, 2400)) - held);
    std::printf ("  Bloom classic start: loudest 50 ms slice in the first second %+.1f dB against the held level\n", loudest);
    CHECK (loudest < 2.0);
}
