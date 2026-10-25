#pragma once

#include <vector>

namespace sing
{

// A real-input FFT of a power-of-two size N, written from the textbook method: a radix-2
// complex FFT of size N/2 on the even and odd samples packed as one complex signal, then
// one pass to split the two halves apart.
//
// The inverse can also run a step at a time (see InverseSteps), so a voice can spread one
// frame's work evenly over many audio blocks instead of doing it all in one.
//
// Spectrum layout: N/2 + 1 complex bins, interleaved re, im (N + 2 floats).
// Scaling: forward is unscaled and inverse(forward(x)) == x (the standard 1/N inverse DFT).
class RealFFT
{
public:
    // A process-wide RealFFT of each size, shared read-only by every voice (the inverse steps
    // are const). makeShared() allocates and is for prepare(); shared() is real-time safe
    // and returns nullptr for a size nobody has made yet.
    static void makeShared (int size);
    static const RealFFT* shared (int size) noexcept;

    RealFFT() = default;
    explicit RealFFT (int size) { prepare (size); }

    // Allocates. size must be a power of two, 4 or more.
    void prepare (int size);

    int size() const noexcept { return n; }
    int numBins() const noexcept { return n / 2 + 1; }

    // input: N real samples. spectrum: N + 2 floats. Uses its own scratch; not real-time safe
    // to call from two threads at once.
    void forward (const float* input, float* spectrum) noexcept;

    // spectrum: N + 2 floats (only bins 0..N/2 are read). output: N real samples.
    void inverse (const float* spectrum, float* output) noexcept;

    // The inverse as a series of steps: step 0 unpacks the spectrum into `work`, each later
    // step runs one butterfly stage, and after the last step `work` holds the N samples.
    // `work` must hold N floats and belongs to the caller, so many voices can share one
    // RealFFT.
    int inverseStepCount() const noexcept { return 1 + stages; }
    void inverseStep (int step, const float* spectrum, float* work) const noexcept;

    // The same inverse in finer pieces, for long frames: each step above is cut into
    // `chunks` pieces (a power of two, at most N / 8), run in order. A piece of a stage is a
    // share of its butterflies, so every piece costs about the same.
    int inverseChunkCount (int chunks) const noexcept { return (2 + stages) * chunks; }
    void inverseChunk (int unit, int chunks, const float* spectrum, float* work) const noexcept;

private:
    void bitReverse (float* z) const noexcept;
    void stage (float* z, int s, bool inverse) const noexcept;
    void stagePart (float* z, int s, bool inverse, int from, int to, float scale) const noexcept;
    void unpack (const float* spectrum, float* work, int from, int to) const noexcept;
    void complexFFT (float* z, bool inverse) const noexcept;

    int n = 0, m = 0, stages = 0;          // m = n / 2 complex points
    std::vector<float> twiddle;             // m/2 complex: exp(-2 pi i k / m)
    std::vector<float> split;               // m/2 + 1 complex: exp(-2 pi i k / n)
    std::vector<int> reversed;              // bit-reversed index for each of m
    std::vector<float> scratch;
};

} // namespace sing
