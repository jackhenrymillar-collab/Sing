#include "PresetManager.h"
#include "Parameters.h"

namespace sing
{

namespace
{
    constexpr auto kFileExtension = ".singpreset";
    constexpr auto kRootTag = "SingPreset";

    struct FactoryPreset
    {
        const char* name;
        PlaceholderVowel vowel;
        float attack, release, stretch, spread, formant;
        float loopStart, loopLength, loopFade;
        int direction;
        int engine;   // 0 Grain, 1 Bloom
        float air, space;
        // Movement (presets that leave it out stay still).
        float orbitSpread = 0.0f, orbitFormant = 0.0f, wander = 0.0f;
    };

    // The four presets from the mockup, two for Bloom and one that moves. Their voices are generated
    // stand-ins until the recorded factory voices exist.
    constexpr FactoryPreset kFactory[] = {
        //  name            voice                      attack release stretch spread formant  start length fade  dir engine air    space (old one knob)
        { "Hollow Choir", PlaceholderVowel::ooh,     2.4f,  7.5f,  68.0f, 35.0f, -2.0f,   30.0f, 40.0f, 28.0f, 0,  0,     20.0f, 35.0f },
        { "Distant Ah",   PlaceholderVowel::ah,      3.8f,  9.0f,  82.0f, 50.0f, -4.0f,   22.0f, 36.0f, 35.0f, 0,  0,     15.0f, 55.0f },
        { "Glass Hum",    PlaceholderVowel::hum,     1.2f,  6.0f,  45.0f, 28.0f,  2.0f,   40.0f, 46.0f, 20.0f, 0,  0,     35.0f, 25.0f },
        { "Last Light",   PlaceholderVowel::breath,  5.0f, 12.0f,  90.0f, 62.0f,  0.0f,   18.0f, 34.0f, 40.0f, 2,  0,     10.0f, 65.0f },
        { "Bloom Halo",   PlaceholderVowel::ooh,     3.0f, 10.0f,  85.0f, 45.0f,  0.0f,   30.0f, 40.0f, 28.0f, 0,  1,     30.0f, 70.0f },
        { "Slow Petals",  PlaceholderVowel::ah,      4.5f, 14.0f,  96.0f, 70.0f, -3.0f,   22.0f, 36.0f, 35.0f, 0,  1,     20.0f, 85.0f },
        // Movement: Spread and Formant drift on every note, and the loop wanders.
        { "Drifting Choir", PlaceholderVowel::ah,    3.0f, 10.0f,  80.0f, 45.0f, -2.0f,   20.0f, 55.0f, 30.0f, 0,  0,     25.0f, 60.0f,  50.0f, 25.0f, 50.0f },
    };
}

VoiceData::Ptr PresetManager::defaultVoice()
{
    static const auto voice = VoiceData::fromPlaceholder (kFactory[0].vowel);
    return voice;
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& s,
                              std::function<VoiceData::Ptr()> get,
                              std::function<void (VoiceData::Ptr)> set)
    : state (s), getVoice (std::move (get)), setVoice (std::move (set))
{
    for (const auto& f : kFactory)
    {
        Preset p;
        p.name = f.name;
        p.vowel = f.vowel;
        p.values = { { ParamID::attack,     f.attack },
                     { ParamID::release,    f.release },
                     { ParamID::stretch,    f.stretch },
                     { ParamID::spread,     f.spread },
                     { ParamID::formant,    f.formant },
                     { ParamID::loopStart,  f.loopStart },
                     { ParamID::loopLength, f.loopLength },
                     { ParamID::loopFade,   f.loopFade },
                     { ParamID::direction,  static_cast<float> (f.direction) },
                     { ParamID::engine,     static_cast<float> (f.engine) },
                     { ParamID::air,        f.air },
                     { ParamID::space,      f.space },
                     { ParamID::orbitSpread,  f.orbitSpread },
                     { ParamID::orbitFormant, f.orbitFormant },
                     { ParamID::wander,       f.wander } };
        upgradeOldSpace (p.values);   // the table holds the old one-knob Space
        factory.push_back (std::move (p));
    }

    refreshUserPresets();
    setLoaded (captureValues(), 0);
}

void PresetManager::setLoaded (Values values, juce::int64 voiceId)
{
    const juce::ScopedLock sl (listLock);
    loaded = std::move (values);
    loadedVoice = voiceId;
}

int PresetManager::getNumPresets() const
{
    const juce::ScopedLock sl (listLock);
    return static_cast<int> (factory.size() + user.size());
}

int PresetManager::getNumFactoryPresets() const noexcept { return static_cast<int> (factory.size()); }

juce::String PresetManager::getPresetName (int index) const
{
    const juce::ScopedLock sl (listLock);
    if (index < 0)
        return "Init";
    if (index < static_cast<int> (factory.size()))
        return factory[static_cast<std::size_t> (index)].name;
    index -= static_cast<int> (factory.size());
    if (index < static_cast<int> (user.size()))
        return user[static_cast<std::size_t> (index)].name;
    return {};
}

juce::String PresetManager::getCurrentName() const { return getPresetName (current.load()); }

PresetManager::Values PresetManager::captureValues() const
{
    Values v;
    for (auto* param : state.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (ranged->getParameterID() != ParamID::bypass)   // bypass is never part of a preset
                v[ranged->getParameterID()] = ranged->convertFrom0to1 (ranged->getValue());
    return v;
}

void PresetManager::applyValues (const Values& values)
{
    for (auto* param : state.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
        if (ranged == nullptr || ranged->getParameterID() == ParamID::bypass)
            continue;

        // Anything the preset leaves out goes back to its default.
        const auto it = values.find (ranged->getParameterID());
        const float normalised = it != values.end() ? ranged->convertTo0to1 (it->second)
                                                    : ranged->getDefaultValue();
        ranged->beginChangeGesture();
        ranged->setValueNotifyingHost (normalised);
        ranged->endChangeGesture();
    }
}

bool PresetManager::isModified() const
{
    Values loadedCopy;
    juce::int64 voiceId = 0;
    {
        const juce::ScopedLock sl (listLock);
        loadedCopy = loaded;
        voiceId = loadedVoice;
    }
    if (const auto voice = getVoice(); voiceId != 0 && voice != nullptr && voice->getId() != voiceId)
        return true;

    for (const auto& [id, value] : captureValues())
    {
        const auto it = loadedCopy.find (id);
        if (it == loadedCopy.end() || std::abs (it->second - value) > 1.0e-3f * std::max (1.0f, std::abs (value)))
            return true;
    }
    return false;
}

void PresetManager::loadPreset (int index)
{
    Preset preset;
    {
        const juce::ScopedLock sl (listLock);
        const int total = static_cast<int> (factory.size() + user.size());
        if (index < 0 || index >= total)
            return;
        preset = index < static_cast<int> (factory.size())
                     ? factory[static_cast<std::size_t> (index)]
                     : user[static_cast<std::size_t> (index) - factory.size()];
    }

    VoiceData::Ptr voice;
    if (preset.vowel)
        voice = VoiceData::fromPlaceholder (*preset.vowel);
    else if (preset.file.existsAsFile())
    {
        Preset full;
        readPresetFile (preset.file, full, true, &voice);
    }

    applyValues (preset.values);
    if (voice != nullptr)
        setVoice (voice);

    const auto now = getVoice();
    setLoaded (captureValues(), now != nullptr ? now->getId() : 0);
    current = index;
    ++changeCount;
}

void PresetManager::loadNext()
{
    const int n = getNumPresets();
    if (n > 0)
        loadPreset ((current.load() + 1 + n) % n);
}

void PresetManager::loadPrevious()
{
    const int n = getNumPresets();
    if (n > 0)
        loadPreset (current.load() <= 0 ? n - 1 : current.load() - 1);
}

juce::File PresetManager::getUserPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
        .getChildFile ("Audio/Presets")
#endif
        .getChildFile ("Overcast Audio/Sing");
}

bool PresetManager::readPresetFile (const juce::File& file, Preset& out, bool withVoice, VoiceData::Ptr* voiceOut)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (kRootTag))
        return false;

    out.name = xml->getStringAttribute ("name", file.getFileNameWithoutExtension());
    out.file = file;
    out.values.clear();
    for (auto* p : xml->getChildWithTagNameIterator ("PARAM"))
        out.values[p->getStringAttribute ("id")] = static_cast<float> (p->getDoubleAttribute ("value"));
    upgradeOldSpace (out.values);

    if (withVoice && voiceOut != nullptr)
        if (auto* v = xml->getChildByName ("VOICE"))
            *voiceOut = VoiceData::fromValueTree (juce::ValueTree::fromXml (*v));
    return true;
}

void PresetManager::refreshUserPresets()
{
    std::vector<Preset> found;
    for (const auto& file : getUserPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kFileExtension))
    {
        Preset p;
        if (readPresetFile (file, p, false, nullptr))
            found.push_back (std::move (p));
    }
    std::sort (found.begin(), found.end(), [] (const Preset& a, const Preset& b) { return a.name.compareNatural (b.name) < 0; });

    const auto currentName = getCurrentName();
    const bool wasUser = current.load() >= static_cast<int> (factory.size());
    {
        const juce::ScopedLock sl (listLock);
        user = std::move (found);
    }

    // Keep pointing at the same user preset after the list is re-sorted.
    if (wasUser)
    {
        int index = -1;
        for (int i = 0; i < static_cast<int> (user.size()); ++i)
            if (user[static_cast<std::size_t> (i)].name == currentName)
                index = static_cast<int> (factory.size()) + i;
        current = index;
    }
    ++changeCount;
}

bool PresetManager::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim();
    if (name.isEmpty())
        return false;

    auto folder = getUserPresetFolder();
    if (! folder.createDirectory())
        return false;

    juce::XmlElement xml (kRootTag);
    xml.setAttribute ("name", name);
    xml.setAttribute ("version", 1);
    for (const auto& [id, value] : captureValues())
    {
        auto* p = xml.createNewChildElement ("PARAM");
        p->setAttribute ("id", id);
        p->setAttribute ("value", value);
    }
    if (const auto voice = getVoice())
        if (auto voiceXml = voice->toValueTree().createXml())
            xml.addChildElement (voiceXml.release());

    const auto file = folder.getChildFile (juce::File::createLegalFileName (name) + kFileExtension);
    if (! xml.writeTo (file))
        return false;

    refreshUserPresets();
    {
        const juce::ScopedLock sl (listLock);
        for (int i = 0; i < static_cast<int> (user.size()); ++i)
            if (user[static_cast<std::size_t> (i)].file == file)
                current = static_cast<int> (factory.size()) + i;
    }
    const auto voice = getVoice();
    setLoaded (captureValues(), voice != nullptr ? voice->getId() : 0);
    ++changeCount;
    return true;
}

void PresetManager::toggleAB()
{
    auto& from = onB ? slotB : slotA;
    auto& to = onB ? slotA : slotB;
    from = { captureValues(), getVoice() };
    if (to.values.empty())
        to = from;
    else
    {
        applyValues (to.values);
        if (to.voice != nullptr)
            setVoice (to.voice);
    }
    onB = ! onB;
    ++changeCount;
}

void PresetManager::writeToState (juce::ValueTree& stateCopy) const
{
    stateCopy.setProperty ("preset", current.load() >= 0 ? getCurrentName() : juce::String(), nullptr);
}

void PresetManager::readFromState (const juce::ValueTree& restored)
{
    // May be called off the message thread, so it only works out which preset is current.
    // The comparison values are the restored ones: the session is "as saved".
    const auto name = restored.getProperty ("preset").toString();
    int index = -1;
    if (name.isNotEmpty())
    {
        const juce::ScopedLock sl (listLock);
        for (int i = 0; index < 0 && i < static_cast<int> (factory.size()); ++i)
            if (factory[static_cast<std::size_t> (i)].name == name)
                index = i;
        for (int i = 0; index < 0 && i < static_cast<int> (user.size()); ++i)
            if (user[static_cast<std::size_t> (i)].name == name)
                index = static_cast<int> (factory.size()) + i;
    }

    const auto voice = getVoice();
    setLoaded (captureValues(), voice != nullptr ? voice->getId() : 0);
    current = index;
    ++changeCount;
}

} // namespace sing
