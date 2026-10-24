#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace sing
{

namespace
{
    // Parameters that change the loop itself, which is rebuilt off the audio thread.
    constexpr const char* kLoopIds[] = { ParamID::loopStart, ParamID::loopLength, ParamID::loopFade, ParamID::direction };
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
    bypassParam = state.getParameter (ParamID::bypass);

    for (auto* id : kLoopIds)
        state.addParameterListener (id, this);

    // A fresh instance starts on the first factory preset, so it sings straight away.
    presets.loadPreset (0);
    rebuildLoop();
    startTimerHz (kRebuildHz);
}

SingProcessor::~SingProcessor()
{
    stopTimer();
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
    exchange.collectGarbage();
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
    loop.direction = static_cast<LoopDirection> (juce::jlimit (0, 2, juce::roundToInt (value (ParamID::direction))));
    loop.pitchPeriod = v->isPitched() ? (float) (v->getSampleRate() / v->rootHz()) : 0.0f;

    const auto& audio = v->getAudio();
    std::array<const float*, 2> channels { audio.getReadPointer (0), audio.getReadPointer (std::min (1, audio.getNumChannels() - 1)) };
    auto source = LoopSource::build (channels.data(), audio.getNumChannels(), audio.getNumSamples(), v->getSampleRate(), loop);

    voiceTrimStart = from;
    voiceTrimLength = length;
    rootHz = v->rootHz();
    rootPitched = v->isPitched();

    // All slots busy means the audio thread is still finishing many old loops: try next tick.
    if (source == nullptr || ! exchange.publish (std::move (source)))
        loopDirty = true;
}

//==============================================================================
bool SingProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SingProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.prepare (sampleRate, juce::jmax (1, samplesPerBlock));
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
    return release != nullptr ? static_cast<double> (release->load()) : 0.0;
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
    s.voice.rootHz = rootHz.load();
    s.voice.pitched = rootPitched.load();
    s.polyphony = juce::roundToInt (voices->load());
    return s;
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
        synth.allSoundOff();

    // Pick up a rebuilt loop, and hand back any old one no note uses any more.
    synth.setSource (exchange.acquire());
    synth.releaseUnusedSources ([this] (const LoopSource* s) { exchange.retire (s); });

    synth.setSettings (readSettings());

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
