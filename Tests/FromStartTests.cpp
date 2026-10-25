// Tests for From start: no loop, each note plays on from Start and holds at the end.

#include "TestHelpers.h"

#include "Bloom/BloomAnalysis.h"
#include "Voice/SingSynth.h"

using namespace sing::test;

namespace
{
// A voice that is a 220 Hz tone for its first 3 s and a 3 kHz tone for its last second, so
// anything heard from the end of the voice shows up at 3 kHz.
std::vector<float> twoPartVoice()
{
    std::vector<float> x ((size_t) (4.0 * sr));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = 0.4f * (float) std::sin (twoPi * ((double) i < 3.0 * sr ? 220.0 : 3000.0) * (double) i / sr);
    return x;
}

std::shared_ptr<const LoopSource> twoPartSource (LoopDirection direction, bool bloom, float windowSeconds = 0.68f)
{
    static const auto x = twoPartVoice();
    LoopSettings s;
    s.start = (int) (0.5 * sr);
    s.length = (int) x.size() - s.start;
    s.direction = direction;
    s.pitchPeriod = (float) (sr / 220.0);
    s.analyseForBloom = bloom;
    s.bloomWindowSeconds = windowSeconds;
    return makeSource (x, s);
}

// Power at one frequency (Goertzel).
double powerAt (const std::vector<float>& x, int from, int n, double hz)
{
    const double c = 2.0 * std::cos (twoPi * hz / sr);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < from + n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (twoPi * (i - from) / n);
        const double s = x[(size_t) i] * w + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

SynthSettings settingsFor (bool bloom, float stretch, bool hold)
{
    SynthSettings s;
    s.voice.attackSeconds = 0.0f;
    s.voice.releaseSeconds = 0.5f;
    s.voice.stretch = stretch;
    s.voice.spread = 0.0f;
    s.voice.hold = hold;
    s.voice.keepVowel = false;
    s.voice.rootHz = 220.0f;
    s.voice.mode = bloom ? StretchMode::bloom : StretchMode::granular;
    s.polyphony = 4;
    return s;
}
} // namespace

TEST_CASE ("From start: the play buffer is the voice from Start, and nothing wraps")
{
    const auto x = twoPartVoice();
    const auto src = twoPartSource (LoopDirection::fromStart, false);
    REQUIRE (src != nullptr);
    CHECK_FALSE (src->wraps());
    CHECK (src->size() == (int) x.size() - (int) (0.5 * sr));
    CHECK (src->effectiveFade() == 0);
    bool same = true;
    for (int i = 0; i < src->size(); ++i)
        same = same && src->channel (0)[i] == x[(size_t) (i + (int) (0.5 * sr))];
    CHECK (same);
    // The guard samples hold the ends rather than wrapping round.
    CHECK (src->channel (0)[-1] == src->channel (0)[0]);
    CHECK (src->channel (0)[src->size() + 2] == src->channel (0)[src->size() - 1]);
    // Positions past either end hold there.
    CHECK (src->place (-100.0) == 0.0);
    CHECK (src->place (1.0e9) == (double) src->size() - 1.0);
    CHECK (src->toVoicePosition (1.0e9) == 0.5 * sr + src->size() - 1.0);
    CHECK (src->distance (10.0, (double) src->size() - 10.0) == (double) src->size() - 20.0);
}

TEST_CASE ("From start: a held note at Start hears nothing from the end of the voice")
{
    // A loop over the same stretch reads round from Start into the end (half a Bloom window,
    // or the grains' jitter), so the 3 kHz part is heard. From start must not.
    for (bool bloom : { false, true })
        for (auto direction : { LoopDirection::forward, LoopDirection::fromStart })
        {
            const auto src = twoPartSource (direction, bloom);
            SingSynth synth;
            synth.prepare (sr, 256);
            synth.setSource (src.get());
            synth.setSettings (settingsFor (bloom, 0.9f, true));
            synth.noteOn (57, 0.9f);
            const int n = (int) (3.0 * sr);
            std::vector<float> left ((size_t) n), right ((size_t) n);
            for (int i = 0; i < n; i += 256)
                synth.render (left.data() + i, right.data() + i, std::min (256, n - i));

            const int from = (int) (0.5 * sr), len = (int) (2.0 * sr);
            const double ratio = db (std::sqrt (powerAt (left, from, len, 3000.0) / std::max (1.0e-30, powerAt (left, from, len, 220.0))));
            INFO ((bloom ? "Bloom" : "Grain") << ", " << (direction == LoopDirection::fromStart ? "From start" : "Forward")
                                              << ": 3 kHz is " << ratio << " dB against 220 Hz");
            CHECK (rms (left.data() + from, len) > 0.01);
            if (direction == LoopDirection::fromStart)
                CHECK (ratio < -50.0);
            else
                CHECK (ratio > -40.0);
        }
}

TEST_CASE ("From start: a note plays on to the end of the voice and holds there")
{
    for (bool bloom : { false, true })
    {
        const auto src = twoPartSource (LoopDirection::fromStart, bloom);
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        synth.setSettings (settingsFor (bloom, 0.0f, false));   // the recorded speed
        synth.noteOn (57, 0.9f);
        // 3.5 s of voice after Start; play 6 s.
        const int n = (int) (6.0 * sr);
        std::vector<float> left ((size_t) n), right ((size_t) n);
        double furthest = 0.0;
        for (int i = 0; i < n; i += 256)
        {
            synth.render (left.data() + i, right.data() + i, std::min (256, n - i));
            furthest = std::max (furthest, synth.newestVoice()->playPosition());
        }
        bool finite = true;
        for (const float v : left)
            finite = finite && std::isfinite (v);
        const int from = (int) (4.8 * sr), len = (int) (1.0 * sr);
        const double level = rms (left.data() + from, len);
        const double ratio = db (std::sqrt (powerAt (left, from, len, 3000.0) / std::max (1.0e-30, powerAt (left, from, len, 220.0))));
        INFO ((bloom ? "Bloom" : "Grain") << ": level at the end " << level << ", 3 kHz " << ratio
                                          << " dB over 220 Hz, furthest " << furthest / sr << " s");
        CHECK (finite);
        CHECK (level > 0.02);     // still sounding, held at the end
        CHECK (ratio > 20.0);     // and what it holds is the end of the voice
        CHECK (furthest <= 4.0 * sr);
    }
}

TEST_CASE ("From start: Bloom's Window can grow to everything after Start")
{
    // A 1 s loop caps a 2.5 s window at 2^15 samples; with no loop the 3.5 s after Start
    // allows the full 2^17.
    static const auto x = twoPartVoice();
    LoopSettings s;
    s.start = (int) (0.5 * sr);
    s.length = (int) (1.0 * sr);
    s.pitchPeriod = (float) (sr / 220.0);
    s.analyseForBloom = true;
    s.bloomWindowSeconds = 2.5f;
    s.bloomNotes = 2;
    const auto looped = makeSource (x, s);
    s.direction = LoopDirection::fromStart;
    s.length = (int) x.size() - s.start;
    const auto open = makeSource (x, s);
    const auto* a = static_cast<const BloomAnalysis*> (looped->analysis (StretchMode::bloom));
    const auto* b = static_cast<const BloomAnalysis*> (open->analysis (StretchMode::bloom));
    REQUIRE (a != nullptr);
    REQUIRE (b != nullptr);
    CHECK (a->fftSize() == 32768);
    CHECK (b->fftSize() == 131072);
    CHECK_FALSE (b->wraps());
    // The last frame's window ends at the end of the voice, and no earlier.
    CHECK (std::abs (b->lastFrameStart() + b->fftSize() - open->size()) < 1.0);
}

TEST_CASE ("From start: Wander only moves forward of Start")
{
    for (bool bloom : { false, true })
    {
        const auto src = twoPartSource (LoopDirection::fromStart, bloom);
        SingSynth synth;
        synth.prepare (sr, 256);
        synth.setSource (src.get());
        auto s = settingsFor (bloom, 0.9f, true);
        s.voice.movement.shape = MoveShape::wave;
        s.voice.movement.cyclesPerSample = 1.0 / (2.0 * sr);
        s.voice.movement.amounts[moveWander] = 0.6f;
        synth.setSettings (s);
        synth.noteOn (57, 0.9f);
        // Where a held note sits at Start, without Wander (Bloom shows its window's middle).
        SingVoice::CopyLight light {};
        synth.newestVoice()->getCopyLights (&light, 1);
        const double atStart = 0.5 * sr + (bloom ? 0.5 * (double) light.zoneSamples : 0.0);
        double lo = 1.0e18, hi = -1.0e18;
        std::vector<float> left (256), right (256);
        for (int block = 0; block < 4 * 48000 / 256; ++block)
        {
            synth.render (left.data(), right.data(), 256);
            if (block * 256 > 24000)
            {
                const double p = synth.newestVoice()->playPosition();
                lo = std::min (lo, p);
                hi = std::max (hi, p);
            }
        }
        INFO ((bloom ? "Bloom" : "Grain") << ": " << (lo - atStart) / sr << " s to " << (hi - atStart) / sr << " s after Start");
        CHECK (lo >= atStart - 0.01 * sr);
        CHECK (hi - lo > 0.4 * 3.5 * sr);   // a 60% orbit over the 3.5 s after Start
    }
}
