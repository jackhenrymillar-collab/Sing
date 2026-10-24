# Third-party notices

## JUCE

Sing is built with JUCE (https://juce.com), used under the plugin developer's JUCE licence.

## Fonts

The panel uses Cormorant Garamond and Jost, both under the SIL Open Font License 1.1. The
licence texts are in `Resources/Fonts/OFL-CormorantGaramond.txt` and
`Resources/Fonts/OFL-Jost.txt`.

## Methods

Sing's DSP is written from published methods. No code was copied from any implementation.

- Granular overlap-add time stretching with Hann windows.
- Pitch detection: A. de Cheveigne and H. Kawahara, "YIN, a fundamental frequency estimator for
  speech and music", JASA 111(4), 2002.
- Formant shifting: E. Moulines and F. Charpentier, "Pitch-synchronous waveform processing
  techniques for text-to-speech synthesis using diphones", Speech Communication 9, 1990
  (TD-PSOLA).
- Placeholder vowels: G. E. Peterson and H. L. Barney, "Control methods used in a study of the
  vowels", JASA 24(2), 1952 (formant frequencies).
