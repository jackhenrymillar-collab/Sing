// Tests for Air and Space, the effects after the voices.

#include "TestHelpers.h"

#include "Effects/Air.h"
#include "Effects/Binaural.h"
#include "Effects/Space.h"
#include "Effects/Tape.h"

#include <functional>

using namespace sing::test;

namespace
{
// Level of x above (high) or below a frequency: a second-order RBJ filter, run twice.
double bandLevel (const std::vector<float>& x, double hz, bool high)
{
    const double w0 = twoPi * hz / sr, c = std::cos (w0), alpha = std::sin (w0) / std::sqrt (2.0);
    const double a0 = 1.0 + alpha;
    const double n0 = (high ? (1.0 + c) : (1.0 - c)) / 2.0 / a0, n1 = (high ? -(1.0 + c) : (1.0 - c)) / a0;
    const double a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
    std::vector<double> y (x.begin(), x.end());
    for (int pass = 0; pass < 2; ++pass)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y)
        {
            const double out = n0 * v + n1 * x1 + n0 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = v; y2 = y1; y1 = out;
            v = out;
        }
    }
    double sum = 0.0;
    for (size_t i = 4800; i < y.size(); ++i)
        sum += y[i] * y[i];
    return db (std::sqrt (sum / (double) (y.size() - 4800)));
}

bool finite (const std::vector<float>& x)
{
    return std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
}

void runBlocks (int n, int block, const std::function<void (int, int)>& f)
{
    for (int i = 0; i < n; i += block)
        f (i, std::min (block, n - i));
}
} // namespace

TEST_CASE ("Air: at 0 the signal passes untouched")
{
    auto l = voice (48000), r = voice (48000);
    const auto refL = l, refR = r;
    Air air;
    air.prepare (sr);
    air.setAmount (0.0f);
    runBlocks (48000, 256, [&] (int i, int n) { air.process (l.data() + i, r.data() + i, n); });
    CHECK (l == refL);
    CHECK (r == refR);
}

TEST_CASE ("Air: lifts the top")
{
    // White noise shows the shelf plainly.
    auto dry = noise (48000);
    auto l = dry, r = dry;
    Air air;
    air.prepare (sr);
    air.setAmount (1.0f);
    air.reset();
    runBlocks (48000, 256, [&] (int i, int m) { air.process (l.data() + i, r.data() + i, m); });

    // Above 12 kHz, and below 1 kHz.
    const auto top = [] (const std::vector<float>& x) { return bandLevel (x, 12000.0, true); };
    const auto low = [] (const std::vector<float>& x) { return bandLevel (x, 1000.0, false); };
    std::printf ("  Air 1 on noise: above 12 kHz %+.1f dB, below 1 kHz %+.1f dB\n", top (l) - top (dry), low (l) - low (dry));
    CHECK (top (l) - top (dry) > 6.0);
    CHECK (top (l) - top (dry) < 9.0);
    CHECK (std::abs (low (l) - low (dry)) < 0.5);
    CHECK (finite (l));
}

TEST_CASE ("Air: breath follows the voice")
{
    const int n = 3 * 48000;
    // One second of voice, one of silence, one of voice.
    auto dry = voice (n);
    std::fill (dry.begin() + 48000, dry.begin() + 96000, 0.0f);
    auto l = dry, r = dry;
    Air air;
    air.prepare (sr);
    air.setAmount (1.0f);
    air.reset();
    runBlocks (n, 256, [&] (int i, int m) { air.process (l.data() + i, r.data() + i, m); });

    // The overall level hardly moves.
    CHECK (std::abs (db (rms (l.data() + 9600, 38400) / rms (dry.data() + 9600, 38400))) < 1.0);
    // Between the notes the breath dies away with the voice.
    CHECK (rms (l.data() + 48000 + 24000, 24000) < 1.0e-4);
    // ...and comes back with it, different left and right (it is wide).
    double diff = 0.0;
    for (int i = 96000 + 9600; i < n; ++i)
        diff += std::abs (l[(size_t) i] - r[(size_t) i]);
    CHECK (diff > 1.0);
    CHECK (finite (l));
}

TEST_CASE ("Space: Level 0 passes the signal untouched, and the reverb stops")
{
    auto l = voice (48000), r = voice (48000);
    const auto refL = l;
    Space space;
    space.prepare (sr, 512);
    space.setLevel (0.0f);
    runBlocks (48000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    CHECK (l == refL);
    CHECK_FALSE (space.isRunning());

    // Up, then back down: once the wet has faded the reverb stops again.
    space.setLevel (0.5f);
    CHECK (space.isRunning());
    runBlocks (24000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    space.setLevel (0.0f);
    runBlocks (24000, 256, [&] (int i, int n) { space.process (l.data() + 24000 + i, r.data() + 24000 + i, n); });
    CHECK (space.isRunning());   // still fading out
    runBlocks (48000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    CHECK_FALSE (space.isRunning());
}

TEST_CASE ("Space: Decay sets how long the tail rings, whatever the Level")
{
    // A short burst of voice, then silence: measure how long the tail rings.
    const int n = 12 * 48000;
    for (const float decay : { 1.5f, 4.0f, 12.0f })
        for (const float level : { 0.3f, 1.0f })
        {
            std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
            const auto burst = voice (24000);
            std::copy (burst.begin(), burst.end(), l.begin());
            std::copy (burst.begin(), burst.end(), r.begin());
            Space space;
            space.prepare (sr, 512);
            space.setDecay (decay);
            space.setSize (0.7f);
            space.setLevel (level);
            space.reset();
            runBlocks (n, 512, [&] (int i, int m) { space.process (l.data() + i, r.data() + i, m); });

            // Decay rate over the second after the burst has passed, extrapolated to 60 dB
            // (a short window, so the shortest decay is still well above the noise floor).
            const double a = db (rms (l.data() + 48000 + 12000, 12000));
            const double b = db (rms (l.data() + 48000 + 36000, 12000));
            const double rt60 = 60.0 / std::max (1.0e-3, (a - b) / 0.5);
            double corr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 48000; i < 3 * 48000; ++i)
            {
                corr += (double) l[(size_t) i] * r[(size_t) i];
                ll += (double) l[(size_t) i] * l[(size_t) i];
                rr += (double) r[(size_t) i] * r[(size_t) i];
            }
            corr /= std::sqrt (ll * rr);
            std::printf ("  Space decay %.1f s, level %.1f: measured %.1f s, tail correlation %.2f, peak %.2f\n", decay, level, rt60,
                         corr, *std::max_element (l.begin(), l.end()));

            INFO ("Decay " << decay << " level " << level);
            CHECK (rt60 > 0.6 * decay);
            CHECK (rt60 < 1.6 * decay);
            CHECK (corr < 0.5);   // wide, even from a mono voice
            CHECK (finite (l));
            CHECK (*std::max_element (l.begin(), l.end()) < 1.0f);
        }
}

TEST_CASE ("Space: Level is the reverb's volume alone")
{
    // The dry voice is never turned down, and the tail scales with Level's gain.
    const int n = 3 * 48000;
    const auto run = [&] (float level, bool tailOnly)
    {
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        const auto burst = voice (24000);
        std::copy (burst.begin(), burst.end(), l.begin());
        std::copy (burst.begin(), burst.end(), r.begin());
        Space space;
        space.prepare (sr, 512);
        space.setDecay (5.0f);
        space.setLevel (level);
        space.reset();
        runBlocks (n, 512, [&] (int i, int m) { space.process (l.data() + i, r.data() + i, m); });
        return tailOnly ? rms (l.data() + 48000, 48000) : rms (l.data(), 960);
    };
    // The first 20 ms is all dry (the reverb waits 25 ms at this Size): Level leaves it be.
    CHECK (std::abs (run (1.0f, false) - run (0.0f, false)) < 1.0e-6);
    const double quarter = db (run (0.5f, true)), full = db (run (1.0f, true));
    std::printf ("  Space level 0.5 vs 1: tail %.1f dB vs %.1f dB\n", quarter, full);
    CHECK (std::abs ((full - quarter) - 20.0 * std::log10 (Space::wetGainFor (1.0f) / Space::wetGainFor (0.5f))) < 0.5);
}

TEST_CASE ("Space: an old one-knob setting keeps its reverb-to-voice balance")
{
    for (const float a : { 0.1f, 0.35f, 0.7f, 1.0f })
    {
        const auto old = Space::legacy (a);
        INFO ("Old Space " << a);
        CHECK (std::abs (Space::wetGainFor (old.level) - old.wet / old.dry) < 1.0e-4f);
        CHECK (std::abs (Space::preDelayMsFor (old.size) - old.preDelayMs) < 1.0e-3f);
        CHECK (old.decaySeconds >= Space::minDecaySeconds);
        CHECK (old.decaySeconds <= Space::maxDecaySeconds);
    }
}

TEST_CASE ("Space: its filter cuts lows and highs from the reverb only")
{
    // A burst of noise, then the tail alone: compare the tail with and without the filter.
    const int n = 4 * 48000;
    const auto tail = [&] (bool filtered, double hz, bool high)
    {
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        uint32_t noise = 3;
        for (int i = 0; i < 24000; ++i)
        {
            noise = noise * 1664525u + 1013904223u;
            l[(size_t) i] = r[(size_t) i] = 0.3f * ((float) (noise >> 8) / 8388608.0f - 1.0f);
        }
        Space space;
        space.prepare (sr, 512);
        space.setLevel (0.75f);
        space.setFilter (filtered, 800.0f, 3000.0f);
        space.reset();
        runBlocks (n, 512, [&] (int i, int m) { space.process (l.data() + i, r.data() + i, m); });
        return bandLevel (std::vector<float> (l.begin() + 48000, l.end()), hz, high);
    };
    const double lowCut = tail (false, 300.0, false) - tail (true, 300.0, false);
    const double highCut = tail (false, 8000.0, true) - tail (true, 8000.0, true);
    std::printf ("  Space filter 800 Hz to 3 kHz: tail under 300 Hz down %.1f dB, over 8 kHz down %.1f dB\n", lowCut, highCut);
    CHECK (lowCut > 12.0);
    CHECK (highCut > 12.0);
}

TEST_CASE ("Binaural: each ear moves by half the beat, in opposite directions")
{
    const int n = 1 << 16;
    Binaural b;
    b.prepare (sr);
    b.set (true, 4.0f, 1.0f);
    b.reset();
    std::vector<float> l ((size_t) n), r ((size_t) n);
    for (int i = 0; i < n; ++i)
        l[(size_t) i] = r[(size_t) i] = (float) std::sin (twoPi * 1000.0 * i / sr);
    b.process (l.data(), r.data(), n);
    // Correlate against 1002 Hz and 998 Hz tones: each ear should match its own.
    const auto match = [&] (const std::vector<float>& x, double hz)
    {
        double c = 0.0, s = 0.0;
        for (int i = 4800; i < n; ++i)
        {
            c += x[(size_t) i] * std::cos (twoPi * hz * i / sr);
            s += x[(size_t) i] * std::sin (twoPi * hz * i / sr);
        }
        return 2.0 * std::sqrt (c * c + s * s) / (n - 4800);
    };
    std::printf ("  Binaural 4 Hz: left at 1002 Hz %.2f, 998 Hz %.2f; right at 998 Hz %.2f, 1002 Hz %.2f\n",
                 match (l, 1002.0), match (l, 998.0), match (r, 998.0), match (r, 1002.0));
    CHECK (match (l, 1002.0) > 0.95);
    CHECK (match (l, 998.0) < 0.05);
    CHECK (match (r, 998.0) > 0.95);
    CHECK (match (r, 1002.0) < 0.05);

    // Off, the sound passes untouched.
    Binaural off;
    off.prepare (sr);
    std::vector<float> a ((size_t) 4800, 0.25f), c = a;
    off.process (a.data(), c.data(), 4800);
    CHECK (a == std::vector<float> ((size_t) 4800, 0.25f));
}

namespace
{
// How much of x, from 'from' on, is a sine at hz (its amplitude).
double toneAmplitude (const std::vector<float>& x, double hz, int from, int to)
{
    double c = 0.0, s = 0.0;
    for (int i = from; i < to; ++i)
    {
        c += x[(size_t) i] * std::cos (twoPi * hz * i / sr);
        s += x[(size_t) i] * std::sin (twoPi * hz * i / sr);
    }
    return 2.0 * std::sqrt (c * c + s * s) / (to - from);
}

struct TapeRun
{
    std::vector<float> left, right;
};

TapeRun runTape (float wow, float flutter, float drive, float age, double hz, float amp, int n, bool on = true)
{
    Tape t;
    t.prepare (sr);
    t.set (on, wow, flutter, drive, age);
    t.reset();
    TapeRun r { sine (hz, n, amp), sine (hz, n, amp) };
    for (int i = 0; i < n; i += 512)
        t.process (r.left.data() + i, r.right.data() + i, std::min (512, n - i));
    return r;
}

// The pitch of a sine over time, in cents from hz: one value per rising zero crossing.
std::vector<double> centsTrack (const std::vector<float>& x, double hz, int from)
{
    std::vector<double> cents;
    double last = -1.0;
    for (size_t i = (size_t) from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double at = (double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (last >= 0.0)
                cents.push_back (1200.0 * std::log2 (sr / (at - last) / hz));
            last = at;
        }
    return cents;
}
} // namespace

TEST_CASE ("Tape: off passes the signal untouched")
{
    const int n = 48000;
    const auto r = runTape (0.5f, 0.5f, 0.5f, 0.5f, 440.0, 0.3f, n, false);
    const auto dry = sine (440.0, n, 0.3f);
    CHECK (r.left == dry);
    CHECK (r.right == dry);
}

TEST_CASE ("Tape: Wow and Flutter sway the pitch, and both ears together")
{
    const int n = 6 * 48000;
    auto peak = [] (const std::vector<double>& c)
    {
        double m = 0.0;
        for (double v : c)
            m = std::max (m, std::abs (v));
        return m;
    };
    const auto still = runTape (0.0f, 0.0f, 0.0f, 0.0f, 1000.0, 0.25f, n);
    const auto wow = runTape (1.0f, 0.0f, 0.0f, 0.0f, 1000.0, 0.25f, n);
    const auto flutter = runTape (0.0f, 1.0f, 0.0f, 0.0f, 1000.0, 0.25f, n);
    const double s = peak (centsTrack (still.left, 1000.0, 4800));
    const double w = peak (centsTrack (wow.left, 1000.0, 4800));
    const double f = peak (centsTrack (flutter.left, 1000.0, 4800));
    std::printf ("  Tape pitch sway: still %.2f cents, Wow 100%% %.1f cents, Flutter 100%% %.1f cents\n", s, w, f);
    CHECK (s < 0.5);
    CHECK (w > 15.0);
    CHECK (w < 40.0);
    CHECK (f > 4.0);
    CHECK (f < 15.0);
    CHECK (wow.left == wow.right);
}

TEST_CASE ("Tape: Drive saturates softly and keeps the level")
{
    const int n = 48000;
    const auto clean = runTape (0.0f, 0.0f, 0.0f, 0.0f, 200.0, 0.25f, n);
    const auto driven = runTape (0.0f, 0.0f, 1.0f, 0.0f, 200.0, 0.25f, n);
    const double cleanThird = db (toneAmplitude (clean.left, 600.0, 4800, n) / toneAmplitude (clean.left, 200.0, 4800, n));
    const double drivenThird = db (toneAmplitude (driven.left, 600.0, 4800, n) / toneAmplitude (driven.left, 200.0, 4800, n));
    const double levelChange = db (rms (driven.left.data() + 4800, n - 4800) / rms (clean.left.data() + 4800, n - 4800));
    std::printf ("  Tape Drive: third harmonic %.1f dB clean, %.1f dB driven; level %+.1f dB\n", cleanThird, drivenThird, levelChange);
    CHECK (cleanThird < -45.0);
    CHECK (drivenThird > -30.0);
    CHECK (std::abs (levelChange) < 1.5);
}

TEST_CASE ("Tape: Age rolls off the highs only")
{
    const int n = 48000;
    auto change = [n] (double hz, float age)
    {
        const auto r = runTape (0.0f, 0.0f, 0.0f, age, hz, 0.1f, n);
        return db (rms (r.left.data() + 4800, n - 4800) / rms (sine (hz, n, 0.1f).data() + 4800, n - 4800));
    };
    std::printf ("  Tape Age 100%%: 300 Hz %+.1f dB, 8 kHz %+.1f dB; Age 0: 8 kHz %+.1f dB\n",
                 change (300.0, 1.0f), change (8000.0, 1.0f), change (8000.0, 0.0f));
    CHECK (std::abs (change (300.0, 1.0f)) < 0.5);
    CHECK (change (8000.0, 1.0f) < -15.0);
    CHECK (std::abs (change (8000.0, 0.0f)) < 0.5);
}

namespace
{
// A note into Space for a second, then the tail; returns the left channel.
std::vector<float> shimmerTail (bool on, int interval, float decay, float rise, int seconds, float hz = 220.0f, float amount = 1.0f)
{
    Space s;
    s.prepare (sr, 512);
    s.setDecay (decay);
    s.setSize (0.6f);
    s.setLevel (0.8f);
    s.setShimmer (on, amount, rise, interval);
    s.reset();
    const int n = seconds * 48000;
    auto l = sine (hz, n, 0.3f), r = l;
    for (int i = 48000; i < n; ++i)
        l[(size_t) i] = r[(size_t) i] = 0.0f;
    for (int i = 0; i < n; i += 512)
        s.process (l.data() + i, r.data() + i, std::min (512, n - i));
    return l;
}
} // namespace

TEST_CASE ("Space: Shimmer adds a halo an octave or a fifth up")
{
    const int from = 2 * 48000, to = 4 * 48000;
    // At the default settings: Amount 40%, Rise 50%.
    const auto plain = shimmerTail (false, 0, 6.0f, 0.5f, 4, 220.0f, 0.4f);
    const auto octave = shimmerTail (true, 0, 6.0f, 0.5f, 4, 220.0f, 0.4f);
    const auto fifth = shimmerTail (true, 1, 6.0f, 0.5f, 4, 220.0f, 0.4f);
    const double plainUp = db (toneAmplitude (plain, 440.0, from, to) / toneAmplitude (plain, 220.0, from, to));
    const double octaveUp = db (toneAmplitude (octave, 440.0, from, to) / toneAmplitude (octave, 220.0, from, to));
    const double fifthUp = db (toneAmplitude (fifth, 330.0, from, to) / toneAmplitude (fifth, 220.0, from, to));
    std::printf ("  Shimmer in the tail, against the note: octave %+.1f dB (none without %+.1f), fifth %+.1f dB\n",
                 octaveUp, plainUp, fifthUp);
    CHECK (plainUp < -20.0);
    CHECK (octaveUp > -12.0);
    CHECK (fifthUp > -12.0);
}

TEST_CASE ("Space: Shimmer at the top of Rise and Decay fades away, never runs off")
{
    const auto x = shimmerTail (true, 1, 60.0f, 1.0f, 40, 150.0f);
    const auto at = [&x] (int second) { return db (rms (x.data() + second * 48000, 5 * 48000)); };
    float peak = 0.0f;
    for (float v : x)
        peak = std::max (peak, std::abs (v));
    std::printf ("  Shimmer fifth, Rise 100%%, Decay 60 s: peak %.2f; tail %.1f dB at 5 s, %.1f dB at 20 s, %.1f dB at 35 s\n",
                 peak, at (5), at (20), at (35));
    CHECK (std::isfinite (peak));
    CHECK (peak < 3.0f);
    CHECK (at (35) < at (20));
    CHECK (at (20) < at (5));
}
