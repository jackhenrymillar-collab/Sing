#pragma once

// WAV reading and writing for the offline tools.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sing::tools
{
struct Audio
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;
};

inline uint32_t u32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t u16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

inline bool readWav (const std::string& path, Audio& out)
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

inline bool writeWav (const std::string& path, const std::vector<float>& l, const std::vector<float>& r, double sampleRate)
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
} // namespace sing::tools
