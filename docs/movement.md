# Movement

Movement keeps a held pad changing slowly, so it never sits still. It is built from orbits:
any of five big knobs, and the loop, can wander around where you set them.

## On the panel

- **Orbit rings.** Stretch, Spread, Formant, Air and Space each have a ring around the dial.
  Drag the ring up or down (or Option-drag the dial) to set how far the knob wanders, as a
  share of its whole range. Double-click the ring to stop it. The gold arc shows where the knob
  wanders, and a small light circles to where it is right now (the newest note's, for Stretch,
  Spread and Formant). An orbit keeps its full width near either end of a knob's range: it
  moves inward rather than being cut short.
- **Wander** (in the Loop row) is the loop's orbit: each note's reading point moves back and
  forth inside the loop, so the vowel slowly changes. 100% lets it travel the whole loop. It
  also moves a held note (Hold, or Stretch at the top), so a frozen pad can drift through the
  voice. To let it travel further, make the loop longer.
- **The Movement strip** (bottom row) sets how every orbit moves:
  - the shape: **Drift** wanders at random and never repeats (each knob on its own path),
    **Wave** is a smooth sine, **Breath** rises over 40% of each cycle and falls over the rest
    (Wave and Breath move every knob in step);
  - **Speed**, the length of one swing, 1 to 120 s, or with **Sync** on, ¼ bar to 32 bars of
    the song, lined up with the bars while the song plays;
  - **Each note**: on (the default), every note follows its own path, so a chord shimmers
    instead of moving as one. Air and Space work on the whole instrument, so they always
    move together.

The strip dims while nothing is set to move. The lights hide under Reduce Motion; the sound
still moves.

## How it works

`Source/DSP/Voice/Movement.h` has the shapes and the orbit maths. Drift is smooth value noise
(random points joined by smootherstep curves, two layers at unrelated speeds), hashed from a
seed and the position, so it needs no state and never repeats.

`SingSynth` keeps one clock, in cycles of the period, for every note. With Sync on and the
song playing, the processor sets it from the song position each block. Each `SingVoice` works
out its own Stretch, Spread and Formant every 64-sample sub-block from the knobs plus its
orbits (its own seed and place in the wave when Each note is on), easing over about 30 ms so
a new shape, a new orbit or a jump in the song never steps. Knob moves themselves stay
immediate.

Wander moves the read head of each Spread copy (`GranularEngine::nudge`) or Bloom's read
position (`BloomVoice::nudge`) by the change in its offset each sub-block. Loop Start,
Length and Window can't move directly, because changing them rebuilds the loop (and Bloom's
analysis) off the audio thread.

Air and Space orbit in the processor, after the notes, on the shared clock.

Every orbit defaults to 0, so sessions and presets from before Movement sound the same.

## Parameters

| ID | What | Range |
| --- | --- | --- |
| `orbitstretch`, `orbitspread`, `orbitformant`, `orbitair`, `orbitspace` | each knob's orbit | 0 to 100% |
| `wander` | the loop's orbit | 0 to 100% |
| `moveshape` | Drift, Wave, Breath | choice |
| `movespeed` | one swing, free | 1 to 120 s |
| `movesync` | follow the song | on/off |
| `movebars` | one swing, synced | ¼ bar to 32 bars |
| `moveeach` | every note its own path | on/off |
