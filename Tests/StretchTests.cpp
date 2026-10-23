// Tests for Sing's loop and granular Stretch engine.

#include "TestHelpers.h"

using namespace sing::test;

TEST_CASE ("Loop: forward seam is smooth after the crossfade")
{
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
}

TEST_CASE ("Loop: Fade falls back to shortening when Start is near the beginning")
{
    auto x = sine (300.0, 48000);
    auto src = makeSource (x, { 100, 10000, 2000, LoopDirection::forward });
    check (src->size() == 8000, "loop shortened by the fade");
    check (src->effectiveFade() == 2000, "fade applied");
}

TEST_CASE ("Loop: Backward and Back & forth")
{
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
}

TEST_CASE ("Granular: speed 1, pitch 1, no jitter reproduces the loop")
{
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
}

TEST_CASE ("Granular: pitch follows the key without changing speed")
{
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
}

TEST_CASE ("Granular: Stretch slows the read head")
{
    auto x = voice (96000);
    auto src = makeSource (x, { 0, 96000, 0, LoopDirection::forward });
    GranularEngine e;
    e.prepare (sr, 512);
    e.setParameters ({ 0.25f, 1.0f, false });
    e.start (*src);
    render (e, 48000);
    check (std::abs (e.headPosition() - 12000.0) < 2.0, "head at " + std::to_string (e.headPosition()));
}

TEST_CASE ("Granular: Hold freezes the read head and keeps sounding")
{
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
}

TEST_CASE ("Granular: level and steadiness across the Stretch knob")
{
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
}

TEST_CASE ("Granular: frozen sine does not flutter")
{
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
}

TEST_CASE ("Granular: swapping the loop mid-note keeps the old one alive until its grains end")
{
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
}

TEST_CASE ("Granular: stereo voice and Glide stay finite")
{
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
}

TEST_CASE ("Stretch knob mapping")
{
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
}

TEST_CASE ("Factory")
{
    auto e = createStretchEngine (StretchMode::granular);
    check (e != nullptr && e->mode() == StretchMode::granular, "granular engine");
}
