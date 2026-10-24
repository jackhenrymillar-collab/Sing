#include "BloomVoice.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double targetSeconds = 0.68;
constexpr int minSize = 1024;
constexpr int phasorBits = 12;
constexpr int phasorCount = 1 << phasorBits;
constexpr int primeUnitsPerCall = 3;

// Random phases come from a table of unit phasors.
const float* phasors() noexcept
{
    static const std::vector<float> table = []
    {
        std::vector<float> t ((size_t) phasorCount * 2);
        for (int i = 0; i < phasorCount; ++i)
        {
            t[(size_t) (2 * i)] = (float) std::cos (2.0 * pi * (i + 0.5) / phasorCount);
            t[(size_t) (2 * i + 1)] = (float) std::sin (2.0 * pi * (i + 0.5) / phasorCount);
        }
        return t;
    }();
    return table.data();
}

int nearestPowerOfTwo (double samples, int lo, int hi) noexcept
{
    int size = lo;
    while (size < hi && (double) size * 1.414 < samples)
        size *= 2;
    return size;
}
} // namespace

void BloomVoice::prepare (double hostSampleRate)
{
    hostRate = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
    maxSize = nearestPowerOfTwo (targetSeconds * hostRate * 1.5, minSize, 1 << 18);

    for (int s = minSize; s <= maxSize; s *= 2)
        RealFFT::makeShared (s);
    (void) phasors();
    (void) BloomAnalysis::decode (0);

    spectrumLeft.assign ((size_t) maxSize + 2, 0.0f);
    spectrumRight.assign ((size_t) maxSize + 2, 0.0f);
    for (int i = 0; i < 3; ++i)
    {
        framesLeft[(size_t) i].assign ((size_t) maxSize, 0.0f);
        framesRight[(size_t) i].assign ((size_t) maxSize, 0.0f);
    }
    sineWindow.resize ((size_t) maxSize + 1);
    for (int i = 0; i <= maxSize; ++i)
        sineWindow[(size_t) i] = (float) std::sin (pi * i / maxSize);

    stop();
}

void BloomVoice::start (const LoopSource& src, double bufferPosition, uint32_t seed, float scheduleOffset) noexcept
{
    stop();
    const auto* analysis = static_cast<const BloomAnalysis*> (src.analysis (StretchMode::bloom));
    if (analysis == nullptr || maxSize == 0)
        return;

    source = &src;
    rng = seed != 0 ? seed : 1u;
    scheduleFraction = std::clamp (scheduleOffset, 0.0f, 1.0f);

    // Synthesise frames as long (in time) as the ones analysed, so a short loop's smaller
    // window carries through.
    const double analysedSeconds = (double) analysis->fftSize() / src.sampleRate();
    size = std::min (nearestPowerOfTwo (analysedSeconds * hostRate, minSize, maxSize), maxSize);
    half = size / 2;
    fft = RealFFT::shared (size);
    if (fft == nullptr)
        return;

    units = spectrumUnits + 2 * fft->inverseStepCount() + 1;
    unitSpacing = 0.6 * half / units;
    firstDue = scheduleFraction * 0.3 * half;

    readPosition = std::fmod (std::max (0.0, bufferPosition), (double) src.size());
    std::fill_n (framesLeft[0].data(), size, 0.0f);
    std::fill_n (framesRight[0].data(), size, 0.0f);
    prev = 0;
    cur = 1;
    next = 1;   // the first frame is built straight into cur
    pos = 0;
    priming = true;
    active = true;
    beginFrame();
}

void BloomVoice::setSource (const LoopSource& src) noexcept
{
    if (! active || ! canPlay (src) || &src == source)
        return;
    // Keep the read position at the same place in the voice.
    const double voicePosition = source->toVoicePosition (readPosition);
    source = &src;
    readPosition = src.fromVoicePosition (voicePosition);
}

void BloomVoice::stop() noexcept
{
    active = false;
    priming = false;
    source = nullptr;
    unitsDone = 0;
    units = 0;
}

double BloomVoice::playPosition (double offsetSeconds) const noexcept
{
    if (! active || source == nullptr)
        return 0.0;
    const double p = currentFramePosition + currentFrameAdvance * (double) pos / std::max (1, half)
                   + offsetSeconds * source->sampleRate();
    return source->toVoicePosition (p);
}

double BloomVoice::windowSamples() const noexcept
{
    if (! active || source == nullptr)
        return 0.0;
    return (double) size * source->sampleRate() / hostRate;
}

uint32_t BloomVoice::nextRandom() noexcept
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void BloomVoice::beginFrame() noexcept
{
    unitsDone = 0;
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));

    // Everything this frame needs is fixed now, so a frame never mixes two settings.
    const double voiceRate = source->sampleRate();
    const float outBinHz = (float) (hostRate / size);
    const int frames = analysis->numFrames();
    const double length = (double) source->size();

    frameLayerCount = std::clamp (params.numLayers, 1, maxLayers);
    for (int i = 0; i < frameLayerCount; ++i)
    {
        const auto& layer = params.layers[(size_t) i];
        auto& f = frameLayers[(size_t) i];
        double at = std::fmod (readPosition + layer.offsetSeconds * voiceRate, length);
        if (at < 0.0)
            at += length;
        const double frame = at / analysis->hop();
        const int a = (int) frame;
        f.frameA = a % frames;
        f.frameB = (a + 1) % frames;
        f.frameMix = (float) (frame - a);

        const float pitch = std::max (layer.pitchRatio, 1.0e-3f);
        f.binScale = outBinHz / (pitch * analysis->binHz());
        // Magnitudes are densities, so a frame of N bins needs sqrt(N) to hold the same
        // power; moving them up by a ratio spreads them over that many more bins, so take
        // away sqrt(ratio) to keep the level.
        f.amplitude = std::sqrt ((float) size / pitch);
        f.gainLeft = layer.gainLeft;
        f.gainRight = layer.gainRight;
    }

    envScale = outBinHz / (std::max (params.formantRatio, 1.0e-3f) * analysis->envelopeHz());
    const float theta = (float) (std::clamp (params.width, 0.0f, 1.0f) * pi * 0.5);
    phaseMixSame = std::cos (theta);
    phaseMixOwn = std::sin (theta);

    // Then move the read position on for the frame after this one.
    buildFramePosition = readPosition;
    buildFrameAdvance = params.hold ? 0.0 : (double) half * (voiceRate / hostRate) * std::max (0.0f, params.speed);
    readPosition = std::fmod (readPosition + buildFrameAdvance, length);
}

void BloomVoice::buildBins (int from, int to) noexcept
{
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));
    const int bins = analysis->numBins();
    const float* table = phasors();
    constexpr int envLast = BloomAnalysis::envelopePoints - 1;

    float* sl = spectrumLeft.data();
    float* sr = spectrumRight.data();
    for (int k = from; k < to; ++k)
    {
        sl[2 * k] = sl[2 * k + 1] = 0.0f;
        sr[2 * k] = sr[2 * k + 1] = 0.0f;
    }
    from = std::max (from, 1);             // no DC
    to = std::min (to, half);              // no Nyquist

    for (int l = 0; l < frameLayerCount; ++l)
    {
        const auto& f = frameLayers[(size_t) l];
        if (f.gainLeft == 0.0f && f.gainRight == 0.0f)
            continue;

        const uint16_t* fineA = analysis->fine (f.frameA);
        const uint16_t* fineB = analysis->fine (f.frameB);
        const float* envA = analysis->envelope (f.frameA);
        const float* envB = analysis->envelope (f.frameB);
        const float mixB = f.frameMix, mixA = 1.0f - mixB;
        const float gl = f.gainLeft * f.amplitude, gr = f.gainRight * f.amplitude;

        for (int k = from; k < to; ++k)
        {
            // The fine structure follows the pitch...
            const float src = (float) k * f.binScale;
            const int b0 = (int) src;
            if (b0 >= bins - 1)
                break;
            const float bt = src - (float) b0;
            const float fine = mixA * (BloomAnalysis::decode (fineA[b0]) + bt * (BloomAnalysis::decode (fineA[b0 + 1]) - BloomAnalysis::decode (fineA[b0])))
                             + mixB * (BloomAnalysis::decode (fineB[b0]) + bt * (BloomAnalysis::decode (fineB[b0 + 1]) - BloomAnalysis::decode (fineB[b0])));

            // ...and the envelope follows Formant only, so the vowel stays put across keys.
            const float e = (float) k * envScale;
            const int e0 = (int) e;
            if (e0 >= envLast)
                continue;
            const float et = e - (float) e0;
            const float env = mixA * (envA[e0] + et * (envA[e0 + 1] - envA[e0])) + mixB * (envB[e0] + et * (envB[e0 + 1] - envB[e0]));

            const float a = fine * env;
            const float* p = table + 2 * (nextRandom() >> (32 - phasorBits));
            const float* q = table + 2 * (nextRandom() >> (32 - phasorBits));
            const float pr = p[0], pi_ = p[1];
            const float qr = phaseMixSame * pr + phaseMixOwn * q[0];
            const float qi = phaseMixSame * pi_ + phaseMixOwn * q[1];
            sl[2 * k] += a * gl * pr;
            sl[2 * k + 1] += a * gl * pi_;
            sr[2 * k] += a * gr * qr;
            sr[2 * k + 1] += a * gr * qi;
        }
    }
}

void BloomVoice::runUnit (int unit) noexcept
{
    const int steps = fft->inverseStepCount();
    const int bins = half + 1;
    if (unit < spectrumUnits)
    {
        const int from = bins * unit / spectrumUnits, to = bins * (unit + 1) / spectrumUnits;
        buildBins (from, to);
    }
    else if (unit < spectrumUnits + steps)
        fft->inverseStep (unit - spectrumUnits, spectrumLeft.data(), framesLeft[(size_t) next].data());
    else if (unit < spectrumUnits + 2 * steps)
        fft->inverseStep (unit - spectrumUnits - steps, spectrumRight.data(), framesRight[(size_t) next].data());
    else
    {
        // The sine window: its squares sum to one at 50% overlap.
        float* fl = framesLeft[(size_t) next].data();
        float* fr = framesRight[(size_t) next].data();
        const int stride = maxSize / size;
        for (int i = 0; i < size; ++i)
        {
            const float w = sineWindow[(size_t) (i * stride)];
            fl[i] *= w;
            fr[i] *= w;
        }
    }
}

void BloomVoice::runDueUnits() noexcept
{
    while (unitsDone < units && (double) pos >= firstDue + unitsDone * unitSpacing)
        runUnit (unitsDone++);
}

void BloomVoice::finishFrame() noexcept
{
    while (unitsDone < units)
        runUnit (unitsDone++);
}

void BloomVoice::rotate() noexcept
{
    // The frame that just finished fading out becomes the one being built.
    const int oldPrev = prev;
    prev = cur;
    cur = next;
    next = oldPrev;
    currentFramePosition = buildFramePosition;
    currentFrameAdvance = buildFrameAdvance;
}

void BloomVoice::render (float* left, float* right, int numSamples) noexcept
{
    if (! active)
        return;

    if (priming)
    {
        // The first frame, a few steps per block. Silent until it is ready.
        for (int i = 0; i < primeUnitsPerCall && unitsDone < units; ++i)
            runUnit (unitsDone++);
        if (unitsDone < units)
            return;
        priming = false;
        cur = 1;
        next = 2;
        pos = 0;
        currentFramePosition = buildFramePosition;
        currentFrameAdvance = buildFrameAdvance;
        beginFrame();
    }

    int done = 0;
    while (done < numSamples)
    {
        runDueUnits();

        int len = std::min (numSamples - done, half - pos);
        if (unitsDone < units)
        {
            const double due = firstDue + unitsDone * unitSpacing;
            len = std::min (len, std::max (1, (int) std::ceil (due - (double) pos)));
        }

        const float* pl = framesLeft[(size_t) prev].data() + half + pos;
        const float* pr = framesRight[(size_t) prev].data() + half + pos;
        const float* cl = framesLeft[(size_t) cur].data() + pos;
        const float* cr = framesRight[(size_t) cur].data() + pos;
        for (int i = 0; i < len; ++i)
        {
            left[done + i] += pl[i] + cl[i];
            right[done + i] += pr[i] + cr[i];
        }

        pos += len;
        done += len;
        if (pos >= half)
        {
            finishFrame();
            rotate();
            pos = 0;
            beginFrame();
        }
    }
}

} // namespace sing
