# Sing

A Mac sampler (VST3 and AU) for long, ambient vocal pads, part of the Overcast range.

Each preset stores its own voice. Sing loops a stretch of it, and turns it into a pad with
Stretch, Spread, Formant, octave layers, Air, Tape and a built-in Space reverb.

Work in progress: Stretch, Spread, Formant, the voices and the panel are built.

## Building

Mac (VST3, AU and Standalone, universal):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build
```

CMake fetches JUCE 8.0.10. To use a local checkout instead, add `-DSING_JUCE_DIR=/path/to/JUCE`.
CI builds and validates the Mac plugins (auval and pluginval) on every pull request and
uploads them as `Sing-macOS`.

The DSP in `Source/DSP` is plain C++ and builds without JUCE, so it can be tested anywhere:

```
cmake -S . -B build -DSING_BUILD_PLUGIN=OFF && cmake --build build
./build/SingDSPTests
./build/sing-stretch-render voice.wav out.wav --stretch 0.9
```

With `-DSING_BUILD_SNAPSHOT=ON` there is also `SingSnapshot`, which renders the panel to a PNG
without a host.

## How it works

- [docs/stretch-engine.md](docs/stretch-engine.md): Stretch and Hold.
- [docs/voices-spread-formant.md](docs/voices-spread-formant.md): voices, notes, Spread and Formant.
- [docs/bloom-space-air.md](docs/bloom-space-air.md): the Bloom engine, Air and Space.
- [docs/movement.md](docs/movement.md): Movement: orbits on the knobs, and Wander.

The factory voices are stand-ins for now, made by formant synthesis, until the recorded voices
are ready. Tape and Find a loop are still to come.
