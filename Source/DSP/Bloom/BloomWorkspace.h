#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sing
{

// The buffers one Bloom note works in: its frames, its spectrum and its running sums. They
// grow with the window (about 80 MB for a 40 s window), far too much to keep for every voice
// at the longest window, so each loop's analysis makes a set sized for its own window when
// the loop is built (off the audio thread). A note claims one when it starts and hands it
// back when it ends.
struct BloomWorkspace
{
    static constexpr int maxOverlap = 4;

    int size = 0;       // synthesis FFT size
    int overlap = 2;    // frames sounding at once: 2 classic, 4 with locked harmonics

    std::vector<float> spectrumLeft, spectrumRight;               // size + 2: complex bins
    std::vector<double> prefixLeft, prefixRight;                  // size / 2 + 2: running power sums
    std::array<std::vector<float>, maxOverlap + 1> framesLeft, framesRight;
    std::vector<uint32_t> phases;                                 // locked harmonics only: per layer and bin

    std::atomic<bool> inUse { false };

    BloomWorkspace (int fftSize, int frameOverlap, int layers, bool locked)
        : size (fftSize), overlap (frameOverlap)
    {
        spectrumLeft.assign ((size_t) size + 2, 0.0f);
        spectrumRight.assign ((size_t) size + 2, 0.0f);
        prefixLeft.assign ((size_t) size / 2 + 2, 0.0);
        prefixRight.assign ((size_t) size / 2 + 2, 0.0);
        for (int i = 0; i <= overlap; ++i)
        {
            framesLeft[(size_t) i].assign ((size_t) size, 0.0f);
            framesRight[(size_t) i].assign ((size_t) size, 0.0f);
        }
        if (locked)
            phases.assign ((size_t) layers * (size_t) (size / 2 + 1), 0u);
    }

    static size_t bytesFor (int fftSize, int frameOverlap, int layers, bool locked) noexcept
    {
        const auto n = (size_t) fftSize;
        size_t b = 2 * (n + 2) * sizeof (float) + 2 * (n / 2 + 2) * sizeof (double)
                 + 2 * (size_t) (frameOverlap + 1) * n * sizeof (float);
        if (locked)
            b += (size_t) layers * (n / 2 + 1) * sizeof (uint32_t);
        return b;
    }

    bool tryClaim() noexcept
    {
        bool expected = false;
        return inUse.compare_exchange_strong (expected, true, std::memory_order_acq_rel);
    }
    void release() noexcept { inUse.store (false, std::memory_order_release); }
};

} // namespace sing
