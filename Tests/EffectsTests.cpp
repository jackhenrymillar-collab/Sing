// Tests for Air and Space, the effects after the voices.

#include "TestHelpers.h"

#include "Effects/Air.h"
#include "Effects/Space.h"

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

TEST_CASE ("Space: at 0 the signal passes untouched, and the reverb stops")
{
    auto l = voice (48000), r = voice (48000);
    const auto refL = l;
    Space space;
    space.prepare (sr, 512);
    space.setAmount (0.0f);
    runBlocks (48000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    CHECK (l == refL);
    CHECK_FALSE (space.isRunning());

    // Up, then back down: once the wet has faded the reverb stops again.
    space.setAmount (0.5f);
    CHECK (space.isRunning());
    runBlocks (24000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    space.setAmount (0.0f);
    runBlocks (24000, 256, [&] (int i, int n) { space.process (l.data() + 24000 + i, r.data() + 24000 + i, n); });
    CHECK (space.isRunning());   // still fading out
    runBlocks (48000, 256, [&] (int i, int n) { space.process (l.data() + i, r.data() + i, n); });
    CHECK_FALSE (space.isRunning());
}

TEST_CASE ("Space: the knob sets one long, lush tail")
{
    // A short burst of voice, then silence: measure how long the tail rings.
    const int n = 12 * 48000;
    for (const float amount : { 0.25f, 0.5f, 1.0f })
    {
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        const auto burst = voice (24000);
        std::copy (burst.begin(), burst.end(), l.begin());
        std::copy (burst.begin(), burst.end(), r.begin());
        Space space;
        space.prepare (sr, 512);
        space.setAmount (amount);
        space.reset();
        runBlocks (n, 512, [&] (int i, int m) { space.process (l.data() + i, r.data() + i, m); });

        // Decay rate between 1 and 3 seconds after the burst, extrapolated to 60 dB.
        const double a = db (rms (l.data() + 48000 + 24000, 24000));
        const double b = db (rms (l.data() + 3 * 48000, 24000));
        const double rt60 = 60.0 / std::max (1.0e-3, (a - b) / 1.75);
        const auto m = Space::mapping (amount);
        double corr = 0.0, ll = 0.0, rr = 0.0;
        for (int i = 48000; i < 3 * 48000; ++i)
        {
            corr += (double) l[(size_t) i] * r[(size_t) i];
            ll += (double) l[(size_t) i] * l[(size_t) i];
            rr += (double) r[(size_t) i] * r[(size_t) i];
        }
        corr /= std::sqrt (ll * rr);
        std::printf ("  Space %.2f: decay %.1f s (set %.1f s), tail correlation %.2f, peak %.2f\n", amount, rt60,
                     m.decaySeconds, corr, *std::max_element (l.begin(), l.end()));

        INFO ("Space " << amount);
        CHECK (rt60 > 0.6 * m.decaySeconds);
        CHECK (rt60 < 1.6 * m.decaySeconds);
        CHECK (corr < 0.5);   // wide, even from a mono voice
        CHECK (finite (l));
        CHECK (*std::max_element (l.begin(), l.end()) < 1.0f);
    }
}
