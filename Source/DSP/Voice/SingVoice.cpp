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
        c.shifter.prepare (hostRate, 0.04f);
        c.left.assign ((size_t) subBlock, 0.0f);
        c.right.assign ((size_t) subBlock, 0.0f);
        c.running = false;
    }
    bloom.prepare (hostRate);
    envelopeGains.assign ((size_t) subBlock, 0.0f);
    mixLeft.assign ((size_t) subBlock, 0.0f);
    mixRight.assign ((size_t) subBlock, 0.0f);
    (void) maxBlockSize;
    stop();
}

float SingVoice::targetSemitones() const noexcept
{
    return (float) note - hzToMidi (voiceSettings.rootHz) + voiceSettings.pitchBendSemitones;
}

float SingVoice::pitchRatio (int copy) const noexcept
{
    const auto shape = copyShape (copy, voiceSettings.spread);
    return std::exp2 ((semitones + shape.cents * 0.01f) / 12.0f);
}

float SingVoice::formantRatioNow() const noexcept
{
    // Playing away from the root resamples the voice, which drags its formants along with the
    // pitch. Undo that, so Formant 0 keeps the recorded vowel on every key, then add the
    // Formant setting. The shifter covers an octave either way; beyond that the vowel starts
    // to follow the pitch.
    return std::exp2 ((voiceSettings.formantSemitones - semitones) / 12.0f);
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
    voiceSettings = settings;
    note = midiNote;
    velocityGain = 0.35f + 0.65f * std::clamp (velocity, 0.0f, 1.0f);
    seedBase = seed != 0 ? seed : 1u;

    const float target = targetSemitones();
    gliding = glide && settings.glideSeconds > 0.0f && std::abs (glideFromSemitones - target) > 1.0e-3f;
    semitones = gliding ? glideFromSemitones : target;
    glideRate = gliding ? std::abs (target - glideFromSemitones) / (float) (settings.glideSeconds * hostRate) : 0.0f;

    active = true;
    killed = false;
    released = false;
    envelope.start (settings.attackSeconds);

    bloomNote = settings.mode == StretchMode::bloom && BloomVoice::canPlay (src);
    if (bloomNote)
    {
        std::array<CopyShape, (size_t) maxCopies> shapes {};
        const float norm = copyShapes (settings.spread, shapes);
        bloom.setParameters (bloomParameters (shapes, norm));
        bloom.start (src, 0.0, seedBase * 2654435761u, (float) (seedBase % 97u) / 97.0f);
        return;
    }

    for (int i = 0; i < maxCopies; ++i)
        if (copyShape (i, settings.spread).weight > 0.0f)
            startCopy (i);
}

void SingVoice::startCopy (int index) noexcept
{
    auto& c = copies[(size_t) index];
    const float ratio = pitchRatio (index);

    c.engine.setSeed (seedBase * 2654435761u + (uint32_t) index * 40503u);
    c.engine.setSettings (StretchKnob::toGranularSettings (voiceSettings.stretch));
    c.engine.setParameters ({ StretchKnob::toSpeed (voiceSettings.stretch), ratio, voiceSettings.hold });
    const double offset = (double) index * copyOffsetSeconds * source->sampleRate();
    c.engine.start (*source, std::fmod (offset, (double) source->size()));

    const float formantRatio = formantRatioNow();
    int prime = c.shifter.start (periodFor (c.engine, ratio), formantRatio);

    // The shifter runs latency() samples behind its input. Feed it that much of the note up
    // front, so its output starts exactly when the note does.
    while (prime > 0)
    {
        const int n = std::min (prime, subBlock);
        std::fill_n (c.left.data(), n, 0.0f);
        std::fill_n (c.right.data(), n, 0.0f);
        c.engine.process (c.left.data(), c.right.data(), n);
        c.shifter.process (c.left.data(), c.right.data(), c.left.data(), c.right.data(), n);
        prime -= n;
    }

    c.running = true;
    c.gain = 0.0f;   // fades in over the first sub-block
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
        return;
    }
    source = &src;
    for (auto& c : copies)
        if (c.running)
            c.engine.setSource (src);
}

bool SingVoice::isUsing (const LoopSource& src) const noexcept
{
    if (! active)
        return false;
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
        bloom.render (mixLeft.data(), mixRight.data(), n);
        applyEnvelope (left, right, n);
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

        const float ratio = pitchRatio (i);
        c.engine.setSettings (granular);
        c.engine.setParameters ({ speed, ratio, voiceSettings.hold });
        c.shifter.setPeriod (periodFor (c.engine, ratio));
        c.shifter.setRatio (formantRatio);

        std::fill_n (c.left.data(), n, 0.0f);
        std::fill_n (c.right.data(), n, 0.0f);
        c.engine.process (c.left.data(), c.right.data(), n);
        c.shifter.process (c.left.data(), c.right.data(), c.left.data(), c.right.data(), n);

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

    applyEnvelope (left, right, n);
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
    p.formantRatio = std::exp2 (voiceSettings.formantSemitones / 12.0f);
    // Some width even at Spread 0, but little enough that the note holds up in mono.
    p.width = 0.3f + 0.7f * voiceSettings.spread;
    p.numLayers = 0;
    for (int i = 0; i < maxCopies; ++i)
    {
        const auto& shape = shapes[(size_t) i];
        if (shape.weight <= 0.0f)
            continue;
        const float theta = (shape.pan + 1.0f) * 0.5f * halfPi;
        auto& layer = p.layers[(size_t) p.numLayers++];
        layer.pitchRatio = pitchRatio (i);
        layer.gainLeft = shape.weight * norm * std::cos (theta) * 1.41421356f;
        layer.gainRight = shape.weight * norm * std::sin (theta) * 1.41421356f;
        layer.offsetSeconds = (double) i * copyOffsetSeconds;
    }
    return p;
}

} // namespace sing
