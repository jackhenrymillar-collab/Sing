#pragma once

#include <array>
#include <vector>

namespace sing
{

// Moves a voice's formants (its vowel colour) up or down without changing its pitch.
//
// Pitch-synchronous overlap-add (TD-PSOLA, Moulines and Charpentier, 1990): the input is cut
// into Hann-windowed grains two pitch periods long, one per period. Each grain is read faster
// or slower by the formant ratio, which squeezes or stretches its spectral envelope, and is
// laid back down one period apart, which keeps the pitch. At ratio 1 the output is the input,
// delayed by latency() samples.
//
// The pitch period is known (it is the voice's root period at the note's pitch), so no
// pitch marks need finding. For breathy or unpitched voices, short fixed grains (5 ms) do
// the same job: resampling a noise grain shifts its spectral envelope by the same ratio.
//
// Real-time safe after prepare().
class FormantShifter
{
public:
    static constexpr float minRatio = 0.5f;
    static constexpr float maxRatio = 2.0f;
    static constexpr float unpitchedGrainSeconds = 0.005f;

    FormantShifter();

    // maxPeriodSeconds: the longest pitch period it will be asked to follow (20 ms is 50 Hz).
    void prepare (double sampleRate, float maxPeriodSeconds = 0.025f);

    // Starts a note. periodSamples is the pitch period at the note's pitch (0 if unpitched).
    // Returns the latency in samples for this note.
    int start (float periodSamples, float ratio) noexcept;

    // Follows a changing pitch (glide, bend) and formant setting.
    void setPeriod (float periodSamples) noexcept;
    void setRatio (float ratio) noexcept;

    int latency() const noexcept { return delay; }

    // Reads numSamples from inLeft/inRight and writes the shifted audio to outLeft/outRight.
    // In and out may be the same buffers. Pass inRight == nullptr for mono.
    void process (const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples) noexcept;

private:
    struct Grain
    {
        bool active = false;
        double centre = 0.0;     // output time of the grain's centre
        float halfLength = 1.0f; // one period
        float ratio = 1.0f;
    };

    float periodFor (float requested) const noexcept;
    void scheduleGrains() noexcept;
    float read (const std::vector<float>& ring, double inputTime) const noexcept;

    double rate = 48000.0;
    int ringSize = 0, ringMask = 0;
    std::vector<float> ringL, ringR;

    int delay = 0;
    float period = 240.0f, maxPeriod = 1200.0f, targetRatio = 1.0f;
    long long now = 0;             // samples processed since start()
    double nextCentre = 0.0;
    std::array<Grain, 8> grains {};

    static constexpr int windowSize = 1024;
    std::vector<float> window;

    // A 20 Hz DC blocker on the output. Grains read at a ratio other than 1 are no longer
    // a whole number of cycles under their window, so each one carries a little DC; laid
    // one period apart they add up to a steady offset (a low thump when notes change).
    float dcCoeff = 0.997f;
    float dcInL = 0.0f, dcOutL = 0.0f, dcInR = 0.0f, dcOutR = 0.0f;
};

} // namespace sing
