// Renders demo WAVs with the DSP engine, without a host. Development only.
//
//   sing_render <out-dir>
//
// Writes each factory preset playing two chords, and a pitch-quality comparison: the
// Hollow Choir voice played dry two octaves down to two and a half up, once through a
// plain linear-interpolation sampler (the common cheap approach) and once through Sing.

#include "dsp/Engine.h"
#include "dsp/Halfband.h"
#include "dsp/VoiceData.h"
#include "dsp/VoiceSynth.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sing::dsp;

namespace
{
    constexpr double kFs = 48000.0;

    void writeWav (const std::filesystem::path& path, const std::vector<float>& l, const std::vector<float>& r)
    {
        std::ofstream f (path, std::ios::binary);
        auto u32 = [&] (std::uint32_t v) { f.write (reinterpret_cast<const char*> (&v), 4); };
        auto u16 = [&] (std::uint16_t v) { f.write (reinterpret_cast<const char*> (&v), 2); };
        const auto frames = static_cast<std::uint32_t> (l.size());
        const std::uint32_t bytes = frames * 2 * 3;
        f.write ("RIFF", 4); u32 (36 + bytes); f.write ("WAVE", 4);
        f.write ("fmt ", 4); u32 (16); u16 (1); u16 (2); u32 (48000); u32 (48000 * 6); u16 (6); u16 (24);
        f.write ("data", 4); u32 (bytes);
        for (std::size_t i = 0; i < l.size(); ++i)
            for (float x : { l[i], r[i] })
            {
                // TPDF dither to 24 bits.
                static std::uint32_t s = 1u;
                s = s * 1664525u + 1013904223u; const float d1 = static_cast<float> (s >> 8) / 16777216.0f;
                s = s * 1664525u + 1013904223u; const float d2 = static_cast<float> (s >> 8) / 16777216.0f;
                const double v = std::clamp (static_cast<double> (x), -1.0, 1.0) * 8388607.0 + (d1 - d2);
                const auto q = static_cast<std::int32_t> (std::clamp (std::lround (v), -8388608L, 8388607L));
                const char b[3] = { static_cast<char> (q & 0xff), static_cast<char> ((q >> 8) & 0xff), static_cast<char> ((q >> 16) & 0xff) };
                f.write (b, 3);
            }
    }

    struct Preset
    {
        const char* name;
        FactoryVoice voice;
        EngineParams p;
    };

    EngineParams make (float start, float length, float fade, int mode, float att, float rel, float str, float spr,
                       float form, float air, float space, bool down, bool up, bool tape, float outDb = 0.0f)
    {
        EngineParams p;
        p.loopStart = start; p.loopLength = length; p.loopFade = fade * 0.01f;
        p.loopMode = static_cast<LoopMode> (mode);
        p.attack = att; p.release = rel; p.stretch = str * 0.01f; p.spread = spr * 0.01f;
        p.formant = form; p.air = air; p.space = space * 0.01f;
        p.octDown = down; p.octUp = up; p.tape = tape; p.outputDb = outDb;
        return p;
    }

    // Plain linear interpolation at the output rate, the way many samplers do it.
    struct LinearReader
    {
        const std::vector<float>* s;
        float read (double pos, double) const noexcept
        {
            if (pos < 0.0 || pos >= static_cast<double> (s->size() - 1)) return 0.0f;
            const auto i = static_cast<std::size_t> (pos);
            const float f = static_cast<float> (pos - static_cast<double> (i));
            return (*s)[i] + f * ((*s)[i + 1] - (*s)[i]);
        }
    };
}

int main (int argc, char* argv[])
{
    const std::filesystem::path out = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories (out);

    // ---- presets ----
    const Preset presets[] = {
        { "Hollow Choir", FactoryVoice::hollowChoir, make (0.30f, 0.40f, 28, 2, 2.4f, 7.5f, 68, 35, -2, 3, 62, true, false, true) },
        { "Distant Ah",   FactoryVoice::distantAh,   make (0.22f, 0.36f, 35, 0, 3.8f, 9.0f, 82, 50, -4, 1, 74, true, false, false) },
        { "Glass Hum",    FactoryVoice::glassHum,    make (0.40f, 0.46f, 20, 0, 1.2f, 6.0f, 45, 28,  2, 5, 55, false, true, false) },
        { "Last Light",   FactoryVoice::lastLight,   make (0.18f, 0.34f, 40, 2, 5.0f, 12.0f, 90, 62,  0, 2, 85, false, false, true, 8.0f) },
    };
    // Two chords: A minor 9 then F major 7 (#11), voiced for a pad.
    const std::vector<int> chordA { 45, 52, 59, 60, 64 };
    const std::vector<int> chordB { 41, 48, 57, 64, 66 };
    int index = 1;
    for (const auto& pr : presets)
    {
        const auto v = synthesise (pr.voice);
        Engine e;
        e.prepare (kFs, 512);
        e.setParams (pr.p);
        e.submitVoice (VoiceData::build (v.samples.data(), v.samples.size(), v.sampleRate, v.rootNote));
        const int total = static_cast<int> (22.0 * kFs);
        std::vector<float> l (static_cast<std::size_t> (total)), r (l.size());
        for (int i = 0; i < total; i += 512)
        {
            const double t = i / kFs;
            if (i == 0) for (int n : chordA) e.noteOn (n, 0.8f);
            if (t >= 7.5 && t - 512 / kFs < 7.5) { for (int n : chordA) e.noteOff (n); for (int n : chordB) e.noteOn (n, 0.8f); }
            if (t >= 15.0 && t - 512 / kFs < 15.0) for (int n : chordB) e.noteOff (n);
            e.process (l.data() + i, r.data() + i, std::min (512, total - i));
        }
        char name[128];
        std::snprintf (name, sizeof (name), "%02d %s.wav", index++, pr.name);
        writeWav (out / name, l, r);
        std::printf ("wrote %s\n", name);
    }

    // ---- pitch quality: linear interpolation vs Sing ----
    const auto choir = synthesise (FactoryVoice::hollowChoir);
    const auto data = VoiceData::build (choir.samples.data(), choir.samples.size(), choir.sampleRate, choir.rootNote);
    const double shifts[] = { -24.0, -12.0, 12.0, 24.0, 31.0 };
    for (const double semis : shifts)
    {
        const double p = std::exp2 (semis / 12.0);
        const int n = static_cast<int> (3.5 * kFs);
        std::vector<float> lin (static_cast<std::size_t> (n)), hq (lin.size());

        // Linear: loop in original samples at the output rate.
        {
            LinearReader reader { &choir.samples };
            Loop loop;
            loop.start = 0.3 * choir.samples.size();
            loop.end = 0.7 * choir.samples.size();
            loop.fade = 0.28 * 0.5 * (loop.end - loop.start);
            LoopPlayer player;
            player.start (loop, loop.start, 1);
            for (auto& x : lin) x = player.next (reader, loop, p, 0.001f);
        }
        // Sing: the same loop in level-0 samples, read at 2x, then the half-band filter.
        {
            Loop loop;
            loop.start = 0.3 * data->getLength();
            loop.end = 0.7 * data->getLength();
            loop.fade = 0.28 * 0.5 * (loop.end - loop.start);
            LoopPlayer player;
            player.start (loop, loop.start, 1);
            std::vector<float> inter (static_cast<std::size_t> (2 * n));
            for (auto& x : inter) x = player.next (*data, loop, p * choir.sampleRate / kFs, 0.001f);
            HalfbandDecimator dec;
            dec.prepare();
            dec.process (inter.data(), hq.data(), n);
        }
        // Short fades so the clips start and end cleanly.
        for (auto* buf : { &lin, &hq })
            for (int i = 0; i < 2400; ++i)
            {
                const float g = static_cast<float> (i) / 2400.0f;
                (*buf)[static_cast<std::size_t> (i)] *= g;
                (*buf)[static_cast<std::size_t> (n - 1 - i)] *= g;
            }
        char a[128], b[128];
        std::snprintf (a, sizeof (a), "Pitch %+d st - linear (typical).wav", static_cast<int> (semis));
        std::snprintf (b, sizeof (b), "Pitch %+d st - Sing.wav", static_cast<int> (semis));
        writeWav (out / a, lin, lin);
        writeWav (out / b, hq, hq);
        std::printf ("wrote %s and %s\n", a, b);
    }
    return 0;
}
