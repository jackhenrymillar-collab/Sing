// Renders the plugin's panel to PNG files without a host. Development only.
//
//   SingSnapshot <out-dir> [preset-number] [seconds] [fps] [scale] [param=value ...]
//
// Any param=value arguments set parameters after the preset loads, in their own
// units (for example loopmode=2 bypass=1 formant=-4). Two more are special:
//   note=60[,64,67]  plays those notes (for `play` seconds, default 2) before drawing,
//                    so the waveform shows playheads
//   still=1          draws the Reduce Motion picture
// With BENCH=1 in the environment it skips writing files and prints the average time
// per frame.
//
// With seconds = 0 it writes one still (panel.png). Otherwise it writes numbered
// frames (frame-0000.png, ...) covering that many seconds of animation; any notes
// keep playing as it goes.

#include "PluginEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstdlib>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

#if JUCE_LINUX || JUCE_BSD || JUCE_WINDOWS
namespace juce::detail { bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages); }
#endif

namespace
{
    // Runs pending message-thread callbacks (the voice arrives through one) for a while.
    // JUCE's own runDispatchLoopUntil needs modal loops, which the plugin doesn't enable.
    void runMessages (int milliseconds)
    {
        const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (milliseconds);
        while (juce::Time::getMillisecondCounter() < end)
        {
           #if JUCE_MAC
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
           #elif JUCE_LINUX || JUCE_BSD || JUCE_WINDOWS
            if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
                juce::Thread::sleep (2);
           #else
            juce::Thread::sleep (5);
           #endif
        }
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    const juce::File outDir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                      : juce::File::getCurrentWorkingDirectory());
    const int preset = argc > 2 ? juce::String (argv[2]).getIntValue() : 1;
    const double seconds = argc > 3 ? juce::String (argv[3]).getDoubleValue() : 0.0;
    const double fps = argc > 4 ? juce::String (argv[4]).getDoubleValue() : 20.0;
    const float scale = argc > 5 ? juce::String (argv[5]).getFloatValue() : 1.0f;
    outDir.createDirectory();

    sing::SingProcessor processor;
    if (preset > 0)
        processor.getPresets().loadPreset (preset - 1);

    juce::Array<int> notes;
    double playSeconds = 2.0;
    bool still = false;
    for (int i = 6; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const auto key = arg.upToFirstOccurrenceOf ("=", false, false);
        const auto value = arg.fromFirstOccurrenceOf ("=", false, false);
        if (key == "note")
        {
            for (const auto& n : juce::StringArray::fromTokens (value, ",", ""))
                notes.add (n.getIntValue());
        }
        else if (key == "play")
            playSeconds = value.getDoubleValue();
        else if (key == "still")
            still = value.getIntValue() != 0;
        else if (auto* p = processor.getState().getParameter (key))
            p->setValueNotifyingHost (p->convertTo0to1 (value.getFloatValue()));
        else
            std::fprintf (stderr, "Unknown parameter: %s\n", key.toRawUTF8());
    }

    // Wait for the voice to be ready, letting its message-thread callback run.
    for (int i = 0; i < 600 && processor.isVoiceLoading(); ++i)
        runMessages (10);
    runMessages (30);

    // Optionally play some notes so the playheads show.
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> buffer (2, blockSize);
    auto playFor = [&] (double duration, bool startNotes)
    {
        const int blocks = juce::roundToInt (duration * sampleRate / blockSize);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            if (startNotes && b == 0)
                for (int n : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, 0.8f), 0);
            buffer.clear();
            processor.processBlock (buffer, midi);
        }
    };
    if (! notes.isEmpty())
    {
        processor.prepareToPlay (sampleRate, blockSize);
        playFor (playSeconds, true);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* singEditor = dynamic_cast<sing::SingEditor*> (editor.get());
    if (singEditor == nullptr)
        return 1;

    auto write = [&] (const juce::String& name)
    {
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        if (std::getenv ("BENCH")) return;
        juce::FileOutputStream out (outDir.getChildFile (name));
        out.setPosition (0);
        out.truncate();
        juce::PNGImageFormat().writeImageToStream (image, out);
    };

    if (seconds <= 0.0)
    {
        // A few frames first, so the playheads have faded in.
        for (int i = 0; i < 4; ++i)
            singEditor->renderFrame (2.4 + i / 30.0, ! still);
        write ("panel.png");
        return 0;
    }

    const int frames = juce::roundToInt (seconds * fps);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < frames; ++i)
    {
        if (! notes.isEmpty())
            playFor (1.0 / fps, false);
        singEditor->renderFrame (static_cast<double> (i) / fps, ! still);
        write ("frame-" + juce::String (i).paddedLeft ('0', 4) + ".png");
    }
    std::printf ("%.2f ms/frame\n", (juce::Time::getMillisecondCounterHiRes() - t0) / frames);
    return 0;
}
