#include "SingSynth.h"

#include "Voice/VoiceAnalysis.h"

#include <algorithm>

namespace sing
{

void SingSynth::prepare (double sampleRate, int maxBlockSize)
{
    for (auto& v : voices)
        v.prepare (sampleRate, maxBlockSize);
    sustained.fill (false);
    sustainDown = false;
}

void SingSynth::setSource (const LoopSource* newSource) noexcept
{
    if (newSource == source || newSource == nullptr)
        return;

    if (source != nullptr)
    {
        if (numOld < maxOldSources)
            oldSources[(size_t) numOld++] = source;
        else
            allSoundOff();   // far too many loop edits in flight: cut notes rather than read freed audio
    }

    source = newSource;
    for (auto& v : voices)
        v.setSource (*source);
}

void SingSynth::setSettings (const SynthSettings& s) noexcept
{
    // The pitch bend comes from MIDI (setPitchBend), not from the settings.
    const float bend = settings.voice.pitchBendSemitones;
    settings = s;
    settings.voice.pitchBendSemitones = bend;
    settings.polyphony = std::clamp (settings.polyphony, 1, maxPolyphony);
    for (auto& v : voices)
        if (v.isActive())
            v.setSettings (settings.voice);
}

SingVoice* SingSynth::findVoiceToStart() noexcept
{
    // Make room under the Voices setting: steal a released note (the quietest), else the oldest.
    int sounding = 0;
    for (const auto& v : voices)
        if (v.isActive() && ! v.isKilled())
            ++sounding;

    while (sounding >= settings.polyphony)
    {
        SingVoice* victim = nullptr;
        for (auto& v : voices)
        {
            if (! v.isActive() || v.isKilled())
                continue;
            if (victim == nullptr)
                victim = &v;
            else if (v.isReleased() != victim->isReleased())
            {
                if (v.isReleased())
                    victim = &v;
            }
            else if (v.isReleased() ? v.getLevel() < victim->getLevel() : v.getStartOrder() < victim->getStartOrder())
                victim = &v;
        }
        if (victim == nullptr)
            break;
        victim->kill();
        --sounding;
    }

    for (auto& v : voices)
        if (! v.isActive())
            return &v;

    // Every slot is busy fading out: reuse the quietest.
    SingVoice* quietest = &voices[0];
    for (auto& v : voices)
        if (v.getLevel() < quietest->getLevel())
            quietest = &v;
    quietest->stop();
    return quietest;
}

void SingSynth::noteOn (int note, float velocity) noexcept
{
    if (source == nullptr)
        return;
    if (velocity <= 0.0f)
    {
        noteOff (note);
        return;
    }

    // A key played again while still held (or sustained) starts a fresh note.
    for (int i = 0; i < voicePool; ++i)
        if (voices[(size_t) i].isActive() && voices[(size_t) i].getNote() == note && ! voices[(size_t) i].isReleased())
        {
            voices[(size_t) i].release();
            sustained[(size_t) i] = false;
        }

    auto* v = findVoiceToStart();
    const auto index = (size_t) (v - voices.data());
    const float semitones = (float) note - hzToMidi (settings.voice.rootHz);

    v->start (*source, note, velocity, settings.voice, hasLastNote,
              lastNoteSemitones + settings.voice.pitchBendSemitones, nextSeed);
    v->setStartOrder (nextOrder++);
    sustained[index] = false;

    nextSeed = nextSeed * 1664525u + 1013904223u;
    lastNoteSemitones = semitones;
    hasLastNote = true;
}

void SingSynth::noteOff (int note) noexcept
{
    for (int i = 0; i < voicePool; ++i)
    {
        auto& v = voices[(size_t) i];
        if (! v.isActive() || v.getNote() != note || v.isReleased())
            continue;
        if (sustainDown)
            sustained[(size_t) i] = true;
        else
            v.release();
    }
}

void SingSynth::setSustain (bool down) noexcept
{
    sustainDown = down;
    if (down)
        return;
    for (int i = 0; i < voicePool; ++i)
        if (sustained[(size_t) i])
        {
            voices[(size_t) i].release();
            sustained[(size_t) i] = false;
        }
}

void SingSynth::setPitchBend (float semitonesBend) noexcept
{
    settings.voice.pitchBendSemitones = semitonesBend;
    for (auto& v : voices)
        if (v.isActive())
            v.setSettings (settings.voice);
}

void SingSynth::allNotesOff() noexcept
{
    sustainDown = false;
    sustained.fill (false);
    for (auto& v : voices)
        v.release();
}

void SingSynth::allSoundOff() noexcept
{
    sustainDown = false;
    sustained.fill (false);
    for (auto& v : voices)
        v.stop();
}

void SingSynth::render (float* left, float* right, int numSamples) noexcept
{
    for (auto& v : voices)
        if (v.isActive())
            v.render (left, right, numSamples);
}

int SingSynth::activeVoiceCount() const noexcept
{
    return (int) std::count_if (voices.begin(), voices.end(), [] (const SingVoice& v) { return v.isActive(); });
}

bool SingSynth::isUsing (const LoopSource& s) const noexcept
{
    return std::any_of (voices.begin(), voices.end(), [&s] (const SingVoice& v) { return v.isUsing (s); });
}

int SingSynth::getPlayPositions (double* positions, float* levels, int maxCount) const noexcept
{
    // Newest first.
    std::array<const SingVoice*, (size_t) voicePool> order {};
    int n = 0;
    for (const auto& v : voices)
        if (v.isActive())
            order[(size_t) n++] = &v;
    std::sort (order.begin(), order.begin() + n,
               [] (const SingVoice* a, const SingVoice* b) { return a->getStartOrder() > b->getStartOrder(); });

    const int count = std::min (n, maxCount);
    for (int i = 0; i < count; ++i)
    {
        positions[i] = order[(size_t) i]->playPosition();
        levels[i] = order[(size_t) i]->getLevel();
    }
    return count;
}

} // namespace sing
