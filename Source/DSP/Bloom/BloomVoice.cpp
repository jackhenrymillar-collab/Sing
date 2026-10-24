#include "BloomVoice.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr int minSize = 1024;
constexpr int phasorBits = 12;
constexpr int phasorCount = 1 << phasorBits;
// While a note's first frame is built (before it sounds), it takes this many steps of work
// per 128 samples: about 80 ms at 48 kHz, and light enough for a chord to start at once.
constexpr double primeUnitsPer128 = 3.0;

// A harmonic's phase runs on smoothly from frame to frame, nudged by up to this fraction of a
// turn each frame: enough for a slow shimmer, not enough for frames to cancel.
constexpr float shimmer = 0.06f;

// Synthesis window: Hann, whose values sum to 2 and squares to 1.5 at 4x overlap, so both
// steady harmonics and noise keep a constant level.
const float windowScale = 1.0f / std::sqrt (1.5f);
const float coherentGain = std::sqrt (1.5f) / 2.0f;

// Sets the level against the dry loop: at the root, Bloom plays at the loop's level, and
// across two octaves of keys within about 3 dB of it, as granular does (the level table in
// the tests).
constexpr float outputGain = 0.9f;

// Fixed random phase offsets between left and right, so the stereo image is wide but steady.
constexpr int offsetBits = 13;
const uint32_t* stereoOffsets() noexcept
{
    static const std::vector<uint32_t> table = []
    {
        std::vector<uint32_t> t ((size_t) 1 << offsetBits);
        uint32_t r = 0x9e3779b9u;
        for (auto& v : t)
        {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            v = r;
        }
        return t;
    }();
    return table.data();
}

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
    maxSize = nearestPowerOfTwo (BloomAnalysis::maxWindowSeconds * hostRate, minSize, 1 << 18);

    for (int s = minSize; s <= maxSize; s *= 2)
        RealFFT::makeShared (s);
    (void) phasors();
    (void) BloomAnalysis::decode (0);

    (void) stereoOffsets();
    phases.assign ((size_t) maxLayers * (size_t) (maxSize / 2 + 1), 0u);
    spectrumLeft.assign ((size_t) maxSize + 2, 0.0f);
    spectrumRight.assign ((size_t) maxSize + 2, 0.0f);
    for (int i = 0; i <= overlap; ++i)
    {
        framesLeft[(size_t) i].assign ((size_t) maxSize, 0.0f);
        framesRight[(size_t) i].assign ((size_t) maxSize, 0.0f);
    }
    synthesisWindow.resize ((size_t) maxSize + 1);
    for (int i = 0; i <= maxSize; ++i)
        synthesisWindow[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / maxSize));

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
    hop = size / overlap;
    fft = RealFFT::shared (size);
    if (fft == nullptr)
        return;

    units = spectrumUnits + 2 * fft->inverseStepCount() + 1;

    readPosition = std::fmod (std::max (0.0, bufferPosition), (double) src.size());
    for (int i = 0; i <= overlap; ++i)
    {
        std::fill_n (framesLeft[(size_t) i].data(), size, 0.0f);
        std::fill_n (framesRight[(size_t) i].data(), size, 0.0f);
    }
    newest = 0;   // a silent frame; the first real one is built into the next slot
    realFrame.fill (false);
    firstFrame = true;
    combMakeup = 0.0f;
    powerBeforeComb = powerAfterComb = 0.0;
    pos = 0;
    primeCredit = 0.0;
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
    const double p = currentFramePosition + currentFrameAdvance * (double) pos / std::max (1, hop)
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

float BloomVoice::harmonicsGain (const BloomAnalysis& analysis, int frame, double bufferPosition, float pitch) const noexcept
{
    // The voice's harmonics sit on the envelope at the recorded pitch. Moved by a ratio (and
    // the envelope by Formant), they land on louder or quieter parts of it: an "ooh" played
    // an octave up loses its strong fundamental and drops by 10 dB. Granular keeps an even
    // level across the keys, so Bloom does too: compare the envelope's power at the
    // harmonics, as recorded and as played.
    const float period = source->periodAt (bufferPosition);
    if (period < 2.0f)
        return 1.0f;
    const double f0 = source->sampleRate() / period;
    const double topHz = 0.45 * std::min (source->sampleRate(), hostRate);
    const float* env = analysis.envelope (frame);
    const double perPoint = 1.0 / analysis.envelopeHz();
    const double formant = std::max (params.formantRatio, 1.0e-3f);
    const auto envAt = [&] (double hz) noexcept
    {
        const double x = hz * perPoint;
        const int i = std::min ((int) x, BloomAnalysis::envelopePoints - 2);
        const auto t = (float) (x - i);
        return env[i] + t * (env[i + 1] - env[i]);
    };

    double recorded = 0.0, played = 0.0;
    for (double hz = f0; hz < topHz; hz += f0)
    {
        const float e = envAt (hz);
        recorded += (double) e * e;
    }
    for (double hz = f0 * pitch; hz < topHz; hz += f0 * pitch)
    {
        const float e = envAt (hz / formant);
        played += (double) e * e;
    }
    if (played <= 0.0 || recorded <= 0.0)
        return 1.0f;
    return (float) std::clamp (std::sqrt (recorded / played), 0.25, 4.0);
}

void BloomVoice::beginFrame() noexcept
{
    unitsDone = 0;
    phaseHops = 1;
    // The work for this frame runs evenly over what is left of the hop.
    firstDue = (double) pos;
    unitSpacing = 0.95 * (double) (hop - pos) / units;
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
        // Classic (unlocked) frames have no harmonics marked, so everything moves with the
        // pitch, as the method shifts the whole spectrum.
        f.noiseScale = analysis->locksHarmonics() && source->periodAt (at) >= 2.0f ? outBinHz / analysis->binHz() : f.binScale;
        f.shiftBins = params.shiftHz / (pitch * analysis->binHz());
        f.noiseShiftBins = f.noiseScale == f.binScale ? f.shiftBins : params.shiftHz / analysis->binHz();
        // Magnitudes are densities, so a frame of N bins needs sqrt(N) to hold the same
        // power. Breath and noise keep their level when the pitch moves, since the envelope
        // stays put.
        f.amplitude = std::sqrt ((float) size);
        // Classic frames carry the harmonics as noise bins, moved with the pitch: each one's
        // peak is widened by the ratio (its power grows by it), and the harmonics land on
        // other parts of the envelope. Make up both, as for locked harmonics.
        if (! analysis->locksHarmonics())
            f.amplitude *= harmonicsGain (*analysis, f.frameA, at, pitch) / std::sqrt (pitch);
        // Harmonics: moving up by a ratio leaves 1/ratio as many under the envelope and
        // widens each one's peak by the ratio (its bins add in step), so sqrt(N / ratio)
        // keeps their power...
        f.tonalAmplitude = analysis->locksHarmonics()
                             ? std::sqrt ((float) size / pitch) * coherentGain
                                   // ...as long as the envelope is as loud where they land.
                                   * harmonicsGain (*analysis, f.frameA, at, pitch)
                             : 0.0f;
        f.gainLeft = layer.gainLeft * outputGain;
        f.gainRight = layer.gainRight * outputGain;
        f.harmonicsPerBin = params.harmonicsCents > 0.0f ? outBinHz / std::max (1.0f, params.rootHz * pitch) : 0.0f;
        f.peakBin = -1;
    }

    powerBeforeComb = powerAfterComb = 0.0;

    // Harmonics: a width in cents, as a fraction of the frequency (cents / (1200 / ln 2)).
    harmonicsWidth = std::clamp (params.harmonicsCents, 0.0f, 1200.0f) / 1731.234f;
    harmonicsCount = std::max (1, params.harmonicsCount);

    // Filter: each edge a twelfth of an octave either side of its frequency.
    filterOn = params.filter;
    if (filterOn)
    {
        const float edge = 1.05946309f;   // 2^(1/12)
        const float lowBin = std::max (params.lowHz, 1.0f) / outBinHz, highBin = std::max (params.highHz, 1.0f) / outBinHz;
        filterStop = lowBin > highBin;
        const float lo = std::min (lowBin, highBin), hi = std::max (lowBin, highBin);
        riseFrom = lo / edge;
        riseTo = lo * edge;
        fallFrom = hi / edge;
        fallTo = hi * edge;
    }

    envScale = outBinHz / (std::max (params.formantRatio, 1.0e-3f) * analysis->envelopeHz());
    stereoOffset = (uint32_t) (std::clamp (params.width, 0.0f, 1.0f) * 65535.0f);

    // Then move the read position on for the frame after this one.
    buildFramePosition = readPosition;
    buildFrameAdvance = params.hold ? 0.0 : (double) hop * (voiceRate / hostRate) * std::max (0.0f, params.speed);

    // Onsets: while a transient is inside the window, move at the recorded speed (or the
    // set speed, if faster), so it passes as it was played instead of smearing over seconds.
    if (! params.hold && params.onsets > 0.0f && params.speed < 1.0f)
    {
        const double realTime = (double) hop * (voiceRate / hostRate);
        const double half = 0.5 * (double) size * voiceRate / hostRate;
        float strength = 0.0f;
        for (const auto& o : analysis->onsets())
        {
            double d = std::fmod (o.position - readPosition, length);
            if (d < 0.0)
                d += length;
            if (d > length * 0.5)
                d -= length;   // the nearest way round the loop
            if (d > -half && d <= half)
                strength = std::max (strength, o.strength);
        }
        const float pull = std::min (1.0f, strength * params.onsets);
        buildFrameAdvance += (realTime - buildFrameAdvance) * (double) pull;
    }
    readPosition = std::fmod (readPosition + buildFrameAdvance, length);
}

float BloomVoice::filterGain (int bin) const noexcept
{
    // Raised-cosine edges on a log frequency scale.
    constexpr float pi = 3.14159265f;
    const float k = (float) bin;
    float rise = 1.0f, fall = 1.0f;
    if (k <= riseFrom)
        rise = 0.0f;
    else if (k < riseTo)
        rise = 0.5f - 0.5f * std::cos (pi * std::log (k / riseFrom) / std::log (riseTo / riseFrom));
    if (k >= fallTo)
        fall = 0.0f;
    else if (k > fallFrom)
        fall = 0.5f + 0.5f * std::cos (pi * std::log (k / fallFrom) / std::log (fallTo / fallFrom));
    const float pass = std::min (rise, fall);
    return filterStop ? 1.0f - pass : pass;
}

float BloomVoice::combGain (const LayerFrame& f, int bin) const noexcept
{
    // Distance to the nearest of the key's harmonics, in bins, against a width that grows
    // with the harmonic (the same number of cents) but never shrinks below most of a bin,
    // or short windows would miss the low harmonics altogether.
    const float x = (float) bin * f.harmonicsPerBin;
    if (x > (float) harmonicsCount + 0.5f)
        return 0.0f;
    const float n = std::max (1.0f, std::floor (x + 0.5f));
    const float binsPerHarmonic = 1.0f / f.harmonicsPerBin;
    const float distance = (x - n) * binsPerHarmonic;
    const float width = std::max (0.75f, n * binsPerHarmonic * harmonicsWidth);
    const float d = distance / width;
    return d * d > 9.0f ? 0.0f : std::exp2 (-d * d);
}

void BloomVoice::buildBins (int from, int to) noexcept
{
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));
    const int bins = analysis->numBins();
    const int outBins = size / 2 + 1;
    const float* table = phasors();
    const uint32_t* offsets = stereoOffsets();
    constexpr int envLast = BloomAnalysis::envelopePoints - 1;
    constexpr int phaseShift = 32 - phasorBits;
    constexpr uint32_t halfTurn = 0x80000000u;
    const auto shimmerRange = (uint32_t) (shimmer * 4294967296.0f);

    const bool lockedMode = analysis->locksHarmonics();
    const float tone = std::clamp (params.tone, -1.0f, 1.0f);
    const float tonalWeight = std::min (1.0f, 1.0f + tone), noiseWeight = std::min (1.0f, 1.0f - tone);
    const float blur = std::clamp (params.blur, 0.0f, 1.0f);

    float* sl = spectrumLeft.data();
    float* sr = spectrumRight.data();
    for (int k = from; k < to; ++k)
    {
        sl[2 * k] = sl[2 * k + 1] = 0.0f;
        sr[2 * k] = sr[2 * k + 1] = 0.0f;
    }
    from = std::max (from, 1);             // no DC
    to = std::min (to, size / 2);          // no Nyquist

    for (int l = 0; l < frameLayerCount; ++l)
    {
        auto& f = frameLayers[(size_t) l];
        if (f.gainLeft == 0.0f && f.gainRight == 0.0f)
            continue;

        // The loop can be swapped while a frame is being built (a new loop start, length or
        // Window), and the new analysis may have fewer frames: keep the indices inside it.
        const int frames = analysis->numFrames();
        const int frameA = f.frameA % frames, frameB = f.frameB % frames;
        const uint16_t* fineA = analysis->fine (frameA);
        const uint16_t* fineB = analysis->fine (frameB);
        const float* envA = analysis->envelope (frameA);
        const float* envB = analysis->envelope (frameB);
        const int16_t* peaks = analysis->peaks (f.frameMix < 0.5f ? frameA : frameB);
        const float mixB = f.frameMix, mixA = 1.0f - mixB;
        const float gl = f.gainLeft, gr = f.gainRight;
        uint32_t* phase = phases.data() + (size_t) l * (size_t) outBins;
        const uint32_t layerSalt = (uint32_t) l * 977u;

        const auto fineAt = [&] (int b, float t) noexcept
        {
            return mixA * (BloomAnalysis::decode (fineA[b]) + t * (BloomAnalysis::decode (fineA[b + 1]) - BloomAnalysis::decode (fineA[b])))
                 + mixB * (BloomAnalysis::decode (fineB[b]) + t * (BloomAnalysis::decode (fineB[b + 1]) - BloomAnalysis::decode (fineB[b])));
        };

        for (int k = from; k < to; ++k)
        {
            // The envelope follows Formant only, so the vowel stays put across keys.
            const float e = (float) k * envScale;
            const int e0 = (int) e;
            if (e0 >= envLast)
                continue;
            const float et = e - (float) e0;
            const float env = mixA * (envA[e0] + et * (envA[e0 + 1] - envA[e0])) + mixB * (envB[e0] + et * (envB[e0 + 1] - envB[e0]));

            // Harmonics follow the pitch: is there one where this bin reads, moved by it?
            // Frequency spread: each bin reads from a random point nearby, a new one every
            // frame, which smears the spectrum by up to 12 % (about two semitones) either way.
            float jitter = 0.0f;
            if (blur > 0.0f)
                jitter = blur * ((float) nextRandom() * (2.0f / 4294967296.0f) - 1.0f);
            float src = (float) k * f.binScale - f.shiftBins;
            src += jitter * std::max (1.5f, 0.12f * src);
            if (src < 1.0f)
                continue;   // shifted below the bottom
            const int nearest = (int) (src + 0.5f);
            const int16_t code = nearest < bins - 1 ? peaks[nearest] : BloomAnalysis::noPeak;
            float fine = 0.0f;
            if (code != BloomAnalysis::noPeak)
                fine = fineAt ((int) src, src - (float) (int) src);
            else
            {
                // Breath and noise: on a pitched voice they stay where they were recorded
                // (they have no pitch to move), stepping round the harmonics there.
                float at = (float) k * f.noiseScale - f.noiseShiftBins;
                at += jitter * std::max (1.5f, 0.12f * at);
                if (at < 1.0f)
                    continue;
                int b = (int) at;
                if (b >= bins - 1)
                    continue;
                float t = at - (float) b;
                if (f.noiseScale != f.binScale && peaks[b] != BloomAnalysis::noPeak)
                {
                    int d = 1;
                    while (d <= 3 && peaks[std::max (0, b - d)] != BloomAnalysis::noPeak && peaks[std::min (bins - 2, b + d)] != BloomAnalysis::noPeak)
                        ++d;
                    b = peaks[std::max (0, b - d)] == BloomAnalysis::noPeak ? std::max (0, b - d) : std::min (bins - 2, b + d);
                    t = 0.0f;
                }
                fine = fineAt (b, t);
            }
            float a = fine * env;
            if (filterOn)
                a *= filterGain (k);
            if (f.harmonicsPerBin > 0.0f)
            {
                // Harmonics: note the power before and after the comb, for the make-up.
                const float c = combGain (f, k);
                const float p = a * a * (f.gainLeft * f.gainLeft + f.gainRight * f.gainRight);
                powerBeforeComb += p;
                powerAfterComb += p * c * c;
                a *= c;
            }
            if (a <= 0.0f)
                continue;

            // Phase. Breath and noise get a new random phase every frame: the smear. A
            // harmonic keeps running at its own frequency from frame to frame, so a held
            // vowel stays steady instead of trembling, and its neighbouring bins follow it
            // half a turn apart, as a windowed sine's do.
            uint32_t ph, offsetIndex;
            // Tone: harmonics against noise and breath.
            float gain = f.amplitude * (code == BloomAnalysis::noPeak ? noiseWeight : tonalWeight);
            if (gain <= 0.0f)
                continue;
            if (code == BloomAnalysis::noPeak || ! lockedMode)
            {
                ph = nextRandom();
                offsetIndex = (uint32_t) k;
            }
            else
            {
                const float peakOut = ((float) nearest + (float) code * (1.0f / BloomAnalysis::peakScale)) / f.binScale;
                const int kp = std::clamp ((int) (peakOut + 0.5f), 1, outBins - 1);
                if (kp != f.peakBin)
                {
                    // Cycles per hop at the harmonic's frequency.
                    const double cycles = (double) peakOut * phaseHops / overlap;
                    const auto advance = (uint32_t) (int64_t) ((cycles - std::floor (cycles)) * 4294967296.0);
                    const uint32_t nudge = (uint32_t) (((uint64_t) nextRandom() * shimmerRange) >> 32) - shimmerRange / 2;
                    f.peakBin = kp;
                    f.peakPhase = firstFrame ? nextRandom() : phase[kp] + advance + nudge;
                }
                ph = f.peakPhase + (((uint32_t) (k - kp) & 1u) ? halfTurn : 0u);
                offsetIndex = (uint32_t) kp;
                // Harmonics stay in phase across the overlapping frames, so they add in
                // amplitude (Hann at 4x overlap sums to 2) where noise adds in power (its
                // squares sum to 1.5).
                gain = f.tonalAmplitude * tonalWeight;
            }
            phase[k] = ph;

            // Right runs ahead of or behind left by up to half a turn times Width.
            const auto offset = (int32_t) offsets[(offsetIndex + layerSalt) & ((1u << offsetBits) - 1u)];
            const uint32_t phR = ph + (uint32_t) (((int64_t) offset * (int64_t) stereoOffset) >> 16);
            const float* p = table + 2 * (ph >> phaseShift);
            const float* q = table + 2 * (phR >> phaseShift);
            const float al = a * gain * gl, ar = a * gain * gr;
            sl[2 * k] += al * p[0];
            sl[2 * k + 1] += al * p[1];
            sr[2 * k] += ar * q[0];
            sr[2 * k + 1] += ar * q[1];
        }
    }
}

void BloomVoice::runUnit (int unit) noexcept
{
    const int steps = fft->inverseStepCount();
    const int bins = size / 2 + 1;
    if (unit < spectrumUnits)
    {
        const int from = bins * unit / spectrumUnits, to = bins * (unit + 1) / spectrumUnits;
        buildBins (from, to);
    }
    else if (unit < spectrumUnits + steps)
        fft->inverseStep (unit - spectrumUnits, spectrumLeft.data(), framesLeft[(size_t) building()].data());
    else if (unit < spectrumUnits + 2 * steps)
        fft->inverseStep (unit - spectrumUnits - steps, spectrumRight.data(), framesRight[(size_t) building()].data());
    else
    {
        float* fl = framesLeft[(size_t) building()].data();
        float* fr = framesRight[(size_t) building()].data();
        const int stride = maxSize / size;
        // Harmonics keeps only a narrow comb, which can leave little of a voice whose pitch
        // wanders off the key's harmonics. Make up what the comb took from this frame (by at
        // most 24 dB, eased from frame to frame), so switching it on keeps the level.
        if (powerBeforeComb > 0.0)
        {
            const double lost = powerAfterComb > 0.0 ? std::sqrt (powerBeforeComb / powerAfterComb) : 16.0;
            const auto target = (float) std::clamp (lost, 1.0, 16.0);
            combMakeup = combMakeup <= 0.0f ? target : combMakeup + 0.5f * (target - combMakeup);
        }
        else
            combMakeup = 0.0f;   // off: the next time it is on, start from that frame's own value
        const float scale = windowScale * std::max (combMakeup, 1.0f);
        for (int i = 0; i < size; ++i)
        {
            const float w = synthesisWindow[(size_t) (i * stride)] * scale;
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
    firstFrame = false;
    // The frame just built starts playing; the oldest, now finished, is built next.
    realFrame[(size_t) building()] = true;
    newest = building();
    filling = false;
    for (int j = 0; j < overlap; ++j)
        filling = filling || ! realFrame[(size_t) ((newest - j + overlap + 1) % (overlap + 1))];
    currentFramePosition = buildFramePosition;
    currentFrameAdvance = buildFrameAdvance;
}

void BloomVoice::renderFilling (float* left, float* right, int len) noexcept
{
    // While a note starts, only some of the overlapping frames are real. Make up the
    // difference, by up to 12 dB, so the note starts near full level instead of swelling in.
    // A harmonic's frame carries the analysis window as well as the synthesis one, so its
    // shape is close to Hann cubed, whose sum at 4x overlap is 1.25.
    const int stride = maxSize / size;
    const float* w = synthesisWindow.data();
    for (int i = 0; i < len; ++i)
    {
        float sum = 0.0f, l = 0.0f, r = 0.0f;
        for (int j = 0; j < overlap; ++j)
        {
            const int f = (newest - j + overlap + 1) % (overlap + 1);
            if (! realFrame[(size_t) f])
                continue;
            const int at = j * hop + pos + i;
            const float x = w[(size_t) (at * stride)];
            sum += x * x * x;
            l += framesLeft[(size_t) f][(size_t) at];
            r += framesRight[(size_t) f][(size_t) at];
        }
        const float gain = sum > 0.0f ? std::min (4.0f, 1.25f / sum) : 0.0f;
        left[i] += l * gain;
        right[i] += r * gain;
    }
}

void BloomVoice::render (float* left, float* right, int numSamples) noexcept
{
    if (! active)
        return;

    if (priming)
    {
        // The first frame, a few steps per block. Silent until it is ready.
        primeCredit += primeUnitsPer128 * numSamples / 128.0;
        while (primeCredit >= 1.0 && unitsDone < units)
        {
            runUnit (unitsDone++);
            primeCredit -= 1.0;
        }
        if (unitsDone < units)
            return;
        priming = false;
        // Each note's frames start at its own point in the hop, so a chord's notes take
        // turns doing their work instead of all working in the same audio blocks.
        pos = std::min (hop - 1, (int) (float) (scheduleFraction * (float) hop));
        rotate();
        // The first frame starts a hop into its window, so the note sounds at once instead
        // of fading in over half a window. The frames that follow fill in the overlap over
        // the next three hops, with renderFilling making up the level meanwhile.
        newest = (newest + 1) % (overlap + 1);
        filling = true;
        beginFrame();
        // So the next frame starts two hops after the first one's start, not one, and its
        // harmonics' phases run on that much further.
        phaseHops = 2;
    }

    int done = 0;
    while (done < numSamples)
    {
        runDueUnits();

        int len = std::min (numSamples - done, hop - pos);
        if (unitsDone < units)
        {
            const double due = firstDue + unitsDone * unitSpacing;
            len = std::min (len, std::max (1, (int) std::ceil (due - (double) pos)));
        }

        if (filling)
        {
            renderFilling (left + done, right + done, len);
            pos += len;
            done += len;
            if (pos >= hop)
            {
                finishFrame();
                rotate();
                pos = 0;
                beginFrame();
            }
            continue;
        }

        // The newest frame plays its first quarter, the one before its second, and so on.
        for (int j = 0; j < overlap; ++j)
        {
            const int f = (newest - j + overlap + 1) % (overlap + 1);
            const float* fl = framesLeft[(size_t) f].data() + j * hop + pos;
            const float* fr = framesRight[(size_t) f].data() + j * hop + pos;
            for (int i = 0; i < len; ++i)
            {
                left[done + i] += fl[i];
                right[done + i] += fr[i];
            }
        }

        pos += len;
        done += len;
        if (pos >= hop)
        {
            finishFrame();
            rotate();
            pos = 0;
            beginFrame();
        }
    }
}

} // namespace sing
