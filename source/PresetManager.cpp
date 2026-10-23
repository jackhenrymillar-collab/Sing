#include "PresetManager.h"
#include "Parameters.h"
#include "VoiceLibrary.h"

namespace sing
{

namespace
{
    constexpr auto kFileExtension = ".singpreset";
    constexpr auto kRootTag = "SingPreset";

    struct FactoryPreset
    {
        const char* name;
        const char* voice;
        float loopStart, loopLength, loopFade;
        int loopMode;
        float attack, release, stretch, spread, formant, air, space;
        bool hold, octDown, octUp, tape;
        float output;
    };

    // The four presets from the panel mockup, each with its own voice.
    constexpr FactoryPreset kFactory[] = {
        //  name            voice           start  length fade  mode  att   rel    str    spr    form   air   space  hold   -1     +1     tape   out
        { "Hollow Choir", "hollow-choir",   0.30f, 0.40f, 28.0f, 2,  2.4f,  7.5f, 68.0f, 35.0f, -2.0f, 3.0f, 62.0f, false, true,  false, true,  0.0f },
        { "Distant Ah",   "distant-ah",     0.22f, 0.36f, 35.0f, 0,  3.8f,  9.0f, 82.0f, 50.0f, -4.0f, 1.0f, 74.0f, false, true,  false, false, 0.0f },
        { "Glass Hum",    "glass-hum",      0.40f, 0.46f, 20.0f, 0,  1.2f,  6.0f, 45.0f, 28.0f,  2.0f, 5.0f, 55.0f, false, false, true,  false, 0.0f },
        { "Last Light",   "last-light",     0.18f, 0.34f, 40.0f, 2,  5.0f, 12.0f, 90.0f, 62.0f,  0.0f, 2.0f, 85.0f, false, false, false, true,  8.0f },
    };
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& s, VoiceLibrary& v)
    : state (s), voices (v)
{
    for (const auto& f : kFactory)
    {
        Preset p;
        p.name = f.name;
        p.voice = VoiceLibrary::factoryTree (f.voice);
        p.values = { { ParamID::loopStart,  f.loopStart },
                     { ParamID::loopLength, f.loopLength },
                     { ParamID::loopFade,   f.loopFade },
                     { ParamID::loopMode,   static_cast<float> (f.loopMode) },
                     { ParamID::attack,     f.attack },
                     { ParamID::release,    f.release },
                     { ParamID::stretch,    f.stretch },
                     { ParamID::spread,     f.spread },
                     { ParamID::formant,    f.formant },
                     { ParamID::air,        f.air },
                     { ParamID::space,      f.space },
                     { ParamID::hold,       f.hold ? 1.0f : 0.0f },
                     { ParamID::octDown,    f.octDown ? 1.0f : 0.0f },
                     { ParamID::octUp,      f.octUp ? 1.0f : 0.0f },
                     { ParamID::tape,       f.tape ? 1.0f : 0.0f },
                     { ParamID::output,     f.output } };
        factory.push_back (std::move (p));
    }

    refreshUserPresets();
    setLoaded (captureValues());
}

void PresetManager::setLoaded (Values values)
{
    const juce::ScopedLock sl (listLock);
    loaded = std::move (values);
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
            if (ranged->getParameterID() != ParamID::bypass)
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
    if (voiceChanged)
        return true;
    Values loadedCopy;
    {
        const juce::ScopedLock sl (listLock);
        loadedCopy = loaded;
    }
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

    applyValues (preset.values);
    if (preset.voice.isValid())
        voices.setTree (preset.voice);
    voiceChanged = false;
    setLoaded (captureValues());
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

bool PresetManager::readPresetFile (const juce::File& file, Preset& out)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return false;
    const auto tree = juce::ValueTree::readFromStream (in);
    if (! tree.hasType (kRootTag))
        return false;

    out.name = tree.getProperty ("name", file.getFileNameWithoutExtension()).toString();
    out.file = file;
    out.values.clear();
    for (const auto& p : tree)
        if (p.hasType ("PARAM"))
            out.values[p.getProperty ("id").toString()] = static_cast<float> (p.getProperty ("value"));
    out.voice = tree.getChildWithName ("VOICE").createCopy();
    return true;
}

void PresetManager::refreshUserPresets()
{
    std::vector<Preset> found;
    for (const auto& file : getUserPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kFileExtension))
    {
        Preset p;
        if (readPresetFile (file, p))
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

    juce::ValueTree tree (kRootTag);
    tree.setProperty ("name", name, nullptr);
    tree.setProperty ("version", 1, nullptr);
    for (const auto& [id, value] : captureValues())
    {
        juce::ValueTree p ("PARAM");
        p.setProperty ("id", id, nullptr);
        p.setProperty ("value", value, nullptr);
        tree.appendChild (p, nullptr);
    }
    tree.appendChild (voices.getTree().createCopy(), nullptr);

    const auto file = folder.getChildFile (juce::File::createLegalFileName (name) + kFileExtension);
    {
        juce::FileOutputStream out (file);
        if (! out.openedOk())
            return false;
        out.setPosition (0);
        out.truncate();
        tree.writeToStream (out);
        out.flush();
        if (out.getStatus().failed())
            return false;
    }

    refreshUserPresets();
    {
        const juce::ScopedLock sl (listLock);
        for (int i = 0; i < static_cast<int> (user.size()); ++i)
            if (user[static_cast<std::size_t> (i)].file == file)
                current = static_cast<int> (factory.size()) + i;
    }
    voiceChanged = false;
    setLoaded (captureValues());
    ++changeCount;
    return true;
}

void PresetManager::toggleAB()
{
    auto& from = onB ? slotB : slotA;
    auto& to = onB ? slotA : slotB;
    from.values = captureValues();
    from.voice = voices.getTree();
    if (to.values.empty())
        to = from;
    else
    {
        applyValues (to.values);
        if (to.voice.isValid() && to.voice != from.voice)
            voices.setTree (to.voice);
    }
    onB = ! onB;
    ++changeCount;
}

void PresetManager::writeToState (juce::ValueTree& stateCopy) const
{
    stateCopy.setProperty ("preset", current.load() >= 0 ? getCurrentName() : juce::String(), nullptr);
    stateCopy.setProperty ("voiceChanged", voiceChanged, nullptr);
}

void PresetManager::readFromState (const juce::ValueTree& restored)
{
    // May be called off the message thread, so it only works out which preset is
    // current. The comparison values are filled in from the preset list.
    const auto name = restored.getProperty ("preset").toString();
    int index = -1;
    Values values;
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

    if (index >= 0)
    {
        const juce::ScopedLock sl (listLock);
        const auto& preset = index < static_cast<int> (factory.size())
                                 ? factory[static_cast<std::size_t> (index)]
                                 : user[static_cast<std::size_t> (index) - factory.size()];
        for (auto* param : state.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            {
                if (ranged->getParameterID() == ParamID::bypass)
                    continue;
                const auto it = preset.values.find (ranged->getParameterID());
                values[ranged->getParameterID()] = it != preset.values.end()
                    ? ranged->convertFrom0to1 (ranged->convertTo0to1 (it->second))
                    : ranged->convertFrom0to1 (ranged->getDefaultValue());
            }
    }
    else
    {
        values = captureValues();
    }

    voiceChanged = static_cast<bool> (restored.getProperty ("voiceChanged", false));
    setLoaded (std::move (values));
    current = index;
    ++changeCount;
}

} // namespace sing
