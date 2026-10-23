# Sing

A Mac sampler (VST3 and AU) for long, ambient vocal pads, part of the Overcast range.

Each preset stores its own voice. Sing loops a stretch of it, and turns it into a pad with
Stretch, Spread, Formant, octave layers, Air, Tape and a built-in Space reverb.

Work in progress.

## Building the DSP

The DSP is plain C++17 in `Source/DSP` and builds without JUCE:

```
cmake -S . -B build && cmake --build build
./build/SingDSPTests
./build/sing-stretch-render voice.wav out.wav --stretch 0.9
```

See [docs/stretch-engine.md](docs/stretch-engine.md) for how Stretch works.
