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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sing;

namespace
{
struct Audio
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;
};

uint32_t u32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
uint16_t u16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

bool readWav (const std::string& path, Audio& out)
{
    std::ifstream f (path, std::ios::binary);
    std::vector<unsigned char> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
        return false;

    int format = 0, numChannels = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size())
    {
        const uint32_t size = u32 (d.data() + pos + 4);
        const unsigned char* body = d.data() + pos + 8;
        if (pos + 8 + size > d.size())
            break;

        if (std::memcmp (d.data() + pos, "fmt ", 4) == 0)
        {
            format = u16 (body);
            numChannels = u16 (body + 2);
            out.sampleRate = (double) u32 (body + 4);
            bits = u16 (body + 14);
            if (format == 0xFFFE && size >= 26)
                format = u16 (body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
        }
        else if (std::memcmp (d.data() + pos, "data", 4) == 0 && numChannels > 0)
        {
            const int bytes = bits / 8;
            const size_t frames = size / (size_t) (bytes * numChannels);
            out.channels.assign ((size_t) numChannels, std::vector<float> (frames));
            for (size_t i = 0; i < frames; ++i)
                for (int c = 0; c < numChannels; ++c)
                {
                    const unsigned char* s = body + (i * (size_t) numChannels + (size_t) c) * (size_t) bytes;
                    float v = 0.0f;
                    if (format == 3 && bits == 32) std::memcpy (&v, s, 4);
                    else if (format == 1 && bits == 16) v = (float) (int16_t) u16 (s) / 32768.0f;
                    else if (format == 1 && bits == 24) v = (float) ((int32_t) (u32 (s - 1) & 0xFFFFFF00u) >> 8) / 8388608.0f;
                    else if (format == 1 && bits == 32) v = (float) ((double) (int32_t) u32 (s) / 2147483648.0);
                    else return false;
                    out.channels[(size_t) c][i] = v;
                }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

bool writeWav (const std::string& path, const std::vector<float>& l, const std::vector<float>& r, double sampleRate)
{
    std::ofstream f (path, std::ios::binary);
    auto put32 = [&] (uint32_t v) { for (int i = 0; i < 4; ++i) f.put ((char) ((v >> (8 * i)) & 0xFF)); };
    auto put16 = [&] (uint16_t v) { f.put ((char) (v & 0xFF)); f.put ((char) (v >> 8)); };
    const uint32_t dataBytes = (uint32_t) (l.size() * 2 * 4);
    f.write ("RIFF", 4); put32 (36 + dataBytes); f.write ("WAVE", 4);
    f.write ("fmt ", 4); put32 (16); put16 (3); put16 (2); put32 ((uint32_t) sampleRate);
    put32 ((uint32_t) sampleRate * 8); put16 (8); put16 (32);
    f.write ("data", 4); put32 (dataBytes);
    for (size_t i = 0; i < l.size(); ++i)
    {
        f.write (reinterpret_cast<const char*> (&l[i]), 4);
        f.write (reinterpret_cast<const char*> (&r[i]), 4);
    }
    return (bool) f;
}
} // namespace

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
