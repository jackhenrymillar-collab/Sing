# Sing

A Mac sampler (VST3 and AU instrument) for long, ambient vocal pads, part of the Overcast range.

Each preset stores its own voice. Sing loops a stretch of it and turns it into a pad with
Stretch (grains), Spread (up to seven detuned copies), Formant, octave layers, Air, Tape and a
built-in Space reverb. Drop a WAV, AIFF or FLAC onto the panel to use your own voice.

## Pitching

Pitch quality is the core of Sing (see `source/dsp/Sinc.h`):

- Each voice is upsampled 2x when it loads, and copies an octave, two octaves... lower in
  rate are made with steep filters, so playing far up never has to squeeze too much in.
- Notes render at twice the host rate through 150 dB Kaiser-windowed sinc kernels that
  stretch when pitching up, then one steep half-band filter returns to the host rate.
- The tests (`tests/PitchTests.cpp`) hold tones pitched from three octaves down to three up
  to within 0.01 dB, with everything else 125 dB down (measured: about 132 dB), and check
  that tones pitched past the audible band vanish rather than fold back.

## Building

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ctest --test-dir build

`-DSING_BUILD_PLUGIN=OFF` builds only the DSP library and its tests (any platform).
`-DSING_BUILD_TOOLS=ON` adds `sing_render`, which writes demo WAVs.
`-DSING_BUILD_SNAPSHOT=ON` adds `SingSnapshot`, which renders the panel to PNG.
