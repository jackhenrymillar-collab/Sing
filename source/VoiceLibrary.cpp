#include "VoiceLibrary.h"

#include "dsp/VoiceData.h"
#include "dsp/VoiceSynth.h"

namespace sing
{

namespace
{
    constexpr auto kTag = "VOICE";
    constexpr double kMaxSeconds = 60.0;
    constexpr int kFinePeaks = 1920; // peak outline kept per voice; the panel asks for fewer

    struct FactoryEntry
    {
        const char* id;
        const char* name;
        const char* description;
        dsp::FactoryVoice voice;
    };

    constexpr FactoryEntry kFactory[] = {
        { "hollow-choir", "Hollow Choir", "Breathy \"ooh\" from a single alto, stacked into a choir", dsp::FactoryVoice::hollowChoir },
        { "distant-ah",   "Distant Ah",   "A soft tenor \"ah\", slowed and smeared into a warm bed",   dsp::FactoryVoice::distantAh },
        { "glass-hum",    "Glass Hum",    "A hummed note with a long, glassy shimmer on top",          dsp::FactoryVoice::glassHum },
        { "last-light",   "Last Light",   "Two breaths, played backwards and forwards into a slow swell", dsp::FactoryVoice::lastLight },
    };

    const FactoryEntry* findFactory (const juce::String& id)
    {
        for (const auto& f : kFactory)
            if (id == f.id)
                return &f;
        return nullptr;
    }

    std::vector<float> finePeaks (const std::vector<float>& s)
    {
        std::vector<float> peaks (kFinePeaks, 0.0f);
        if (s.empty())
            return peaks;
        float top = 1.0e-9f;
        for (std::size_t p = 0; p < peaks.size(); ++p)
        {
            const auto a = s.size() * p / peaks.size();
            const auto b = std::max (a + 1, s.size() * (p + 1) / peaks.size());
            float m = 0.0f;
            for (auto i = a; i < b && i < s.size(); ++i)
                m = std::max (m, std::abs (s[i]));
            peaks[p] = m;
            top = std::max (top, m);
        }
        for (auto& p : peaks)
            p /= top;
        return peaks;
    }

    // The voice's audio, compressed losslessly for sessions and presets.
    juce::MemoryBlock encode (const std::vector<float>& s, double rate, juce::String& format)
    {
        juce::MemoryBlock block;
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
            juce::FlacAudioFormat flac;
            auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                            .withSampleRate (rate)
                                                            .withNumChannels (1)
                                                            .withBitsPerSample (24)
                                                            .withQualityOptionIndex (5));
            if (writer != nullptr)
            {
                const float* chans[] = { s.data() };
                if (writer->writeFromFloatArrays (chans, 1, static_cast<int> (s.size())))
                {
                    writer.reset();
                    format = "flac";
                    return block;
                }
            }
        }
        // FLAC can't take every sample rate: keep the raw floats instead.
        block.reset();
        block.append (s.data(), s.size() * sizeof (float));
        format = "f32";
        return block;
    }

    bool decode (const juce::ValueTree& tree, std::vector<float>& out, double& rate)
    {
        const auto* block = tree.getProperty ("audio").getBinaryData();
        if (block == nullptr || block->getSize() == 0)
            return false;
        rate = static_cast<double> (tree.getProperty ("rate", 48000.0));
        if (tree.getProperty ("format").toString() == "f32")
        {
            out.resize (block->getSize() / sizeof (float));
            std::memcpy (out.data(), block->getData(), out.size() * sizeof (float));
            return ! out.empty();
        }
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatReader> reader (
            flac.createReaderFor (new juce::MemoryInputStream (*block, false), true));
        if (reader == nullptr)
            return false;
        rate = reader->sampleRate;
        const auto n = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples, static_cast<juce::int64> (kMaxSeconds * rate)));
        juce::AudioBuffer<float> buffer (1, n);
        reader->read (&buffer, 0, n, 0, true, false);
        out.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
        return n > 0;
    }
}

struct VoiceLibrary::Prepared
{
    juce::ValueTree tree;
    VoiceInfo info;
    std::vector<float> peaks;
    std::unique_ptr<dsp::VoiceData> data;
    int generation = 0;
};

VoiceLibrary::VoiceLibrary() = default;

VoiceLibrary::~VoiceLibrary()
{
    alive->store (false);
    pool.removeAllJobs (true, 10000);
}

juce::StringArray VoiceLibrary::factoryIds()
{
    juce::StringArray ids;
    for (const auto& f : kFactory)
        ids.add (f.id);
    return ids;
}

juce::String VoiceLibrary::factoryName (const juce::String& id)
{
    const auto* f = findFactory (id);
    return f != nullptr ? juce::String (f->name) : juce::String();
}

juce::ValueTree VoiceLibrary::factoryTree (const juce::String& id)
{
    juce::ValueTree t (kTag);
    t.setProperty ("source", "factory", nullptr);
    t.setProperty ("id", id, nullptr);
    return t;
}

juce::ValueTree VoiceLibrary::getTree() const
{
    const juce::ScopedLock sl (lock);
    return tree.isValid() ? tree : factoryTree (kFactory[0].id);
}

VoiceLibrary::Audio VoiceLibrary::renderFactory (const juce::String& id)
{
    const auto* f = findFactory (id);
    const auto v = dsp::synthesise (f != nullptr ? f->voice : dsp::FactoryVoice::hollowChoir);
    return { v.samples, v.sampleRate };
}

void VoiceLibrary::setTree (const juce::ValueTree& t, bool wait)
{
    if (! t.isValid() || ! t.hasType (kTag))
        return;
    const int gen = ++generation;
    if (wait)
    {
        ++jobsRunning;
        prepare (t.createCopy(), gen, true);
        return;
    }
    ++jobsRunning;
    pool.addJob ([this, copy = t.createCopy(), gen, flag = alive]
    {
        if (flag->load())
            prepare (copy, gen, false);
        else
            --jobsRunning;
    });
}

void VoiceLibrary::prepare (juce::ValueTree t, int gen, bool sync)
{
    auto result = std::make_shared<Prepared>();
    result->generation = gen;

    std::vector<float> samples;
    double rate = 48000.0;
    float root = 60.0f;
    if (t.getProperty ("source").toString() == "factory")
    {
        const auto* f = findFactory (t.getProperty ("id").toString());
        if (f == nullptr)
            f = &kFactory[0];
        const auto v = dsp::synthesise (f->voice);
        samples = v.samples;
        rate = v.sampleRate;
        root = v.rootNote;
        result->info = { f->name, f->description, 0.0, root, true };
    }
    else
    {
        if (! decode (t, samples, rate))
        {
            --jobsRunning;
            return;
        }
        root = static_cast<float> (t.getProperty ("root", 60.0f));
        result->info = { t.getProperty ("name", "Voice").toString(), {}, 0.0, root, false };
    }

    // Generation check before the slow part: a newer request makes this one pointless.
    if (generation.load() != gen)
    {
        --jobsRunning;
        return;
    }

    result->info.lengthSeconds = static_cast<double> (samples.size()) / rate;
    result->peaks = finePeaks (samples);
    result->data = dsp::VoiceData::build (samples.data(), samples.size(), rate, root);
    result->tree = t;

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (sync || mm == nullptr || mm->isThisTheMessageThread())
    {
        finish (result);
        return;
    }
    juce::MessageManager::callAsync ([this, result, flag = alive]
    {
        if (flag->load())
            finish (result);
    });
}

void VoiceLibrary::finish (std::shared_ptr<Prepared> p)
{
    --jobsRunning;
    if (p->generation != generation.load() || p->data == nullptr)
        return;
    {
        const juce::ScopedLock sl (lock);
        tree = p->tree;
        info = p->info;
        peaks = std::move (p->peaks);
    }
    if (onReady)
        onReady (std::move (p->data));
}

bool VoiceLibrary::loadFile (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Sing can't read " + file.getFileName() + ". Try a WAV, AIFF or FLAC file.";
        return false;
    }
    const double rate = reader->sampleRate;
    if (rate < 8000.0 || rate > 384000.0)
    {
        error = "That file's sample rate isn't supported.";
        return false;
    }
    const auto n = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples, static_cast<juce::int64> (kMaxSeconds * rate)));
    const int channels = static_cast<int> (std::max (1u, reader->numChannels));
    juce::AudioBuffer<float> buffer (channels, std::max (1, n));
    buffer.clear();
    reader->read (&buffer, 0, n, 0, true, true);

    // Sing plays one voice in mono and spreads it itself.
    std::vector<float> mono (static_cast<std::size_t> (n), 0.0f);
    for (int c = 0; c < channels; ++c)
    {
        const float* src = buffer.getReadPointer (c);
        for (int i = 0; i < n; ++i)
            mono[static_cast<std::size_t> (i)] += src[i] / static_cast<float> (channels);
    }
    dsp::trimSilence (mono, rate);
    if (static_cast<double> (mono.size()) < 0.1 * rate)
    {
        error = "That file is too short (or silent) to make a pad from.";
        return false;
    }

    float root = dsp::detectPitch (mono.data(), mono.size(), rate);
    if (root < 0.0f)
        root = 60.0f; // no clear pitch (a breath or a whisper): treat it as middle C

    juce::String format;
    const auto audio = encode (mono, rate, format);
    juce::ValueTree t (kTag);
    t.setProperty ("source", "user", nullptr);
    t.setProperty ("name", file.getFileNameWithoutExtension(), nullptr);
    t.setProperty ("root", root, nullptr);
    t.setProperty ("rate", rate, nullptr);
    t.setProperty ("format", format, nullptr);
    t.setProperty ("audio", juce::var (audio), nullptr);
    setTree (t);
    return true;
}

VoiceInfo VoiceLibrary::getInfo() const
{
    const juce::ScopedLock sl (lock);
    return info;
}

std::vector<float> VoiceLibrary::getWaveform (int points) const
{
    const juce::ScopedLock sl (lock);
    std::vector<float> out (static_cast<std::size_t> (std::max (1, points)), 0.0f);
    if (peaks.empty())
        return out;
    for (std::size_t p = 0; p < out.size(); ++p)
    {
        const auto a = peaks.size() * p / out.size();
        const auto b = std::max (a + 1, peaks.size() * (p + 1) / out.size());
        float m = 0.0f;
        for (auto i = a; i < b && i < peaks.size(); ++i)
            m = std::max (m, peaks[i]);
        out[p] = m;
    }
    return out;
}

} // namespace sing
