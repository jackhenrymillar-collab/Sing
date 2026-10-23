// Tests for Sing's loop and granular Stretch engine. Plain C++, no test framework needed:
// run the executable, it prints each check and returns non-zero on failure.

#include "Stretch/GranularEngine.h"
#include "Stretch/LoopSource.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace sing;

namespace
{
int failures = 0;
int checks = 0;

void check (bool ok, const std::string& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::printf ("  FAIL: %s\n", what.c_str());
    }
}

void test (const char* name, const std::function<void()>& body)
{
    std::printf ("%s\n", name);
    body();
}

constexpr double sr = 48000.0;
constexpr double twoPi = 6.28318530717958647692;

std::vector<float> sine (double hz, int n, float amp = 0.5f)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = amp * (float) std::sin (twoPi * hz * i / sr);
    return x;
}

// A vowel-ish test voice: harmonics of 220 Hz with a slow vibrato, plus a little breath noise.
std::vector<float> voice (int n)
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

std::vector<float> noise (int n, uint32_t seed = 777)
{
    std::vector<float> x ((size_t) n);
    for (auto& v : x)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        v = (float) ((double) seed / 4294967296.0 - 0.5);
    }
    return x;
}

double rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt (s / std::max (1, n));
}

double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

std::shared_ptr<const LoopSource> makeSource (const std::vector<float>& x, LoopSettings s)
{
    const float* ch[] = { x.data() };
    return LoopSource::build (ch, 1, (int) x.size(), sr, s);
}

struct Render
{
    std::vector<float> left, right;
};

Render render (GranularEngine& e, int n, int block = 256)
{
    Render r { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; i += block)
        e.process (r.left.data() + i, r.right.data() + i, std::min (block, n - i));
    return r;
}

bool allFinite (const Render& r)
{
    for (size_t i = 0; i < r.left.size(); ++i)
        if (! std::isfinite (r.left[i]) || ! std::isfinite (r.right[i]))
            return false;
    return true;
}

double zeroCrossingHz (const std::vector<float>& x, int from, int n)
{
    int crossings = 0;
    for (int i = from + 1; i < from + n; ++i)
        if (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f)
            ++crossings;
    return crossings * sr / n;
}

// Standard deviation, in dB, of the level measured in 20 ms windows. Low means steady.
double levelWobbleDb (const std::vector<float>& x, int from, int n)
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

GranularEngine::Settings plainSettings()
{
    GranularEngine::Settings s;
    s.jitterSeconds = 0.0f;
    s.timingJitter = 0.0f;
    s.stereoWidth = 0.0f;
    return s;
}
} // namespace

int main()
{
    test ("Loop: forward seam is smooth after the crossfade", [] {
        auto x = sine (441.3, 48000);
        auto src = makeSource (x, { 6000, 20065, 2400, LoopDirection::forward });
        const float* b = src->channel (0);
        const int n = src->size();
        check (n == 20065, "loop keeps its length when there is room before Start");
        check (src->effectiveFade() == 2400, "full fade used");

        float maxStep = 0.0f;
        for (int i = 1; i < n; ++i)
            maxStep = std::max (maxStep, std::abs (b[i] - b[i - 1]));
        const float seamStep = std::abs (b[0] - b[n - 1]);
        check (seamStep <= maxStep * 1.05f, "seam step " + std::to_string (seamStep) + " vs max " + std::to_string (maxStep));
        check (b[-1] == b[n - 1] && b[n] == b[0] && b[n + 2] == b[2], "guard samples wrap around");

        auto raw = makeSource (x, { 6000, 20065, 0, LoopDirection::forward });
        const float rawSeam = std::abs (raw->channel (0)[0] - raw->channel (0)[n - 1]);
        check (rawSeam > maxStep * 2.0f, "without Fade the seam does click (test is meaningful)");
    });

    test ("Loop: Fade falls back to shortening when Start is near the beginning", [] {
        auto x = sine (300.0, 48000);
        auto src = makeSource (x, { 100, 10000, 2000, LoopDirection::forward });
        check (src->size() == 8000, "loop shortened by the fade");
        check (src->effectiveFade() == 2000, "fade applied");
    });

    test ("Loop: Backward and Back & forth", [] {
        auto x = noise (20000);
        auto fwd = makeSource (x, { 1000, 5000, 0, LoopDirection::forward });
        auto bwd = makeSource (x, { 1000, 5000, 0, LoopDirection::backward });
        auto pp = makeSource (x, { 1000, 5000, 0, LoopDirection::pingPong });

        bool reversed = true;
        for (int i = 0; i < 5000; ++i)
            reversed &= bwd->channel (0)[i] == fwd->channel (0)[4999 - i];
        check (reversed, "Backward is the loop reversed");
        check (pp->size() == 9998, "Back & forth is 2 * length - 2");
        check (pp->channel (0)[5000] == x[1000 + 4998], "bounce turns at the loop end");

        check (std::abs (fwd->toVoicePosition (10.0) - 1010.0) < 1e-9, "forward position maps to the voice");
        check (std::abs (bwd->toVoicePosition (0.0) - 5999.0) < 1e-9, "backward starts at the loop end");
        check (std::abs (pp->toVoicePosition (6000.0) - (1000.0 + 3998.0)) < 1e-9, "back & forth maps the return trip");
    });

    test ("Granular: speed 1, pitch 1, no jitter reproduces the loop", [] {
        auto x = voice (96000);
        auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setSettings (plainSettings());
        e.setParameters ({ 1.0f, 1.0f, false });
        e.start (*src);
        auto r = render (e, 48000);

        double maxErr = 0.0;
        for (int i = 0; i < 48000; ++i)
            maxErr = std::max (maxErr, (double) std::abs (r.left[(size_t) i] - x[(size_t) i]));
        check (maxErr < 1.0e-3, "max error " + std::to_string (maxErr));
        check (allFinite (r), "output finite");
    });

    test ("Granular: pitch follows the key without changing speed", [] {
        auto x = sine (220.0, 96000);
        auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward, (float) (sr / 220.0) });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setSettings (StretchKnob::toGranularSettings (0.5f));
        e.setParameters ({ 0.2f, 2.0f, false });
        e.start (*src);
        auto r = render (e, 48000);
        const double hz = zeroCrossingHz (r.left, 4800, 38400);
        check (std::abs (hz - 440.0) < 6.0, "octave up measures " + std::to_string (hz) + " Hz");
        check (std::abs (e.headPosition() - 9600.0) < 2.0, "read head moved at its own speed");
    });

    test ("Granular: Stretch slows the read head", [] {
        auto x = voice (96000);
        auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setParameters ({ 0.25f, 1.0f, false });
        e.start (*src);
        render (e, 48000);
        check (std::abs (e.headPosition() - 12000.0) < 2.0, "head at " + std::to_string (e.headPosition()));
    });

    test ("Granular: Hold freezes the read head and keeps sounding", [] {
        auto x = voice (96000);
        auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setParameters ({ 1.0f, 1.0f, false });
        e.start (*src);
        render (e, 4800);
        e.setParameters ({ 1.0f, 1.0f, true });
        render (e, 48000);   // let the slow-down settle
        const double held = e.headPosition();
        auto r = render (e, 48000);
        check (std::abs (e.headPosition() - held) < 1.0, "head moved " + std::to_string (e.headPosition() - held));
        check (rms (r.left.data(), 48000) > 0.5 * rms (x.data(), 96000), "still sounding");
        check (e.activeGrainCount() >= 3, "grain cloud alive");
    });

    test ("Granular: level and steadiness across the Stretch knob", [] {
        struct Material { const char* name; std::vector<float> x; float period; };
        const float p220 = (float) (sr / 220.0);
        std::vector<Material> materials { { "sine", sine (220.0, 96000), p220 },
                                          { "voice", voice (96000), p220 },
                                          { "voice, pitch unknown", voice (96000), 0.0f },
                                          { "noise", noise (96000), 0.0f } };

        for (auto& m : materials)
        {
            auto src = makeSource (m.x, { 0, 96000, 0, LoopDirection::forward, m.period });
            const double inLevel = rms (m.x.data(), 96000);

            for (float knob : { 0.0f, 0.3f, 0.6f, 0.9f, 1.0f })
            {
                GranularEngine e;
                e.prepare (sr, 512);
                e.setSeed (42);
                e.setSettings (StretchKnob::toGranularSettings (knob));
                e.setParameters ({ StretchKnob::toSpeed (knob), 1.0f, false });
                e.start (*src);
                auto r = render (e, 96000);

                std::vector<float> mid (r.left.size());
                for (size_t i = 0; i < mid.size(); ++i)
                    mid[i] = 0.5f * (r.left[i] + r.right[i]);

                const double level = db (rms (r.left.data() + 9600, 86400) / inLevel);
                const double wobble = levelWobbleDb (mid, 9600, 86400);
                std::printf ("  %-20s knob %.1f  level %+5.2f dB  wobble %.2f dB\n", m.name, knob, level, wobble);
                check (std::abs (level) < 2.0, std::string (m.name) + " level within 2 dB");
                check (allFinite (r), "output finite");
            }
        }
    });

    test ("Granular: frozen sine does not flutter", [] {
        auto x = sine (220.0, 96000);
        auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward, (float) (sr / 220.0) });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setSeed (7);
        e.setSettings (StretchKnob::toGranularSettings (1.0f));
        e.setParameters ({ 0.0f, 1.0f, false });
        e.start (*src);
        auto r = render (e, 96000);
        const double wobble = levelWobbleDb (r.left, 9600, 86400);
        std::printf ("  wobble %.2f dB\n", wobble);
        check (wobble < 3.0, "level wobble under 3 dB");
    });

    test ("Granular: swapping the loop mid-note keeps the old one alive until its grains end", [] {
        auto x = voice (96000);
        auto a = makeSource (x, { 0, 48000, 0, LoopDirection::forward });
        auto b = makeSource (x, { 0, 48000, 0, LoopDirection::backward });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setParameters ({ 1.0f, 1.0f, false });
        e.start (*a);
        render (e, 10000);
        const double before = e.playPosition();
        e.setSource (*b);
        check (std::abs (e.playPosition() - before) < 2.0, "play light stays in place");
        check (e.isUsing (*a), "old loop still in use by sounding grains");
        render (e, 48000);
        check (! e.isUsing (*a), "old loop released after a grain length");
    });

    test ("Granular: stereo voice and Glide stay finite", [] {
        auto l = voice (48000);
        auto rr = noise (48000);
        const float* ch[] = { l.data(), rr.data() };
        auto src = LoopSource::build (ch, 2, 48000, 44100.0, { 0, 48000, 400, LoopDirection::pingPong });
        GranularEngine e;
        e.prepare (sr, 512);
        e.setParameters ({ 0.1f, 0.5f, false });
        e.start (*src);
        Render all;
        for (int i = 0; i < 20; ++i)
        {
            e.setParameters ({ 0.1f, 0.5f + 0.1f * (float) i, false });
            auto r = render (e, 2400, 64);
            if (! allFinite (r)) { check (false, "finite during glide"); return; }
        }
        check (true, "finite during glide");
    });

    test ("Stretch knob mapping", [] {
        check (StretchKnob::toSpeed (0.0f) == 1.0f, "knob 0 is the recorded speed");
        check (StretchKnob::toSpeed (1.0f) == 0.0f, "knob 1 is the held breath");
        float last = 2.0f;
        bool monotonic = true;
        for (int i = 0; i <= 100; ++i)
        {
            const float s = StretchKnob::toSpeed ((float) i / 100.0f);
            monotonic &= s <= last;
            last = s;
        }
        check (monotonic, "speed falls as the knob turns up");
    });

    test ("Factory", [] {
        auto e = createStretchEngine (StretchMode::granular);
        check (e != nullptr && e->mode() == StretchMode::granular, "granular engine");
    });

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
