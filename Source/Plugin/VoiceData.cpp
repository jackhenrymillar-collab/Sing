#include "VoiceData.h"

#include <mutex>

#include "Voice/VoiceAnalysis.h"

namespace sing
{

namespace
{
    juce::int64 nextId()
    {
        static std::atomic<juce::int64> counter { 1 };
        return counter++;
    }

    struct PlaceholderInfo { PlaceholderVowel vowel; const char* key; const char* name; const char* description; };

    constexpr PlaceholderInfo kPlaceholders[] = {
        { PlaceholderVowel::ooh,    "ooh",    "Hollow Ooh",  "A breathy \"ooh\" (stand-in until the recorded voices arrive)" },
        { PlaceholderVowel::ah,     "ah",     "Soft Ah",     "A soft \"ah\" (stand-in until the recorded voices arrive)" },
        { PlaceholderVowel::hum,    "hum",    "Low Hum",     "A hummed note (stand-in until the recorded voices arrive)" },
        { PlaceholderVowel::breath, "breath", "Two Breaths", "Two slow breaths (stand-in until the recorded voices arrive)" },
    };

    const PlaceholderInfo& infoFor (PlaceholderVowel v)
    {
        for (const auto& p : kPlaceholders)
            if (p.vowel == v)
                return p;
        return kPlaceholders[0];
    }
}

juce::String noteName (int midiNote)
{
    return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 3);
}

VoiceData::Ptr VoiceData::fromAudio (juce::AudioBuffer<float> buffer, double rate, const juce::String& voiceName,
                                     const juce::String& voiceDescription)
{
    std::shared_ptr<VoiceData> v (new VoiceData());
    v->audio = std::move (buffer);
    v->sampleRate = rate;
    v->name = voiceName;
    v->description = voiceDescription;
    v->analyse();
    return v;
}

VoiceData::Ptr VoiceData::fromPlaceholder (PlaceholderVowel vowel)
{
    // Making a placeholder takes a moment, and hosts can switch presets many times a second,
    // so each one is made once and shared. Voices are immutable, so sharing is safe.
    static std::mutex lock;
    static std::array<Ptr, std::size (kPlaceholders)> made;
    const auto index = (size_t) (&infoFor (vowel) - kPlaceholders);
    const std::scoped_lock hold (lock);
    if (made[index] == nullptr)
        made[index] = makePlaceholder (vowel);
    return made[index];
}

VoiceData::Ptr VoiceData::makePlaceholder (PlaceholderVowel vowel)
{
    const auto p = makePlaceholderVoice (vowel, 48000.0, 5.0f);
    juce::AudioBuffer<float> buffer (1, (int) p.samples.size());
    buffer.copyFrom (0, 0, p.samples.data(), (int) p.samples.size());

    const auto& info = infoFor (vowel);
    std::shared_ptr<VoiceData> v (new VoiceData());
    v->audio = std::move (buffer);
    v->sampleRate = p.sampleRate;
    v->name = info.name;
    v->description = info.description;
    v->placeholder = vowel;
    v->analyse();
    return v;
}

VoiceData::Ptr VoiceData::fromFile (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Sing can't read " + file.getFileName() + ". Try a WAV or AIFF file.";
        return nullptr;
    }
    if (reader->sampleRate < 8000.0 || reader->lengthInSamples < 1024)
    {
        error = file.getFileName() + " is too short to use.";
        return nullptr;
    }

    const auto maxLength = (juce::int64) (maxSeconds * reader->sampleRate);
    const int length = (int) std::min (reader->lengthInSamples, maxLength);
    const int channels = (int) std::min (reader->numChannels, 2u);
    juce::AudioBuffer<float> buffer (channels, length);
    reader->read (&buffer, 0, length, 0, true, channels > 1);

    juce::String description = juce::String (reader->lengthInSamples > maxLength ? "First 60 s of " : "") + file.getFileName();
    return fromAudio (std::move (buffer), reader->sampleRate, file.getFileNameWithoutExtension(), description);
}

void VoiceData::analyse()
{
    id = nextId();
    const int n = audio.getNumSamples();

    // Analyse a mono mix.
    std::vector<float> mono ((size_t) n);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const float* x = audio.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += x[i] / (float) audio.getNumChannels();
    }

    const auto range = findAudibleRange (mono.data(), n, sampleRate);
    audibleStart = range.start;
    audibleEnd = std::max (range.start + std::min (n, 1024), range.end);

    const auto pitch = detectPitch (mono.data() + audibleStart, audibleEnd - audibleStart, sampleRate);
    detectedHz = pitch.hz;
    pitchTrack = std::make_shared<PitchTrack> (trackPitch (mono.data(), n, sampleRate));
    computePeaks();
}

void VoiceData::computePeaks()
{
    const int from = trimStart(), len = std::max (1, trimLength());
    float loudest = 0.0f;
    for (int p = 0; p < numPeaks; ++p)
    {
        const int a = from + (int) ((juce::int64) len * p / numPeaks);
        const int b = std::max (a + 1, from + (int) ((juce::int64) len * (p + 1) / numPeaks));
        float peak = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            peak = std::max (peak, audio.getMagnitude (ch, a, std::min (b, audio.getNumSamples()) - a));
        peaks[(size_t) p] = peak;
        loudest = std::max (loudest, peak);
    }
    if (loudest > 0.0f)
        for (auto& p : peaks)
            p /= loudest;
}

float VoiceData::rootHz() const noexcept
{
    if (rootOverride)
        return midiToHz ((float) *rootOverride);
    return detectedHz > 0.0f ? detectedHz : midiToHz (60.0f);
}

VoiceData::Ptr VoiceData::withRootOverride (std::optional<int> midiNote) const
{
    std::shared_ptr<VoiceData> v (new VoiceData (*this));
    v->rootOverride = midiNote;
    v->id = nextId();
    return v;
}

VoiceData::Ptr VoiceData::withTrim (bool trim) const
{
    std::shared_ptr<VoiceData> v (new VoiceData (*this));
    v->trimSilence = trim;
    v->id = nextId();
    v->computePeaks();
    return v;
}

//==============================================================================
juce::ValueTree VoiceData::toValueTree() const
{
    juce::ValueTree tree ("VOICE");
    tree.setProperty ("name", name, nullptr);
    tree.setProperty ("description", description, nullptr);
    tree.setProperty ("trim", trimSilence, nullptr);
    tree.setProperty ("root", rootOverride ? *rootOverride : -1, nullptr);

    if (placeholder)
    {
        tree.setProperty ("placeholder", infoFor (*placeholder).key, nullptr);
        return tree;
    }

    // The recording itself, as 24-bit FLAC: lossless and about half the size of WAV.
    juce::MemoryBlock flac;
    {
        juce::FlacAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (flac, false);
        auto options = juce::AudioFormatWriterOptions()
                           .withSampleRate (sampleRate)
                           .withNumChannels (audio.getNumChannels())
                           .withBitsPerSample (24);
        if (auto writer = format.createWriterFor (stream, options))
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    tree.setProperty ("flac", flac.toBase64Encoding(), nullptr);
    tree.setProperty ("detectedHz", detectedHz, nullptr);
    return tree;
}

VoiceData::Ptr VoiceData::fromValueTree (const juce::ValueTree& tree)
{
    if (! tree.hasType ("VOICE"))
        return nullptr;

    Ptr base;
    const auto key = tree.getProperty ("placeholder").toString();
    if (key.isNotEmpty())
    {
        for (const auto& p : kPlaceholders)
            if (key == p.key)
                base = fromPlaceholder (p.vowel);
    }
    else
    {
        juce::MemoryBlock data;
        if (! data.fromBase64Encoding (tree.getProperty ("flac").toString()))
            return nullptr;

        juce::FlacAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (
            format.createReaderFor (new juce::MemoryInputStream (data, false), true));
        if (reader == nullptr)
            return nullptr;

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        base = fromAudio (std::move (buffer), reader->sampleRate, tree.getProperty ("name").toString(),
                          tree.getProperty ("description").toString());
    }

    if (base == nullptr)
        return nullptr;

    std::shared_ptr<VoiceData> v (new VoiceData (*base));
    v->name = tree.getProperty ("name", v->name).toString();
    v->description = tree.getProperty ("description", v->description).toString();
    v->trimSilence = (bool) tree.getProperty ("trim", true);
    const int root = tree.getProperty ("root", -1);
    v->rootOverride = root >= 0 ? std::optional<int> (root) : std::nullopt;
    v->id = nextId();
    v->computePeaks();
    return v;
}

} // namespace sing
