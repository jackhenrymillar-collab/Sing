#include "PresetManager.h"
#include "Parameters.h"

#include <algorithm>

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
        // The choir modules (presets that leave them out keep them off).
        struct Choir
        {
            int singers = 0;                                               // Ensemble: 0 off, else 2 to 8
            float detune = 10.0f, vibrato = 40.0f, width = 70.0f;
            float halo = 0.0f, haloDrift = 20.0f;                          // Halo: level 0 off
            int haloOctave = 1;                                            // 0 down, 1 same, 2 up
            float vowelDepth = 0.0f, vowel = 50.0f, breathe = 25.0f;       // Vowel: depth 0 off
            int harmonyAbove = 0, harmonyBelow = 0, harmonyKey = 0, harmonyScale = 0;   // Harmoniser: both Off is off
        } choir {};
    };

    // The four presets from the mockup, two for Bloom, one that moves, and ten on voices that change
    // across the recording. Their voices are generated stand-ins until the recorded ones exist.
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
        // Voices that move: long loops, mostly back and forth, and gentler Stretch than the first
        // presets, so the glide, swell or sweep unfolds over tens of seconds instead of freezing. Air
        // is off: its breath layer would put back the noise these voices leave out.
        //  name             voice                          attack release stretch spread formant  start length fade  dir engine air    space  orbit spread/formant, wander
        { "Opening Petal",  PlaceholderVowel::openingOoh,    2.5f,  9.0f,  30.0f, 40.0f,  0.0f,    8.0f, 84.0f, 30.0f, 2,  0,      0.0f, 45.0f },
        { "Brightening",    PlaceholderVowel::risingEe,      3.5f, 11.0f,  35.0f, 50.0f, -2.0f,   10.0f, 80.0f, 30.0f, 2,  1,      0.0f, 65.0f },
        { "Messa di Voce",  PlaceholderVowel::swellingAlto,  2.0f,  8.0f,  25.0f, 30.0f,  0.0f,    5.0f, 85.0f, 25.0f, 2,  0,      0.0f, 40.0f },
        { "Found Voice",    PlaceholderVowel::wakingAh,      3.0f, 10.0f,  30.0f, 45.0f,  0.0f,    3.0f, 85.0f, 35.0f, 2,  0,      0.0f, 60.0f },
        { "Chapel Six",     PlaceholderVowel::smallChoir,    3.0f, 12.0f,  70.0f, 55.0f, -1.0f,   15.0f, 70.0f, 30.0f, 0,  1,      0.0f, 75.0f,   0.0f,  0.0f, 25.0f },
        { "Om Field",       PlaceholderVowel::lowOm,         2.5f, 10.0f,  35.0f, 35.0f,  1.0f,   10.0f, 80.0f, 30.0f, 2,  0,      0.0f, 55.0f,   0.0f,  0.0f, 30.0f },
        { "Glass Soprano",  PlaceholderVowel::highEe,        4.0f, 12.0f,  75.0f, 60.0f,  0.0f,   20.0f, 60.0f, 35.0f, 0,  1,      0.0f, 80.0f,  30.0f,  0.0f,  0.0f },
        { "Tidal Wah",      PlaceholderVowel::slowWah,       1.5f,  8.0f,  15.0f, 30.0f,  0.0f,   12.5f, 75.0f, 20.0f, 0,  0,      0.0f, 45.0f },
        { "Throat Song",    PlaceholderVowel::overtoneDrone, 2.0f,  9.0f,  25.0f, 25.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 50.0f },
        { "Evening Sigh",   PlaceholderVowel::fallingSigh,   3.5f, 12.0f,  40.0f, 45.0f,  0.0f,   40.0f, 50.0f, 35.0f, 0,  1,      0.0f, 70.0f,   0.0f, 20.0f, 30.0f },
        // Synths: the filter or the waveshape moves, so gentle Stretch lets the sweep be heard.
        //  name              voice                          attack release stretch spread formant  start length fade  dir engine air    space  orbit spread/formant, wander
        { "Filter Tide",     PlaceholderVowel::sweepingSaw,   1.5f,  8.0f,  25.0f, 40.0f,  0.0f,    2.0f, 96.0f, 20.0f, 2,  0,      0.0f, 45.0f },
        { "String Machine",  PlaceholderVowel::pwmStrings,    2.0f,  9.0f,  20.0f, 50.0f,  0.0f,   15.0f, 75.0f, 30.0f, 0,  0,      0.0f, 55.0f },
        { "Shape Shifter",   PlaceholderVowel::morphPad,      2.5f, 10.0f,  35.0f, 45.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  1,      0.0f, 60.0f },
        { "Wobble Floor",    PlaceholderVowel::wobbleBass,    0.2f,  4.0f,  10.0f, 15.0f,  0.0f,    5.0f, 90.0f, 10.0f, 0,  0,      0.0f, 25.0f },
        { "Sync Glider",     PlaceholderVowel::syncSweep,     1.0f,  8.0f,  25.0f, 35.0f,  0.0f,    2.0f, 96.0f, 20.0f, 2,  0,      0.0f, 45.0f },
        { "Rising Wall",     PlaceholderVowel::supersawRise,  3.0f, 10.0f,  30.0f, 55.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  1,      0.0f, 65.0f },
        { "FM Tides",        PlaceholderVowel::fmMorph,       1.5f,  9.0f,  30.0f, 35.0f,  0.0f,    2.0f, 96.0f, 20.0f, 2,  0,      0.0f, 50.0f },
        { "Folded Light",    PlaceholderVowel::wavefoldDrone, 2.0f,  9.0f,  25.0f, 40.0f,  0.0f,    2.0f, 96.0f, 20.0f, 2,  0,      0.0f, 55.0f },
        { "Jet Stream",      PlaceholderVowel::flangedSaw,    1.5f,  8.0f,  20.0f, 35.0f,  0.0f,    2.0f, 96.0f, 20.0f, 2,  0,      0.0f, 45.0f },
        { "Robot Choir",     PlaceholderVowel::talkingSquare, 1.0f,  8.0f,  15.0f, 30.0f,  0.0f,    2.0f, 96.0f, 15.0f, 0,  0,      0.0f, 50.0f },
        // Instruments. The struck and plucked ones start fast so the attack is heard.
        { "Cello Bows",      PlaceholderVowel::cello,         1.0f,  8.0f,  25.0f, 30.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 45.0f },
        { "Silk Strings",    PlaceholderVowel::violinSection, 2.5f, 10.0f,  40.0f, 50.0f,  0.0f,   15.0f, 70.0f, 30.0f, 0,  1,      0.0f, 65.0f },
        { "Night Flute",     PlaceholderVowel::flute,         1.0f,  8.0f,  25.0f, 30.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 55.0f },
        { "Low Reed",        PlaceholderVowel::clarinet,      1.0f,  8.0f,  25.0f, 30.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 45.0f },
        { "Horn Swell",      PlaceholderVowel::frenchHorn,    1.5f,  9.0f,  25.0f, 35.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 55.0f },
        { "Organ Stops",     PlaceholderVowel::churchOrgan,   0.5f,  6.0f,  20.0f, 25.0f,  0.0f,    2.0f, 96.0f, 15.0f, 2,  0,      0.0f, 60.0f },
        { "Piano Haze",      PlaceholderVowel::grandPiano,    0.02f, 10.0f, 45.0f, 40.0f,  0.0f,    0.0f, 60.0f, 30.0f, 0,  1,      0.0f, 65.0f },
        { "Vibes After Dark", PlaceholderVowel::vibraphone,   0.01f, 8.0f,  30.0f, 30.0f,  0.0f,    0.0f, 70.0f, 30.0f, 2,  0,      0.0f, 55.0f },
        { "Harp Echoes",     PlaceholderVowel::harp,          0.01f, 8.0f,  30.0f, 35.0f,  0.0f,    0.0f, 95.0f, 20.0f, 0,  0,      0.0f, 55.0f },
        { "Musette",         PlaceholderVowel::accordion,     1.0f,  8.0f,  20.0f, 30.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 45.0f },
        // The choir modules: a crowd of singers on one note, a frozen halo under it, a vowel laid
        // over a hum, harmony notes that follow the key, and all of them at once.
        { "Eight Voices",   PlaceholderVowel::ooh,           2.5f,  9.0f,  50.0f, 30.0f,  0.0f,   30.0f, 40.0f, 28.0f, 0,  0,      0.0f, 55.0f,   0.0f,  0.0f,  0.0f,
          { .singers = 8, .detune = 14.0f, .vibrato = 45.0f, .width = 90.0f } },
        { "Low Halo",       PlaceholderVowel::ah,            3.0f, 11.0f,  80.0f, 45.0f, -2.0f,   22.0f, 36.0f, 35.0f, 0,  1,      0.0f, 70.0f,   0.0f,  0.0f,  0.0f,
          { .halo = 60.0f, .haloDrift = 30.0f, .haloOctave = 0 } },
        { "Mouth Music",    PlaceholderVowel::overtoneDrone, 2.0f,  8.0f,  25.0f, 25.0f,  0.0f,    5.0f, 90.0f, 25.0f, 2,  0,      0.0f, 40.0f,   0.0f,  0.0f,  0.0f,
          { .vowelDepth = 80.0f, .vowel = 40.0f, .breathe = 60.0f } },
        { "Minor Thirds",   PlaceholderVowel::swellingAlto,  2.0f,  8.0f,  25.0f, 30.0f,  0.0f,    5.0f, 85.0f, 25.0f, 2,  0,      0.0f, 50.0f,   0.0f,  0.0f,  0.0f,
          { .harmonyAbove = 1, .harmonyKey = 9, .harmonyScale = 1 } },
        { "Whole Cathedral", PlaceholderVowel::smallChoir,   3.0f, 12.0f,  70.0f, 55.0f, -1.0f,   15.0f, 70.0f, 30.0f, 0,  1,      0.0f, 75.0f,   0.0f,  0.0f, 25.0f,
          { .singers = 5, .detune = 10.0f, .halo = 35.0f, .harmonyBelow = 5 } },
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
                     { ParamID::wander,       f.wander },
                     { ParamID::ensembleOn,      f.choir.singers >= 2 ? 1.0f : 0.0f },
                     { ParamID::ensembleSingers, static_cast<float> (std::max (2, f.choir.singers)) },
                     { ParamID::ensembleDetune,  f.choir.detune },
                     { ParamID::ensembleVibrato, f.choir.vibrato },
                     { ParamID::ensembleWidth,   f.choir.width },
                     { ParamID::haloOn,          f.choir.halo > 0.0f ? 1.0f : 0.0f },
                     { ParamID::haloLevel,       f.choir.halo > 0.0f ? f.choir.halo : 50.0f },
                     { ParamID::haloDrift,       f.choir.haloDrift },
                     { ParamID::haloOctave,      static_cast<float> (f.choir.haloOctave) },
                     { ParamID::vowelOn,         f.choir.vowelDepth > 0.0f ? 1.0f : 0.0f },
                     { ParamID::vowelShape,      f.choir.vowel },
                     { ParamID::vowelDepth,      f.choir.vowelDepth > 0.0f ? f.choir.vowelDepth : 60.0f },
                     { ParamID::vowelBreathe,    f.choir.breathe },
                     { ParamID::harmonyOn,       f.choir.harmonyAbove > 0 || f.choir.harmonyBelow > 0 ? 1.0f : 0.0f },
                     { ParamID::harmonyKey,      static_cast<float> (f.choir.harmonyKey) },
                     { ParamID::harmonyScale,    static_cast<float> (f.choir.harmonyScale) },
                     { ParamID::harmonyAbove,    static_cast<float> (f.choir.harmonyAbove) },
                     { ParamID::harmonyBelow,    static_cast<float> (f.choir.harmonyBelow) } };
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
