#include "Engine.h"

#include "Blocks.h"
#include "Formant.h"
#include "Halfband.h"
#include "Shared.h"
#include "Space.h"
#include "VoiceData.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace sing::dsp
{

namespace
{
    constexpr int kControl = 16;        // internal samples per control step
    constexpr int kMaxNotes = 24;       // 16 playable plus room for notes fading out
    constexpr int kCopies = 7;          // Spread: copies per note
    constexpr int kLayerCopies = 3;     // copies in each octave layer
    constexpr int kLayers = 3;          // the note, -1 octave, +1 octave
    constexpr int kStreams = kCopies + 2 * kLayerCopies;
    constexpr int kChunk = 256;         // host samples rendered at a time

    // Spread: where each copy sits. Detune in units of the full spread (cents below),
    // pan from -1 to 1. Copy 0 is the note itself.
    constexpr std::array<float, kCopies> kDetune { 0.0f, -1.0f, 1.0f, -0.62f, 0.62f, -0.3f, 0.3f };
    constexpr std::array<float, kCopies> kPan { 0.0f, -1.0f, 1.0f, -0.7f, 0.7f, -0.4f, 0.4f };
    constexpr float kSpreadCents = 22.0f;

    constexpr float kLayerGain[kLayers] { 1.0f, 0.6f, 0.4f };
    constexpr float kLayerOctave[kLayers] { 0.0f, -1.0f, 1.0f };

    constexpr float kMasterTrim = 1.0f;

    float smoothstep (float x) noexcept
    {
        x = std::clamp (x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
}

struct Engine::Impl
{
    // ---- one stream: a single read of the voice (a Spread copy, or an octave layer) ----
    struct Stream
    {
        LoopPlayer player;
        GrainCloud cloud;
        DriftLfo drift;
        float granMix = 0.0f;   // 0 = plain loop playback, 1 = grains
        float gain = 0.0f;      // current, ramps to target each control step
        float panL = 0.7071f, panR = 0.7071f;
        double ratio = 1.0;
        int layer = 0, copy = 0;
        bool started = false;
    };

    struct Note
    {
        bool active = false;
        int key = 60;
        float velocity = 1.0f;
        bool gate = false, sustained = false;
        std::uint64_t age = 0;

        enum class Stage { attack, sustain, release, quick } stage = Stage::attack;
        float attackPos = 0.0f;  // 0..1 through the attack
        float level = 0.0f;      // output gain of the envelope
        float releaseMul = 1.0f;

        float semis = 60.0f;     // current pitch (glides to key)
        const VoiceData* data = nullptr;
        std::array<Stream, kStreams> streams {};
    };

    // ---- state ----
    double fs = 48000.0, fsInt = 96000.0;
    int maxBlock = 512;
    EngineParams params;

    std::atomic<VoiceData*> pending { nullptr };
    VoiceData* current = nullptr;
    std::array<VoiceData*, 8> fading {};
    std::array<std::atomic<VoiceData*>, 16> retired {};
    std::atomic<bool> haveVoice { false };

    std::array<Note, kMaxNotes> notes {};
    std::uint64_t ageCounter = 0;
    float lastSemis = -1.0f;
    float bend = 0.0f;
    bool pedal = false;
    Random rng { 12345u };

    // Loop, smoothed at control rate.
    Smoother loopStart, loopLength;

    // Tape.
    DriftLfo wow;
    Lfo flutter;
    float tapeMix = 0.0f;
    AdaaTanh satL, satR;

    // Buffers at the internal rate.
    std::vector<float> intL, intR;
    HalfbandDecimator decL, decR;

    // Host-rate effects.
    FormantShifter formant;
    DriftLfo formantDrift;
    Smoother formantSmooth;
    Biquad shelfL, shelfR, breathHpL, breathHpR, breathLpL, breathLpR;
    float shelfDb = -100.0f;
    EnvelopeFollower breathEnv;
    Smoother airAmount;
    Space space;
    Smoother outGain, bypassGain;

    std::array<std::atomic<float>, kMaxPlayheads> playheads {};

    // -------------------------------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        fsInt = 2.0 * sampleRate;
        maxBlock = maxBlockSize;
        const double controlRate = fsInt / kControl;

        loopStart.prepare (controlRate, 40.0f);
        loopLength.prepare (controlRate, 40.0f);
        wow.prepare (controlRate);
        wow.setFrequency (0.55f);
        wow.setPhase (0.37f);
        flutter.prepare (controlRate);
        flutter.setFrequency (6.3f);

        intL.assign (2 * kChunk + 8, 0.0f);
        intR.assign (2 * kChunk + 8, 0.0f);
        decL.prepare();
        decR.prepare();

        formant.prepare (fs);
        formantDrift.prepare (fs / 64.0);
        formantDrift.setFrequency (0.09f);
        formantDrift.setPhase (0.71f);
        formantSmooth.prepare (fs, 30.0f);
        breathHpL.setHighpass (2400.0f, fs);
        breathHpR.setHighpass (2400.0f, fs);
        breathLpL.setLowpass (10000.0f, fs);
        breathLpR.setLowpass (10000.0f, fs);
        breathEnv.prepare (fs, 15.0f, 250.0f);
        airAmount.prepare (fs, 50.0f);
        space.prepare (fs);
        outGain.prepare (fs, 30.0f);
        bypassGain.prepare (fs, 8.0f);

        for (auto& n : notes)
            for (std::size_t s = 0; s < n.streams.size(); ++s)
                n.streams[s].drift.prepare (controlRate);

        reset();
    }

    void reset()
    {
        for (auto& n : notes)
            n.active = false;
        decL.reset(); decR.reset();
        formant.reset();
        space.reset();
        satL.reset(); satR.reset();
        shelfL.reset(); shelfR.reset();
        breathHpL.reset(); breathHpR.reset(); breathLpL.reset(); breathLpR.reset();
        breathEnv.reset();
        loopStart.snapTo (params.loopStart);
        loopLength.snapTo (params.loopLength);
        tapeMix = params.tape ? 1.0f : 0.0f;
        formantSmooth.snapTo (params.formant);
        airAmount.snapTo (params.air);
        outGain.snapTo (std::pow (10.0f, params.outputDb / 20.0f));
        bypassGain.snapTo (params.bypass ? 0.0f : 1.0f);
        updateShelf (params.air);
        for (auto& p : playheads)
            p.store (-1.0f);
    }

    int latency() const noexcept { return decL.latency() + formant.latency(); }

    // ---- voices ----------------------------------------------------------------------
    void pickUpVoice() noexcept
    {
        VoiceData* incoming = pending.exchange (nullptr);
        if (incoming == nullptr)
            return;
        if (current != nullptr)
        {
            for (auto& n : notes)
                if (n.active && n.data == current)
                    quickRelease (n);
            bool parked = false;
            for (auto& f : fading)
                if (f == nullptr) { f = current; parked = true; break; }
            if (! parked)
                retire (current); // never expected: eight voice changes within one fade
        }
        current = incoming;
    }

    void retire (VoiceData* v) noexcept
    {
        for (auto& slot : retired)
        {
            VoiceData* expected = nullptr;
            if (slot.compare_exchange_strong (expected, v))
                return;
        }
        // All slots full: keep it fading and try again next block.
        for (auto& f : fading)
            if (f == nullptr) { f = v; return; }
    }

    void releaseFinishedVoices() noexcept
    {
        for (auto& f : fading)
        {
            if (f == nullptr)
                continue;
            bool inUse = false;
            for (const auto& n : notes)
                inUse = inUse || (n.active && n.data == f);
            if (! inUse)
            {
                VoiceData* v = f;
                f = nullptr;
                retire (v);
            }
        }
    }

    // ---- notes -----------------------------------------------------------------------
    void quickRelease (Note& n) noexcept
    {
        n.stage = Note::Stage::quick;
        n.releaseMul = static_cast<float> (std::pow (1.0e-4, 1.0 / (0.015 * fsInt)));
    }

    void startRelease (Note& n) noexcept
    {
        if (n.stage == Note::Stage::quick)
            return;
        n.stage = Note::Stage::release;
        n.releaseMul = static_cast<float> (std::pow (1.0e-4, 1.0 / (std::max (0.01f, params.release) * fsInt)));
    }

    int soundingNotes() const noexcept
    {
        int count = 0;
        for (const auto& n : notes)
            if (n.active && n.stage != Note::Stage::quick)
                ++count;
        return count;
    }

    void noteOn (int key, float velocity) noexcept
    {
        pickUpVoice();
        if (current == nullptr)
            return;

        // Over the polyphony: fade out the oldest (released notes first).
        while (soundingNotes() >= std::max (1, params.voices))
        {
            Note* oldest = nullptr;
            for (auto& n : notes)
                if (n.active && n.stage != Note::Stage::quick)
                {
                    const bool released = n.stage == Note::Stage::release;
                    const bool oldestReleased = oldest != nullptr && oldest->stage == Note::Stage::release;
                    if (oldest == nullptr || (released && ! oldestReleased) || (released == oldestReleased && n.age < oldest->age))
                        oldest = &n;
                }
            if (oldest == nullptr)
                break;
            quickRelease (*oldest);
        }

        // A free slot, or else the quietest note already fading out.
        Note* slot = nullptr;
        for (auto& n : notes)
            if (! n.active) { slot = &n; break; }
        if (slot == nullptr)
        {
            for (auto& n : notes)
                if (slot == nullptr || n.level < slot->level)
                    slot = &n;
        }

        Note& n = *slot;
        n.active = true;
        n.key = key;
        n.velocity = 0.35f + 0.65f * std::clamp (velocity, 0.0f, 1.0f);
        n.gate = true;
        n.sustained = false;
        n.age = ++ageCounter;
        n.stage = Note::Stage::attack;
        n.attackPos = 0.0f;
        n.level = 0.0f;
        n.data = current;
        n.semis = (params.glideMs > 0.5f && lastSemis >= 0.0f) ? lastSemis : static_cast<float> (key);
        lastSemis = static_cast<float> (key);

        const Loop loop = currentLoop (*current);
        const int dir = loop.mode == LoopMode::backward ? -1 : 1;
        for (int s = 0; s < kStreams; ++s)
        {
            auto& st = n.streams[static_cast<std::size_t> (s)];
            st.layer = s < kCopies ? 0 : (s < kCopies + kLayerCopies ? 1 : 2);
            st.copy = s < kCopies ? s : (s - kCopies) % kLayerCopies;
            st.gain = 0.0f;
            st.granMix = wantsGrains() ? 1.0f : 0.0f;
            // The note itself starts at the loop's start; the copies start at random points
            // in the loop, so they never line up.
            double startPos = dir > 0 ? loop.start : loop.end;
            if (st.copy > 0 || st.layer > 0)
                startPos = loop.start + loop.length() * rng.unit();
            st.player.start (loop, startPos, dir);
            st.cloud.start (loop, startPos, dir, static_cast<std::uint32_t> (n.age * 31u + static_cast<std::uint64_t> (s)));
            st.drift.setPhase (rng.unit() * 7.0f);
            st.drift.setFrequency (0.11f + 0.2f * rng.unit());
            st.started = true;
        }
    }

    void noteOff (int key) noexcept
    {
        for (auto& n : notes)
            if (n.active && n.gate && n.key == key)
            {
                n.gate = false;
                if (pedal) n.sustained = true;
                else startRelease (n);
            }
    }

    void sustain (bool down) noexcept
    {
        pedal = down;
        if (! down)
            for (auto& n : notes)
                if (n.active && n.sustained)
                {
                    n.sustained = false;
                    startRelease (n);
                }
    }

    void allOff() noexcept
    {
        for (auto& n : notes)
            if (n.active)
            {
                n.gate = false;
                n.sustained = false;
                startRelease (n);
            }
    }

    bool wantsGrains() const noexcept { return params.hold || params.stretch > 0.005f; }

    Loop currentLoop (const VoiceData& v) const noexcept
    {
        const double length = v.getLength();
        const double minLength = std::min (length, 0.03 * 2.0 * v.getSourceRate()); // 30 ms
        Loop loop;
        double len = std::max (minLength, static_cast<double> (loopLength.getCurrent()) * length);
        double start = std::clamp (static_cast<double> (loopStart.getCurrent()) * length, 0.0, length);
        if (start + len > length)
            start = std::max (0.0, length - len);
        len = std::min (len, length - start);
        loop.start = start;
        loop.end = start + len;
        const double minFade = std::min (0.005 * 2.0 * v.getSourceRate(), 0.5 * len); // 5 ms
        loop.fade = std::max (minFade, static_cast<double> (params.loopFade) * 0.5 * len);
        loop.mode = params.loopMode;
        return loop;
    }

    // ---- rendering --------------------------------------------------------------------
    void renderNote (Note& n, float* outL, float* outR, int numInt, float wowSemis) noexcept
    {
        const VoiceData& v = *n.data;
        const Loop loop = currentLoop (v);
        const double toRatio = v.getSourceRate() / fs; // level-0 samples per internal sample at the root
        const float stretch = params.hold ? 1.0f : params.stretch;
        const bool grains = wantsGrains();
        const double speed = params.hold ? 0.0 : std::pow (1.0 - static_cast<double> (stretch), 1.5);
        const float grainSamples = static_cast<float> ((0.09 + 0.16 * stretch) * fsInt);
        const double jitter = (0.004 + 0.05 * stretch) * 2.0 * v.getSourceRate();
        const float granStep = static_cast<float> (1.0 / (0.03 * fsInt));
        const float ghostStep = static_cast<float> (1.0 / (0.006 * fsInt));
        const float attackStep = static_cast<float> (1.0 / (std::max (0.001f, params.attack) * fsInt));

        // Glide.
        const float glideSeconds = params.glideMs * 0.001f;
        const float glideCoef = glideSeconds > 0.0005f
            ? 1.0f - std::exp (-static_cast<float> (numInt) / (glideSeconds / 3.0f * static_cast<float> (fsInt)))
            : 1.0f;
        n.semis += glideCoef * (static_cast<float> (n.key) - n.semis);

        // Gains for this control step.
        const float spread = params.spread;
        std::array<float, kStreams> target {};
        float layerNorm[kLayers] {};
        for (int s = 0; s < kStreams; ++s)
        {
            const auto& st = n.streams[static_cast<std::size_t> (s)];
            float g = st.copy == 0 ? 1.0f : std::clamp (spread * 6.0f - static_cast<float> (st.copy - 1), 0.0f, 1.0f);
            if (st.layer == 1 && ! params.octDown) g = 0.0f;
            if (st.layer == 2 && ! params.octUp) g = 0.0f;
            target[static_cast<std::size_t> (s)] = g;
            layerNorm[st.layer] += g * g;
        }
        for (int s = 0; s < kStreams; ++s)
        {
            auto& st = n.streams[static_cast<std::size_t> (s)];
            const float norm = layerNorm[st.layer] > 0.0f ? 1.0f / std::sqrt (layerNorm[st.layer]) : 0.0f;
            target[static_cast<std::size_t> (s)] *= norm * kLayerGain[st.layer];
        }

        const float base = n.semis + bend + wowSemis - v.getRootNote();
        const float invN = 1.0f / static_cast<float> (numInt);

        for (int s = 0; s < kStreams; ++s)
        {
            auto& st = n.streams[static_cast<std::size_t> (s)];
            const float g1 = target[static_cast<std::size_t> (s)];
            const float g0 = st.gain;
            const float drift = st.drift.next();
            if (g0 <= 0.0f && g1 <= 0.0f)
                continue;

            const float driftCents = st.copy == 0 && st.layer == 0 ? 3.0f : 3.0f + 5.0f * spread;
            const float cents = kDetune[static_cast<std::size_t> (st.copy)] * kSpreadCents * spread + driftCents * drift;
            const float semis = base + 12.0f * kLayerOctave[st.layer] + 0.01f * cents;
            st.ratio = toRatio * std::exp2 (static_cast<double> (semis) / 12.0);

            const float pan = kPan[static_cast<std::size_t> (st.copy)] * std::min (1.0f, spread * 1.4f);
            const float angle = (pan + 1.0f) * 0.25f * std::numbers::pi_v<float>;
            st.panL = std::cos (angle) * std::numbers::sqrt2_v<float>;
            st.panR = std::sin (angle) * std::numbers::sqrt2_v<float>;

            // Switching between plain playback and grains crossfades over 30 ms.
            if (grains && st.granMix <= 0.0f)
                st.cloud.start (loop, st.player.position(), st.player.direction(), static_cast<std::uint32_t> (n.age * 131u + static_cast<std::uint64_t> (s)));
            if (! grains && st.granMix >= 1.0f)
                st.player.start (loop, st.cloud.position(), st.cloud.direction());

            const double scanRate = st.ratio * speed;
            float level = n.level;
            float attackPos = n.attackPos;
            auto stage = n.stage;
            float gm = st.granMix;
            for (int i = 0; i < numInt; ++i)
            {
                // Envelope (each stream runs the same sums, so they stay identical).
                if (stage == Note::Stage::attack)
                {
                    attackPos += attackStep;
                    if (attackPos >= 1.0f) { attackPos = 1.0f; stage = Note::Stage::sustain; }
                    level = smoothstep (attackPos);
                }
                else if (stage != Note::Stage::sustain)
                {
                    level *= n.releaseMul;
                }

                gm = grains ? std::min (1.0f, gm + granStep) : std::max (0.0f, gm - granStep);
                float x = 0.0f;
                if (gm < 1.0f)
                    x += (1.0f - gm) * st.player.next (v, loop, st.ratio, ghostStep);
                if (gm > 0.0f)
                    x += gm * st.cloud.process (v, loop, st.ratio, scanRate, grainSamples, jitter);

                const float g = (g0 + (g1 - g0) * (static_cast<float> (i + 1) * invN)) * level * n.velocity;
                outL[i] += x * g * st.panL;
                outR[i] += x * g * st.panR;
            }
            st.granMix = gm;
            st.gain = g1;
        }

        // Advance the envelope once for the note itself.
        for (int i = 0; i < numInt; ++i)
        {
            if (n.stage == Note::Stage::attack)
            {
                n.attackPos += attackStep;
                if (n.attackPos >= 1.0f) { n.attackPos = 1.0f; n.stage = Note::Stage::sustain; }
                n.level = smoothstep (n.attackPos);
            }
            else if (n.stage != Note::Stage::sustain)
            {
                n.level *= n.releaseMul;
            }
        }
        if ((n.stage == Note::Stage::release || n.stage == Note::Stage::quick) && n.level < 1.0e-4f)
            n.active = false;
    }

    void updateShelf (float db) noexcept
    {
        if (std::abs (db - shelfDb) < 0.01f)
            return;
        shelfDb = db;
        shelfL.setHighShelf (5500.0f, db, fs);
        shelfR.setHighShelf (5500.0f, db, fs);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        pickUpVoice();
        releaseFinishedVoices();

        loopStart.setTarget (params.loopStart);
        loopLength.setTarget (params.loopLength);
        formantSmooth.setTarget (params.formant);
        airAmount.setTarget (params.air);
        outGain.setTarget (std::pow (10.0f, params.outputDb / 20.0f));
        bypassGain.setTarget (params.bypass ? 0.0f : 1.0f);
        space.setAmount (params.space);

        int done = 0;
        while (done < numSamples)
        {
            const int n = std::min (kChunk, numSamples - done);
            const int numInt = 2 * n;
            std::fill (intL.begin(), intL.begin() + numInt, 0.0f);
            std::fill (intR.begin(), intR.begin() + numInt, 0.0f);

            const float tapeStep = static_cast<float> (kControl / (0.05 * fsInt));
            for (int c = 0; c < numInt; c += kControl)
            {
                const int len = std::min (kControl, numInt - c);
                loopStart.next();
                loopLength.next();
                tapeMix = params.tape ? std::min (1.0f, tapeMix + tapeStep) : std::max (0.0f, tapeMix - tapeStep);
                const float wowCents = tapeMix * (7.0f * wow.next() + 1.2f * flutter.next());
                for (auto& note : notes)
                    if (note.active)
                        renderNote (note, intL.data() + c, intR.data() + c, len, 0.01f * wowCents);
            }

            // Tape saturation, at the internal rate so it doesn't alias.
            for (int i = 0; i < numInt; ++i)
            {
                float l = intL[static_cast<std::size_t> (i)] * kMasterTrim;
                float r = intR[static_cast<std::size_t> (i)] * kMasterTrim;
                if (tapeMix > 0.0f)
                {
                    constexpr float drive = 1.8f;
                    const float sl = satL.process (l * drive) * (1.0f / drive) * 1.08f;
                    const float sr = satR.process (r * drive) * (1.0f / drive) * 1.08f;
                    l += tapeMix * (sl - l);
                    r += tapeMix * (sr - r);
                }
                intL[static_cast<std::size_t> (i)] = l;
                intR[static_cast<std::size_t> (i)] = r;
            }

            float* outL = left + done;
            float* outR = right + done;
            decL.process (intL.data(), outL, n);
            decR.process (intR.data(), outR, n);

            for (int i = 0; i < n; ++i)
            {
                if ((i & 63) == 0)
                {
                    const float drift = 0.15f * formantDrift.next();
                    formant.setShift (formantSmooth.skip (64) + drift);
                    updateShelf (airAmount.skip (64));
                }
                float l = outL[i], r = outR[i];
                formant.process (l, r);

                // Air: a high shelf plus breath noise that follows the voice.
                const float air = airAmount.getCurrent();
                const float env = breathEnv.process (0.5f * (std::abs (l) + std::abs (r)));
                l = shelfL.process (l);
                r = shelfR.process (r);
                const float breath = 0.22f * (air / 8.0f) * env;
                l += breath * breathLpL.process (breathHpL.process (rng.bipolar()));
                r += breath * breathLpR.process (breathHpR.process (rng.bipolar()));

                space.process (l, r);

                const float g = outGain.next() * bypassGain.next();
                outL[i] = softLimit (l * g, 0.95f);
                outR[i] = softLimit (r * g, 0.95f);
            }
            done += n;
        }

        // Where each note is, for the panel.
        int p = 0;
        for (const auto& note : notes)
        {
            if (! note.active || note.data == nullptr || p >= kMaxPlayheads || note.stage == Note::Stage::quick)
                continue;
            const auto& st = note.streams[0];
            const double pos = st.granMix > 0.5f ? st.cloud.position() : st.player.position();
            playheads[static_cast<std::size_t> (p++)].store (static_cast<float> (pos / note.data->getLength()));
        }
        for (; p < kMaxPlayheads; ++p)
            playheads[static_cast<std::size_t> (p)].store (-1.0f);
    }

    ~Impl()
    {
        delete pending.exchange (nullptr);
        delete current;
        for (auto* f : fading) delete f;
        for (auto& r : retired) delete r.exchange (nullptr);
    }
};

Engine::Engine() : impl (std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void Engine::reset() { impl->reset(); }
int Engine::getLatencySamples() const noexcept { return impl->latency(); }
void Engine::setParams (const EngineParams& p) noexcept { impl->params = p; }
void Engine::noteOn (int note, float velocity) noexcept { impl->noteOn (note, velocity); }
void Engine::noteOff (int note) noexcept { impl->noteOff (note); }
void Engine::sustainPedal (bool down) noexcept { impl->sustain (down); }
void Engine::pitchBend (float semitones) noexcept { impl->bend = semitones; }
void Engine::allNotesOff() noexcept { impl->allOff(); }
void Engine::process (float* left, float* right, int numSamples) noexcept { impl->process (left, right, numSamples); }

void Engine::submitVoice (std::unique_ptr<VoiceData> v)
{
    impl->haveVoice = v != nullptr;
    // A voice the audio thread never picked up can be freed straight away.
    delete impl->pending.exchange (v.release());
    collectGarbage();
}

void Engine::collectGarbage()
{
    for (auto& r : impl->retired)
        delete r.exchange (nullptr);
}

bool Engine::hasVoice() const noexcept { return impl->haveVoice.load(); }

void Engine::getPlayheads (std::array<float, kMaxPlayheads>& out) const noexcept
{
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = impl->playheads[i].load();
}

int Engine::getActiveNotes() const noexcept
{
    int count = 0;
    for (const auto& n : impl->notes)
        count += n.active ? 1 : 0;
    return count;
}

} // namespace sing::dsp
