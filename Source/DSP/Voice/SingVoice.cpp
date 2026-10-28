#include "SingVoice.h"

#include "Voice/VoiceAnalysis.h"

#include <algorithm>
#include <cmath>

namespace sing
{

//==============================================================================
void Envelope::start (float attackSeconds) noexcept
{
    stage = Stage::attack;
    value = 0.0f;
    setTimes (attackSeconds, 1.0f);
}

void Envelope::setTimes (float attackSeconds, float releaseSeconds) noexcept
{
    attackStep = 1.0f / (float) (std::max (0.002f, attackSeconds) * rate);
    if (stage != Stage::release)
        releaseCoeff = (float) std::exp (std::log (0.001) / (std::max (0.005f, releaseSeconds) * rate));
}

void Envelope::release (float releaseSeconds) noexcept
{
    if (stage == Stage::idle || stage == Stage::kill)
        return;
    releaseCoeff = (float) std::exp (std::log (0.001) / (std::max (0.005f, releaseSeconds) * rate));
    stage = Stage::release;
}

void Envelope::kill() noexcept
{
    if (stage == Stage::idle)
        return;
    killStep = std::max (value, 1.0e-3f) / (float) (0.005 * rate);
    stage = Stage::kill;
}

void Envelope::render (float* gains, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        switch (stage)
        {
            case Stage::attack:
                value += attackStep;
                if (value >= 1.0f) { value = 1.0f; stage = Stage::sustain; }
                break;
            case Stage::sustain:
                break;
            case Stage::release:
                value *= releaseCoeff;
                if (value < 1.0e-5f) { value = 0.0f; stage = Stage::idle; }
                break;
            case Stage::kill:
                value -= killStep;
                if (value <= 0.0f) { value = 0.0f; stage = Stage::idle; }
                break;
            case Stage::idle:
                value = 0.0f;
                break;
        }
        gains[i] = value;
    }
}

//==============================================================================
namespace
{
// Copies fan out symmetrically from the centre: 0, +1/3, -1/3, +2/3, -2/3, +1, -1.
constexpr std::array<float, SingVoice::maxCopies> copyPositions { 0.0f, 1.0f / 3.0f, -1.0f / 3.0f,
                                                                   2.0f / 3.0f, -2.0f / 3.0f, 1.0f, -1.0f };
constexpr float maxDetuneCents = 22.0f;
constexpr float copyOffsetSeconds = 0.05f;   // copies start at different places, like different singers
}

SingVoice::CopyShape SingVoice::copyShape (int copy, float spread) noexcept
{
    spread = std::clamp (spread, 0.0f, 1.0f);
    const float p = copyPositions[(size_t) std::clamp (copy, 0, maxCopies - 1)];

    CopyShape s;
    // Copies join one after another as Spread turns up, each fading in over its sixth of
    // the knob. The centre copy is always there.
    s.weight = copy == 0 ? 1.0f : std::clamp (spread * 6.0f - (float) (copy - 1), 0.0f, 1.0f);
    s.cents = p * (8.0f + (maxDetuneCents - 8.0f) * spread);
    s.pan = p * (0.3f + 0.7f * spread);
    return s;
}

void SingVoice::prepare (double sampleRate, int maxBlockSize)
{
    hostRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    envelope.prepare (hostRate);
    for (auto& c : copies)
    {
        c.engine.prepare (hostRate, subBlock);
        c.left.assign ((size_t) subBlock, 0.0f);
        c.right.assign ((size_t) subBlock, 0.0f);
        c.running = false;
    }
    shifter.prepare (hostRate, 0.04f);
    stft.prepare (hostRate, 0.04f);
    bloom.prepare (hostRate);
    ensemble.prepare (hostRate);
    haloEngine.prepare (hostRate, subBlock);
    haloLeft.assign ((size_t) subBlock, 0.0f);
    haloRight.assign ((size_t) subBlock, 0.0f);
    envelopeGains.assign ((size_t) subBlock, 0.0f);
    mixLeft.assign ((size_t) subBlock, 0.0f);
    mixRight.assign ((size_t) subBlock, 0.0f);
    const auto varispeedSize = (size_t) (std::ceil ((float) subBlock * maxVarispeed) + 8.0f);
    varispeedLeft.assign (varispeedSize, 0.0f);
    varispeedRight.assign (varispeedSize, 0.0f);
    (void) maxBlockSize;
    stop();
}

float SingVoice::targetSemitones() const noexcept
{
    return (float) note - hzToMidi (voiceSettings.rootHz) + voiceSettings.pitchBendSemitones;
}

float SingVoice::bloomAnchorSemitones() const noexcept
{
    return (float) note - hzToMidi (voiceSettings.rootHz);
}

float SingVoice::bloomVarispeed() const noexcept
{
    return std::clamp (std::exp2 ((semitones - bloomAnchorSemitones()) / 12.0f), 1.0f / maxVarispeed, maxVarispeed);
}

float SingVoice::pitchRatio (int copy) const noexcept
{
    const auto shape = copyShape (copy, voiceSettings.spread);
    return std::exp2 ((semitones + shape.cents * 0.01f) / 12.0f);
}

float SingVoice::grainRatio (int copy) const noexcept
{
    if (! stftNote)
        return pitchRatio (copy);
    return std::exp2 (copyShape (copy, voiceSettings.spread).cents / 1200.0f);
}

float SingVoice::formantRatioNow() const noexcept
{
    // Playing away from the root resamples the voice, which drags its formants along with the
    // pitch. Undo that, so Formant 0 keeps the recorded vowel on every key, then add the
    // Formant setting. The shifter covers an octave either way; beyond that the vowel starts
    // to follow the pitch.
    if (! voiceSettings.keepVowel)
        return std::exp2 (voiceSettings.formantSemitones / 12.0f);
    return std::exp2 ((voiceSettings.formantSemitones - semitones) / 12.0f);
}

SingVoice::StftShift SingVoice::stftShift() const noexcept
{
    // Keep vowel on: the envelope stays where it was recorded, moved only by Formant. Off:
    // the envelope moves with the key, like resampling, plus Formant.
    StftShift s;
    s.factor = std::exp2 (semitones / 12.0f);
    if (voiceSettings.keepVowel)
    {
        s.envelope = true;
        s.envelopeRatio = std::exp2 (voiceSettings.formantSemitones / 12.0f);
    }
    else
    {
        s.envelope = std::abs (voiceSettings.formantSemitones) > 0.01f;
        s.envelopeRatio = std::exp2 ((voiceSettings.formantSemitones + semitones) / 12.0f);
    }
    return s;
}

float SingVoice::periodFor (const GranularEngine& engine, float ratio) const noexcept
{
    // The note actually sounding where the copy reads, which for a voice that moves between
    // notes is not the root. Using the root there would lay a second, wrong set of harmonics
    // over the sound.
    const float voicePeriod = engine.currentPeriod();
    if (voicePeriod < 2.0f || source == nullptr)
        return 0.0f;
    const double hz = source->sampleRate() / (double) voicePeriod;
    return (float) (hostRate / (hz * (double) ratio));
}

void SingVoice::start (const LoopSource& src, int midiNote, float velocity, const VoiceSettings& settings,
                       bool glide, float glideFromSemitones, uint32_t seed) noexcept
{
    stop();
    source = &src;
    baseSettings = voiceSettings = settings;
    note = midiNote;
    velocityGain = 0.35f + 0.65f * std::clamp (velocity, 0.0f, 1.0f);
    seedBase = seed != 0 ? seed : 1u;

    // This note's own path through the movement, used when Each note is on.
    moveSeed = movement::hash (seedBase ^ 0xa5a5f00du);
    movePhase = (double) movement::hash (moveSeed) / 4294967296.0;
    wanderApplied = 0.0;

    const float target = targetSemitones();
    gliding = glide && settings.glideSeconds > 0.0f && std::abs (glideFromSemitones - target) > 1.0e-3f;
    semitones = gliding ? glideFromSemitones : target;
    glideRate = gliding ? std::abs (target - glideFromSemitones) / (float) (settings.glideSeconds * hostRate) : 0.0f;

    active = true;
    killed = false;
    released = false;
    envelope.start (settings.attackSeconds);
    updateMovement (0, true);
    ensemble.start (seedBase ^ 0x5bd1e995u);
    ensemble.set (settings.ensembleSingers, settings.ensembleDetune, settings.ensembleVibrato, settings.ensembleWidth);
    haloRunning = false;
    haloGain = 0.0f;
    haloAge = 0.0;

    stftNote = settings.pitchMode == PitchMode::stft;
    bloomNote = settings.mode == StretchMode::bloom && BloomVoice::canPlay (src);
    // One silent sample before the first, for the interpolation to read.
    std::fill (varispeedLeft.begin(), varispeedLeft.end(), 0.0f);
    std::fill (varispeedRight.begin(), varispeedRight.end(), 0.0f);
    varispeedFill = 1;
    varispeedRead = 1.0;
    varispeedRate = std::exp2 ((semitones - bloomAnchorSemitones()) / 12.0f);
    varispeedRate = std::clamp (varispeedRate, 1.0f / maxVarispeed, maxVarispeed);
    if (bloomNote)
    {
        std::array<CopyShape, (size_t) maxCopies> shapes {};
        const float norm = copyShapes (voiceSettings.spread, shapes);
        bloom.setParameters (bloomParameters (shapes, norm));
        // Every Bloom note buffer in use (only with very long windows): play granular.
        bloomNote = bloom.start (src, 0.0, seedBase * 2654435761u, (float) (seedBase % 97u) / 97.0f);
    }
    if (bloomNote)
    {
        applyWander (true);
        if (stftNote)
            primeBloomShifter();
        else if (settings.haloLevel > 0.0f)
            shifter.start (0.0f, formantRatioNow());   // the halo's own; its period follows once it runs
        return;
    }

    for (int i = 0; i < maxCopies; ++i)
        if (copyShape (i, voiceSettings.spread).weight > 0.0f)
            startCopy (i);
    applyWander (true);
    primeShifter();
}

float SingVoice::haloRatio() const noexcept
{
    // The halo plays at the note's pitch (Spread's centre copy), an octave either way if asked.
    // In STFT mode the grains play at the recorded pitch and the shifter moves the mix.
    const float octave = std::exp2 ((float) voiceSettings.haloOctave);
    if (stftNote)
        return octave;
    return std::exp2 (semitones / 12.0f) * octave;
}

void SingVoice::startHalo() noexcept
{
    if (source == nullptr)
        return;
    haloEngine.setSeed (seedBase * 2246822519u + 7u);
    haloEngine.setSettings (StretchKnob::toGranularSettings (1.0f));
    haloEngine.setParameters ({ 0.0f, haloRatio(), true });
    // The moment the note began: where its centre copy started, with Wander's offset.
    haloEngine.start (*source, std::fmod (std::max (0.0, wanderApplied), (double) std::max (1, source->size())));
    haloRunning = true;
    haloGain = 0.0f;
    haloAge = 0.0;
}

void SingVoice::renderHalo (int n, bool preShifter) noexcept
{
    // A frozen engine plays quieter than a moving one, so the halo is given more than unity.
    const float targetGain = std::clamp (voiceSettings.haloLevel, 0.0f, 1.0f) * 1.4f;
    if (! haloRunning)
    {
        if (targetGain <= 0.0f)
            return;
        startHalo();
    }
    if (targetGain <= 0.0f && haloGain <= 0.0f)
    {
        haloEngine.stop();
        haloRunning = false;
        return;
    }

    // Drift: the held moment wanders back and forth through the voice, a slow sway whose
    // reach grows with the setting (up to about a second either way).
    const double t = haloAge;
    haloAge += (double) n / hostRate;
    const float drift = std::clamp (voiceSettings.haloDrift, 0.0f, 1.0f);
    if (drift > 0.0f && source != nullptr)
    {
        const double sway = std::sin (6.2831853 * 0.07 * t + (double) (seedBase % 1000u) * 0.0063);
        haloEngine.nudge ((double) drift * 0.45 * sway * (double) n * source->sampleRate() / hostRate);
    }

    haloEngine.setParameters ({ 0.0f, haloRatio(), true });
    std::fill_n (haloLeft.data(), n, 0.0f);
    std::fill_n (haloRight.data(), n, 0.0f);
    haloEngine.process (haloLeft.data(), haloRight.data(), n);

    if (! preShifter)
    {
        // Bloom, formant mode: keep the halo's vowel as the note keeps its own.
        shifter.setPeriod (periodFor (haloEngine, haloRatio()));
        shifter.setRatio (formantRatioNow() / std::exp2 ((float) voiceSettings.haloOctave));
        shifter.process (haloLeft.data(), haloRight.data(), haloLeft.data(), haloRight.data(), n);
    }

    const float g0 = haloGain, dg = (targetGain - g0) / (float) n;
    for (int s = 0; s < n; ++s)
    {
        const float g = g0 + dg * (float) (s + 1);
        mixLeft[(size_t) s] += haloLeft[(size_t) s] * g;
        mixRight[(size_t) s] += haloRight[(size_t) s] * g;
    }
    haloGain = targetGain;
}

void SingVoice::finishSubBlock (float* left, float* right, int n) noexcept
{
    ensemble.set (voiceSettings.ensembleSingers, voiceSettings.ensembleDetune, voiceSettings.ensembleVibrato, voiceSettings.ensembleWidth);
    ensemble.process (mixLeft.data(), mixRight.data(), n);
    applyEnvelope (left, right, n);
}

void SingVoice::setSettings (const VoiceSettings& settings) noexcept
{
    baseSettings = settings;
    refreshMoved();
}

void SingVoice::refreshMoved() noexcept
{
    voiceSettings = baseSettings;
    const auto& o = moveOffset;
    voiceSettings.stretch = std::clamp (baseSettings.stretch + o[moveStretch], 0.0f, 1.0f);
    voiceSettings.spread = std::clamp (baseSettings.spread + o[moveSpread], 0.0f, 1.0f);
    voiceSettings.formantSemitones = std::clamp (baseSettings.formantSemitones + 24.0f * o[moveFormant], -12.0f, 12.0f);
}

void SingVoice::updateMovement (int numSamples, bool jump) noexcept
{
    const auto& m = baseSettings.movement;
    const uint32_t seed = m.eachNote ? moveSeed : movement::sharedSeed;
    const double cycles = moveClock + (m.eachNote ? movePhase : 0.0);
    moveClock += m.cyclesPerSample * (double) numSamples;

    const auto moved = [&] (int target, float base) noexcept
    {
        const float width = m.amounts[(size_t) target];
        if (width <= 0.0f)
            return base;
        return movement::orbit (base, width, movement::value (m.shape, cycles, movement::targetSeed (seed, target)));
    };

    std::array<float, numMoveTargets> target {};
    target[moveStretch] = moved (moveStretch, baseSettings.stretch) - baseSettings.stretch;
    target[moveSpread] = moved (moveSpread, baseSettings.spread) - baseSettings.spread;
    const float formantNorm = (baseSettings.formantSemitones + 12.0f) / 24.0f;
    target[moveFormant] = moved (moveFormant, formantNorm) - formantNorm;
    // Wander: a share of the loop either side of where the note would be, up to half the
    // loop each way at 100%. With no loop (From start) only forward of it, up to the whole
    // voice after Start, so nothing before Start is heard.
    const bool forwardOnly = source != nullptr && ! source->wraps();
    const float wander = m.amounts[moveWander] > 0.0f ? movement::value (m.shape, cycles, movement::targetSeed (seed, moveWander)) : -1.0f;
    target[moveWander] = m.amounts[moveWander] > 0.0f
        ? (forwardOnly ? 0.5f * m.amounts[moveWander] * (wander + 1.0f) : 0.5f * m.amounts[moveWander] * wander)
        : 0.0f;

    // Ease toward the new place over about 30 ms: switching shape, a jump in the song's
    // position or a new orbit never steps.
    const float ease = jump ? 1.0f : 1.0f - std::exp (-(float) numSamples / (0.03f * (float) hostRate));
    for (int t = 0; t < numMoveTargets; ++t)
        moveOffset[(size_t) t] += (target[(size_t) t] - moveOffset[(size_t) t]) * ease;

    refreshMoved();
}

void SingVoice::applyWander (bool jump) noexcept
{
    if (source == nullptr)
        return;
    // Loop samples: the play buffer runs at the voice's rate.
    const double loopLength = (double) std::max (1, std::min (source->settings().length, source->size()));
    const double wanted = (double) moveOffset[moveWander] * loopLength;
    const double delta = wanted - wanderApplied;
    wanderApplied = wanted;
    if (std::abs (delta) < 1.0e-9 && ! jump)
        return;
    if (bloomNote)
        bloom.nudge (delta);
    else
        for (auto& c : copies)
            if (c.running)
                c.engine.nudge (delta);
}

SingVoice::Moved SingVoice::getMoved() const noexcept
{
    Moved m;
    m.stretch = voiceSettings.stretch;
    m.spread = voiceSettings.spread;
    m.formant = (voiceSettings.formantSemitones + 12.0f) / 24.0f;
    m.wander = moveOffset[moveWander];
    return m;
}

void SingVoice::startCopy (int index) noexcept
{
    auto& c = copies[(size_t) index];
    const float ratio = grainRatio (index);

    c.engine.setSeed (seedBase * 2654435761u + (uint32_t) index * 40503u);
    c.engine.setSettings (StretchKnob::toGranularSettings (voiceSettings.stretch));
    c.engine.setParameters ({ StretchKnob::toSpeed (voiceSettings.stretch), ratio, voiceSettings.hold });
    const double offset = (double) index * copyOffsetSeconds * source->sampleRate();
    c.engine.start (*source, std::fmod (offset, (double) source->size()));

    c.running = true;
    c.gain = 0.0f;   // fades in over the first sub-block
}

void SingVoice::primeShifter() noexcept
{
    // The shifter runs latency() samples behind its input. Feed it that much of the note up
    // front (the copies mixed, as renderSubBlock does at the start), so its output starts
    // exactly when the note does.
    int prime = 0;
    if (stftNote)
    {
        const auto shift = stftShift();
        prime = stft.start (periodFor (copies[0].engine, grainRatio (0)), shift.factor, shift.envelope, shift.envelopeRatio);
    }
    else
        prime = shifter.start (periodFor (copies[0].engine, pitchRatio (0)), formantRatioNow());
    std::array<CopyShape, (size_t) maxCopies> shapes {};
    const float norm = copyShapes (voiceSettings.spread, shapes);
    constexpr float halfPi = 1.57079632679f;
    while (prime > 0)
    {
        const int n = std::min (prime, subBlock);
        std::fill_n (mixLeft.data(), n, 0.0f);
        std::fill_n (mixRight.data(), n, 0.0f);
        for (int i = 0; i < maxCopies; ++i)
        {
            auto& c = copies[(size_t) i];
            if (! c.running)
                continue;
            std::fill_n (c.left.data(), n, 0.0f);
            std::fill_n (c.right.data(), n, 0.0f);
            c.engine.process (c.left.data(), c.right.data(), n);
            const auto& shape = shapes[(size_t) i];
            const float g = shape.weight * norm;
            const float theta = (shape.pan + 1.0f) * 0.5f * halfPi;
            const float panL = std::cos (theta) * 1.41421356f * g, panR = std::sin (theta) * 1.41421356f * g;
            for (int s = 0; s < n; ++s)
            {
                mixLeft[(size_t) s] += c.left[(size_t) s] * panL;
                mixRight[(size_t) s] += c.right[(size_t) s] * panR;
            }
            c.gain = g;
        }
        if (stftNote)
            stft.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
        else
            shifter.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
        prime -= n;
    }
}

float SingVoice::rootPeriod() const noexcept
{
    // Bloom notes in STFT mode play at the root: the voice's own period, in host samples.
    if (source == nullptr || source->settings().pitchPeriod < 2.0f)
        return 0.0f;
    return (float) (source->settings().pitchPeriod * hostRate / source->sampleRate());
}

void SingVoice::primeBloomShifter() noexcept
{
    // As primeShifter: Bloom renders the note's first latency() samples up front, so the
    // shifted note starts when the note does.
    const auto shift = stftShift();
    int prime = stft.start (rootPeriod(), shift.factor, shift.envelope, shift.envelopeRatio);
    while (prime > 0)
    {
        const int n = std::min (prime, subBlock);
        std::fill_n (mixLeft.data(), n, 0.0f);
        std::fill_n (mixRight.data(), n, 0.0f);
        bloom.render (mixLeft.data(), mixRight.data(), n);
        stft.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
        prime -= n;
    }
}

void SingVoice::release() noexcept
{
    if (! active || released)
        return;
    released = true;
    envelope.release (voiceSettings.releaseSeconds);
}

void SingVoice::kill() noexcept
{
    if (! active)
        return;
    killed = true;
    released = true;
    envelope.kill();
}

void SingVoice::stop() noexcept
{
    active = false;
    killed = false;
    released = false;
    for (auto& c : copies)
    {
        c.engine.stop();
        c.running = false;
        c.gain = 0.0f;
    }
    bloom.stop();
    bloomNote = false;
    stftNote = false;
    haloEngine.stop();
    haloRunning = false;
    source = nullptr;
}

void SingVoice::setSource (const LoopSource& src) noexcept
{
    if (! active)
        return;
    if (bloomNote)
    {
        // A Bloom note keeps its old loop if the new one has no Bloom analysis (the engine
        // was switched while it played); isUsing keeps that loop alive until the note ends.
        bloom.setSource (src);
        if (haloRunning)
            haloEngine.setSource (src);
        return;
    }
    source = &src;
    for (auto& c : copies)
        if (c.running)
            c.engine.setSource (src);
    if (haloRunning)
        haloEngine.setSource (src);
}

bool SingVoice::isUsing (const LoopSource& src) const noexcept
{
    if (! active)
        return false;
    if (haloRunning && haloEngine.isUsing (src))
        return true;
    if (bloomNote)
        return bloom.isUsing (src);
    for (const auto& c : copies)
        if (c.running && c.engine.isUsing (src))
            return true;
    return false;
}

double SingVoice::playPosition() const noexcept
{
    if (bloomNote)
        return bloom.playPosition();
    return copies[0].running ? copies[0].engine.playPosition() : 0.0;
}

int SingVoice::getCopyLights (CopyLight* lights, int maxCount) const noexcept
{
    if (! active)
        return 0;

    int n = 0;
    if (bloomNote)
    {
        // Bloom's copies read the same window, each a little further on.
        const auto zone = (float) bloom.windowSamples();
        for (int i = 0; i < maxCopies && n < maxCount; ++i)
        {
            const auto shape = copyShape (i, voiceSettings.spread);
            if (shape.weight > 0.0f)
                lights[n++] = { bloom.playPosition ((double) i * copyOffsetSeconds), shape.weight, shape.pan, pitchRatio (i), zone };
        }
        return n;
    }

    for (int i = 0; i < maxCopies && n < maxCount; ++i)
    {
        const auto& c = copies[(size_t) i];
        const auto shape = copyShape (i, voiceSettings.spread);
        if (! c.running || shape.weight <= 0.0f)
            continue;
        lights[n++] = { c.engine.playPosition(), shape.weight, shape.pan, pitchRatio (i), 0.0f };
    }
    return n;
}

void SingVoice::render (float* left, float* right, int numSamples) noexcept
{
    int done = 0;
    while (active && done < numSamples)
    {
        const int n = std::min (subBlock, numSamples - done);
        renderSubBlock (left + done, right + done, n);
        done += n;
    }
}

void SingVoice::renderSubBlock (float* left, float* right, int n) noexcept
{
    updateMovement (n, false);
    applyWander (false);

    // Pitch: glide from the previous note in constant time, then follow bends directly.
    const float target = targetSemitones();
    if (gliding)
    {
        const float step = glideRate * (float) n;
        if (std::abs (target - semitones) <= step)
        {
            semitones = target;
            gliding = false;
        }
        else
            semitones += target > semitones ? step : -step;
    }
    else
        semitones = target;

    envelope.setTimes (voiceSettings.attackSeconds, voiceSettings.releaseSeconds);

    // Spread: work out each copy's share, normalised so the copies add up to the same power.
    std::array<CopyShape, (size_t) maxCopies> shapes {};
    const float norm = copyShapes (voiceSettings.spread, shapes);

    std::fill_n (mixLeft.data(), n, 0.0f);
    std::fill_n (mixRight.data(), n, 0.0f);

    if (bloomNote)
    {
        bloom.setParameters (bloomParameters (shapes, norm));
        if (stftNote)
            bloom.render (mixLeft.data(), mixRight.data(), n);
        else
            renderBloomVarispeed (n);
        if (stftNote)
        {
            renderHalo (n, true);
            const auto shift = stftShift();
            stft.setShift (shift.factor, shift.envelope, shift.envelopeRatio);
            stft.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
        }
        else
            renderHalo (n, false);
        finishSubBlock (left, right, n);
        return;
    }

    const auto granular = StretchKnob::toGranularSettings (voiceSettings.stretch);
    const float speed = StretchKnob::toSpeed (voiceSettings.stretch);
    const float formantRatio = formantRatioNow();
    constexpr float halfPi = 1.57079632679f;

    for (int i = 0; i < maxCopies; ++i)
    {
        auto& c = copies[(size_t) i];
        const auto& shape = shapes[(size_t) i];
        const float targetGain = shape.weight * norm;

        if (! c.running)
        {
            if (targetGain <= 0.0f)
                continue;
            startCopy (i);
        }

        const float ratio = grainRatio (i);
        c.engine.setSettings (granular);
        c.engine.setParameters ({ speed, ratio, voiceSettings.hold });

        std::fill_n (c.left.data(), n, 0.0f);
        std::fill_n (c.right.data(), n, 0.0f);
        c.engine.process (c.left.data(), c.right.data(), n);

        // Equal-power pan, unity in the centre. The copy's level ramps over the sub-block.
        const float theta = (shape.pan + 1.0f) * 0.5f * halfPi;
        const float panL = std::cos (theta) * 1.41421356f;
        const float panR = std::sin (theta) * 1.41421356f;
        const float g0 = c.gain, g1 = targetGain;
        const float dg = (g1 - g0) / (float) n;
        for (int s = 0; s < n; ++s)
        {
            const float g = g0 + dg * (float) (s + 1);
            mixLeft[(size_t) s] += c.left[(size_t) s] * g * panL;
            mixRight[(size_t) s] += c.right[(size_t) s] * g * panR;
        }
        c.gain = g1;

        if (targetGain <= 0.0f && i > 0)
        {
            c.engine.stop();
            c.running = false;
        }
    }

    renderHalo (n, true);

    if (stftNote)
    {
        const auto shift = stftShift();
        stft.setPeriod (periodFor (copies[0].engine, grainRatio (0)));
        stft.setShift (shift.factor, shift.envelope, shift.envelopeRatio);
        stft.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
    }
    else
    {
        shifter.setPeriod (periodFor (copies[0].engine, pitchRatio (0)));
        shifter.setRatio (formantRatio);
        shifter.process (mixLeft.data(), mixRight.data(), mixLeft.data(), mixRight.data(), n);
    }

    finishSubBlock (left, right, n);
}

void SingVoice::renderBloomVarispeed (int n) noexcept
{
    // The speed moves smoothly across the sub-block, from where the last one ended.
    const float from = varispeedRate, to = bloomVarispeed();
    varispeedRate = to;
    const double last = varispeedRead + 0.5 * (double) (from + to) * (double) n;
    // The interpolation reads up to two samples past the point.
    const int need = std::min ((int) varispeedLeft.size(), (int) last + 3);
    if (need > varispeedFill)
    {
        const int len = need - varispeedFill;
        std::fill_n (varispeedLeft.data() + varispeedFill, len, 0.0f);
        std::fill_n (varispeedRight.data() + varispeedFill, len, 0.0f);
        bloom.render (varispeedLeft.data() + varispeedFill, varispeedRight.data() + varispeedFill, len);
        varispeedFill = need;
    }

    // Four-point Hermite, as the grains read.
    auto read = [] (const float* x, int i, float f)
    {
        const float c0 = x[i];
        const float c1 = 0.5f * (x[i + 1] - x[i - 1]);
        const float c2 = x[i - 1] - 2.5f * x[i] + 2.0f * x[i + 1] - 0.5f * x[i + 2];
        const float c3 = 0.5f * (x[i + 2] - x[i - 1]) + 1.5f * (x[i] - x[i + 1]);
        return ((c3 * f + c2) * f + c1) * f + c0;
    };
    double pos = varispeedRead;
    const float dr = (to - from) / (float) n;
    for (int s = 0; s < n; ++s)
    {
        const int i = std::clamp ((int) pos, 1, varispeedFill - 3);
        const float f = (float) std::clamp (pos - (double) i, 0.0, 1.0);
        mixLeft[(size_t) s] = read (varispeedLeft.data(), i, f);
        mixRight[(size_t) s] = read (varispeedRight.data(), i, f);
        pos += (double) (from + dr * ((float) s + 0.5f));
    }

    // Keep from one sample before the reading point on.
    const int keepFrom = std::clamp ((int) pos - 1, 0, varispeedFill);
    std::copy (varispeedLeft.begin() + keepFrom, varispeedLeft.begin() + varispeedFill, varispeedLeft.begin());
    std::copy (varispeedRight.begin() + keepFrom, varispeedRight.begin() + varispeedFill, varispeedRight.begin());
    varispeedFill -= keepFrom;
    varispeedRead = pos - (double) keepFrom;
}

void SingVoice::applyEnvelope (float* left, float* right, int n) noexcept
{
    envelope.render (envelopeGains.data(), n);
    for (int s = 0; s < n; ++s)
    {
        const float g = envelopeGains[(size_t) s] * velocityGain;
        left[s] += mixLeft[(size_t) s] * g;
        right[s] += mixRight[(size_t) s] * g;
    }

    if (envelope.isIdle())
        stop();
}

float SingVoice::copyShapes (float spread, std::array<CopyShape, (size_t) maxCopies>& shapes) noexcept
{
    float power = 0.0f;
    for (int i = 0; i < maxCopies; ++i)
    {
        shapes[(size_t) i] = copyShape (i, spread);
        power += shapes[(size_t) i].weight * shapes[(size_t) i].weight;
    }
    return 1.0f / std::sqrt (std::max (power, 1.0e-6f));
}

BloomVoice::Parameters SingVoice::bloomParameters (const std::array<CopyShape, (size_t) maxCopies>& shapes, float norm) const noexcept
{
    constexpr float halfPi = 1.57079632679f;
    BloomVoice::Parameters p;
    p.speed = StretchKnob::toSpeed (voiceSettings.stretch);
    p.hold = voiceSettings.hold;
    // Bloom moves the envelope separately from the pitch, so Formant 0 needs no
    // compensation to keep the recorded vowel on every key.
    // With Keep vowel off, the envelope moves with the key, as plain resampling would.
    // The varispeed after Bloom (glide, bend) moves the envelope too, so take that back out.
    p.formantRatio = std::exp2 ((voiceSettings.formantSemitones + (voiceSettings.keepVowel ? 0.0f : semitones)) / 12.0f)
                   / bloomVarispeed();
    // In STFT mode Bloom plays at the root (Spread's detune aside) and the STFT shifter moves
    // it to the key and sets the vowel, as it does for Grain.
    if (stftNote)
        p.formantRatio = 1.0f;
    // Some width even at Spread 0, but little enough that the note holds up in mono.
    p.width = 0.3f + 0.7f * voiceSettings.spread;
    p.shiftHz = voiceSettings.shiftHz;
    p.blur = voiceSettings.smear;
    p.tone = voiceSettings.tone;
    p.onsets = voiceSettings.onsets;
    p.rootHz = voiceSettings.rootHz;
    p.harmonicsCents = voiceSettings.harmonicsCents;
    p.harmonicsCount = voiceSettings.harmonicsCount;
    p.filter = voiceSettings.filter;
    p.lowHz = voiceSettings.lowHz;
    p.highHz = voiceSettings.highHz;
    p.filterSlope = voiceSettings.filterSlope;
    p.compressor = voiceSettings.compressor;
    p.octaves = voiceSettings.octaves;
    p.octaveLevels = voiceSettings.octaveLevels;
    p.numLayers = 0;
    for (int i = 0; i < maxCopies; ++i)
    {
        const auto& shape = shapes[(size_t) i];
        if (shape.weight <= 0.0f)
            continue;
        const float theta = (shape.pan + 1.0f) * 0.5f * halfPi;
        auto& layer = p.layers[(size_t) p.numLayers++];
        layer.pitchRatio = stftNote ? grainRatio (i) : std::exp2 ((bloomAnchorSemitones() + shape.cents * 0.01f) / 12.0f);
        layer.gainLeft = shape.weight * norm * std::cos (theta) * 1.41421356f;
        layer.gainRight = shape.weight * norm * std::sin (theta) * 1.41421356f;
        layer.offsetSeconds = (double) i * copyOffsetSeconds;
    }
    return p;
}

} // namespace sing
