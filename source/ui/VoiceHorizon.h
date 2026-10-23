#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <vector>

namespace sing::ui
{

// The voice drawn across the middle of the panel like a horizon: 120 mirrored bars,
// with the loop as the lit stretch. The loop's fades are drawn as a ramp, a gold
// handle marks each end, and a glowing line shows where each sounding note is.
//
// Drag a handle to move that end of the loop; drag inside the lit stretch to move the
// whole loop. Hold Shift for fine moves; double-click resets the loop.
//
// The bars and the loop are cached as one image, rebuilt when the voice, the loop or
// the screen's pixel density changes; only the playheads are drawn every frame.
class VoiceHorizon final : public juce::Component
{
public:
    // The waveform's place in the panel, in panel units. The component is a little
    // bigger, so the handles (which stand 10 units proud) can be grabbed at the ends.
    static juce::Rectangle<float> waveArea() noexcept { return { 40.0f, 206.0f, 960.0f, 120.0f }; }
    static juce::Rectangle<int> componentBounds() noexcept { return { 30, 186, 980, 160 }; }

    static constexpr int numBars = 120;
    static constexpr int maxPlayheads = 16;

    explicit VoiceHorizon (juce::AudioProcessorValueTreeState&);
    ~VoiceHorizon() override;

    // The voice's peak outline, 0..1 (any length; it's grouped into 120 bars).
    void setWaveform (const std::vector<float>& peaks);
    // While a new voice is being prepared the bars dim.
    void setLoading (bool);

    // Called every frame: reads the loop parameters and where the notes are
    // (fractions of the voice, -1 = none). Repaints only when something moved.
    void update (const std::array<float, maxPlayheads>& playheads, bool motion);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Loop
    {
        float start = 0.3f, end = 0.7f, fade = 0.3f; // fractions; fade is the share of half the loop
        bool operator!= (const Loop& o) const
        {
            return ! juce::approximatelyEqual (start, o.start) || ! juce::approximatelyEqual (end, o.end)
                || ! juce::approximatelyEqual (fade, o.fade);
        }
    };

    enum class Grab { none, start, end, body };

    Loop readLoop() const;
    juce::Rectangle<float> wave() const; // the waveform in local coordinates
    float xFor (float fraction) const;
    float fractionAt (float x) const;
    Grab grabAt (juce::Point<float>) const;
    void setLoop (float start, float length);
    void rebuildImage (float pixelScale);
    const juce::Image& playheadSprite();

    juce::RangedAudioParameter& startParam;
    juce::RangedAudioParameter& lengthParam;
    juce::RangedAudioParameter& fadeParam;

    std::array<float, numBars> bars {};
    bool loading = false;
    Loop loop, imageLoop;
    bool imageLoading = false;
    int waveVersion = 0, imageVersion = -1;
    float imageScale = 0.0f;
    juce::Image image;
    juce::Image sprite;
    float spriteScale = 0.0f;

    // Playheads, and how visible each is (they fade in and out when motion is on).
    std::array<float, maxPlayheads> heads {}, headAlpha {};

    Grab grab = Grab::none;
    Loop dragStart;
    float dragFrom = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceHorizon)
};

} // namespace sing::ui
