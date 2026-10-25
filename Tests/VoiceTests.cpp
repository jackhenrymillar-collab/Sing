// Tests for pitch detection, the formant shifter, the loop hand-off and the synth voices.

#include "TestHelpers.h"

#include "Voice/FormantShifter.h"
#include "Common/RealFFT.h"
#include "Voice/PlaceholderVoices.h"
#include "Voice/SingSynth.h"
#include "Voice/SourceExchange.h"
#include "Voice/VoiceAnalysis.h"

#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace sing::test;
using Catch::Matchers::WithinAbs;

namespace
{
// Share of energy above 1.5 kHz: a rough brightness measure, to see formants move.
double brightness (const std::vector<float>& x, int from, int n)
{
    // One-pole high-pass at about 1.5 kHz.
    const double a = std::exp (-twoPi * 1500.0 / sr);
    double lp = 0.0, hi = 0.0, all = 0.0;
    for (int i = from; i < from + n; ++i)
    {
        lp = (1.0 - a) * x[(size_t) i] + a * lp;
        const double h = x[(size_t) i] - lp;
        hi += h * h;
        all += (double) x[(size_t) i] * x[(size_t) i];
    }
    return all > 0.0 ? hi / all : 0.0;
}

std::vector<float> shift (const std::vector<float>& x, float periodSamples, float ratio, int* latency = nullptr)
{
    FormantShifter f;
    f.prepare (sr, 0.04f);
    const int d = f.start (periodSamples, ratio);
    if (latency != nullptr)
        *latency = d;
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); i += 256)
    {
        const int n = (int) std::min<size_t> (256, x.size() - i);
        f.process (x.data() + i, nullptr, y.data() + i, nullptr, n);
    }
    return y;
}

std::shared_ptr<const LoopSource> placeholderSource (PlaceholderVowel vowel)
{
    const auto v = makePlaceholderVoice (vowel, sr, 4.0f);
    LoopSettings s;
    s.start = (int) (1.0 * sr);
    s.length = (int) (2.0 * sr);
    s.fade = (int) (0.3 * sr);
    s.pitchPeriod = v.rootHz > 0.0f ? (float) (sr / v.rootHz) : 0.0f;
    const float* ch[] = { v.samples.data() };
    return LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
}

Render renderSynth (SingSynth& synth, int n, int block = 256)
{
    Render r { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; i += block)
        synth.render (r.left.data() + i, r.right.data() + i, std::min (block, n - i));
    return r;
}

std::vector<float> mono (const Render& r)
{
    std::vector<float> m (r.left.size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (r.left[i] + r.right[i]);
    return m;
}

SynthSettings plainSynth (float rootHz)
{
    SynthSettings s;
    s.voice.attackSeconds = 0.0f;
    s.voice.releaseSeconds = 0.5f;
    s.voice.stretch = 0.3f;
    s.voice.spread = 0.0f;
    s.voice.rootHz = rootHz;
    s.polyphony = 8;
    return s;
}
} // namespace

//==============================================================================
TEST_CASE ("Pitch detection finds the root of pitched voices")
{
    CHECK_THAT (detectPitch (sine (220.0, 48000).data(), 48000, sr).hz, WithinAbs (220.0, 0.5));
    CHECK_THAT (detectPitch (voice (48000).data(), 48000, sr).hz, WithinAbs (220.0, 2.0));

    const auto ooh = makePlaceholderVoice (PlaceholderVowel::ooh, sr, 3.0f);
    const auto ah = makePlaceholderVoice (PlaceholderVowel::ah, sr, 3.0f);
    const auto hum = makePlaceholderVoice (PlaceholderVowel::hum, sr, 3.0f);
    CHECK_THAT (detectPitch (ooh.samples.data(), (int) ooh.samples.size(), sr).hz, WithinAbs (ooh.rootHz, 2.0));
    CHECK_THAT (detectPitch (ah.samples.data(), (int) ah.samples.size(), sr).hz, WithinAbs (ah.rootHz, 2.0));
    CHECK_THAT (detectPitch (hum.samples.data(), (int) hum.samples.size(), sr).hz, WithinAbs (hum.rootHz, 2.5));
    CHECK_THAT (hzToMidi (440.0f), WithinAbs (69.0, 1e-4));
    CHECK_THAT (midiToHz (60.0f), WithinAbs (261.63, 0.01));
}

TEST_CASE ("The moving placeholder voices are pitched where they say, and change as they go")
{
    for (const auto vowel : allPlaceholderVowels)
    {
        const float seconds = placeholderSeconds (vowel);
        const auto v = makePlaceholderVoice (vowel, sr, seconds);
        REQUIRE (v.samples.size() == (size_t) (seconds * sr));

        float peak = 0.0f;
        for (float x : v.samples)
        {
            REQUIRE (std::isfinite (x));
            peak = std::max (peak, std::abs (x));
        }
        CHECK (peak > 0.2f);
        CHECK (peak <= 0.5001f);

        if (v.rootHz > 0.0f)
        {
            // Within a quarter tone of the note it is written on.
            const auto found = detectPitch (v.samples.data(), (int) v.samples.size(), sr);
            REQUIRE (found.pitched());
            CHECK (std::abs (12.0f * std::log2 (found.hz / v.rootHz)) < 0.5f);
        }
    }

    // The swell really swells: the middle is much louder than the start.
    const auto alto = makePlaceholderVoice (PlaceholderVowel::swellingAlto, sr, 8.0f);
    auto rms = [&] (const std::vector<float>& x, double from, double to)
    {
        double sum = 0.0;
        const int a = (int) (from * sr), b = (int) (to * sr);
        for (int i = a; i < b; ++i)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return std::sqrt (sum / (b - a));
    };
    CHECK (rms (alto.samples, 4.5, 5.5) > 2.0 * rms (alto.samples, 0.5, 1.5));

    // The sigh ends lower than it starts.
    const auto sigh = makePlaceholderVoice (PlaceholderVowel::fallingSigh, sr, 8.0f);
    const auto early = detectPitch (sigh.samples.data() + (int) (1.0 * sr), (int) (2.0 * sr), sr);
    const auto late = detectPitch (sigh.samples.data() + (int) (6.6 * sr), (int) (0.6 * sr), sr);
    REQUIRE (early.pitched());
    REQUIRE (late.pitched());
    CHECK (12.0f * std::log2 (early.hz / late.hz) > 1.2f);
}

TEST_CASE ("The synth sources move their filter, and the instruments behave like theirs")
{
    // How bright a stretch of sound is: the share of its energy above 1.5 kHz, from the
    // energy of its first difference (a gentle high-pass) against its own.
    auto brightness = [] (const std::vector<float>& x, double from, double to)
    {
        double e = 0.0, d = 0.0;
        for (int i = (int) (from * sr) + 1; i < (int) (to * sr); ++i)
        {
            e += (double) x[(size_t) i] * x[(size_t) i];
            const double diff = (double) x[(size_t) i] - x[(size_t) i - 1];
            d += diff * diff;
        }
        return d / std::max (e, 1.0e-12);
    };
    auto rms = [] (const std::vector<float>& x, double from, double to)
    {
        double sum = 0.0;
        const int a = (int) (from * sr), b = (int) (to * sr);
        for (int i = a; i < b; ++i)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return std::sqrt (sum / (b - a));
    };

    // The saw's filter is open in the middle of the take and closed at the start.
    const auto saw = makePlaceholderVoice (PlaceholderVowel::sweepingSaw, sr, 8.0f);
    CHECK (brightness (saw.samples, 3.5, 4.5) > 4.0 * brightness (saw.samples, 0.3, 0.8));

    // The morph ends as a square under an open filter, far brighter than the triangle it starts as.
    const auto morph = makePlaceholderVoice (PlaceholderVowel::morphPad, sr, 8.0f);
    CHECK (brightness (morph.samples, 6.8, 7.3) > 4.0 * brightness (morph.samples, 1.0, 1.5));

    // A piano note dies away; an organ holds.
    const auto piano = makePlaceholderVoice (PlaceholderVowel::grandPiano, sr, 8.0f);
    CHECK (rms (piano.samples, 0.0, 0.5) > 4.0 * rms (piano.samples, 6.5, 7.0));
    const auto organ = makePlaceholderVoice (PlaceholderVowel::churchOrgan, sr, 8.0f);
    CHECK (rms (organ.samples, 6.5, 7.0) > 0.5 * rms (organ.samples, 0.5, 1.0));
}

TEST_CASE ("Pitch detection calls breaths and noise unpitched")
{
    const auto breath = makePlaceholderVoice (PlaceholderVowel::breath, sr, 3.0f);
    CHECK_FALSE (detectPitch (breath.samples.data(), (int) breath.samples.size(), sr).pitched());
    const auto n = noise (48000);
    CHECK_FALSE (detectPitch (n.data(), 48000, sr).pitched());
}

TEST_CASE ("Pitch track follows a voice that moves between notes")
{
    // Half a second each of three notes, like a sung phrase.
    std::vector<float> x;
    for (double hz : { 494.0, 416.0, 440.0 })
    {
        for (int i = 0; i < 24000; ++i)
        {
            double s = 0.0;
            for (int h = 1; h <= 6; ++h)
                s += std::sin (twoPi * hz * h * i / sr) / h;
            x.push_back ((float) (0.2 * s));
        }
    }
    const auto track = trackPitch (x.data(), (int) x.size(), sr);
    CHECK_THAT (sr / track.periodAt (12000.0), WithinAbs (494.0, 3.0));
    CHECK_THAT (sr / track.periodAt (36000.0), WithinAbs (416.0, 3.0));
    CHECK_THAT (sr / track.periodAt (60000.0), WithinAbs (440.0, 3.0));

    const auto quiet = std::vector<float> (48000, 0.0f);
    CHECK (trackPitch (quiet.data(), 48000, sr).periodAt (24000.0) == 0.0f);
}

TEST_CASE ("Silence trim finds the audible part")
{
    std::vector<float> x (96000, 0.0f);
    const auto tone = sine (300.0, 48000);
    std::copy (tone.begin(), tone.end(), x.begin() + 24000);
    const auto r = findAudibleRange (x.data(), (int) x.size(), sr);
    CHECK (std::abs (r.start - 24000) <= 960);
    CHECK (std::abs (r.end - 72000) <= 960);
}

//==============================================================================
TEST_CASE ("Formant shifter at ratio 1 passes the voice through, delayed")
{
    for (float period : { (float) (sr / 220.0), 0.0f })
    {
        const auto x = voice (48000);
        int d = 0;
        const auto y = shift (x, period, 1.0f, &d);

        const auto& expected = x;   // exactly, just later

        double maxErr = 0.0;
        for (int i = d + 10; i < 48000; ++i)
            maxErr = std::max (maxErr, (double) std::abs (y[(size_t) i] - expected[(size_t) (i - d)]));
        INFO ("period " << period << ", latency " << d << ", max error " << maxErr);
        CHECK (maxErr < 2.0e-3);
        CHECK (d < 0.03 * sr);
    }
}

TEST_CASE ("Formant shifter adds no DC offset")
{
    // The old time-domain shifter's grains each carried a little DC, which added up to a
    // steady offset and a low thump. The spectral shifter leaves the 0 Hz bin alone.
    const auto x = sine (587.0, 96000);
    for (float ratio : { 0.6f, 0.75f, 1.6f })
    {
        const auto y = shift (x, (float) (sr / 587.0), ratio);
        double mean = 0.0, power = 0.0;
        for (size_t i = 48000; i < y.size(); ++i)
        {
            mean += y[i];
            power += (double) y[i] * y[i];
        }
        mean /= 48000.0;
        const double level = std::sqrt (power / 48000.0);
        INFO ("ratio " << ratio << ": DC " << db (std::abs (mean) / level) << " dB below the signal");
        CHECK (std::abs (mean) < level * 1.0e-3);
    }
}

TEST_CASE ("Formant shifter moves the vowel colour and keeps the pitch")
{
    const auto ooh = makePlaceholderVoice (PlaceholderVowel::ooh, sr, 3.0f);
    const float period = (float) (sr / ooh.rootHz);
    const int from = 48000, n = 48000;
    const double base = brightness (ooh.samples, from, n);

    const auto up = shift (ooh.samples, period, std::exp2 (7.0f / 12.0f));
    const auto down = shift (ooh.samples, period, std::exp2 (-7.0f / 12.0f));
    const double bUp = brightness (up, from, n), bDown = brightness (down, from, n);
    INFO ("brightness: original " << base << ", up " << bUp << ", down " << bDown);
    CHECK (bUp > base * 1.5);
    CHECK (bDown < base * 0.85);

    CHECK_THAT (detectPitch (up.data() + from, n, sr).hz, WithinAbs (ooh.rootHz, 3.0));
    CHECK_THAT (detectPitch (down.data() + from, n, sr).hz, WithinAbs (ooh.rootHz, 3.0));

    // Level stays about the same.
    const double lev = db (rms (up.data() + from, n) / rms (ooh.samples.data() + from, n));
    INFO ("level change up " << lev << " dB");
    CHECK (std::abs (lev) < 3.0);
}

TEST_CASE ("Formant shifter on a breath brightens and darkens it too")
{
    const auto breath = makePlaceholderVoice (PlaceholderVowel::breath, sr, 3.0f);
    const double base = brightness (breath.samples, 24000, 96000);
    const auto up = shift (breath.samples, 0.0f, 1.6f);
    const auto down = shift (breath.samples, 0.0f, 0.6f);
    CHECK (brightness (up, 24000, 96000) > base * 1.3);
    CHECK (brightness (down, 24000, 96000) < base * 0.8);
}

//==============================================================================
TEST_CASE ("Source exchange hands loops over and frees them")
{
    SourceExchange ex;
    auto x = voice (48000);

    auto a = makeSource (x, { 0, 24000, 0, LoopDirection::forward });
    auto b = makeSource (x, { 0, 12000, 0, LoopDirection::forward });
    auto c = makeSource (x, { 0, 6000, 0, LoopDirection::forward });
    std::weak_ptr<const LoopSource> wa = a, wb = b;
    const LoopSource* rawA = a.get();
    const LoopSource* rawC = c.get();

    CHECK (ex.acquire() == nullptr);
    REQUIRE (ex.publish (std::move (a)));
    CHECK (ex.acquire() == rawA);

    // b is replaced before the audio thread sees it: freed straight away.
    REQUIRE (ex.publish (std::move (b)));
    REQUIRE (ex.publish (std::move (c)));
    ex.collectGarbage();
    CHECK (wb.expired());

    CHECK (ex.acquire() == rawC);
    CHECK_FALSE (wa.expired());   // still in use until retired
    ex.retire (rawA);
    ex.collectGarbage();
    CHECK (wa.expired());

    // The latest loop is never retired.
    ex.retire (rawC);
    ex.collectGarbage();
    CHECK (ex.acquire() == rawC);
}

//==============================================================================
TEST_CASE ("Envelope: Attack and Release times")
{
    Envelope e;
    e.prepare (sr);
    e.start (0.5f);
    e.setTimes (0.5f, 2.0f);
    std::vector<float> g ((size_t) sr);
    e.render (g.data(), (int) (0.25 * sr));
    CHECK_THAT (e.level(), WithinAbs (0.5, 0.01));
    e.render (g.data(), (int) (0.26 * sr));
    CHECK_THAT (e.level(), WithinAbs (1.0, 1e-6));

    e.release (2.0f);
    e.render (g.data(), (int) sr);
    e.render (g.data(), (int) sr);
    CHECK_THAT (db (e.level()), WithinAbs (-60.0, 0.5));
}

TEST_CASE ("Spread copies fan out and join one by one")
{
    CHECK (SingVoice::copyShape (0, 0.0f).weight == 1.0f);
    for (int i = 1; i < SingVoice::maxCopies; ++i)
        CHECK (SingVoice::copyShape (i, 0.0f).weight == 0.0f);
    for (int i = 0; i < SingVoice::maxCopies; ++i)
        CHECK (SingVoice::copyShape (i, 1.0f).weight == 1.0f);
    CHECK (SingVoice::copyShape (1, 1.0f).cents > 0.0f);
    CHECK (SingVoice::copyShape (2, 1.0f).cents < 0.0f);
    CHECK_THAT (SingVoice::copyShape (6, 1.0f).pan, WithinAbs (-1.0, 1e-6));
    CHECK_THAT (SingVoice::copyShape (5, 1.0f).cents, WithinAbs (22.0, 1e-4));
}

//==============================================================================
TEST_CASE ("Synth: a key plays the voice at that pitch")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (plainSynth (220.0f));

    // A3 is 220 Hz, the voice's root: play an octave up.
    synth.noteOn (57 + 12, 1.0f);
    const auto r = renderSynth (synth, 48000);
    const auto m = mono (r);
    CHECK_THAT (detectPitch (m.data() + 9600, 38400, sr).hz, WithinAbs (440.0, 4.0));
    CHECK (rms (m.data() + 9600, 38400) > 0.05);
    CHECK (allFinite (r));
}

TEST_CASE ("Synth: Stretch 0 at the root plays the voice as recorded")
{
    // With Stretch, Spread and Formant at 0, the note at the root key is the loop itself:
    // no grains of their own, no resonance.
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    auto settings = plainSynth (220.0f);
    settings.voice.stretch = 0.0f;
    synth.setSettings (settings);
    synth.noteOn (57, 1.0f);   // A3, the root
    const auto r = renderSynth (synth, 48000);

    double err = 0.0, ref = 0.0;
    for (int i = 4800; i < 48000; ++i)
    {
        const float x = src->channel (0)[i % src->size()];
        err += std::pow ((double) r.left[(size_t) i] - x, 2.0);
        ref += (double) x * x;
    }
    INFO ("difference " << db (std::sqrt (err / ref)) << " dB below the voice");
    CHECK (std::sqrt (err / ref) < 0.01);   // 40 dB down
}

TEST_CASE ("Synth: Formant keeps the pitch of a played note")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    auto s = plainSynth (220.0f);
    s.voice.formantSemitones = -6.0f;
    synth.setSettings (s);
    synth.noteOn (57 + 5, 1.0f);   // D4
    const auto m = mono (renderSynth (synth, 48000));
    CHECK_THAT (detectPitch (m.data() + 9600, 38400, sr).hz, WithinAbs (220.0 * std::exp2 (5.0 / 12.0), 4.0));
}

TEST_CASE ("Synth: note off fades out over Release, then the voice frees")
{
    auto src = placeholderSource (PlaceholderVowel::ah);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (plainSynth (196.0f));
    synth.noteOn (55, 0.8f);
    renderSynth (synth, 24000);
    synth.noteOff (55);
    CHECK (synth.activeVoiceCount() == 1);
    const auto r = renderSynth (synth, 48000);
    CHECK (synth.activeVoiceCount() == 0);
    CHECK (rms (r.left.data() + 40000, 8000) < 1.0e-4);
}

TEST_CASE ("Synth: the Voices limit steals notes without clicks")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    auto s = plainSynth (220.0f);
    s.polyphony = 4;
    synth.setSettings (s);

    float maxStep = 0.0f;
    std::vector<float> all;
    for (int n = 0; n < 10; ++n)
    {
        synth.noteOn (50 + n * 2, 0.9f);
        const auto r = renderSynth (synth, 2400);
        all.insert (all.end(), r.left.begin(), r.left.end());
    }
    int sounding = 0;
    for (int i = 0; i < SingSynth::voicePool; ++i)
        if (synth.getVoice (i).isActive() && ! synth.getVoice (i).isKilled())
            ++sounding;
    CHECK (sounding == 4);

    for (size_t i = 1; i < all.size(); ++i)
        maxStep = std::max (maxStep, std::abs (all[i] - all[i - 1]));
    INFO ("largest sample step " << maxStep);
    CHECK (maxStep < 0.25f);
}

TEST_CASE ("Synth: sustain pedal holds released notes")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    synth.setSettings (plainSynth (220.0f));
    synth.setSustain (true);
    synth.noteOn (60, 1.0f);
    renderSynth (synth, 4800);
    synth.noteOff (60);
    renderSynth (synth, 48000);
    CHECK (synth.activeVoiceCount() == 1);
    CHECK_FALSE (synth.getVoice (0).isReleased());
    synth.setSustain (false);
    renderSynth (synth, 48000);
    CHECK (synth.activeVoiceCount() == 0);
}

TEST_CASE ("Synth: Spread widens the note and keeps its level")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    auto play = [&src] (float spread)
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = plainSynth (220.0f);
        s.voice.spread = spread;
        synth.setSettings (s);
        synth.noteOn (57, 1.0f);
        return renderSynth (synth, 96000);
    };

    const auto narrow = play (0.0f);
    const auto wide = play (1.0f);

    auto sideRatio = [] (const Render& r)
    {
        double mid = 0.0, side = 0.0;
        for (size_t i = 9600; i < r.left.size(); ++i)
        {
            const double m = 0.5 * (r.left[i] + r.right[i]), sd = 0.5 * (r.left[i] - r.right[i]);
            mid += m * m;
            side += sd * sd;
        }
        return side / mid;
    };
    INFO ("side/mid: narrow " << sideRatio (narrow) << ", wide " << sideRatio (wide));
    CHECK (sideRatio (wide) > sideRatio (narrow) * 2.0);

    const double lNarrow = rms (narrow.left.data() + 9600, 86400) + rms (narrow.right.data() + 9600, 86400);
    const double lWide = rms (wide.left.data() + 9600, 86400) + rms (wide.right.data() + 9600, 86400);
    INFO ("level change " << db (lWide / lNarrow) << " dB");
    CHECK (std::abs (db (lWide / lNarrow)) < 3.0);
    CHECK (allFinite (wide));
}

TEST_CASE ("Synth: Glide slides from the last note in the set time")
{
    auto src = placeholderSource (PlaceholderVowel::ooh);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (src.get());
    auto s = plainSynth (220.0f);
    s.voice.glideSeconds = 0.5f;
    synth.setSettings (s);
    synth.noteOn (57, 1.0f);
    renderSynth (synth, 4800);
    synth.noteOn (69, 1.0f);
    renderSynth (synth, 12000);   // a quarter of a second

    const SingVoice* newest = nullptr;
    for (int i = 0; i < SingSynth::voicePool; ++i)
        if (synth.getVoice (i).isActive() && synth.getVoice (i).getNote() == 69)
            newest = &synth.getVoice (i);
    REQUIRE (newest != nullptr);
    CHECK_THAT (newest->currentSemitones(), WithinAbs (6.0, 0.3));
    renderSynth (synth, 14400);
    CHECK_THAT (newest->currentSemitones(), WithinAbs (12.0, 1e-4));
}

TEST_CASE ("Synth: swapping the loop keeps notes playing and releases the old one")
{
    auto a = placeholderSource (PlaceholderVowel::ooh);
    auto b = placeholderSource (PlaceholderVowel::ah);
    SingSynth synth;
    synth.prepare (sr, 256);
    synth.setSource (a.get());
    synth.setSettings (plainSynth (220.0f));
    synth.noteOn (57, 1.0f);
    renderSynth (synth, 4800);

    synth.setSource (b.get());
    std::vector<const LoopSource*> retired;
    synth.releaseUnusedSources ([&] (const LoopSource* s) { retired.push_back (s); });
    CHECK (retired.empty());   // grains still finishing on the old loop
    const auto r = renderSynth (synth, 24000);
    synth.releaseUnusedSources ([&] (const LoopSource* s) { retired.push_back (s); });
    REQUIRE (retired.size() == 1);
    CHECK (retired[0] == a.get());
    CHECK (rms (r.left.data() + 12000, 12000) > 0.02);
}

TEST_CASE ("Placeholder voices are clean")
{
    for (auto vowel : { PlaceholderVowel::ooh, PlaceholderVowel::ah, PlaceholderVowel::hum, PlaceholderVowel::breath })
    {
        const auto v = makePlaceholderVoice (vowel, 44100.0, 2.0f);
        float peak = 0.0f;
        bool finite = true;
        for (float s : v.samples)
        {
            peak = std::max (peak, std::abs (s));
            finite &= std::isfinite (s);
        }
        CHECK (finite);
        CHECK (peak > 0.2f);
        CHECK (peak <= 0.5001f);
    }
}

TEST_CASE ("Synth: Formant 0 keeps the vowel colour away from the root")
{
    auto src = placeholderSource (PlaceholderVowel::ah);
    auto play = [&src] (int note, float formant)
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = plainSynth (196.0f);
        s.voice.formantSemitones = formant;
        synth.setSettings (s);
        synth.noteOn (note, 1.0f);
        return mono (renderSynth (synth, 72000));
    };

    const double atRoot = brightness (play (55, 0.0f), 24000, 48000);
    const double fifthUp = brightness (play (62, 0.0f), 24000, 48000);
    const double fifthUpBright = brightness (play (62, 7.0f), 24000, 48000);
    INFO ("brightness: root " << atRoot << ", a fifth up " << fifthUp << ", a fifth up with Formant +7 " << fifthUpBright);
    CHECK (fifthUp < atRoot * 1.35);
    CHECK (fifthUpBright > fifthUp * 1.3);
}

TEST_CASE ("Synth: Keep vowel off is plain pitch shifting")
{
    auto src = placeholderSource (PlaceholderVowel::ah);
    auto play = [&src] (int note, bool keep)
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = plainSynth (196.0f);
        s.voice.keepVowel = keep;
        synth.setSettings (s);
        synth.noteOn (note, 1.0f);
        return mono (renderSynth (synth, 72000));
    };

    // Off, a fifth up sounds brighter (the vowel moved up with the key); on, it doesn't.
    const double atRoot = brightness (play (55, true), 24000, 48000);
    const double kept = brightness (play (62, true), 24000, 48000);
    const double plain = brightness (play (62, false), 24000, 48000);
    INFO ("brightness: root " << atRoot << ", a fifth up kept " << kept << ", plain " << plain);
    CHECK (plain > kept * 1.3);
    CHECK_THAT (detectPitch (play (62, false).data() + 24000, 48000, sr).hz, WithinAbs (196.0 * std::exp2 (7.0 / 12.0), 3.0));
}

TEST_CASE ("Formant shifter adds no partials of its own")
{
    // A steady harmonic vowel, formants moved a fifth either way: all the power stays on the
    // harmonics (the shifter only reshapes magnitudes).
    std::vector<float> x (96000);
    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = 0.0;
        for (int h = 1; h * 220 < 9000; ++h)
        {
            const double hz = h * 220.0;
            v += (std::exp (-std::pow ((hz - 700.0) / 300.0, 2.0)) + 0.3 * std::exp (-std::pow ((hz - 2600.0) / 500.0, 2.0)) + 0.02)
               * std::sin (6.283185307179586 * hz * (double) i / sr + h);
        }
        x[i] = (float) (0.1 * v);
    }
    for (const float ratio : { 0.75f, 1.5f })
    {
        const auto y = shift (x, (float) (sr / 220.0), ratio);
        constexpr int size = 16384;
        sing::RealFFT fft (size);
        std::vector<float> frame ((size_t) size), spectrum ((size_t) size + 2);
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = y[(size_t) (48000 + i)] * (float) (0.5 - 0.5 * std::cos (6.283185307179586 * i / size));
        fft.forward (frame.data(), spectrum.data());
        double on = 0.0, off = 0.0;
        for (int k = 1; k < size / 2; ++k)
        {
            const double hz = k * sr / size;
            const double p = (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)] + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
            (std::abs (hz - 220.0 * std::round (hz / 220.0)) < 10.0 ? on : off) += p;
        }
        INFO ("ratio " << ratio << ": off the harmonics " << db (std::sqrt (off / on)) << " dB");
        CHECK (off < on * 1.0e-4);   // 40 dB down
    }
}

namespace
{
// Level of each 20 ms slice, in dB.
std::vector<double> sliceLevels (const Render& r, int from, int to)
{
    std::vector<double> out;
    constexpr int slice = 960;
    for (int a = from; a + slice <= to; a += slice)
    {
        double e = 0.0;
        for (int i = a; i < a + slice; ++i)
            e += (double) r.left[(size_t) i] * r.left[(size_t) i] + (double) r.right[(size_t) i] * r.right[(size_t) i];
        out.push_back (10.0 * std::log10 (e / (2.0 * slice) + 1.0e-12));
    }
    return out;
}

// 10 % to 90 % spread of the slice levels, and their mean power in dB.
std::pair<double, double> levelSpreadAndMean (const Render& r, int from, int to)
{
    auto levels = sliceLevels (r, from, to);
    double power = 0.0;
    for (double l : levels)
        power += std::pow (10.0, l / 10.0);
    std::sort (levels.begin(), levels.end());
    return { levels[levels.size() * 9 / 10] - levels[levels.size() / 10], 10.0 * std::log10 (power / (double) levels.size()) };
}
} // namespace

TEST_CASE ("Synth: Grain stays in phase with a voice whose pitch moves, at any Stretch and key")
{
    // Grains are placed whole pitch periods apart so they add up cleanly. On a voice with a
    // pitch track (vibrato), that only works while the distance being measured is short:
    // measured across many periods, each grain lands at a different point of the cycle and
    // the cloud phases. Turning Stretch up, or playing away from the root, used to do that,
    // and it stayed after Stretch came back to 0.
    const auto v = makePlaceholderVoice (PlaceholderVowel::ooh, sr, 5.0f);
    LoopSettings s;
    s.start = (int) (0.5 * sr);
    s.length = (int) (3.0 * sr);
    s.fade = (int) (0.3 * sr);
    s.pitchPeriod = (float) (sr / v.rootHz);
    s.pitchTrack = std::make_shared<PitchTrack> (trackPitch (v.samples.data(), (int) v.samples.size(), sr));
    const float* ch[] = { v.samples.data() };
    const auto src = LoopSource::build (ch, 1, (int) v.samples.size(), sr, s);
    const int root = (int) std::lround (12.0 * std::log2 (v.rootHz / 440.0) + 69.0);

    for (int semitones : { 0, 7, -5 })
    {
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto settings = plainSynth (v.rootHz);
        settings.voice.stretch = 0.0f;
        settings.voice.pitched = true;
        synth.setSettings (settings);
        synth.noteOn (root + semitones, 0.8f);

        const auto fresh = levelSpreadAndMean (renderSynth (synth, 3 * 48000), 24000, 3 * 48000);
        settings.voice.stretch = 0.85f;
        synth.setSettings (settings);
        const auto up = levelSpreadAndMean (renderSynth (synth, 5 * 48000), 48000, 5 * 48000);
        settings.voice.stretch = 0.0f;
        synth.setSettings (settings);
        const auto back = levelSpreadAndMean (renderSynth (synth, 3 * 48000), 24000, 3 * 48000);

        INFO (semitones << " st: spread fresh " << fresh.first << ", stretched " << up.first << ", back at 0 " << back.first
                        << " dB; level back at 0 " << back.second - fresh.second << " dB");
        CHECK (fresh.first < 3.0);
        CHECK (up.first < 3.0);
        CHECK (back.first < fresh.first + 1.0);
        CHECK (std::abs (back.second - fresh.second) < 0.5);
    }
}
