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

    // A key played again while still held (or sustained) starts a fresh note, with its harmony.
    for (int i = 0; i < voicePool; ++i)
        if (voices[(size_t) i].isActive() && playedKeys[(size_t) i] == note && ! voices[(size_t) i].isReleased())
        {
            voices[(size_t) i].release();
            sustained[(size_t) i] = false;
        }

    startVoice (note, note, velocity, true);

    // The Harmoniser's notes, in key, above and below.
    const auto& h = settings.harmony;
    if (h.on)
        for (const bool up : { true, false })
        {
            const int interval = up ? h.above : h.below;
            if (interval <= 0)
                continue;
            const int harmony = harmonyNote (note, h.key, h.scale, interval, up);
            if (harmony != note && harmony >= 0 && harmony < 128)
                startVoice (harmony, note, velocity * h.level, false);
        }

    lastNoteSemitones = (float) note - hzToMidi (settings.voice.rootHz);
    hasLastNote = true;
}

void SingSynth::startVoice (int note, int playedKey, float velocity, bool glide) noexcept
{
    auto* v = findVoiceToStart();
    const auto index = (size_t) (v - voices.data());
    v->setMoveClock (moveCycles);
    v->start (*source, note, velocity, settings.voice, glide && hasLastNote,
              lastNoteSemitones + settings.voice.pitchBendSemitones, nextSeed);
    v->setStartOrder (nextOrder++);
    sustained[index] = false;
    playedKeys[index] = playedKey;
    nextSeed = nextSeed * 1664525u + 1013904223u;
}

int harmonyNote (int note, int key, int scale, int interval, bool up) noexcept
{
    if (interval <= 0)
        return note;
    scale = std::clamp (scale, 0, numHarmonyScales - 1);
    const int size = harmonyScaleSize[scale];
    const int* steps = harmonyScales[scale];
    key = ((key % 12) + 12) % 12;
    const int pc = (((note - key) % 12) + 12) % 12;

    // The nearest step of the scale (the lower one when two are as near).
    int nearest = 0, best = 99;
    for (int i = 0; i < size; ++i)
    {
        int d = std::abs (pc - steps[i]);
        d = std::min (d, 12 - d);
        if (d < best)
        {
            best = d;
            nearest = i;
        }
    }
    // Steps up or down the scale: a third is two steps, a fourth three, and so on; an
    // octave is the whole scale.
    const int stepCount = interval >= 5 ? size : interval + 1;
    const int index = nearest + (up ? stepCount : -stepCount);
    const int octave = index >= 0 ? index / size : -((-index + size - 1) / size);
    const int degree = index - octave * size;
    // Both the snapped note and the harmony are measured from the key below the note.
    return note - pc + steps[degree] + 12 * octave;
}

void SingSynth::noteOff (int note) noexcept
{
    for (int i = 0; i < voicePool; ++i)
    {
        auto& v = voices[(size_t) i];
        if (! v.isActive() || playedKeys[(size_t) i] != note || v.isReleased())
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
        {
            v.setMoveClock (moveCycles);
            v.render (left, right, numSamples);
        }
    // Wrapped now and then, so the clock keeps its precision in a session left running for days.
    moveCycles += settings.voice.movement.cyclesPerSample * (double) numSamples;
    if (moveCycles > 1.0e6)
        moveCycles -= 1.0e6;
}

const SingVoice* SingSynth::newestVoice() const noexcept
{
    const SingVoice* newest = nullptr;
    for (const auto& v : voices)
        if (v.isActive() && ! v.isReleased() && (newest == nullptr || v.getStartOrder() > newest->getStartOrder()))
            newest = &v;
    if (newest == nullptr)
        for (const auto& v : voices)
            if (v.isActive() && (newest == nullptr || v.getStartOrder() > newest->getStartOrder()))
                newest = &v;
    return newest;
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

int SingSynth::getPlayLights (PlayLight* lights, int maxNotes, int maxCount) const noexcept
{
    std::array<const SingVoice*, (size_t) voicePool> order {};
    int n = 0;
    for (const auto& v : voices)
        if (v.isActive())
            order[(size_t) n++] = &v;
    std::sort (order.begin(), order.begin() + n,
               [] (const SingVoice* a, const SingVoice* b) { return a->getStartOrder() > b->getStartOrder(); });

    int count = 0;
    for (int note = 0; note < std::min (n, maxNotes) && count < maxCount; ++note)
    {
        const auto& v = *order[(size_t) note];
        std::array<SingVoice::CopyLight, (size_t) SingVoice::maxCopies> copies {};
        const int k = v.getCopyLights (copies.data(), SingVoice::maxCopies);
        for (int c = 0; c < k && count < maxCount; ++c)
        {
            const auto& cl = copies[(size_t) c];
            lights[count++] = { cl.position, v.getLevel(), cl.weight, cl.pan, cl.pitchRatio, cl.zoneSamples, note, c,
                                (uint32_t) v.getStartOrder() };
        }
    }
    return count;
}

} // namespace sing
