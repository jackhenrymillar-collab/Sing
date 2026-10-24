# Stretch engine

Sing's Stretch turns a short vocal loop into an endless pad by replaying it in small,
overlapping grains. This page describes the granular engine.

The code is plain C++ (C++20) in `Source/DSP/Stretch`, with no JUCE dependency, so it builds and is
tested on any machine. The plugin links it as the `SingDSP` library.

## Pieces

| File | What it does |
| --- | --- |
| `LoopSource` | The loop, ready to play. Built off the audio thread whenever the voice, Start, Length, Fade or Direction changes, then shared read-only with every playing note. |
| `StretchEngine` | The interface every Stretch engine implements, the shared `StretchParameters`, and the factory. |
| `GranularEngine` | The granular engine, plus `StretchKnob`, which maps the panel's Stretch knob to engine settings. |

### The loop

`LoopSource::build` renders the loop into one circular play buffer that is always read
forwards:

- **Forward:** the loop, with the seam crossfaded (equal power). The loop's tail is blended into
  the audio just before Start, so the loop keeps its exact length. If Start is too close to the
  beginning of the voice for that, the tail is blended over the loop's own head instead, which
  shortens the loop by the Fade length.
- **Backward:** the same buffer, reversed.
- **Back & forth:** the loop forwards then backwards. A bounce has no seam, so no crossfade.

Because Direction is baked into the buffer, engines never deal with it. `toVoicePosition`
maps a buffer position back to the voice for the waveform's play light.

## The granular engine

Each note has a read head moving through the loop at Stretch speed: 1 is the recorded speed,
0 is the frozen "held breath". Grains start at a steady rate (grain length ÷ overlap):

1. Each grain starts near the read head, offset by a random **jitter**.
2. It reads the loop at the note's pitch, independent of the head's speed, with 4-point Hermite
   interpolation.
3. It is shaped by a **Hann window** and overlap-added with the others. With a whole number of
   grains per grain length, the windows sum flat.
4. Each grain gets a small random pan for width.

**Hold** stops the read head (smoothly, over about 50 ms). The grains keep coming with fresh
jitter, so the frozen cloud keeps breathing for as long as the key is held.

Details that matter for vocal pads:

- **In-phase grains.** When the voice's pitch is known, each grain's jitter is rounded to
  whole pitch periods of the note sounding where the grain reads (`LoopSource::periodAt`, from
  the voice's pitch track, or `LoopSettings::pitchPeriod` when there is none), so overlapping grains of a steady
  vowel add up cleanly instead of beating against each other. In tests this cut the level
  wobble on a held pure tone from 4.7 dB to under 1 dB. Unpitched voices (breaths, whispers)
  skip it, and `Settings::phaseAlign` turns it off.
- **Minimum jitter near a standstill.** Below a quarter speed, jitter grows to at least half a
  grain. Without it, a frozen head makes the grains repeat the same audio at the grain rate,
  which rings like a comb and flutters.
- **Level compensation.** Grains reading the same audio in step add up in amplitude; grains
  reading unrelated audio add up in power. The engine estimates which is happening and scales
  each grain so the level stays within about 1.5 dB of the dry loop across the whole Stretch
  range (see the level table the tests print).
- **No clicks on loop edits.** `setSource` swaps in a rebuilt loop mid-note. Grains already
  sounding finish on the old loop, so the change crossfades over one grain.
- **Starts at once.** A note starts with a full cloud laid out as if it had been playing, so
  there is no grain fade-in; the voice's Attack shapes the onset.

### The Stretch knob

`StretchKnob` maps the knob (0 to 1) to the engine:

| Knob | Speed | Grain | Jitter |
| --- | --- | --- | --- |
| 0 | 1 (recorded speed) | 90 ms | 0 |
| 0.5 | about 1/17 | 165 ms | 69 ms |
| 0.97 and up | 0 (held breath) | 240 ms | 180 ms |

Speed falls exponentially to 1/256 at 0.97 of the travel; the last 3 % is the held breath.
At 0 the grains have no jitter, random pan or random timing, so Stretch 0 plays the voice
exactly as recorded; those come in over the first tenth of the knob (8 ms of jitter by 0.1).
These are starting values to tune by ear.

### Real-time safety and CPU

`prepare` allocates. `start`, `setSource`, `setParameters` and `process` do not allocate or
lock. Each engine has a fixed pool of 64 grains.

16 notes at overlap 4 use about 5 % of one core (measured on the cloud Linux build machine, -O2,
not a Mac). Spread's copies multiply this (see [voices-spread-formant.md](voices-spread-formant.md));
sharing grain reads between Spread copies is the next saving to make.

## Bloom

The second engine, Bloom, is a spectral smear rather than a `StretchEngine`: see
[bloom-space-air.md](bloom-space-air.md). `LoopSource::build` makes its analysis when
`LoopSettings::analyseForBloom` is set, and `createStretchEngine (StretchMode::bloom)` still
returns granular, which plays any Bloom note whose loop has no analysis.

## Trying it by ear

```
cmake -S . -B build && cmake --build build
./build/sing-stretch-render voice.wav out.wav --stretch 0.9 --root-hz 220 --seconds 20 \
    --loop-start 0.4 --loop-length 1.5 --fade 0.3 --hold-after 12
```

Run `./build/sing-stretch-render` with no arguments for every option, and `./build/SingDSPTests`
for the tests.

## Sources

Written from the published literature on granular synthesis (Gabor; Curtis Roads,
*Microsound*; Barry Truax's real-time granulation) and on pitch-synchronous overlap-add. No
third-party code.
