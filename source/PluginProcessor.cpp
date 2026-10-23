#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "dsp/VoiceData.h"

namespace sing
{

SingProcessor::SingProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "Sing", createParameterLayout())
{
    loopStart  = state.getRawParameterValue (ParamID::loopStart);
    loopLength = state.getRawParameterValue (ParamID::loopLength);
    loopFade   = state.getRawParameterValue (ParamID::loopFade);
    loopMode   = state.getRawParameterValue (ParamID::loopMode);
    attack     = state.getRawParameterValue (ParamID::attack);
    release    = state.getRawParameterValue (ParamID::release);
    stretch    = state.getRawParameterValue (ParamID::stretch);
    spread     = state.getRawParameterValue (ParamID::spread);
    formant    = state.getRawParameterValue (ParamID::formant);
    air        = state.getRawParameterValue (ParamID::air);
    space      = state.getRawParameterValue (ParamID::space);
    hold       = state.getRawParameterValue (ParamID::hold);
    octDown    = state.getRawParameterValue (ParamID::octDown);
    octUp      = state.getRawParameterValue (ParamID::octUp);
    tape       = state.getRawParameterValue (ParamID::tape);
    voices     = state.getRawParameterValue (ParamID::voices);
    glide      = state.getRawParameterValue (ParamID::glide);
    output     = state.getRawParameterValue (ParamID::output);
    bypass     = state.getRawParameterValue (ParamID::bypass);
    bypassParam = state.getParameter (ParamID::bypass);

    library.onReady = [this] (std::unique_ptr<dsp::VoiceData> data)
    {
        engine.submitVoice (std::move (data));
        ++voiceChanges;
    };

    // Start on the first factory preset, with its voice ready straight away.
    presets.loadPreset (0);
    library.setTree (library.getTree(), true);
    garbageTimer.startTimer (500);
}

SingProcessor::~SingProcessor()
{
    garbageTimer.stopTimer();
    library.onReady = nullptr;
}

bool SingProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SingProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setParams (readParams());
    engine.prepare (sampleRate, juce::jmax (1, samplesPerBlock));
    setLatencySamples (engine.getLatencySamples());
}

void SingProcessor::reset()
{
    engine.reset();
}

dsp::EngineParams SingProcessor::readParams() const noexcept
{
    dsp::EngineParams p;
    p.loopStart = loopStart->load();
    p.loopLength = loopLength->load();
    p.loopFade = loopFade->load() * 0.01f;
    p.loopMode = static_cast<dsp::LoopMode> (juce::jlimit (0, 2, juce::roundToInt (loopMode->load())));
    p.attack = attack->load();
    p.release = release->load();
    p.stretch = stretch->load() * 0.01f;
    p.spread = spread->load() * 0.01f;
    p.formant = formant->load();
    p.air = air->load();
    p.space = space->load() * 0.01f;
    p.hold = hold->load() > 0.5f;
    p.octDown = octDown->load() > 0.5f;
    p.octUp = octUp->load() > 0.5f;
    p.tape = tape->load() > 0.5f;
    p.voices = juce::roundToInt (voices->load());
    p.glideMs = glide->load();
    p.outputDb = output->load();
    p.bypass = bypass->load() > 0.5f;
    return p;
}

void SingProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    if (numSamples == 0)
        return;

    engine.setParams (readParams());
    float* left = buffer.getWritePointer (0);
    // Mono output: render stereo into scratch and fold down.
    float scratch[2048];
    const bool mono = buffer.getNumChannels() < 2;
    float* right = mono ? nullptr : buffer.getWritePointer (1);

    auto render = [&] (int from, int to)
    {
        while (from < to)
        {
            const int n = mono ? juce::jmin (to - from, static_cast<int> (std::size (scratch))) : to - from;
            engine.process (left + from, mono ? scratch : right + from, n);
            if (mono)
                for (int i = 0; i < n; ++i)
                    left[from + i] = 0.5f * (left[from + i] + scratch[i]);
            from += n;
        }
    };

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        render (pos, at);
        pos = at;

        const auto m = meta.getMessage();
        if (m.isNoteOn())
            engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            engine.noteOff (m.getNoteNumber());
        else if (m.isSustainPedalOn())
            engine.sustainPedal (true);
        else if (m.isSustainPedalOff())
            engine.sustainPedal (false);
        else if (m.isPitchWheel())
            engine.pitchBend (2.0f * static_cast<float> (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            engine.allNotesOff();
    }
    render (pos, numSamples);
}

std::array<float, SingProcessor::kMaxPlayheads> SingProcessor::getPlayheads() const noexcept
{
    std::array<float, kMaxPlayheads> out {};
    engine.getPlayheads (out);
    return out;
}

VoiceInfo SingProcessor::getVoiceInfo() const { return library.getInfo(); }
std::vector<float> SingProcessor::getWaveform() const { return library.getWaveform (kWaveformPoints); }

bool SingProcessor::loadVoiceFile (const juce::File& file, juce::String& error)
{
    if (! library.loadFile (file, error))
        return false;
    presets.voiceReplaced();
    // A new voice starts with its loop on the middle stretch, where a sung note is steadiest.
    auto set = [this] (const char* id, float value)
    {
        if (auto* p = state.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    };
    set (ParamID::loopStart, 0.3f);
    set (ParamID::loopLength, 0.4f);
    set (ParamID::loopFade, 30.0f);
    return true;
}

juce::AudioProcessorEditor* SingProcessor::createEditor()
{
    return new SingEditor (*this);
}

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
    copy.setProperty ("version", 1, nullptr);
    copy.removeChild (copy.getChildWithName ("VOICE"), nullptr);
    copy.appendChild (library.getTree().createCopy(), nullptr);
    // Binary, because the voice's audio is inside.
    juce::MemoryOutputStream out (destData, false);
    copy.writeToStream (out);
}

void SingProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
    if (! tree.hasType (state.state.getType()))
        return;

    auto voice = tree.getChildWithName ("VOICE").createCopy();
    tree.removeChild (tree.getChildWithName ("VOICE"), nullptr);
    state.replaceState (tree);

    // replaceState skips a parameter whose stored value already matches, judged after
    // snapping, so set every parameter explicitly.
    for (const auto& child : tree)
        if (child.hasType ("PARAM"))
            if (auto* param = state.getParameter (child.getProperty ("id").toString()))
                param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (child.getProperty ("value"))));

    presets.readFromState (tree);
    uiScale = static_cast<float> (tree.getProperty ("uiScale", 1.0));
    if (voice.isValid())
        library.setTree (voice, true); // ready before the next block, so offline renders aren't silent
}

} // namespace sing

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sing::SingProcessor();
}
