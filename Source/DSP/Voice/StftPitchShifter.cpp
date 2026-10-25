#include "StftPitchShifter.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr int baseSize = 4096;        // shortest frame at 48 kHz
constexpr float periodsPerFrame = 8.0f;
constexpr int overlap = 8;            // a new frame every eighth of one
constexpr int cepstrumDecimation = 4; // the envelope's spectrum: one point per four bins
constexpr double pi = 3.14159265358979323846;
constexpr double twoPi = 2.0 * pi;

double wrapPhase (double p) noexcept
{
    return p - twoPi * std::floor (p / twoPi + 0.5);
}

// atan2 to about 1e-5 radians (a minimax polynomial on [0, 1] and octant folding): the
// library's own option, ENABLE_ARCTANGENT_APPROXIMATION, does the same to save time.
float fastAtan2 (float y, float x) noexcept
{
    const float ax = std::abs (x), ay = std::abs (y);
    const float mx = std::max (ax, ay), mn = std::min (ax, ay);
    if (mx <= 0.0f)
        return 0.0f;
    const float a = mn / mx, s = a * a;
    float r = ((((-0.0046496474f * s + 0.0242181f) * s - 0.0596838f) * s + 0.1064738f) * s - 0.1421753f) * s;
    r = ((r + 0.1997708f) * s - 0.3333276f) * s * a + a;
    if (ay > ax) r = 1.57079632679f - r;
    if (x < 0.0f) r = 3.14159265359f - r;
    return y < 0.0f ? -r : r;
}

// sin and cos of a phase in [-pi, pi], to about 1e-6 (Taylor terms to x^9 on a quarter turn).
void fastSinCos (float p, float& sn, float& cs) noexcept
{
    // Fold to [-pi/2, pi/2], where the polynomials are accurate; cos changes sign.
    float sign = 1.0f;
    if (p > 1.57079632679f) { p = 3.14159265359f - p; sign = -1.0f; }
    else if (p < -1.57079632679f) { p = -3.14159265359f - p; sign = -1.0f; }
    const float q = p * p;
    sn = p * (1.0f + q * (-1.6666667e-1f + q * (8.3333333e-3f + q * (-1.9841270e-4f + q * (2.7557319e-6f - q * 2.5052108e-8f)))));
    cs = sign * (1.0f + q * (-0.5f + q * (4.1666667e-2f + q * (-1.3888889e-3f + q * (2.4801587e-5f + q * (-2.7557319e-7f + q * 2.0876757e-9f))))));
}
} // namespace

void StftPitchShifter::prepare (double sampleRate, float maxPeriodSeconds)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // Long frames: at least eight periods of the lowest pitch, and about 85 ms (4096 samples
    // at 48 kHz) at the least, so the harmonics of low voices sit in separate bins.
    const double scale = rate / 48000.0;
    minSize = 1024;
    while ((double) minSize < (double) baseSize * scale * 0.75)
        minSize *= 2;
    maxSize = minSize;
    while ((double) maxSize < periodsPerFrame * maxPeriodSeconds * rate && maxSize < 32768)
        maxSize *= 2;

    ffts.clear();
    cepstrumFfts.clear();
    for (int s = minSize; s <= maxSize; s *= 2)
    {
        ffts.emplace_back (s);
        cepstrumFfts.emplace_back (s / cepstrumDecimation);
    }

    const auto n = (size_t) maxSize;
    const size_t bins = n / 2 + 1;
    window.assign (n, 0.0f);
    frame.assign (n, 0.0f);
    cepstrum.assign (n, 0.0f);
    logSpec.assign (n + 2, 0.0f);
    for (auto* c : { &left, &right })
    {
        for (auto* v : { &c->in, &c->out, &c->acc })
            v->assign (n, 0.0f);
        c->spec.assign (n + 2, 0.0f);
        for (auto* v : { &c->lastPhase, &c->phase, &c->mag, &c->freq })
            v->assign (bins, 0.0f);
    }
    for (auto* v : { &envelope, &movedEnvelope, &shiftedMag, &shiftedFreq })
        v->assign (bins, 0.0f);
    start (0.0f, 1.0f, false, 1.0f);
}

int StftPitchShifter::start (float periodSamples, float f, bool env, float envRatio) noexcept
{
    setPeriod (periodSamples);
    setShift (f, env, envRatio);

    size = minSize;
    if (period >= 2.0f)
        while (size < maxSize && (float) size < periodsPerFrame * period)
            size *= 2;
    hop = size / overlap;
    const auto sizeIndex = (size_t) std::lround (std::log2 ((double) size / minSize));
    fft = &ffts[sizeIndex];
    cepstrumFft = &cepstrumFfts[sizeIndex];

    // Hann, scaled so analysis times synthesis window overlap-adds to exactly one.
    const float norm = std::sqrt ((float) hop / (0.375f * (float) size));
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = norm * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));

    for (auto* c : { &left, &right })
        for (auto* v : { &c->in, &c->out, &c->acc })
            std::fill (v->begin(), v->end(), 0.0f);
    running = false;

    // Each note runs its frames at its own point in the hop, so a chord's FFTs spread out.
    static unsigned int notes = 0;
    const auto turn = (unsigned int) (++notes * 2654435761u) >> 8;
    rover = size - hop + (int) ((unsigned long long) turn * (unsigned) hop >> 24);
    return latency();
}

void StftPitchShifter::setPeriod (float periodSamples) noexcept
{
    period = periodSamples >= 2.0f ? periodSamples : 0.0f;
}

void StftPitchShifter::setShift (float f, bool env, float envRatio) noexcept
{
    // Within half a cent, no change.
    factor = std::abs (f - 1.0f) < 0.0003f ? 1.0f : std::clamp (f, minFactor, maxFactor);
    envelopeRatio = std::abs (envRatio - 1.0f) < 0.0003f ? 1.0f : std::clamp (envRatio, minFactor, maxFactor);
    envelopeOn = env;
}

void StftPitchShifter::process (const float* xL, const float* xR, float* yL, float* yR, int numSamples) noexcept
{
    stereo = xR != nullptr && yR != nullptr;
    const int lag = size - hop;
    for (int s = 0; s < numSamples; ++s)
    {
        left.in[(size_t) rover] = xL[s];
        right.in[(size_t) rover] = stereo ? xR[s] : 0.0f;
        yL[s] = left.out[(size_t) (rover - lag)];
        if (stereo)
            yR[s] = right.out[(size_t) (rover - lag)];
        if (++rover >= size)
        {
            processFrame();
            rover = lag;
        }
    }
}

void StftPitchShifter::analyse (Channel& c) noexcept
{
    const int n = size, bins = size / 2 + 1;
    for (int i = 0; i < n; ++i)
        frame[(size_t) i] = c.in[(size_t) i] * window[(size_t) i];
    fft->forward (frame.data(), c.spec.data());

    // The phase vocoder: each bin's exact frequency, in bins, from how far its phase moved
    // since the last frame beyond what the bin's own frequency explains.
    const double inc = twoPi * hop / n;
    for (int k = 0; k < bins; ++k)
    {
        const float re = c.spec[(size_t) (2 * k)], im = c.spec[(size_t) (2 * k + 1)];
        const double ph = (double) fastAtan2 (im, re);
        const double expected = (double) k * inc;
        if (! running)
            c.lastPhase[(size_t) k] = (float) wrapPhase (ph - expected);
        const double delta = wrapPhase (ph - (double) c.lastPhase[(size_t) k] - expected);
        c.lastPhase[(size_t) k] = (float) wrapPhase (ph);
        c.mag[(size_t) k] = std::sqrt (re * re + im * im);
        c.freq[(size_t) k] = (float) ((double) k + delta / inc);
        if (! running)
            c.phase[(size_t) k] = (float) ph;   // synthesis starts from the input's phases
    }
}

void StftPitchShifter::findEnvelope() noexcept
{
    // Cepstral smoothing: log magnitudes, inverse FFT to the cepstrum, keep only the
    // quefrencies below the lifter, FFT back. The lifter sits below the pitch period (or
    // at 1 ms), so the harmonics' ripple is removed and the vowel's shape is left.
    const int n = size, bins = size / 2 + 1;
    float loudest = 0.0f;
    for (int k = 0; k < bins; ++k)
    {
        const float m = stereo ? std::sqrt (0.5f * (left.mag[(size_t) k] * left.mag[(size_t) k]
                                                    + right.mag[(size_t) k] * right.mag[(size_t) k]))
                               : left.mag[(size_t) k];
        envelope[(size_t) k] = m;
        loudest = std::max (loudest, m);
    }
    const float floor = std::max (loudest * 1.0e-5f, 1.0e-12f);

    // The envelope is smooth, so the cepstrum runs on a spectrum a quarter as fine (each
    // point the mean of four bins): a quarter of the work, and the same envelope.
    const int m = n / cepstrumDecimation, smallBins = m / 2 + 1;
    for (int j = 0; j < smallBins; ++j)
    {
        const int lo = std::max (0, j * cepstrumDecimation - cepstrumDecimation / 2);
        const int hi = std::min (bins, j * cepstrumDecimation + cepstrumDecimation / 2);
        float sum = 0.0f;
        for (int k = lo; k < hi; ++k)
            sum += envelope[(size_t) k];
        logSpec[(size_t) (2 * j)] = std::log (sum / (float) std::max (1, hi - lo) + floor);
        logSpec[(size_t) (2 * j + 1)] = 0.0f;
    }
    cepstrumFft->inverse (logSpec.data(), cepstrum.data());

    const double lifterSeconds = period >= 2.0f ? std::min (0.001, 0.7 * period / rate) : 0.001;
    // The coarser spectrum still gives a cepstrum in samples, only shorter: frames hold at
    // least eight periods, so the pitch's own peak (at one period) stays inside it.
    const int cut = std::clamp ((int) (lifterSeconds * rate), 1, m / 2 - 1);
    for (int i = cut + 1; i < m - cut; ++i)
        cepstrum[(size_t) i] = 0.0f;
    cepstrumFft->forward (cepstrum.data(), logSpec.data());
    for (int k = 0; k < bins; ++k)
    {
        const float at = (float) k / (float) cepstrumDecimation;
        const int j0 = std::min ((int) at, smallBins - 1), j1 = std::min (j0 + 1, smallBins - 1);
        const float t = at - (float) j0;
        envelope[(size_t) k] = std::exp (logSpec[(size_t) (2 * j0)] + t * (logSpec[(size_t) (2 * j1)] - logSpec[(size_t) (2 * j0)]));
    }
    const float envFloor = floor * 10.0f;

    // The envelope as the output will have it: read at bin / envelopeRatio.
    for (int k = 0; k < bins; ++k)
    {
        const float at = (float) k / envelopeRatio;
        const int k0 = (int) at;
        if (k0 >= bins - 1) { movedEnvelope[(size_t) k] = envelope[(size_t) (bins - 1)]; continue; }
        const float t = at - (float) k0;
        movedEnvelope[(size_t) k] = envelope[(size_t) k0] + t * (envelope[(size_t) k0 + 1] - envelope[(size_t) k0]);
    }
    for (int k = 0; k < bins; ++k)
        envelope[(size_t) k] = std::max (envelope[(size_t) k], envFloor);
}

void StftPitchShifter::shiftAndSynthesise (Channel& c) noexcept
{
    const int n = size, bins = size / 2 + 1;
    const float nyquist = (float) (bins - 1);

    double before = 0.0;
    for (int k = 1; k < bins - 1; ++k)
        before += (double) c.mag[(size_t) k] * c.mag[(size_t) k];

    if (envelopeOn)
        for (int k = 0; k < bins; ++k)
            c.mag[(size_t) k] /= envelope[(size_t) k];

    // Read the bins at k / factor (linear between neighbours, DC and Nyquist left out), and
    // multiply the frequencies by factor.
    const float inv = 1.0f / factor;
    for (int k = 0; k < bins; ++k)
    {
        const float at = (float) k * inv;
        const int k0 = (int) at;
        float m = 0.0f, f = 0.0f;
        if (k0 > 0 && k0 < bins - 1)
        {
            const float t = at - (float) k0;
            m = c.mag[(size_t) k0] + t * (c.mag[(size_t) k0 + 1] - c.mag[(size_t) k0]);
            f = (c.freq[(size_t) k0] + t * (c.freq[(size_t) k0 + 1] - c.freq[(size_t) k0])) * factor;
        }
        if (f <= 0.0f || f >= nyquist)
            m = 0.0f;
        shiftedMag[(size_t) k] = envelopeOn ? m * movedEnvelope[(size_t) k] : m;
        shiftedFreq[(size_t) k] = f;
    }

    // Keep the frame's power (the library's normalisation): reading the bins closer
    // together or further apart narrows or widens each partial, which would otherwise make
    // keys below the root quieter and keys above louder. The overlapping frames' phases no
    // longer line up exactly once shifted, which loses about 2.6 dB in the overlap-add
    // (measured on voices and steady tones); makeUp puts that back, so off-root keys sit
    // level with the root (where the sound passes through untouched).
    double after = 0.0;
    for (int k = 1; k < bins - 1; ++k)
        after += (double) shiftedMag[(size_t) k] * shiftedMag[(size_t) k];
    if (after > 1.0e-30)
    {
        constexpr double makeUp = 1.35;
        const auto g = (float) std::min (8.0, makeUp * std::sqrt (before / after));
        for (int k = 0; k < bins; ++k)
            shiftedMag[(size_t) k] *= g;
    }

    // Each bin's phase runs on at its new frequency.
    const double inc = twoPi * hop / n;
    for (int k = 0; k < bins; ++k)
    {
        const double ph = wrapPhase ((double) c.phase[(size_t) k] + (double) shiftedFreq[(size_t) k] * inc);
        c.phase[(size_t) k] = (float) ph;
        const float m = (k == 0 || k == bins - 1) ? 0.0f : shiftedMag[(size_t) k];
        float sn, cs;
        fastSinCos ((float) ph, sn, cs);
        c.spec[(size_t) (2 * k)] = m * cs;
        c.spec[(size_t) (2 * k + 1)] = m * sn;
    }

    fft->inverse (c.spec.data(), frame.data());
    for (int i = 0; i < n; ++i)
        c.acc[(size_t) i] += frame[(size_t) i] * window[(size_t) i];
}

void StftPitchShifter::processFrame() noexcept
{
    const int n = size;
    const bool identity = factor == 1.0f && (! envelopeOn || envelopeRatio == 1.0f);

    if (identity)
    {
        // Nothing to change: the windows alone, which overlap-add back to the input.
        for (auto* c : { &left, &right })
            for (int i = 0; i < n; ++i)
                c->acc[(size_t) i] += c->in[(size_t) i] * window[(size_t) i] * window[(size_t) i];
        running = false;
    }
    else
    {
        // A centred note has the same left and right: one channel's work does for both.
        const bool twoChannels = stereo && ! std::equal (left.in.begin(), left.in.begin() + n, right.in.begin());
        analyse (left);
        if (twoChannels)
            analyse (right);
        else
            right.mag = left.mag;   // same size: no allocation
        if (envelopeOn)
            findEnvelope();
        shiftAndSynthesise (left);
        if (twoChannels)
            shiftAndSynthesise (right);
        else
        {
            // Carry the phases along so the right channel can take over seamlessly.
            right.lastPhase = left.lastPhase;
            right.phase = left.phase;
            if (stereo)
                for (int i = 0; i < n; ++i)
                    right.acc[(size_t) i] += frame[(size_t) i] * window[(size_t) i];
        }
        running = true;
    }

    for (auto* c : { &left, &right })
    {
        std::copy_n (c->acc.begin(), hop, c->out.begin());
        std::copy (c->acc.begin() + hop, c->acc.begin() + n, c->acc.begin());
        std::fill (c->acc.begin() + (n - hop), c->acc.begin() + n, 0.0f);
        std::copy (c->in.begin() + hop, c->in.begin() + n, c->in.begin());
    }
}

} // namespace sing
