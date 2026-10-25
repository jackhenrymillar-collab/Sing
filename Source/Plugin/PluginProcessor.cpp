#include "PluginProcessor.h"

#include <cmath>
#include "PluginEditor.h"

namespace sing
{

namespace
{
    // Parameters that change the loop itself, which is rebuilt off the audio thread. The
    // engine is one: Bloom needs its analysis built with the loop.
    constexpr const char* kLoopIds[] = { ParamID::loopStart, ParamID::loopLength, ParamID::loopFade, ParamID::direction, ParamID::engine, ParamID::window };
    constexpr int kRebuildHz = 30;
}

SingProcessor::SingProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "Sing", createParameterLayout()),
      voice (PresetManager::defaultVoice()),
      presets (state, [this] { return getVoice(); }, [this] (VoiceData::Ptr v) { setVoice (std::move (v)); })
{
    attack  = state.getRawParameterValue (ParamID::attack);
    release = state.getRawParameterValue (ParamID::release);
    stretch = state.getRawParameterValue (ParamID::stretch);
    spread  = state.getRawParameterValue (ParamID::spread);
    formant = state.getRawParameterValue (ParamID::formant);
    hold    = state.getRawParameterValue (ParamID::hold);
    voices  = state.getRawParameterValue (ParamID::voices);
    glide   = state.getRawParameterValue (ParamID::glide);
    output  = state.getRawParameterValue (ParamID::output);
    bypass  = state.getRawParameterValue (ParamID::bypass);
    engine  = state.getRawParameterValue (ParamID::engine);
    airAmount   = state.getRawParameterValue (ParamID::air);
    spaceAmount = state.getRawParameterValue (ParamID::space);
    shiftHz = state.getRawParameterValue (ParamID::shift);
    smear = state.getRawParameterValue (ParamID::smear);
    tone = state.getRawParameterValue (ParamID::tone);
    keepVowel = state.getRawParameterValue (ParamID::keepVowel);
    onsetsOn = state.getRawParameterValue (ParamID::onsetsOn);
    onsets = state.getRawParameterValue (ParamID::onsets);
    shiftOn = state.getRawParameterValue (ParamID::shiftOn);
    smearOn = state.getRawParameterValue (ParamID::smearOn);
    toneOn = state.getRawParameterValue (ParamID::toneOn);
    harmonicsOn = state.getRawParameterValue (ParamID::harmonicsOn);
    harmonicsWidth = state.getRawParameterValue (ParamID::harmonicsWidth);
    harmonicsCount = state.getRawParameterValue (ParamID::harmonicsCount);
    filterOn = state.getRawParameterValue (ParamID::filterOn);
    filterLow = state.getRawParameterValue (ParamID::filterLow);
    filterHigh = state.getRawParameterValue (ParamID::filterHigh);
    filterSlope = state.getRawParameterValue (ParamID::filterSlope);
    compressorOn = state.getRawParameterValue (ParamID::compressorOn);
    compressorAmount = state.getRawParameterValue (ParamID::compressor);
    octavesOn = state.getRawParameterValue (ParamID::octavesOn);
    octaveDown2 = state.getRawParameterValue (ParamID::octaveDown2);
    octaveDown1 = state.getRawParameterValue (ParamID::octaveDown1);
    octaveRoot = state.getRawParameterValue (ParamID::octaveRoot);
    octaveUp1 = state.getRawParameterValue (ParamID::octaveUp1);
    octaveUp15 = state.getRawParameterValue (ParamID::octaveUp15);
    octaveUp2 = state.getRawParameterValue (ParamID::octaveUp2);
    binauralOn = state.getRawParameterValue (ParamID::binauralOn);
    binauralRate = state.getRawParameterValue (ParamID::binauralRate);
    binauralAmount = state.getRawParameterValue (ParamID::binauralAmount);
    spaceFilterOn = state.getRawParameterValue (ParamID::spaceFilterOn);
    spaceLow = state.getRawParameterValue (ParamID::spaceLow);
    spaceHigh = state.getRawParameterValue (ParamID::spaceHigh);
    orbitAmounts[moveStretch] = state.getRawParameterValue (ParamID::orbitStretch);
    orbitAmounts[moveSpread] = state.getRawParameterValue (ParamID::orbitSpread);
    orbitAmounts[moveFormant] = state.getRawParameterValue (ParamID::orbitFormant);
    orbitAmounts[moveAir] = state.getRawParameterValue (ParamID::orbitAir);
    orbitAmounts[moveSpace] = state.getRawParameterValue (ParamID::orbitSpace);
    orbitAmounts[moveWander] = state.getRawParameterValue (ParamID::wander);
    moveShape = state.getRawParameterValue (ParamID::moveShape);
    moveSpeed = state.getRawParameterValue (ParamID::moveSpeed);
    moveSync = state.getRawParameterValue (ParamID::moveSync);
    moveBars = state.getRawParameterValue (ParamID::moveBars);
    moveEach = state.getRawParameterValue (ParamID::moveEach);
    for (auto& o : orbitPositions)
        o = -1.0f;
    bypassParam = state.getParameter (ParamID::bypass);

    for (auto* id : kLoopIds)
        state.addParameterListener (id, this);

    // A fresh instance starts on the first factory preset, so it sings straight away.
    presets.loadPreset (0);
    builder.startThread();
    rebuildLoop();
    startTimerHz (kRebuildHz);
}

SingProcessor::~SingProcessor()
{
    stopTimer();
    builder.stopThread (4000);
    for (auto* id : kLoopIds)
        state.removeParameterListener (id, this);
}

//==============================================================================
VoiceData::Ptr SingProcessor::getVoice() const
{
    const juce::ScopedLock sl (voiceLock);
    return voice;
}

void SingProcessor::setVoice (VoiceData::Ptr newVoice)
{
    if (newVoice == nullptr)
        return;
    {
        const juce::ScopedLock sl (voiceLock);
        voice = std::move (newVoice);
    }
    loopDirty = true;
    if (juce::MessageManager::existsAndIsCurrentThread())
        rebuildLoop();
}

bool SingProcessor::loadVoiceFile (const juce::File& file, juce::String& error)
{
    auto loaded = VoiceData::fromFile (file, error);
    if (loaded == nullptr)
        return false;

    // A new voice starts with its loop over the middle of the sound.
    for (const auto& [id, value] : { std::pair { ParamID::loopStart, 25.0f }, std::pair { ParamID::loopLength, 50.0f } })
        if (auto* p = state.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    setVoice (std::move (loaded));
    return true;
}

void SingProcessor::parameterChanged (const juce::String&, float)
{
    // May arrive on the audio thread (automation): just flag it for the timer.
    loopDirty = true;
}

void SingProcessor::timerCallback()
{
    if (loopDirty.load())
        rebuildLoop();
}

void SingProcessor::rebuildLoop()
{
    const auto v = getVoice();
    if (v == nullptr)
        return;

    loopDirty = false;

    const int from = v->trimStart();
    const int length = std::max (1, v->trimLength());
    const auto value = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };

    LoopSettings loop;
    const int minLoop = std::min (length, (int) (0.02 * v->getSampleRate()));
    loop.start = from + juce::jlimit (0, length - minLoop, juce::roundToInt (value (ParamID::loopStart) * 0.01f * (float) length));
    loop.length = juce::jlimit (minLoop, from + length - loop.start, juce::roundToInt (value (ParamID::loopLength) * 0.01f * (float) length));
    loop.fade = juce::roundToInt (value (ParamID::loopFade) * 0.01f * (float) loop.length * 0.5f);
    loop.direction = static_cast<LoopDirection> (juce::jlimit (0, 3, juce::roundToInt (value (ParamID::direction))));
    // From start: no loop, so everything from Start to the end of the voice.
    if (loop.direction == LoopDirection::fromStart)
    {
        loop.length = from + length - loop.start;
        loop.fade = 0;
    }
    loop.pitchPeriod = v->isPitched() ? (float) (v->getSampleRate() / v->rootHz()) : 0.0f;
    loop.pitchTrack = v->getPitchTrack();
    loop.analyseForBloom = juce::roundToInt (value (ParamID::engine)) == 1;
    loop.bloomWindowSeconds = value (ParamID::window);
    // Bloom's note buffers are made at the size notes will play at.
    loop.hostSampleRate = getSampleRate();
    loop.bloomNotes = SingSynth::voicePool;

    builder.request ({ v, loop, from, length });
}

void SingProcessor::LoopBuilder::request (Request r)
{
    {
        const juce::ScopedLock sl (lock);
        pending = std::move (r);
    }
    wake.signal();
}

void SingProcessor::LoopBuilder::run()
{
    while (! threadShouldExit())
    {
        wake.wait (100);
        owner.exchange.collectGarbage();

        std::optional<Request> job;
        {
            const juce::ScopedLock sl (lock);
            job.swap (pending);
        }
        if (! job || job->voice == nullptr)
            continue;

        const auto& v = *job->voice;
        const auto& audio = v.getAudio();
        std::array<const float*, 2> channels { audio.getReadPointer (0), audio.getReadPointer (std::min (1, audio.getNumChannels() - 1)) };
        auto source = LoopSource::build (channels.data(), audio.getNumChannels(), audio.getNumSamples(), v.getSampleRate(), job->loop);

        // A newer request came in while building: build that one instead.
        {
            const juce::ScopedLock sl (lock);
            if (pending)
                continue;
        }

        // All slots busy means the audio thread is still finishing many old loops: try
        // again shortly, unless something newer turns up first.
        if (source == nullptr || ! owner.exchange.publish (std::move (source)))
        {
            const juce::ScopedLock sl (lock);
            if (! pending)
                pending = std::move (job);
            continue;
        }

        owner.voiceTrimStart = job->trimStart;
        owner.voiceTrimLength = job->trimLength;
        owner.rootHz = v.rootHz();
        owner.rootPitched = v.isPitched();
    }
}

//==============================================================================
bool SingProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SingProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Bloom's note buffers depend on the host rate: rebuild the loop for a new one.
    if (std::abs (sampleRate - preparedRate) > 0.5)
        loopDirty = true;
    preparedRate = sampleRate;
    synth.prepare (sampleRate, juce::jmax (1, samplesPerBlock));
    air.prepare (sampleRate);
    air.setAmount (airAmount->load() * 0.01f);
    air.reset();
    binaural.prepare (sampleRate);
    binaural.set (binauralOn->load() > 0.5f, binauralRate->load(), binauralAmount->load() * 0.01f);
    binaural.reset();
    space.prepare (sampleRate, juce::jmax (512, samplesPerBlock));
    space.setFilter (spaceFilterOn->load() > 0.5f, spaceLow->load(), spaceHigh->load());
    space.setAmount (spaceAmount->load() * 0.01f);
    space.reset();
    monoRight.assign ((size_t) juce::jmax (512, samplesPerBlock), 0.0f);
    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (output->load(), -36.0f));
    bypassStep = static_cast<float> (1.0 / (0.02 * sampleRate));
    bypassMix = bypass->load() > 0.5f ? 0.0f : 1.0f;
    if (loopDirty.load() && juce::MessageManager::existsAndIsCurrentThread())
        rebuildLoop();
}

void SingProcessor::reset()
{
    panicPending = true;
}

double SingProcessor::getTailLengthSeconds() const
{
    if (release == nullptr)
        return 0.0;
    const float amount = spaceAmount->load() * 0.01f;
    const double reverb = amount > 0.0f ? 1.2 * Space::mapping (amount).decaySeconds : 0.0;
    return static_cast<double> (release->load()) + reverb;
}

SynthSettings SingProcessor::readSettings() const noexcept
{
    SynthSettings s;
    s.voice.attackSeconds = attack->load();
    s.voice.releaseSeconds = release->load();
    s.voice.stretch = stretch->load() * 0.01f;
    s.voice.spread = spread->load() * 0.01f;
    s.voice.formantSemitones = formant->load();
    s.voice.hold = hold->load() > 0.5f;
    s.voice.glideSeconds = glide->load() * 0.001f;
    const auto isOn = [] (const std::atomic<float>* p) { return p->load() > 0.5f; };
    s.voice.shiftHz = isOn (shiftOn) ? shiftHz->load() : 0.0f;
    s.voice.smear = isOn (smearOn) ? smear->load() * 0.01f : 0.0f;
    s.voice.tone = isOn (toneOn) ? tone->load() * 0.01f : 0.0f;
    s.voice.onsets = isOn (onsetsOn) ? onsets->load() * 0.01f : 0.0f;
    s.voice.harmonicsCents = isOn (harmonicsOn) ? harmonicsWidth->load() : 0.0f;
    s.voice.harmonicsCount = juce::roundToInt (harmonicsCount->load());
    s.voice.filter = isOn (filterOn);
    s.voice.lowHz = filterLow->load();
    s.voice.highHz = filterHigh->load();
    s.voice.filterSlope = juce::roundToInt (filterSlope->load());
    s.voice.compressor = isOn (compressorOn) ? compressorAmount->load() * 0.01f : 0.0f;
    s.voice.octaves = isOn (octavesOn);
    s.voice.octaveLevels = { octaveDown2->load() * 0.01f, octaveDown1->load() * 0.01f, octaveRoot->load() * 0.01f,
                             octaveUp1->load() * 0.01f, octaveUp15->load() * 0.01f, octaveUp2->load() * 0.01f };
    s.voice.keepVowel = keepVowel->load() > 0.5f;
    s.voice.rootHz = rootHz.load();
    s.voice.pitched = rootPitched.load();
    s.polyphony = juce::roundToInt (voices->load());
    s.voice.mode = juce::roundToInt (engine->load()) == 1 ? StretchMode::bloom : StretchMode::granular;
    s.voice.movement = readMovement (getSampleRate());
    return s;
}

MovementSettings SingProcessor::readMovement (double sampleRate) const noexcept
{
    MovementSettings m;
    m.shape = static_cast<MoveShape> (juce::jlimit (0, 2, juce::roundToInt (moveShape->load())));
    for (int t = 0; t < numMoveTargets; ++t)
        m.amounts[(size_t) t] = juce::jlimit (0.0f, 1.0f, orbitAmounts[(size_t) t]->load() * 0.01f);
    m.eachNote = moveEach->load() > 0.5f;

    // The period: free in seconds, or synced to the song's bars at its tempo.
    double seconds = juce::jmax (0.1f, moveSpeed->load());
    if (moveSync->load() > 0.5f)
    {
        double bpm = 120.0, beatsPerBar = 4.0;
        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
            {
                if (const auto b = pos->getBpm())
                    bpm = juce::jlimit (20.0, 999.0, *b);
                if (const auto sig = pos->getTimeSignature())
                    beatsPerBar = juce::jmax (1.0, 4.0 * sig->numerator / juce::jmax (1, sig->denominator));
            }
        seconds = moveBarsAt (juce::roundToInt (moveBars->load())) * beatsPerBar * 60.0 / bpm;
    }
    m.cyclesPerSample = sampleRate > 0.0 ? 1.0 / (seconds * sampleRate) : 0.0;
    return m;
}

void SingProcessor::followSong (const MovementSettings& m) noexcept
{
    // Synced and the song playing: put the movement's clock where the song is, so the same
    // bar always moves the same way. Stopped, the clock just runs on.
    if (moveSync->load() <= 0.5f)
        return;
    auto* ph = getPlayHead();
    if (ph == nullptr)
        return;
    const auto pos = ph->getPosition();
    if (! pos || ! pos->getIsPlaying())
        return;
    const auto ppq = pos->getPpqPosition();
    if (! ppq)
        return;
    double beatsPerBar = 4.0;
    if (const auto sig = pos->getTimeSignature())
        beatsPerBar = juce::jmax (1.0, 4.0 * sig->numerator / juce::jmax (1, sig->denominator));
    const double beatsPerCycle = moveBarsAt (juce::roundToInt (moveBars->load())) * beatsPerBar;
    if (m.cyclesPerSample > 0.0 && beatsPerCycle > 0.0)
        synth.setMoveClock (*ppq / beatsPerCycle);
}

void SingProcessor::handleMidi (const juce::MidiMessage& m) noexcept
{
    if (m.isNoteOn())
        synth.noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        synth.noteOff (m.getNoteNumber());
    else if (m.isSustainPedalOn())
        synth.setSustain (true);
    else if (m.isSustainPedalOff())
        synth.setSustain (false);
    else if (m.isPitchWheel())
        synth.setPitchBend (2.0f * static_cast<float> (m.getPitchWheelValue() - 8192) / 8192.0f);   // +-2 semitones
    else if (m.isAllSoundOff())
        synth.allSoundOff();
    else if (m.isAllNotesOff() || m.isResetAllControllers())
        synth.allNotesOff();
}

void SingProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    if (numSamples == 0)
        return;

    if (panicPending.exchange (false))
    {
        synth.allSoundOff();
        space.reset();
        air.reset();
        binaural.reset();
    }

    // Pick up a rebuilt loop, and hand back any old one no note uses any more.
    synth.setSource (exchange.acquire());
    synth.releaseUnusedSources ([this] (const LoopSource* s) { exchange.retire (s); });

    const auto settings = readSettings();
    synth.setSettings (settings);
    followSong (settings.voice.movement);
    const double clockAtStart = synth.getMoveClock();

    float* left = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    std::array<float, 512> scratch {};

    // Render between MIDI events, so notes start on the right sample.
    auto renderTo = [&] (int from, int to)
    {
        for (int pos = from; pos < to;)
        {
            const int n = std::min (to - pos, 512);
            if (right != nullptr)
                synth.render (left + pos, right + pos, n);
            else
            {
                // Mono output: render stereo and fold down.
                std::fill (scratch.begin(), scratch.begin() + n, 0.0f);
                synth.render (left + pos, scratch.data(), n);
                for (int i = 0; i < n; ++i)
                    left[pos + i] = 0.5f * (left[pos + i] + scratch[(size_t) i]);
            }
            pos += n;
        }
    };

    int rendered = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        renderTo (rendered, at);
        rendered = at;
        handleMidi (meta.getMessage());
    }
    renderTo (rendered, numSamples);

    // A bad sample (NaN or infinity) would stick in Air's filters and Space's reverb and
    // silence them for good. None should ever reach here; if one does, drop it and clear
    // the effects so they recover at once.
    {
        bool bad = false;
        for (auto* ch : { left, right })
            if (ch != nullptr)
                for (int i = 0; i < numSamples; ++i)
                    if (! std::isfinite (ch[i]))
                    {
                        ch[i] = 0.0f;
                        bad = true;
                    }
        if (bad)
        {
            air.reset();
            space.reset();
        }
    }

    // Binaural beats, then Air, then Space: the reverb hears the lifted top and the breath.
    binaural.set (binauralOn->load() > 0.5f, binauralRate->load(), binauralAmount->load() * 0.01f);

    // Air and Space orbit here, after the notes, so every note shares their path.
    {
        const auto& m = settings.voice.movement;
        const float ease = 1.0f - std::exp (-(float) numSamples / (0.03f * (float) getSampleRate()));
        const auto moved = [&] (int target, float base)
        {
            const float width = m.amounts[(size_t) target];
            const float wanted = width > 0.0f
                ? movement::orbit (base, width, movement::value (m.shape, clockAtStart, movement::targetSeed (movement::sharedSeed, target))) - base
                : 0.0f;
            auto& offset = fxOffset[(size_t) target];
            offset += (wanted - offset) * ease;
            const float now = juce::jlimit (0.0f, 1.0f, base + offset);
            orbitPositions[(size_t) target] = width > 0.0f ? now : -1.0f;
            return now;
        };
        air.setAmount (moved (moveAir, airAmount->load() * 0.01f));
        space.setAmount (moved (moveSpace, spaceAmount->load() * 0.01f));
    }

    // The newest note's orbits, for the panel.
    {
        const auto& m = settings.voice.movement;
        const auto* newest = synth.newestVoice();
        const auto moved = newest != nullptr ? newest->getMoved() : SingVoice::Moved {};
        const auto show = [&] (int target, float value)
        {
            orbitPositions[(size_t) target] = newest != nullptr && m.amounts[(size_t) target] > 0.0f ? value : -1.0f;
        };
        show (moveStretch, moved.stretch);
        show (moveSpread, moved.spread);
        show (moveFormant, moved.formant);
        show (moveWander, moved.wander + 0.5f);
    }
    space.setFilter (spaceFilterOn->load() > 0.5f, spaceLow->load(), spaceHigh->load());
    if (right != nullptr)
    {
        binaural.process (left, right, numSamples);
        air.process (left, right, numSamples);
        space.process (left, right, numSamples);
    }
    else
    {
        for (int pos = 0; pos < numSamples;)
        {
            const int n = std::min (numSamples - pos, (int) monoRight.size());
            std::copy (left + pos, left + pos + n, monoRight.begin());
            air.process (left + pos, monoRight.data(), n);
            space.process (left + pos, monoRight.data(), n);
            for (int i = 0; i < n; ++i)
                left[pos + i] = 0.5f * (left[pos + i] + monoRight[(size_t) i]);
            pos += n;
        }
    }

    // Output level, and the evening star's bypass, which fades Sing to silence.
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (output->load(), -36.0f));
    const float target = bypass->load() > 0.5f ? 0.0f : 1.0f;
    float g = bypassMix;
    for (int i = 0; i < numSamples; ++i)
    {
        g = target > g ? juce::jmin (target, g + bypassStep) : juce::jmax (target, g - bypassStep);
        const float gain = outputGain.getNextValue() * g;
        left[i] *= gain;
        if (right != nullptr)
            right[i] *= gain;
    }
    bypassMix = g;

    // Play lights for the waveform: every Spread copy of the newest notes.
    std::array<SingSynth::PlayLight, maxLights> found {};
    const int count = synth.getPlayLights (found.data(), maxLightNotes, maxLights);
    const double start = voiceTrimStart.load(), length = std::max (1, voiceTrimLength.load());
    for (int i = 0; i < maxLights; ++i)
    {
        auto& out = lights[(size_t) i];
        if (i < count)
        {
            const auto& l = found[(size_t) i];
            out.position = static_cast<float> ((l.position - start) / length);
            out.level = l.level;
            out.weight = l.weight;
            out.pan = l.pan;
            out.pitchRatio = l.pitchRatio;
            out.zone = static_cast<float> (l.zoneSamples / length);
            out.copy = l.copy;
            out.noteId = l.noteId;
            out.note = l.note;
        }
        else
            out.note = -1;
    }
}

std::array<float, numMoveTargets> SingProcessor::getOrbitPositions() const noexcept
{
    std::array<float, numMoveTargets> result {};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = orbitPositions[i].load();
    return result;
}

std::array<SingProcessor::Light, SingProcessor::maxLights> SingProcessor::getLights() const noexcept
{
    std::array<Light, maxLights> result {};
    for (int i = 0; i < maxLights; ++i)
    {
        const auto& in = lights[(size_t) i];
        auto& l = result[(size_t) i];
        l.note = in.note.load();
        if (l.note < 0)
            continue;
        l.position = in.position.load();
        l.level = in.level.load();
        l.weight = in.weight.load();
        l.pan = in.pan.load();
        l.pitchRatio = in.pitchRatio.load();
        l.zone = in.zone.load();
        l.copy = in.copy.load();
        l.noteId = in.noteId.load();
    }
    return result;
}

juce::AudioProcessorEditor* SingProcessor::createEditor()
{
    return new SingEditor (*this);
}

//==============================================================================
int SingProcessor::getNumPrograms()
{
    return presets.getNumFactoryPresets();
}

int SingProcessor::getCurrentProgram()
{
    const int index = presets.getCurrentIndex();
    return index >= 0 && index < presets.getNumFactoryPresets() ? index : 0;
}

void SingProcessor::setCurrentProgram (int index)
{
    if (index >= 0 && index < presets.getNumFactoryPresets())
        presets.loadPreset (index);
}

const juce::String SingProcessor::getProgramName (int index)
{
    return index >= 0 && index < presets.getNumFactoryPresets() ? presets.getPresetName (index) : juce::String();
}

void SingProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto copy = state.copyState();
    presets.writeToState (copy);
    copy.setProperty ("uiScale", uiScale.load(), nullptr);
    if (const auto v = getVoice())
        copy.appendChild (v->toValueTree(), nullptr);
    if (auto xml = copy.createXml())
        copyXmlToBinary (*xml, destData);
}

void SingProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (state.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const auto voiceNode = tree.getChildWithName ("VOICE");
    tree.removeChild (voiceNode, nullptr);

    if (auto restored = VoiceData::fromValueTree (voiceNode))
        setVoice (std::move (restored));

    state.replaceState (tree);

    // replaceState skips a parameter whose stored value already matches, judged after
    // snapping, so set every parameter explicitly (as Overcast does).
    for (const auto& child : tree)
        if (child.hasType ("PARAM"))
            if (auto* param = state.getParameter (child.getProperty ("id").toString()))
                param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (child.getProperty ("value"))));

    presets.readFromState (tree);
    uiScale = static_cast<float> (tree.getProperty ("uiScale", 1.0));
    loopDirty = true;
}

} // namespace sing

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sing::SingProcessor();
}
