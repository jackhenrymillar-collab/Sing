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

Sing also tracks the pitch over time (YIN every 20 ms, with a median of five frames against
octave slips), because many voices move between notes. The root only decides which key plays
the voice at its recorded pitch. Everything that works pitch by pitch (grains placed in phase,
the formant shifter's envelope width) follows the note actually sounding where it reads. Using the root
there instead lays a second, wrong set of harmonics over any part of the voice that sings a
different note, which sounds like a resonance.

The loop handles (Start, Length, Fade) are percentages of the trimmed recording, so a preset
keeps its loop when the voice is trimmed differently. The fade is up to half the loop.

A preset saves the voice inside it as 24-bit FLAC (base64 in the preset XML and the plugin
state), so a project or a shared `.singpreset` always has its sound.

### Placeholder voices

The first four factory voices are stand-ins made by formant synthesis (vowel formants from
Peterson and Barney, 1952) until the recorded voices exist: Hollow Ooh (220 Hz), Soft Ah (196 Hz),
Low Hum (262 Hz) and Two Breaths (unpitched).

Ten more change across the recording (note names as above, middle C is C3), so where the loop sits and how fast Stretch reads it
change the sound. Each is a short score in `PlaceholderVoices.cpp` (vowel, pitch, vibrato,
breath and loudness at each moment of an 8 s take), sung by additive synthesis through
gliding formants, with breath noise through the same formants:

| Voice | Root | What moves |
|---|---|---|
| Opening Ooh | A2 220 Hz | "ooh" opens through "oh" to "ah" and closes, louder when open |
| Rising Ee | B2 247 Hz | "ah" to "eh" to "ee", brighter, vibrato quickening |
| Swelling Alto | G2 196 Hz | straight "oh" swells and eases back; vibrato grows from nothing |
| Whisper to Ah | A2 220 Hz | a whisper that becomes a sung "ah" by 3 s |
| Small Choir | G2 196 Hz | six singers 9 cents apart, each with its own vibrato; "ah" rounds to "oh" |
| Low Om | A1 110 Hz | low "oh" closes through "ooh" to "mm" |
| High Ee | C4 523 Hz | light soprano "ee" easing to "ih" and back |
| Slow Wah | E2 165 Hz | "ooh" and "ah" in turn every 2 s |
| Overtone Drone | C2 131 Hz | a narrow resonance sweeps the harmonic series (throat singing) |
| Falling Sigh | A#2 233 Hz | "ah" sighs down a whole tone into breath |

Their presets use gentler Stretch (15 to 40 on Grain) than the first ones, because Stretch
slows reading exponentially (about 1/100 speed at 80) and the movement would otherwise freeze.
New voices are levelled by loudness (about -16 dB RMS) rather than peak. Note names follow Logic and Ableton, where
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

Formant changes the vowel size (throat size) without changing pitch. It is a spectral
envelope shifter, in the style of Röbel and Rodet (DAFx 2005), and it keeps the pitch and the
vowel apart:

1. **Analysis.** Hann-windowed FFT frames, at least four pitch periods long (1024 samples at
   48 kHz for most voices, 2048 for low ones), a new frame every quarter frame.
2. **Separation.** Each frame's spectral envelope (the vowel: formants and timbre) is traced
   through the tops of the harmonics: a running maximum over about one and a half harmonics,
   then a smoothed log. Under it is the excitation: the harmonics and the breath.
3. **Pitch.** The keys shift the pitch by reading the loop faster or slower, which moves the
   envelope along with the harmonics.
4. **Reshaping.** The shifter reads the envelope at frequency / ratio and multiplies each bin by
   new envelope over old (at most 24 dB either way). Only magnitudes change, so the harmonics
   keep their exact frequencies and phases: it adds no partials of its own (a test checks this
   is more than 40 dB down on a steady vowel).
5. **Reconstruction.** Inverse FFT, Hann again, overlap-add. At ratio 1 it is an exact delay.

The shifter is primed at note start so its delay (one frame, about 21 ms) isn't heard. It runs
once per note, after the Spread copies are mixed (they share the note's formant setting), and
each note runs its frames at its own point in the hop so a chord's FFTs are spread out. A
centred note (left equal to right) needs one FFT each way instead of two.

The old shifter was TD-PSOLA (one-period grains, resampled). It only sounds clean when the
grain spacing matches the voice's pitch exactly; vibrato, pitch-track error and the switch to
5 ms grains on unpitched moments each put a buzz between the harmonics. On the "ah" voice a
fifth and an octave up it measured 6 to 11 dB more power off the harmonics than plain
resampling.

**Formant 0 keeps the recorded vowel on every key.** Playing a sampler higher normally
squeezes the formants too (the chipmunk effect). Sing undoes that: the shift ratio is
2^((Formant - semitones from root) / 12), clamped to one octave each way. So at Formant 0 the
voice sounds the same size across the keyboard, and the knob moves it from there.

**KEEP VOWEL** (parameter `keepvowel`, on by default) switches that off. Off, the keys do plain
pitch shifting: the vowel moves with the key, so higher keys sound smaller and lower keys
larger, and Formant moves it from there. With Formant at 0 the shifter then does nothing at
all. In Bloom, Keep vowel off moves the envelope with the key too.


## CPU

Measured on the cloud Linux build machine (-O2, 48 kHz, 128-sample blocks, Stretch 60%, 16
notes spread over the keyboard, Keep vowel on), not a Mac:

| 16 notes    | One core | 99.9th percentile block (budget 2.67 ms) |
|-------------|----------|------------------------------------------|
| Spread 0    | about 13% | about 0.7 to 2.5 ms |
| Spread 100% | about 26% | about 2.1 to 2.8 ms |

Full Spread is seven grain engines per note, so it is the expensive case. With the old
per-copy PSOLA shifter it was about 39% for the same 16 notes; one spectral shifter per note
costs more at Spread 0 (it was about 7%) and less with Spread.

## Not built yet

Air, Space, Tape, Octaves, Movement, Find a loop and the Bloom engine.
