// Synth and instrument sources: generated, noise-free stand-ins next to the sung voices.
//
// Every sound is a bank of sine partials (additive synthesis), described block by block by a
// small function: which partials sound at this moment, how loud, at what frequency. Synths are
// a classic waveform (saw, square, triangle, pulse, FM, wavefolder) shaped by a filter, both
// worked out per harmonic, so a moving cutoff or a changing waveshape never aliases or clicks.
// Instruments copy what makes the real thing recognisable: its partials (stretched for a
// piano, a vibraphone's 1 : 4 : 10 bar), how each partial dies away, body resonances, bowing,
// bellows and vibrato. There is no noise in any of them.

#include "PlaceholderVoices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sing
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double twoPi = 2.0 * pi;
constexpr double designSeconds = 8.0;

struct Partial
{
    double hz;
    double amp;   // signed: a negative amplitude is the same partial upside down
};

using Partials = std::vector<Partial>;

// Fills the partials of one layer (a unison voice, a string of a piano note, a reed) at time t,
// in seconds of an 8 s take.
using Fill = void (*) (double t, int layer, Partials& out);

struct Patch
{
    int layers = 1;
    Fill fill = nullptr;
    double rootHz = 0.0;
};

//==============================================================================
// Shapes, filters and helpers, all as gains for one partial.

double smooth (double edge0, double edge1, double x)
{
    const double t = std::clamp ((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// 0 to 1 and back, once per period, starting at 0.
double cycle (double t, double period, double phase = 0.0)
{
    return 0.5 - 0.5 * std::cos (twoPi * t / period + phase);
}

double cents (double c) { return std::pow (2.0, c / 1200.0); }

// Layer k of n spread evenly across +-spread cents.
double spreadCents (int layer, int layers, double spread)
{
    return layers > 1 ? cents (spread * (2.0 * layer / (layers - 1) - 1.0)) : 1.0;
}

// Fourier series of the classic waveforms, each about 1 at its peak.
double saw (int h) { return (h % 2 == 1 ? 2.0 : -2.0) / (pi * h); }
double square (int h) { return h % 2 == 1 ? 4.0 / (pi * h) : 0.0; }
double triangle (int h) { return h % 2 == 1 ? ((h / 2) % 2 == 0 ? 8.0 : -8.0) / (pi * pi * h * h) : 0.0; }
double pulse (int h, double width) { return 4.0 / (pi * h) * std::sin (pi * h * width); }

// A resonant low-pass, two poles: resonance 0 is flat (Q 0.7), 1 rings hard (Q about 9).
double lowpass2 (double f, double cutoff, double resonance)
{
    const double q = 0.707 + resonance * 8.0;
    const double r = f / cutoff;
    const double re = 1.0 - r * r, im = r / q;
    return 1.0 / std::sqrt (re * re + im * im);
}

// Four poles: a resonant stage and a plain one, 24 dB per octave like a ladder filter.
double lowpass4 (double f, double cutoff, double resonance)
{
    return lowpass2 (f, cutoff, resonance) * lowpass2 (f, cutoff, 0.0);
}

// A resonant peak (band-pass) with unity gain at its centre.
double peak (double f, double centre, double q)
{
    const double r = f / centre;
    const double re = 1.0 - r * r, im = r / q;
    return (1.0 / q) / std::sqrt (re * re + (im * im));
}

// Bessel function of the first kind, J_n(x), by its power series (fine for x up to about 12).
double besselJ (int n, double x)
{
    double term = 1.0;
    for (int k = 1; k <= n; ++k)
        term *= x / (2.0 * k);
    double sum = term;
    const double x2 = -0.25 * x * x;
    for (int k = 1; k < 60; ++k)
    {
        term *= x2 / ((double) k * (double) (n + k));
        sum += term;
        if (std::abs (term) < 1.0e-14 * std::max (1.0, std::abs (sum)))
            break;
    }
    return sum;
}

int harmonicsBelow (double f0, double hz) { return std::clamp ((int) (hz / f0), 1, 200); }

// A slow, uneven wander in pitch, so no layer sounds machine-locked.
double drift (double t, int layer)
{
    return 1.0 + 0.0008 * std::sin (twoPi * 0.23 * t + 1.7 * layer) + 0.0005 * std::sin (twoPi * 0.61 * t + 0.9 * layer);
}

//==============================================================================
// Synths: a waveform with a moving filter or a moving shape.

void sweepingSaw (double t, int layer, Partials& out)
{
    // Three detuned saws under a ladder filter that opens and closes once across the take.
    const double f0 = 110.0 * spreadCents (layer, 3, 8.0) * drift (t, layer);
    const double cutoff = 300.0 * std::pow (2.0, 3.7 * cycle (t, 8.0));
    const double level = smooth (0.0, 0.25, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
        out.push_back ({ f0 * h, level * saw (h) * lowpass4 (f0 * h, cutoff, 0.55) });
}

void pwmStrings (double t, int layer, Partials& out)
{
    // A string machine: four pulse waves whose widths sweep slowly, each a little out of step.
    const double f0 = 220.0 * spreadCents (layer, 4, 7.0) * drift (t, layer) * (1.0 + 0.002 * std::sin (twoPi * 5.0 * t + layer));
    const double width = 0.5 - 0.38 * cycle (t, 4.0, 0.8 * layer);
    const double cutoff = 2600.0 + 700.0 * std::sin (twoPi * t / 8.0);
    const double level = smooth (0.0, 1.0, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
        out.push_back ({ f0 * h, level * pulse (h, width) * lowpass2 (f0 * h, cutoff, 0.1) });
}

void morphPad (double t, int layer, Partials& out)
{
    // The waveform morphs from triangle to saw to square while the filter slowly opens.
    const double f0 = 146.83 * spreadCents (layer, 2, 5.0) * drift (t, layer);
    const double m = smooth (0.5, 7.5, t) * 2.0;   // 0 triangle, 1 saw, 2 square
    const double wTri = std::max (0.0, 1.0 - m), wSaw = 1.0 - std::abs (m - 1.0), wSquare = std::max (0.0, m - 1.0);
    const double cutoff = 800.0 * std::pow (2.0, 2.6 * smooth (0.0, 7.0, t));
    const double level = smooth (0.0, 0.8, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
    {
        const double shape = wTri * triangle (h) + wSaw * saw (h) + 0.8 * wSquare * square (h);
        out.push_back ({ f0 * h, level * shape * lowpass4 (f0 * h, cutoff, 0.3) });
    }
}

void wobbleBass (double t, int, Partials& out)
{
    // Saw and square under a resonant filter wobbled by an LFO that speeds up across the take.
    const double f0 = 82.41 * drift (t, 0);
    const double lfoPhase = twoPi * (1.0 * t + 0.125 * t * t);   // 1 Hz rising to 3 Hz
    const double lfo = 0.5 - 0.5 * std::cos (lfoPhase);
    const double cutoff = 150.0 * std::pow (2.0, 4.5 * lfo);
    const double level = smooth (0.0, 0.05, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
        out.push_back ({ f0 * h, level * (0.6 * saw (h) + 0.4 * square (h)) * lowpass4 (f0 * h, cutoff, 0.75) });
}

void syncSweep (double t, int, Partials& out)
{
    // Like hard sync: a bright peak rides up and down the saw's harmonics (1.5 to 12 times
    // the note), the way a synced oscillator's pitch sweep sounds.
    const double f0 = 130.81 * drift (t, 0);
    const double ratio = 1.5 * std::pow (2.0, 3.0 * cycle (t, 8.0));
    const double level = smooth (0.0, 0.1, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
    {
        const double d = std::log2 (h / ratio);
        const double lift = 1.0 + 10.0 * std::exp (-d * d / (2.0 * 0.12 * 0.12));
        out.push_back ({ f0 * h, level * saw (h) * lift * lowpass2 (f0 * h, 8000.0, 0.0) });
    }
}

void supersawRise (double t, int layer, Partials& out)
{
    // Seven saws spreading wider as the filter climbs: a slow riser.
    const double spread = 6.0 + 19.0 * smooth (0.0, 7.0, t);
    const double f0 = 220.0 * spreadCents (layer, 7, spread) * drift (t, layer);
    const double cutoff = 400.0 * std::pow (2.0, 4.5 * smooth (0.3, 7.0, t));
    const double level = smooth (0.0, 0.6, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
        out.push_back ({ f0 * h, level * saw (h) * lowpass4 (f0 * h, cutoff, 0.2) });
}

void fmMorph (double t, int layer, Partials& out)
{
    // Two-operator FM, carrier and modulator at the note: as the index rises the tone goes
    // from a pure sine to a bright, brassy spectrum and back.
    const double f0 = 196.0 * (layer == 0 ? 1.0 : cents (4.0)) * drift (t, layer);
    const double index = 0.3 + 4.7 * cycle (t, 8.0);
    const double level = smooth (0.0, 0.3, t) * (layer == 0 ? 1.0 : 0.6);
    // Sidebands at (1 + n) f0 with amplitude J_n(index); the negative ones fold back.
    for (int h = 1, n = std::min (harmonicsBelow (f0, 16000.0), 30); h <= n; ++h)
    {
        const double a = besselJ (h - 1, index) - (h % 2 == 0 ? 1.0 : -1.0) * besselJ (h + 1, index);
        out.push_back ({ f0 * h, level * a });
    }
}

void wavefoldDrone (double t, int, Partials& out)
{
    // A sine through a wavefolder, sin (g sin x): folding harder adds odd harmonics in waves
    // (J_k (g)), twice across the take.
    const double f0 = 130.81 * drift (t, 0);
    const double gain = 0.8 + 10.0 * cycle (t, 4.0);
    const double level = smooth (0.0, 0.4, t);
    for (int h = 1, n = std::min (harmonicsBelow (f0, 16000.0), 31); h <= n; h += 2)
        out.push_back ({ f0 * h, level * 2.0 * besselJ (h, gain) * lowpass2 (f0 * h, 6000.0, 0.0) });
}

void flangedSaw (double t, int, Partials& out)
{
    // A saw through a flanger: a comb of notches sweeping up and down the spectrum.
    const double f0 = 164.81 * drift (t, 0);
    const double delay = 0.0006 * std::pow (2.0, 3.0 * cycle (t, 8.0));
    const double level = smooth (0.0, 0.3, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
    {
        const double phase = twoPi * f0 * h * delay;
        const double comb = std::sqrt (1.0 + 0.85 * 0.85 + 2.0 * 0.85 * std::cos (phase)) / 1.85;
        out.push_back ({ f0 * h, level * saw (h) * comb * lowpass2 (f0 * h, 5000.0, 0.0) });
    }
}

void talkingSquare (double t, int, Partials& out)
{
    // A square wave through two moving resonances, like a talk box saying "ah ee oo ah".
    const double f0 = 110.0 * drift (t, 0);
    const double p = std::fmod (t / designSeconds, 1.0) * 3.0;   // ah to ee to oo and back to ah
    struct V { double f1, f2; };
    constexpr std::array<V, 4> path { { { 750, 1200 }, { 300, 2300 }, { 320, 800 }, { 750, 1200 } } };
    const int seg = std::min (2, (int) p);
    const double u = smooth (0.0, 1.0, p - seg);
    const double f1 = path[(size_t) seg].f1 * std::pow (path[(size_t) seg + 1].f1 / path[(size_t) seg].f1, u);
    const double f2 = path[(size_t) seg].f2 * std::pow (path[(size_t) seg + 1].f2 / path[(size_t) seg].f2, u);
    const double width = 0.5 - 0.1 * cycle (t, 2.7);
    const double level = smooth (0.0, 0.1, t);
    for (int h = 1, n = harmonicsBelow (f0, 16000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double shape = 0.12 + peak (f, f1, 7.0) + 0.7 * peak (f, f2, 9.0);
        out.push_back ({ f, level * pulse (h, width) * shape * lowpass2 (f, 6000.0, 0.0) });
    }
}

//==============================================================================
// Instruments.

// A body or bore's resonances: a floor plus a few peaks.
double body (double f, std::initializer_list<std::array<double, 3>> peaks, double floor)
{
    double g = floor;
    for (const auto& p : peaks)
        g += p[2] * peak (f, p[0], p[1]);
    return g;
}

void cello (double t, int, Partials& out)
{
    // A bowed string: saw-like, brighter as the bow digs in, through the body's resonances.
    // Two long bows with a soft change of direction in the middle, and vibrato after the start.
    const double bowChange = 1.0 - 0.45 * std::exp (-std::pow ((t - 4.0) / 0.18, 2.0));
    const double swell = 0.7 + 0.3 * std::sin (pi * std::fmod (t, 4.0) / 4.0);
    const double dyn = smooth (0.0, 0.35, t) * bowChange * swell;
    const double vib = 0.0035 * smooth (0.5, 1.3, t) * std::sin (twoPi * 5.6 * t);
    const double f0 = 130.81 * (1.0 + vib) * drift (t, 0);
    const double tilt = 1.0 + 0.5 * (1.0 - dyn);
    for (int h = 1, n = harmonicsBelow (f0, 12000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double b = body (f, { { 280, 4, 1.0 }, { 520, 5, 0.8 }, { 1100, 4, 0.6 }, { 2600, 3, 0.4 } }, 0.35);
        out.push_back ({ f, dyn * b * (1.0 / std::pow (h, tilt)) * lowpass2 (f, 6000.0, 0.0) });
    }
}

void violinSection (double t, int layer, Partials& out)
{
    // Six violins, each with its own vibrato and tuning, swelling together then easing back.
    const double rate = 5.2 + 0.24 * layer;
    const double vib = 0.004 * smooth (0.3, 1.2, t) * std::sin (twoPi * rate * t + 1.3 * layer);
    const double f0 = 440.0 * spreadCents (layer, 6, 10.0) * (1.0 + vib) * drift (t, layer);
    const double dyn = smooth (0.0, 1.2, t) * (0.65 + 0.35 * smooth (1.0, 5.0, t) - 0.3 * smooth (5.2, 7.8, t));
    for (int h = 1, n = harmonicsBelow (f0, 14000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double b = body (f, { { 290, 4, 0.8 }, { 470, 5, 0.8 }, { 1000, 2, 0.6 }, { 2700, 3, 0.7 }, { 3500, 4, 0.4 } }, 0.3);
        out.push_back ({ f, dyn * b / std::pow (h, 0.9 + 0.3 * (1.0 - dyn)) * lowpass2 (f, 8000.0, 0.0) });
    }
}

void flute (double t, int, Partials& out)
{
    // Mostly fundamental; blown harder, the upper harmonics come up. A swell, then a softer one.
    const double dyn = smooth (0.0, 0.25, t) * (0.6 + 0.4 * std::pow (std::sin (pi * t / 5.0), 2.0) * (t < 5.0 ? 1.0 : 0.0)
                                                + (t >= 5.0 ? 0.25 * std::pow (std::sin (pi * (t - 5.0) / 3.0), 2.0) : 0.0));
    const double vib = 0.005 * smooth (0.4, 1.0, t) * std::sin (twoPi * 5.0 * t);
    const double f0 = 523.25 * (1.0 + vib) * drift (t, 0);
    constexpr std::array<double, 8> base { 1.0, 0.32, 0.12, 0.07, 0.035, 0.02, 0.012, 0.007 };
    for (int h = 1; h <= 8; ++h)
        out.push_back ({ f0 * h, dyn * base[(size_t) h - 1] * std::pow (0.6 + 0.6 * dyn, h - 1) });
}

void clarinet (double t, int, Partials& out)
{
    // Odd harmonics dominate the low register, evens creep in up high. No vibrato; a long
    // crescendo and diminuendo that brightens the tone as it grows.
    const double dyn = smooth (0.0, 0.2, t) * (0.45 + 0.55 * std::pow (std::sin (pi * std::min (t, 7.6) / 7.6), 1.5));
    const double f0 = 196.0 * drift (t, 0);
    const double cutoff = 1400.0 + 3000.0 * dyn;
    for (int h = 1, n = harmonicsBelow (f0, 12000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double a = h % 2 == 1 ? 1.0 / h : (f > 1500.0 ? 0.5 / h : 0.06 / h);
        out.push_back ({ f, dyn * a * lowpass2 (f, cutoff, 0.15) });
    }
}

void frenchHorn (double t, int, Partials& out)
{
    // Warm and round when soft; at the top of the crescendo the brass "opens" and brightens.
    const double dyn = smooth (0.0, 0.3, t) * (0.4 + 0.6 * smooth (0.5, 5.0, t) - 0.35 * smooth (5.5, 7.8, t));
    const double f0 = 174.61 * drift (t, 0) * (1.0 + 0.002 * smooth (2.0, 3.0, t) * std::sin (twoPi * 4.8 * t));
    const double cutoff = 700.0 * std::pow (2.0, 2.2 * dyn);
    for (int h = 1, n = harmonicsBelow (f0, 12000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double b = 0.4 + peak (f, 450.0, 1.5);
        out.push_back ({ f, dyn * b / h * lowpass4 (f, cutoff, 0.1) });
    }
}

void churchOrgan (double t, int layer, Partials& out)
{
    // Stops drawn one after another: soft flutes, then a principal, the 4', and a mixture.
    // Two ranks a hair apart give the slow beating of real pipes.
    const double f0 = 130.81 * (layer == 0 ? 1.0 : cents (1.5));
    const double flutes = smooth (0.0, 0.15, t);
    const double principal = smooth (1.8, 2.6, t);
    const double octave = smooth (3.4, 4.2, t);
    const double mixture = smooth (5.0, 5.8, t);
    const double g = layer == 0 ? 1.0 : 0.7;
    for (int h = 1, n = harmonicsBelow (f0, 14000.0); h <= n; ++h)
    {
        double a = 0.0;
        if (h <= 3)
            a += flutes * (h == 1 ? 1.0 : h == 2 ? 0.12 : 0.05);                       // 8' flute
        a += principal * 0.7 / std::pow (h, 1.3) * (h <= 14 ? 1.0 : 0.0);             // 8' principal
        if (h % 2 == 0)
            a += octave * 0.45 / std::pow (h / 2, 1.4) * (h <= 20 ? 1.0 : 0.0);        // 4' octave
        if (h == 3 || h == 4 || h == 6 || h == 8 || h == 12 || h == 16)
            a += mixture * 0.18;                                                       // mixture
        out.push_back ({ f0 * h, g * a });
    }
}

// A struck or plucked partial's level: an attack of a few milliseconds and a decay.
double strike (double t, double at, double decay)
{
    if (t < at)
        return 0.0;
    const double s = t - at;
    return std::min (1.0, s / 0.004) * std::exp (-s / decay);
}

void grandPiano (double t, int layer, Partials& out)
{
    // Three strings a hair apart (the slow beating of a piano note), slightly stretched
    // partials, a hammer that excites the upper ones less, and two-stage decays: fast at first,
    // then a long singing tail. Higher partials die away sooner.
    const double f0 = 261.63 * spreadCents (layer, 3, 0.9);
    constexpr double stiffness = 0.0004;
    for (int h = 1, n = harmonicsBelow (f0, 12000.0); h <= n; ++h)
    {
        const double hz = f0 * h * std::sqrt (1.0 + stiffness * h * h);
        const double hammer = std::sin (pi * h / 8.0 + 0.2) / h / (1.0 + std::pow (h / 10.0, 2.0));
        const double tau = 7.0 / (1.0 + 0.12 * h + 0.004 * h * h);
        const double env = 0.65 * strike (t, 0.0, 0.25 * tau) + 0.35 * strike (t, 0.0, tau);
        out.push_back ({ hz, hammer * env });
    }
}

void vibraphone (double t, int, Partials& out)
{
    // A tuned bar (partials at 1, 4 and 10 times the note) with the motor's tremolo, which
    // slows down across the take.
    const double f0 = 349.23;
    const double tremPhase = twoPi * (5.5 * t - 0.15 * t * t);   // 5.5 Hz slowing to 3.1 Hz
    const double trem = 1.0 - 0.45 * (0.5 - 0.5 * std::cos (tremPhase));
    const double fade = 1.0 + 0.2 * smooth (0.0, 8.0, t);   // the resonator keeps the fundamental going
    out.push_back ({ f0, trem * strike (t, 0.0, 7.0 * fade) });
    out.push_back ({ f0 * 4.0, trem * 0.3 * strike (t, 0.0, 1.6) });
    out.push_back ({ f0 * 10.0, trem * 0.1 * strike (t, 0.0, 0.5) });
    out.push_back ({ f0 * 2.0, trem * 0.05 * strike (t, 0.0, 3.0) });
}

void harp (double t, int, Partials& out)
{
    // A string plucked near its middle, then plucked again halfway through the take.
    const double f0 = 196.0;
    for (int h = 1, n = harmonicsBelow (f0, 10000.0); h <= n; ++h)
    {
        const double a = std::sin (pi * h * 0.3) / (h * h) * 3.0;
        const double tau = 4.5 / (1.0 + 0.15 * h);
        out.push_back ({ f0 * h * std::sqrt (1.0 + 0.00005 * h * h), a * (strike (t, 0.0, tau) + 0.8 * strike (t, 4.0, tau)) });
    }
}

void accordion (double t, int layer, Partials& out)
{
    // Three reeds tuned apart (the musette's beating), reedy and bright, with the bellows
    // pushing, turning round at 4 s, and pulling.
    const double f0 = 220.0 * spreadCents (layer, 3, 14.0);
    const double bellows = smooth (0.0, 0.15, t) * (0.75 + 0.25 * std::sin (pi * std::fmod (t, 4.0) / 4.0))
                         * (1.0 - 0.55 * std::exp (-std::pow ((t - 4.0) / 0.12, 2.0)));
    for (int h = 1, n = harmonicsBelow (f0, 12000.0); h <= n; ++h)
    {
        const double f = f0 * h;
        const double reed = (h % 2 == 1 ? 1.3 : 1.0) / std::pow (h, 0.85);
        out.push_back ({ f, bellows * reed * (0.5 + peak (f, 1600.0, 1.2)) * lowpass2 (f, 2500.0 + 3000.0 * bellows, 0.0) });
    }
}

Patch patchFor (PlaceholderVowel v)
{
    switch (v)
    {
        case PlaceholderVowel::sweepingSaw:   return { 3, sweepingSaw, 110.0 };
        case PlaceholderVowel::pwmStrings:    return { 4, pwmStrings, 220.0 };
        case PlaceholderVowel::morphPad:      return { 2, morphPad, 146.83 };
        case PlaceholderVowel::wobbleBass:    return { 1, wobbleBass, 82.41 };
        case PlaceholderVowel::syncSweep:     return { 1, syncSweep, 130.81 };
        case PlaceholderVowel::supersawRise:  return { 7, supersawRise, 220.0 };
        case PlaceholderVowel::fmMorph:       return { 2, fmMorph, 196.0 };
        case PlaceholderVowel::wavefoldDrone: return { 1, wavefoldDrone, 130.81 };
        case PlaceholderVowel::flangedSaw:    return { 1, flangedSaw, 164.81 };
        case PlaceholderVowel::talkingSquare: return { 1, talkingSquare, 110.0 };
        case PlaceholderVowel::cello:         return { 1, cello, 130.81 };
        case PlaceholderVowel::violinSection: return { 6, violinSection, 440.0 };
        case PlaceholderVowel::flute:         return { 1, flute, 523.25 };
        case PlaceholderVowel::clarinet:      return { 1, clarinet, 196.0 };
        case PlaceholderVowel::frenchHorn:    return { 1, frenchHorn, 174.61 };
        case PlaceholderVowel::churchOrgan:   return { 2, churchOrgan, 130.81 };
        case PlaceholderVowel::grandPiano:    return { 3, grandPiano, 261.63 };
        case PlaceholderVowel::vibraphone:    return { 1, vibraphone, 349.23 };
        case PlaceholderVowel::harp:          return { 1, harp, 196.0 };
        case PlaceholderVowel::accordion:     return { 3, accordion, 220.0 };
        default:                              return {};
    }
}

//==============================================================================
PlaceholderVoice play (const Patch& patch, double sr, int n)
{
    constexpr int block = 64;
    const double dur = (double) n / sr;
    const double toDesign = designSeconds / dur;
    const double topHz = std::min (0.45 * sr, 16000.0);

    PlaceholderVoice voice;
    voice.sampleRate = sr;
    voice.rootHz = (float) patch.rootHz;

    // Each partial is a phasor (cos, sin) turned by its frequency every sample, so changing
    // frequencies stay phase-continuous. Levels glide linearly across each block.
    struct Osc { double c = 1.0, s = 0.0, lastAmp = 0.0; };
    std::vector<std::vector<Osc>> oscs ((size_t) patch.layers);
    Partials parts;
    std::vector<double> out ((size_t) n, 0.0);

    for (int start = 0; start < n; start += block)
    {
        const int len = std::min (block, n - start);
        const double t = (double) start / sr * toDesign;
        for (int layer = 0; layer < patch.layers; ++layer)
        {
            parts.clear();
            patch.fill (t, layer, parts);
            auto& bank = oscs[(size_t) layer];
            const bool first = start == 0;
            if (bank.size() < parts.size())
                bank.resize (parts.size());

            for (size_t k = 0; k < bank.size(); ++k)
            {
                auto& o = bank[k];
                double hz = k < parts.size() ? parts[k].hz : 0.0;
                double amp = k < parts.size() ? parts[k].amp : 0.0;
                if (hz <= 0.0 || hz >= topHz)
                    amp = 0.0;
                else
                    amp *= 1.0 - smooth (0.75 * topHz, topHz, hz);
                const double from = first ? amp : o.lastAmp;
                o.lastAmp = amp;
                if (from == 0.0 && amp == 0.0)
                    continue;

                const double w = twoPi * hz / sr;
                const double cw = std::cos (w), sw = std::sin (w);
                const double step = (amp - from) / (double) block;
                double a = from, c = o.c, s = o.s;
                for (int i = 0; i < len; ++i)
                {
                    const double nc = c * cw - s * sw;
                    s = s * cw + c * sw;
                    c = nc;
                    a += step;
                    out[(size_t) (start + i)] += a * s;
                }
                const double r = 1.0 / std::sqrt (c * c + s * s);
                o.c = c * r;
                o.s = s * r;
            }
        }
    }

    // A short fade at each end so the take never clicks, then level by loudness (about
    // -16 dB RMS), never letting the peak pass -6 dB.
    double peakLevel = 0.0, power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double tt = (double) i / sr;
        out[(size_t) i] *= smooth (0.0, 0.003, tt) * (1.0 - smooth (dur - 0.5, dur, tt));
        peakLevel = std::max (peakLevel, std::abs (out[(size_t) i]));
        power += out[(size_t) i] * out[(size_t) i];
    }
    const double rms = std::sqrt (power / (double) n);
    const double gain = peakLevel > 0.0 ? std::min (0.5 / peakLevel, 0.16 / std::max (rms, 1.0e-9)) : 1.0;
    voice.samples.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        voice.samples[(size_t) i] = (float) (out[(size_t) i] * gain);
    return voice;
}
} // namespace

bool isPlaceholderSource (PlaceholderVowel vowel)
{
    return patchFor (vowel).fill != nullptr;
}

PlaceholderVoice makePlaceholderSource (PlaceholderVowel vowel, double sampleRate, float seconds)
{
    return play (patchFor (vowel), sampleRate, std::max (1, (int) (seconds * sampleRate)));
}

} // namespace sing
