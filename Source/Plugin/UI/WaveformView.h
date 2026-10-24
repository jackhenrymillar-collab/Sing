#pragma once

#include "VoiceData.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace sing::ui
{

// The voice drawn across the middle of the panel like a horizon. The loop is the lit stretch,
// with a handle at each end, its fades drawn as ramps, and a light for each sounding note.
// Each note's light has a faint ghost for every Spread copy, tinted by its pan, a soft halo
// over the stretch of the voice its grains are reading, and a short tail showing how far it
// moved in the last moment (none when Stretch holds it still).
//
// Drag a handle to move that end of the loop, or drag inside the loop to slide it. The
// component is 976 x 160 panel units; the waveform itself is the 960 x 120 inside it.
class WaveformView final : public juce::Component
{
public:
    static constexpr float margin = 8.0f, top = 20.0f, waveWidth = 960.0f, waveHeight = 120.0f;
    static constexpr float width = waveWidth + 2.0f * margin, height = waveHeight + 40.0f;

    explicit WaveformView (juce::AudioProcessorValueTreeState&);

    void setVoice (VoiceData::Ptr);

    // One light per Spread copy of a sounding note, newest note first and its centre copy first.
    struct Light
    {
        float position = -1.0f;   // 0 to 1 across the waveform
        float level = 0.0f;       // the note's envelope
        float weight = 0.0f;      // the copy's place in the Spread, 0 to 1
        float pan = 0.0f;         // -1 to 1
        float pitchRatio = 1.0f;
        float zone = 0.0f;        // width being read, 0 to 1, centred; 0 for the granular zone
        int note = -1;            // 0 for the newest note; -1 for an unused light
        int copy = 0;             // 0 for the centre copy
        uint32_t noteId = 0;
    };
    static constexpr int maxLights = 28;

    // clockSeconds is the panel's animation clock, which times the tails.
    void setLights (const std::array<Light, maxLights>&, bool show, double clockSeconds);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    enum class Grab { none, start, end, body };

    float loopLeft() const noexcept;    // waveform units, 0 to waveWidth
    float loopRight() const noexcept;
    float fadeWidth() const noexcept;
    Grab grabAt (juce::Point<float>) const noexcept;
    void rebuildBars (float pixelScale);

    juce::AudioProcessorValueTreeState& state;
    juce::ParameterAttachment startAttachment, lengthAttachment, fadeAttachment;
    float startPct = 30.0f, lengthPct = 40.0f, fadePct = 25.0f;

    VoiceData::Ptr voice;
    juce::Image bars;
    float barsScale = 0.0f, barsStart = -1.0f, barsLength = -1.0f;
    juce::int64 barsVoice = -1;

    void paintLights (juce::Graphics&, juce::Rectangle<float> region);
    void rememberTails (double nowMs);

    std::atomic<float>* stretchParam = nullptr;
    std::atomic<float>* holdParam = nullptr;

    std::array<Light, maxLights> lights {};
    bool showLights = true;

    // The centre copy's recent positions for each note, for its tail.
    static constexpr int tailPoints = 48;
    static constexpr double tailMs = 1500.0;
    struct Tail
    {
        uint32_t noteId = 0;
        bool used = false;
        int count = 0, next = 0;
        std::array<float, tailPoints> positions {};
        std::array<double, tailPoints> times {};
    };
    std::array<Tail, 4> tails {};
    double clockMs = 0.0;

    Grab grab = Grab::none;
    float grabStartPct = 0.0f, grabLengthPct = 0.0f;
};

} // namespace sing::ui
