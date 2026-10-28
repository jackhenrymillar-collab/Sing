// sing-stretch-render: renders a voice through Sing's Stretch engine to a WAV file, so settings
// can be judged by ear before the plugin exists.
//
//   sing-stretch-render in.wav out.wav [options]
//
//   --stretch K        Stretch knob, 0 to 1 (1 is the held breath). Default 0.6
//   --semitones S      pitch of the note relative to the voice. Default 0
//   --seconds T        length of the render. Default 20
//   --loop-start S     loop start, seconds. Default: start of the file
//   --loop-length S    loop length, seconds. Default: to the end of the file
//   --fade S           loop crossfade, seconds. Default 0.25
//   --direction D      forward, backward or pingpong. Default forward
//   --hold-after S     engage Hold after S seconds
//   --grain S, --jitter S, --overlap N, --width W   override the knob's granular settings
//   --root-hz F        the voice's root pitch, for in-phase grains. Default: unpitched
//   --seed N           random seed. Default 1

#include "Stretch/GranularEngine.h"
#include "../WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sing;
using namespace sing::tools;

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: sing-stretch-render in.wav out.wav [--stretch 0..1] [--semitones S] [--seconds T]\n"
                              "       [--loop-start S] [--loop-length S] [--fade S] [--direction forward|backward|pingpong]\n"
                              "       [--hold-after S] [--grain S] [--jitter S] [--overlap N] [--width W] [--root-hz F] [--seed N]\n");
        return 2;
    }

    Audio in;
    if (! readWav (argv[1], in) || in.channels.empty() || in.channels[0].size() < 16)
    {
        std::fprintf (stderr, "could not read %s (16, 24 or 32-bit PCM, or 32-bit float WAV)\n", argv[1]);
        return 1;
    }

    float knob = 0.6f, semitones = 0.0f, seconds = 20.0f, holdAfter = -1.0f;
    double loopStart = 0.0, loopLength = -1.0, fade = 0.25;
    LoopDirection direction = LoopDirection::forward;
    float grain = -1.0f, jitter = -1.0f, width = -1.0f;
    int overlap = -1;
    uint32_t seed = 1;
    double rootHz = 0.0;

    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--stretch") knob = std::stof (v);
        else if (k == "--semitones") semitones = std::stof (v);
        else if (k == "--seconds") seconds = std::stof (v);
        else if (k == "--loop-start") loopStart = std::stod (v);
        else if (k == "--loop-length") loopLength = std::stod (v);
        else if (k == "--fade") fade = std::stod (v);
        else if (k == "--hold-after") holdAfter = std::stof (v);
        else if (k == "--grain") grain = std::stof (v);
        else if (k == "--jitter") jitter = std::stof (v);
        else if (k == "--overlap") overlap = std::stoi (v);
        else if (k == "--width") width = std::stof (v);
        else if (k == "--root-hz") rootHz = std::stod (v);
        else if (k == "--seed") seed = (uint32_t) std::stoul (v);
        else if (k == "--direction")
            direction = std::strcmp (v, "backward") == 0 ? LoopDirection::backward
                      : std::strcmp (v, "pingpong") == 0 ? LoopDirection::pingPong
                                                         : LoopDirection::forward;
        else { std::fprintf (stderr, "unknown option %s\n", k.c_str()); return 2; }
    }

    const int numSamples = (int) in.channels[0].size();
    LoopSettings loop;
    loop.start = (int) (loopStart * in.sampleRate);
    loop.length = loopLength > 0.0 ? (int) (loopLength * in.sampleRate) : numSamples - loop.start;
    loop.fade = (int) (fade * in.sampleRate);
    loop.direction = direction;
    loop.pitchPeriod = rootHz > 0.0 ? (float) (in.sampleRate / rootHz) : 0.0f;

    std::vector<const float*> ptrs;
    for (auto& c : in.channels) ptrs.push_back (c.data());
    auto source = LoopSource::build (ptrs.data(), (int) ptrs.size(), numSamples, in.sampleRate, loop);
    if (source == nullptr) { std::fprintf (stderr, "bad loop\n"); return 1; }

    const double outRate = in.sampleRate;
    GranularEngine engine;
    engine.prepare (outRate, 512);
    engine.setSeed (seed);

    auto settings = StretchKnob::toGranularSettings (knob);
    if (grain > 0.0f) settings.grainSeconds = grain;
    if (jitter >= 0.0f) settings.jitterSeconds = jitter;
    if (overlap > 0) settings.overlap = overlap;
    if (width >= 0.0f) settings.stereoWidth = width;
    engine.setSettings (settings);

    StretchParameters params;
    params.speed = StretchKnob::toSpeed (knob);
    params.pitchRatio = std::exp2 (semitones / 12.0f);
    engine.setParameters (params);
    engine.start (*source);

    const int total = (int) (seconds * outRate);
    std::vector<float> l ((size_t) total), r ((size_t) total);
    const int block = 256;
    for (int i = 0; i < total; i += block)
    {
        if (holdAfter >= 0.0f && ! params.hold && i >= (int) (holdAfter * outRate))
        {
            params.hold = true;
            engine.setParameters (params);
        }
        engine.process (l.data() + i, r.data() + i, std::min (block, total - i));
    }

    if (! writeWav (argv[2], l, r, outRate)) { std::fprintf (stderr, "could not write %s\n", argv[2]); return 1; }

    std::printf ("Rendered %.1f s: speed %.4f (1/%.1f), grain %.0f ms, jitter %.0f ms, overlap %d, loop %.2f s\n",
                 seconds, params.speed, params.speed > 0.0f ? 1.0f / params.speed : INFINITY,
                 settings.grainSeconds * 1000.0f, settings.jitterSeconds * 1000.0f, settings.overlap,
                 source->size() / in.sampleRate);
    return 0;
}
