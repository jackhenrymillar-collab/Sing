// sing-voice-render: plays notes through Sing's synth (the plugin's voices, without the
// effects) and writes the result, for comparing settings by ear.
//
//   sing-voice-render in.wav out.wav [options]
//   --note N           a MIDI note to hold (repeat for a chord). Default 60
//   --root N           the voice's root as a MIDI note. Default: detected
//   --mode M           pitch mode: formant or stft. Default formant
//   --keep-vowel 0|1   Keep vowel. Default 1
//   --formant S        Formant knob, semitones. Default 0
//   --engine E         grain or bloom. Default grain
//   --window S         Bloom's window, seconds. Default 0.68
//   --stretch K        Stretch knob, 0 to 1. Default 0
//   --spread K         Spread knob, 0 to 1. Default 0
//   --seconds T        how long the notes are held. Default 8
//   --tail T           release tail after. Default 2
//   --glide MS         Glide time. Default 0
//   --next N           halfway through, release the notes and play N instead (to hear Glide)

#include "Voice/SingSynth.h"
#include "Voice/VoiceAnalysis.h"
#include "../WavFile.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sing;
using namespace sing::tools;

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: sing-voice-render in.wav out.wav [--note N]... [--root N] [--mode formant|stft]\n"
                              "       [--keep-vowel 0|1] [--formant S] [--engine grain|bloom] [--window S]\n"
                              "       [--stretch K] [--spread K] [--seconds T] [--tail T]\n");
        return 1;
    }
    Audio in;
    if (! readWav (argv[1], in) || in.channels.empty())
    {
        std::fprintf (stderr, "could not read %s\n", argv[1]);
        return 1;
    }

    std::vector<int> notes;
    int nextNote = -1;
    float rootNote = -1.0f, seconds = 8.0f, tail = 2.0f, window = 0.68f;
    bool bloom = false;
    SynthSettings settings;
    auto& v = settings.voice;
    v.stretch = 0.0f;
    v.spread = 0.0f;
    v.attackSeconds = 0.05f;
    v.releaseSeconds = 1.5f;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const char* val = argv[i + 1];
        if (k == "--note") notes.push_back (std::stoi (val));
        else if (k == "--root") rootNote = std::stof (val);
        else if (k == "--mode") v.pitchMode = std::string (val) == "stft" ? PitchMode::stft : PitchMode::formant;
        else if (k == "--keep-vowel") v.keepVowel = std::stoi (val) != 0;
        else if (k == "--formant") v.formantSemitones = std::stof (val);
        else if (k == "--engine") bloom = std::string (val) == "bloom";
        else if (k == "--window") window = std::stof (val);
        else if (k == "--stretch") v.stretch = std::stof (val);
        else if (k == "--spread") v.spread = std::stof (val);
        else if (k == "--seconds") seconds = std::stof (val);
        else if (k == "--tail") tail = std::stof (val);
        else if (k == "--glide") v.glideSeconds = std::stof (val) * 0.001f;
        else if (k == "--next") nextNote = std::stoi (val);
        else { std::fprintf (stderr, "unknown option %s\n", k.c_str()); return 1; }
    }
    if (notes.empty())
        notes.push_back (60);

    const int n = (int) in.channels[0].size();
    std::vector<float> mono ((size_t) n);
    for (const auto& ch : in.channels)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += ch[(size_t) i] / (float) in.channels.size();
    const auto pitch = detectPitch (mono.data(), n, in.sampleRate);
    v.rootHz = rootNote >= 0.0f ? midiToHz (rootNote) : (pitch.pitched() ? pitch.hz : midiToHz (60.0f));
    v.pitched = rootNote >= 0.0f || pitch.pitched();
    std::fprintf (stderr, "root %.1f Hz (MIDI %.2f)\n", v.rootHz, hzToMidi (v.rootHz));

    LoopSettings loop;
    loop.start = 0;
    loop.length = n;
    loop.fade = (int) (0.25 * in.sampleRate);
    loop.pitchPeriod = v.pitched ? (float) (in.sampleRate / v.rootHz) : 0.0f;
    loop.analyseForBloom = bloom;
    loop.bloomWindowSeconds = window;
    loop.hostSampleRate = in.sampleRate;
    v.mode = bloom ? StretchMode::bloom : StretchMode::granular;
    loop.pitchTrack = std::make_shared<PitchTrack> (trackPitch (mono.data(), n, in.sampleRate));
    std::vector<const float*> chans;
    for (const auto& ch : in.channels)
        chans.push_back (ch.data());
    const auto source = LoopSource::build (chans.data(), (int) chans.size(), n, in.sampleRate, loop);

    const double rate = in.sampleRate;
    constexpr int block = 256;
    SingSynth synth;
    synth.prepare (rate, block);
    synth.setSource (source.get());
    settings.polyphony = 16;
    synth.setSettings (settings);
    for (int note : notes)
        synth.noteOn (note, 0.8f);

    const auto held = (size_t) (seconds * rate), total = (size_t) ((seconds + tail) * rate);
    const auto half = nextNote >= 0 ? held / 2 : total + 1;
    std::vector<float> l (total), r (total);
    for (size_t pos = 0; pos < total; pos += block)
    {
        if (pos <= half && pos + block > half)
        {
            for (int note : notes)
                synth.noteOff (note);
            notes.assign (1, nextNote);
            synth.noteOn (nextNote, 0.8f);
        }
        if (pos <= held && pos + block > held)
            for (int note : notes)
                synth.noteOff (note);
        const int len = (int) std::min ((size_t) block, total - pos);
        synth.render (l.data() + pos, r.data() + pos, len);
    }
    if (! writeWav (argv[2], l, r, rate)) { std::fprintf (stderr, "could not write %s\n", argv[2]); return 1; }
    return 0;
}
