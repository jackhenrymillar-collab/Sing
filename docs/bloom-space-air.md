# Bloom, Air and Space

Bloom is Sing's second Stretch engine: it smears the voice into a smooth, endless glow. Air
and Space are effects on the whole instrument, after the voices. The DSP is plain C++ in
`Source/DSP/Bloom` and `Source/DSP/Effects`; the tests are `Tests/BloomTests.cpp` and
`Tests/EffectsTests.cpp`.

## Bloom

The **GRAIN / BLOOM** switch on the panel (parameter `engine`) picks the engine for new notes.
A note keeps its engine until it ends.

Bloom is written from Paul Nasca's published description of his extreme time-stretch method
(the GPL implementations were not read). The idea: cut the sound into long windows, keep only
how loud each frequency is, and rebuild it with new phases. Smearing the phases is what turns a
voice into a glow. Sing adds what a playable vocal pad needs on top.

### Analysis (`BloomAnalysis`)

When Bloom is selected, every loop is analysed as it is built (on the loop builder thread):

- **Frames:** Hann windows of about 0.68 s (32768 samples at 48 kHz; see Window below), every
  eighth of a window.
  Shorter loops get shorter windows, so a window never holds the loop twice. The windows read
  round the loop, so the seam is already blended.
- **Envelope:** the vowel's shape, drawn through the tops of the harmonics (a running maximum
  over about 1.5 harmonics, then smoothed). It is kept at 1024 points per frame and is floored
  60 dB below its loudest point.
- **Fine structure:** the magnitudes with the envelope divided out (the harmonics and the
  breath), stored as 16-bit log values.
- **Classic (the default):** the magnitudes are kept exactly as analysed, every bin.
- **Locked harmonics** (`LoopSettings::bloomLockHarmonics`, not on the panel): peaks at
  least 6 dB over the local floor are found as harmonics. Vibrato splits a harmonic into
  close sidebands a few Hz apart, so peaks within 3 % (and at least 8 Hz) of a stronger one
  join it, and each harmonic is redrawn as one clean main lobe of the window.

- **Onsets:** sudden rises in the sound (a consonant, a breath in, a hit), found with short
  frames (1024 samples every 5 ms) by spectral flux: the average rise in dB over 24 bands
  since 10 ms before, peaks at least 4 dB over the median of the 0.4 s around them. A steady
  vowel with vibrato has none.

A 2 s loop takes about 25 ms to analyse.

### Playing (`BloomVoice`)

Each note builds a new frame every quarter window (about 0.17 s) and overlap-adds it with a
Hann window:

1. For each Spread copy, read the analysis at the copy's position. The whole spectrum (the
   fine structure) moves with the pitch; the **envelope** moves with Formant only, so the
   vowel stays put across the keys.
2. **Phases:** every bin gets a new, fully random phase every frame, harmonics included.
   This is the classic method (window, FFT, random phases with the magnitudes kept,
   overlap-add) and it gives Bloom its smear. The level shimmers a little as a result
   (about 4.6 dB between the 10 % and 90 % points of 50 ms slices on Jack's psy vox sample;
   granular moves 6 to 10 dB).
   - Left and right differ by a fixed offset per bin, scaled by Spread (Width).
   - With locked harmonics instead, harmonics keep running at their own frequency from
     frame to frame with a small random nudge (phase locking in the style of Laroche and
     Dolson), and breath stays at its recorded pitch. A held vowel then moves by under
     3 dB, but it is further from the classic sound.
3. Two inverse FFTs per frame (left and right), however many Spread copies there are.
4. Move the read position on by a hop times the Stretch speed. Hold stops it: that is
   freeze, and since every frame gets new random phases the frozen moment stays a living
   texture rather than a short loop.
5. **Onsets:** while an onset is inside the window, the read position moves at the
   recorded speed instead (scaled by the onset's strength), so a transient passes as it was
   played instead of smearing over many seconds. At Stretch speed 0.05 a window crosses an
   onset in about 1.8 s instead of about 15 s.

**Level.** The note matches the loop's level at the root, and stays within about 3 dB of it
over two octaves of keys and the whole Stretch range, as granular does. Harmonics moved up land
on quieter or louder parts of the envelope, and each one's peak widens by the ratio;
`harmonicsGain` compares the envelope at the harmonics as recorded and as played, and makes up
the difference.

**Starting a note.** The first frame takes about 85 ms to build (paced so a chord can start
at once). It then starts a hop into its window, with make-up gain while the overlap fills, so
the note sounds straight away at close to full level.

**CPU.** The work for each frame is cut into about a hundred small steps (a slice of bins, or
one stage of an FFT), spread evenly over the hop. Each note starts its steps at its own point
in the hop, so a chord's notes take turns. On the cloud Linux build machine (-O2, 128-sample
blocks):

| 16 notes | Average | Worst block (budget 2.67 ms) |
| --- | --- | --- |
| Spread 0 | 8 % of a core | about 1.4 ms |
| Spread 1 | 22 % of a core | about 2.3 ms |

### Spectral controls

While Bloom is on, a row under the knobs shows four controls (granular ignores them):

| Control | Parameter | Range | What it does |
| --- | --- | --- | --- |
| Window | `window` | 0.1 to 1.4 s (default 0.68 s) | The analysis and synthesis window, rounded to a power of two samples and never longer than the loop. Longer is smoother and slower to change. Changing it rebuilds the analysis; notes already sounding keep their window. |
| Shift | `shift` | -500 to +500 Hz | Frequency shift: every partial moves by the same number of Hz, so harmonics become inharmonic (bell-like). The vowel envelope stays put. |
| Smear | `smear` | 0 to 100 % | Frequency spread: each bin reads from a random point up to 12 % (about two semitones) away, a new one every frame, so the spectrum blurs. |
| Tone | `tone` | Breath 100 to Pure 100 | Tonal against noise: down fades out the harmonics (found as in the analysis) and keeps the breath and noise; up does the opposite. |

All four default to the sound before they existed.

A Bloom note whose loop has no analysis yet (just switched, still building) plays granular.

### Measured

| Test | Result |
| --- | --- |
| Pitch at -12, 0, +12 semitones | within 1 % |
| Level against the loop, over keys and Stretch | -2.1 to +0.8 dB |
| Held "ah" with vibrato, 50 ms slices | classic: 8.6 dB, never more than 5.5 dB under the held level; locked: 0.3 dB |
| Note start | 84 ms |
| Spread 0 to 1, left-right correlation | 0.91 to about -0.2 |

On Jack's psy vox sample at Stretch 0.8, 10 % to 90 % spread of 50 ms slice levels across two
octaves of keys: classic 3.4 to 4.6 dB, locked 1.5 to 1.9 dB (granular's full range: 6 to 10 dB).

## Air

A gentle lift at the top, with a breath layer that follows the voice:

- a high shelf at 8 kHz, up to +8 dB;
- soft noise, band-passed to 3 to 10 kHz, whose level follows the voice through an envelope
  follower (5 ms up, 120 ms down). It breathes with the notes, is silent between them, and
  sits about 22 dB under the voice at full Air. Left and right get different noise.

At 0 the signal passes untouched.

## Space

A long reverb on the whole instrument: Overcast's Aurora machine, copied from Overcast
(`Source/DSP/Effects/Aurora.h` and `ReverbBlocks.h`). Aurora's core follows Cloud Seed by
Ghost Note Audio (MIT, see THIRD_PARTY_NOTICES.md). One knob sets everything:

| Space | Size | Decay | Pre-delay | Wet | Dry |
| --- | --- | --- | --- | --- | --- |
| 0 | off: the signal passes untouched | | | | |
| 0.25 | small | 3.6 s | 18 ms | 0.31 | 0.98 |
| 0.5 | medium | 6.3 s | 25 ms | 0.57 | 0.9 |
| 1 | huge | 20 s | 40 ms | 0.8 | 0.6 |

The reverb hears the voice through a 150 Hz low cut, so long tails stay clear. When Space is
turned down to 0 the wet fades out over about half a second, then the reverb stops running.
Measured decay at 0.25, 0.5 and 1: 4.5 s, 7.8 s and 20 s. Air and Space together use about 5 %
of a core.

The signal runs synth, then Air, then Space, then Output.

## The panel

With Bloom selected, the blue hour fades over about a second into the Bloom sky
(`UI/BloomSky`): a dark night with drifting colour haze, a huge faint flower turning behind
the knobs, and about 80 firefly lights rising from the bottom. Each firefly has a heart of
one colour and a faint halo of the opposite colour (six complementary pairs, slowly turning
round the colour wheel). They pulse in their own rhythms and glow brighter while notes sound.
With Reduce Motion on, the sky is one still picture.

## Presets

Two new factory presets use Bloom: **Bloom Halo** and **Slow Petals**. The four earlier
presets now set Air and Space too. Air, Space and the engine default to 0 and Grain, so
sessions and user presets saved before this change sound as they did.
