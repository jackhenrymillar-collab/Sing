# Third-party notices

## JUCE

Sing is built with JUCE (https://juce.com), used under the plugin developer's JUCE licence.

## Fonts

The panel uses Cormorant Garamond and Jost, both under the SIL Open Font License 1.1. The
licence texts are in `Resources/Fonts/OFL-CormorantGaramond.txt` and
`Resources/Fonts/OFL-Jost.txt`.

## Cloud Seed Core

Space uses Aurora, the reverb machine from Overcast (by the same author), copied into
`Source/DSP/Effects`. Aurora's core follows Cloud Seed by Ghost Note Audio
(https://github.com/GhostNoteAudio/CloudSeedCore). Sing is not made by or affiliated with Ghost
Note Audio.

```
MIT License

Copyright (c) 2024 Ghost Note Audio

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Methods

Apart from Space (above), Sing's DSP is written from published methods. No code was copied
from any implementation.

- Granular overlap-add time stretching with Hann windows.
- Pitch detection: A. de Cheveigne and H. Kawahara, "YIN, a fundamental frequency estimator for
  speech and music", JASA 111(4), 2002.
- Formant shifting: spectral envelope shifting after A. Röbel and X. Rodet, "Efficient
  spectral envelope estimation and its application to pitch shifting and envelope
  preservation", DAFx 2005 (envelope through the harmonic peaks, reapplied after moving it).
- Bloom: spectral time stretching with randomised phases, after Paul Nasca's published
  description of his "extreme time stretch" method. Written from the description only (his
  implementations are GPL and were not read). With the optional locked harmonics, peaks are kept phase-locked frame to
  frame, after J. Laroche and M. Dolson, "Improved phase vocoder time-scale modification of
  audio", IEEE Trans. Speech and Audio Processing 7(3), 1999.
- Air's filters: R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
  coefficients" (high shelf, low- and high-pass).
- Placeholder vowels: G. E. Peterson and H. L. Barney, "Control methods used in a study of the
  vowels", JASA 24(2), 1952 (formant frequencies).
