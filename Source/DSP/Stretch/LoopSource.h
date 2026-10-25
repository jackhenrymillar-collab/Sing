#pragma once

#include "Voice/VoiceAnalysis.h"

#include <array>
#include <memory>
#include <vector>

namespace sing
{

enum class LoopDirection
{
    forward,
    backward,
    pingPong
};

enum class StretchMode
{
    granular,
    bloom,   // Paulstretch-style spectral smear (Bloom/BloomVoice.h)
    count
};

struct LoopSettings
{
    int start = 0;          // first sample of the loop, in the voice
    int length = 0;         // loop length in samples
    int fade = 0;           // crossfade length in samples at the loop seam. Below 2 ms, the
                            // end is moved (up to 12 ms) to match Start and joined with a
                            // 2 ms fade instead, so even Fade 0 never cuts hard.
    LoopDirection direction = LoopDirection::forward;

    // Length of one cycle of the voice at its root pitch, in samples, from pitch detection.
    // 0 when the voice is unpitched (breaths, whispers).
    float pitchPeriod = 0.0f;

    // Build Bloom's analysis too (about 0.1 s of work per 10 s of loop, and a few MB).
    bool analyseForBloom = false;

    // Bloom's phases. Off (the default) is the classic method: every bin of every frame gets
    // a fully random phase, magnitudes kept exactly as analysed. On, harmonics keep their
    // phases running from frame to frame instead, so a held vowel trembles less.
    bool bloomLockHarmonics = false;

    // Bloom's window length in seconds (the Window control), 0.1 to 40. Longer is smoother
    // and slower to change; shorter keeps more of the voice's movement. Rounded to a power
    // of two samples, and never longer than 98% of the loop.
    float bloomWindowSeconds = 0.68f;

    // For Bloom's per-note buffers, made with the analysis: the host's sample rate (0: the
    // voice's own) and how many notes may play Bloom at once. Long windows get fewer.
    double hostSampleRate = 0.0;
    int bloomNotes = 20;

    // The voice's pitch over time, when it has been measured. Voices that move between notes
    // need it: pitch-synchronous processing must follow the note actually sounding, not the
    // root. Without it, pitchPeriod is used everywhere.
    std::shared_ptr<const PitchTrack> pitchTrack;
};

// Per-engine analysis attached to a LoopSource when it is built.
// The granular engine needs none. Bloom will store its magnitude spectrogram here.
struct SourceAnalysis
{
    virtual ~SourceAnalysis() = default;
};

// The loop, ready to play. Built off the audio thread whenever the voice, Start, Length,
// Fade or Direction changes, then shared read-only with every playing note.
//
// The loop is rendered into one circular "play buffer" that is always read forwards:
// - Forward:  the loop, with its seam already crossfaded.
// - Backward: the same, reversed.
// - Back & forth: the loop forwards, then backwards (2 * length - 2 samples, no seam).
// So engines only ever read forwards with wrap-around, whatever Direction is.
class LoopSource
{
public:
    // channels: numChannels pointers (1 or 2) to numSamples samples of the voice.
    static std::shared_ptr<const LoopSource> build (const float* const* channels,
                                                    int numChannels,
                                                    int numSamples,
                                                    double sampleRate,
                                                    LoopSettings settings);

    int numChannels() const noexcept { return (int) channels.size(); }
    int size() const noexcept { return bufferSize; }          // play buffer length
    double sampleRate() const noexcept { return rate; }
    const LoopSettings& settings() const noexcept { return loop; }
    int effectiveFade() const noexcept { return fadeUsed; }

    // Samples of the play buffer. Index must be in [0, size()). For 4-point interpolation,
    // index -1 and size() to size() + 2 are also valid and hold the wrapped-around samples.
    const float* channel (int ch) const noexcept
    {
        return channels[(size_t) (ch < numChannels() ? ch : 0)].data() + guardBefore;
    }

    static constexpr int guardBefore = 1;
    static constexpr int guardAfter = 3;

    // Converts a play buffer position into a sample position in the original voice,
    // for drawing the play light on the waveform.
    double toVoicePosition (double bufferPosition) const noexcept;

    // Converts a sample position in the voice into a play buffer position.
    double fromVoicePosition (double voicePosition) const noexcept;

    // The pitch period, in voice samples, at a play buffer position: from the pitch track
    // when there is one, otherwise settings().pitchPeriod. 0 where the voice is unpitched.
    float periodAt (double bufferPosition) const noexcept
    {
        return loop.pitchTrack != nullptr ? loop.pitchTrack->periodAt (toVoicePosition (bufferPosition)) : loop.pitchPeriod;
    }

    const SourceAnalysis* analysis (StretchMode mode) const noexcept
    {
        return analyses[(size_t) mode].get();
    }

private:
    LoopSource() = default;

    std::vector<std::vector<float>> channels;
    int bufferSize = 0;
    double rate = 44100.0;
    LoopSettings loop;
    int fadeUsed = 0;
    std::array<std::shared_ptr<const SourceAnalysis>, (size_t) StretchMode::count> analyses;
};

} // namespace sing
