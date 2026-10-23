# Voices, Spread and Formant

How a note is played, from the MIDI key to the stereo output. The code is in
`Source/DSP/Voice` (plain C++, no JUCE) and the plugin wraps it in `Source/Plugin`.

## The voice a preset plays

Each preset carries its own voice (`VoiceData`). When a voice is loaded Sing:

1. Trims silence from each end (10 ms RMS windows below -50 dB, plus 10 ms of padding).
   Trim can be turned off from the voice menu.
2. Finds the root with YIN (de Cheveigne and Kawahara, 2002): up to 24 frames across the
   recording, a frame counts as pitched when its aperiodicity is under 0.2, a majority of
   frames must be pitched, and the root is their median. Breath and whispers come out
   unpitched. The root can be set by hand from the voice menu.

The loop handles (Start, Length, Fade) are percentages of the trimmed recording, so a preset
keeps its loop when the voice is trimmed differently. The fade is up to half the loop.

A preset saves the voice inside it as 24-bit FLAC (base64 in the preset XML and the plugin
state), so a project or a shared `.singpreset` always has its sound.

### Placeholder voices

The four factory voices are stand-ins made by formant synthesis (vowel formants from
Peterson and Barney, 1952) until the recorded voices exist: Hollow Ooh (220 Hz), Soft Ah (196 Hz),
Low Hum (262 Hz) and Two Breaths (unpitched). Note names follow Logic and Ableton, where
middle C is C3. They store only their id in presets, so swapping
in the recordings later is one change in `PlaceholderVoices.cpp` and `VoiceData`.

## Notes

`SingSynth` holds a pool of 20 voices and plays up to the Voices setting (1 to 16). Past
that it steals the quietest released note, else the oldest held one, with a 5 ms fade so there
is no click. The sustain pedal holds notes; pitch bend is +/-2 semitones.

Each note has:

- **Attack**: linear, 0 to 20 s.
- **Release**: exponential, reaching -60 dB at the Release time (0 to 30 s).
- **Velocity**: gain 0.35 + 0.65 x velocity, so soft notes are quieter but never vanish.
- **Glide**: with Glide above 0, a new note slides from the last note's pitch.
- **Hold**: freezes the grains where they are (see the Stretch doc).

The loop moves from the message thread to the audio thread through `SourceExchange`, a
lock-free hand-off. Notes already playing keep their old loop until they end, so moving a
handle never clicks a sounding note.

## Spread

Spread turns one note into up to seven copies, placed at {0, +/-1/3, +/-2/3, +/-1}:

- **Detune**: position x (8 + 14 x Spread) cents, so the outer copies reach 22 cents.
- **Pan**: position x (0.3 + 0.7 x Spread).
- **Copies join one at a time** as Spread rises (weight = clamp(Spread x 6 - (i - 1), 0, 1)),
  so there is no jump in sound as the knob turns.
- **Level** is power-normalised, so Spread widens without getting louder.
- **Timing**: each copy starts 50 ms further into the loop than the last, so they don't move
  in step and the chorus sounds like several singers, not one phased voice.

Each copy has its own grain engine.

## Formant

Formant changes the vowel size (throat size) without changing pitch. It uses TD-PSOLA
(Moulines and Charpentier, 1990):

- Grains are two pitch periods long, one period apart, Hann windowed.
- Each grain is read at the formant ratio and placed back at the original spacing, so the
  pitch stays and the spectral envelope moves.
- Unpitched voices use 5 ms grains.
- The shifter is primed at note start so its delay (about 1.5 periods) isn't heard.

**Formant 0 keeps the recorded vowel on every key.** Playing a sampler higher normally
squeezes the formants too (the chipmunk effect). Sing undoes that: the shift ratio is
2^((Formant - semitones from root) / 12), clamped to one octave each way. So at Formant 0 the
voice sounds the same size across the keyboard, and the knob moves it from there.

Formant runs on each Spread copy separately, so the copies keep their detune.

## CPU

Measured on the cloud Linux build machine (-O2, 48 kHz, 128-sample blocks, Stretch 80%,
Formant -3), not a Mac:

| 8 notes        | One core |
|----------------|----------|
| Spread 0       | about 6% |
| Spread 50%     | about 23% |
| Spread 100%    | about 50% |

Full Spread is seven engines and seven shifters per note, so it is the expensive case.
Sharing grain reads between copies, and skipping the shifter when Formant compensation is
near 1, are the next savings.

## Not built yet

Air, Space, Tape, Octaves, Movement, Find a loop and the Bloom engine.
