// Renders the plugin's panel to PNG files without a host. Development only.
//
//   SingSnapshot <out-dir> [preset-number] [seconds] [fps] [scale] [param=value ...]
//
// Any param=value arguments set parameters after the preset loads, in their own
// units (for example formant=-3 bypass=1 spread=80). With BENCH=1 in the
// environment it skips writing files and prints the average time per frame.
//
// With seconds = 0 it writes one still (panel.png). Otherwise it writes numbered
// frames (frame-0000.png, ...) covering that many seconds of animation.

#include "PluginEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstdlib>

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
    for (int i = 6; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (auto* p = processor.getState().getParameter (arg.upToFirstOccurrenceOf ("=", false, false)))
            p->setValueNotifyingHost (p->convertTo0to1 (arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* singEditor = dynamic_cast<sing::SingEditor*> (editor.get());

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
        singEditor->renderFrame (2.4, true);
        write ("panel.png");
        return 0;
    }

    const int frames = juce::roundToInt (seconds * fps);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < frames; ++i)
    {
        singEditor->renderFrame (static_cast<double> (i) / fps, true);
        write ("frame-" + juce::String (i).paddedLeft ('0', 4) + ".png");
    }
    std::printf ("%.2f ms/frame\n", (juce::Time::getMillisecondCounterHiRes() - t0) / frames);
    return 0;
}
