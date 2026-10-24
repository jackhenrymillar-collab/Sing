#pragma once

#include "VoiceData.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace sing::ui
{

// The voice drawn across the middle of the panel like a horizon. The loop is the lit stretch,
// with a handle at each end, its fades drawn as ramps, and a light for each sounding note.
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

    // Play positions, 0 to 1 across the waveform (negative for none), and their levels.
    void setLights (const std::array<float, 4>& positions, const std::array<float, 4>& levels, bool show);

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

    std::array<float, 4> lightPositions { -1.0f, -1.0f, -1.0f, -1.0f }, lightLevels {};
    bool showLights = true;

    Grab grab = Grab::none;
    float grabStartPct = 0.0f, grabLengthPct = 0.0f;
};

} // namespace sing::ui
