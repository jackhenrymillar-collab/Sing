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

// A harmonic's phase runs on smoothly from frame to frame, nudged by up to this fraction of a
// turn each frame: enough for a slow shimmer, not enough for frames to cancel.
constexpr float shimmer = 0.06f;

// Locked harmonics' synthesis window: Hann, whose values sum to 2 and squares to 1.5 at 4x
// overlap, so both steady harmonics and noise keep a constant level.
const float windowScale = 1.0f / std::sqrt (1.5f);
const float coherentGain = std::sqrt (1.5f) / 2.0f;

// Window shapes, sampled finely enough to read with straight-line interpolation at any size.
constexpr int windowPoints = 1 << 16;

// Hann over the whole frame.
const float* hannTable() noexcept
{
    static const std::vector<float> table = []
    {
        std::vector<float> t ((size_t) windowPoints + 1);
        for (int i = 0; i <= windowPoints; ++i)
            t[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / windowPoints));
        return t;
    }();
    return table.data();
}

// Classic frames at 2x overlap: Hann divided by the root of its squares summed with the other
// frame's, so the two frames' squares add to exactly 1 at every sample. Frames of noise with
// random phases add in power, so the crossfade between them never dips (plain Hann at 2x
// would dip by 3 dB halfway).
const float* evenPowerTable() noexcept
{
    static const std::vector<float> table = []
    {
        std::vector<float> t ((size_t) windowPoints + 1);
        for (int i = 0; i <= windowPoints; ++i)
        {
            const double a = 0.5 - 0.5 * std::cos (2.0 * pi * i / windowPoints);
            const double b = 1.0 - a;   // Hann half a frame away
            t[(size_t) i] = (float) (a / std::sqrt (a * a + b * b));
        }
        return t;
    }();
    return table.data();
}

// Work per unit: a unit of each stage costs about the same as any other.
constexpr int gatherBinLayersPerUnit = 1024;
constexpr int binsPerUnit = 4096;
constexpr int butterfliesPerUnit = 2048;
constexpr int samplesPerUnit = 8192;

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

} // namespace

void BloomVoice::prepare (double hostSampleRate)
{
    hostRate = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
    // Long windows' FFTs are made with their loop's analysis.
    for (int s = minSize; s <= 1 << 16; s *= 2)
        RealFFT::makeShared (s);
    (void) phasors();
    (void) hannTable();
    (void) evenPowerTable();
    (void) BloomAnalysis::decode (0);
    (void) stereoOffsets();
    stop();
}

bool BloomVoice::start (const LoopSource& src, double bufferPosition, uint32_t seed, float scheduleOffset) noexcept
{
    stop();
    const auto* analysis = static_cast<const BloomAnalysis*> (src.analysis (StretchMode::bloom));
    if (analysis == nullptr)
        return false;
    work = analysis->claimWorkspace();
    if (work == nullptr)
        return false;
    fft = RealFFT::shared (work->size);
    if (fft == nullptr)
    {
        work->release();
        work = nullptr;
        return false;
    }

    source = &src;
    workspaceOwner = &src;
    rng = seed != 0 ? seed : 1u;
    scheduleFraction = std::clamp (scheduleOffset, 0.0f, 1.0f);

    // Frames as long (in time) as the ones analysed, so a short loop's smaller window
    // carries through: the analysis made the buffers at that size.
    size = work->size;
    overlap = work->overlap;
    hop = size / overlap;
    fftChunks = std::max (1, size / 4 / butterfliesPerUnit);

    readPosition = std::fmod (std::max (0.0, bufferPosition), (double) src.size());
    // The play light shows the note's start while its first frame is built, not wherever
    // this voice's last note had got to.
    currentFramePosition = buildFramePosition = readPosition;
    currentFrameAdvance = buildFrameAdvance = 0.0;
    // No clearing (a long window's frames are tens of MB): frames not yet built are marked
    // in realFrame and never played, and building a frame writes all of it.
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
    // The first frame in about 80 ms (30 blocks of 128 at 48 kHz), at 3 to 16 units a
    // block: long windows take longer to start rather than loading the CPU.
    primeRate = std::clamp ((double) units / (0.08 * hostRate / 128.0), 3.0, 16.0);
    return true;
}

void BloomVoice::setSource (const LoopSource& src) noexcept
{
    if (! active || ! canPlay (src) || &src == source)
        return;
    // A loop analysed the other way (locked harmonics or not) needs other buffers: keep
    // playing the old one.
    const auto* analysis = static_cast<const BloomAnalysis*> (src.analysis (StretchMode::bloom));
    if (analysis->locksHarmonics() != (overlap == 4))
        return;
    // Move into the new loop's buffers when they are the same size, by swapping what the two
    // hold (no copying), so the old loop can be let go. Otherwise (the Window changed) the
    // note keeps the old loop's buffers, and its window, until it ends.
    if (workspaceOwner != &src)
        if (auto* fresh = analysis->claimWorkspace())
        {
            if (fresh->size == work->size && fresh->overlap == work->overlap && fresh->phases.size() == work->phases.size())
            {
                fresh->spectrumLeft.swap (work->spectrumLeft);
                fresh->spectrumRight.swap (work->spectrumRight);
                fresh->prefixLeft.swap (work->prefixLeft);
                fresh->prefixRight.swap (work->prefixRight);
                fresh->framesLeft.swap (work->framesLeft);
                fresh->framesRight.swap (work->framesRight);
                fresh->phases.swap (work->phases);
                work->release();
                work = fresh;
                workspaceOwner = &src;
            }
            else
                fresh->release();
        }
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
    workspaceOwner = nullptr;
    if (work != nullptr)
        work->release();
    work = nullptr;
    unitsDone = 0;
    units = 0;
}

void BloomVoice::nudge (double samples) noexcept
{
    if (! active || source == nullptr || samples == 0.0)
        return;
    const auto length = (double) source->size();
    readPosition = std::fmod (readPosition + samples, length);
    if (readPosition < 0.0)
        readPosition += length;
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
    // The work for this frame runs evenly over what is left of the hop (spacing set below).
    firstDue = (double) pos;
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));

    // Everything this frame needs is fixed now, so a frame never mixes two settings.
    const double voiceRate = source->sampleRate();
    outBinHz = (float) (hostRate / size);
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

    // Filter: sharp edges a twelfth of an octave either side of each frequency, or smooth
    // slopes from there.
    filterOn = params.filter;
    filterSlope = std::clamp (params.filterSlope, 0, 2);
    if (filterOn)
    {
        const float edge = 1.05946309f;   // 2^(1/12)
        lowBin = std::max (params.lowHz, 1.0f) / outBinHz;
        highBin = std::max (params.highHz, 1.0f) / outBinHz;
        filterStop = lowBin > highBin;
        const float lo = std::min (lowBin, highBin), hi = std::max (lowBin, highBin);
        riseFrom = lo / edge;
        riseTo = lo * edge;
        fallFrom = hi / edge;
        fallTo = hi * edge;
    }

    envScale = outBinHz / (std::max (params.formantRatio, 1.0e-3f) * analysis->envelopeHz());

    // Compressor: toward the loop's typical frame power, read where the centre copy is.
    frameCompressorGain = 1.0f;
    if (params.compressor > 0.0f && frameLayerCount > 0 && analysis->typicalPower() > 0.0f)
    {
        const auto& f = frameLayers[0];
        const float p = (1.0f - f.frameMix) * analysis->framePower (f.frameA) + f.frameMix * analysis->framePower (f.frameB);
        const float ratio = analysis->typicalPower() / std::max (p, analysis->typicalPower() * 1.0e-6f);
        frameCompressorGain = std::clamp (std::pow (ratio, 0.5f * std::clamp (params.compressor, 0.0f, 1.0f)), 0.0625f, 4.0f);
    }

    // The classic modules, fixed for the frame.
    frameLocked = analysis->locksHarmonics();
    frameOctaves = false;
    if (! frameLocked && params.octaves)
    {
        float total = 0.0f;
        for (int i = 0; i < numOctaves; ++i)
        {
            const float l = std::clamp (params.octaveLevels[(size_t) i], 0.0f, 1.0f);
            octaveWeights[(size_t) i] = l * l;
            total += l * l;
            frameOctaves = frameOctaves || (i != 2 && l > 0.0f);
        }
        if (total > 0.0f)
            for (auto& w : octaveWeights)
                w /= total;
        else
            frameOctaves = false;
    }
    const float smear = std::clamp (params.blur, 0.0f, 1.0f);
    frameSmear = ! frameLocked && smear > 0.0f;
    // Up to 0.6 octave either side (about half the frequency), rising with the square so the
    // first part of the control stays fine.
    smearWidth = frameSmear ? std::exp2 (0.6f * smear * smear) - 1.0f : 0.0f;

    // The work, in units of about equal cost.
    const int outBins = size / 2 + 1;
    const auto pieces = [] (int count, int per) { return std::max (1, (count + per - 1) / per); };
    stagePieces.fill (0);
    stagePieces[gather] = pieces (outBins, std::max (256, gatherBinLayersPerUnit / std::max (1, frameLayerCount)));
    if (! frameLocked)
    {
        if (frameOctaves)
        {
            stagePieces[prefixA] = pieces (outBins, 8 * binsPerUnit);
            stagePieces[octaveMix] = pieces (outBins, binsPerUnit / numOctaves);
        }
        if (frameSmear)
        {
            stagePieces[prefixB] = pieces (outBins, 8 * binsPerUnit);
            stagePieces[smearMix] = pieces (outBins, binsPerUnit);
        }
        stagePieces[finalPass] = pieces (outBins, binsPerUnit / 2);
    }
    // Which half of each complex bin holds the power: gather writes the real half, Octaves
    // the imaginary half, and Smear the half Octaves didn't.
    smearSlot = frameOctaves ? 1 : 0;
    finalSlot = frameSmear ? 1 - smearSlot : smearSlot;
    stagePieces[inverseLeft] = stagePieces[inverseRight] = fft->inverseChunkCount (fftChunks);
    stagePieces[windowing] = pieces (size, samplesPerUnit);
    stageStart[0] = 0;
    for (int i = 0; i < numStages; ++i)
        stageStart[(size_t) i + 1] = stageStart[(size_t) i] + stagePieces[(size_t) i];
    units = stageStart[numStages];
    unitSpacing = 0.95 * (double) (hop - pos) / units;

    stereoOffset = (uint32_t) (std::clamp (params.width, 0.0f, 1.0f) * 65535.0f);

    // Then move the read position on for the frame after this one.
    buildFramePosition = readPosition;
    buildFrameAdvance = params.hold ? 0.0 : (double) hop * (voiceRate / hostRate) * std::max (0.0f, params.speed);

    // Onsets: while a transient is inside the window, move at the recorded speed (or the
    // set speed, if faster), so it passes as it was played instead of smearing over seconds.
    // Not while frozen (Hold, or Stretch at Held): a held note stays where it starts, even
    // on a transient (such as a loop seam with no Fade).
    const bool frozen = params.hold || params.speed <= 0.0f;
    if (! frozen && params.onsets > 0.0f && params.speed < 1.0f)
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
    const float k = (float) bin;
    if (filterSlope > 0)
    {
        // Smooth: the magnitude of a Butterworth high-pass at the low edge and low-pass at
        // the high edge, one pole each (6 dB per octave) or two (12 dB per octave).
        const auto rolloff = [this] (float x) noexcept { return filterSlope == 1 ? x * x : x * x * x * x; };
        const float hp = 1.0f / std::sqrt (1.0f + rolloff (lowBin / std::max (k, 1.0e-3f)));
        const float lp = 1.0f / std::sqrt (1.0f + rolloff (k / highBin));
        if (! filterStop)
            return hp * lp;
        // Cutting a band out: what lies below the high edge and above the low edge.
        return std::min (1.0f, std::sqrt (hp * hp + lp * lp));
    }

    // Sharp: raised-cosine edges on a log frequency scale.
    constexpr float pi = 3.14159265f;
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

void BloomVoice::gatherLocked (int from, int to) noexcept
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

    constexpr bool lockedMode = true;
    const float tone = std::clamp (params.tone, -1.0f, 1.0f);
    const float tonalWeight = std::min (1.0f, 1.0f + tone), noiseWeight = std::min (1.0f, 1.0f - tone);
    const float blur = std::clamp (params.blur, 0.0f, 1.0f);

    float* sl = work->spectrumLeft.data();
    float* sr = work->spectrumRight.data();
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
        uint32_t* phase = work->phases.data() + (size_t) l * (size_t) outBins;
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
            // Smear (locked harmonics only): each bin reads from a random point nearby, a new
            // one every frame, which smears the spectrum by up to 12 % either way.
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

void BloomVoice::gatherPower (int from, int to) noexcept
{
    // Classic: each layer's magnitudes, moved by its pitch and the Formant setting, added up
    // as power into the real half of each bin (left and right apart). The phases come later.
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));
    const int bins = analysis->numBins();
    constexpr int envLast = BloomAnalysis::envelopePoints - 1;
    const float tone = std::clamp (params.tone, -1.0f, 1.0f);
    const float tonalWeight = std::min (1.0f, 1.0f + tone), noiseWeight = std::min (1.0f, 1.0f - tone);

    float* sl = work->spectrumLeft.data();
    float* sr = work->spectrumRight.data();
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
        const float gl2 = f.gainLeft * f.gainLeft * f.amplitude * f.amplitude;
        const float gr2 = f.gainRight * f.gainRight * f.amplitude * f.amplitude;

        for (int k = from; k < to; ++k)
        {
            // The envelope follows Formant only, so the vowel stays put across keys.
            const float e = (float) k * envScale;
            const int e0 = (int) e;
            if (e0 >= envLast)
                break;
            const float et = e - (float) e0;
            const float env = mixA * (envA[e0] + et * (envA[e0 + 1] - envA[e0])) + mixB * (envB[e0] + et * (envB[e0 + 1] - envB[e0]));

            // The fine structure, moved by the pitch and the frequency shift.
            const float src = (float) k * f.binScale - f.shiftBins;
            if (src < 1.0f)
                continue;   // shifted below the bottom
            const int b = (int) src;
            if (b >= bins - 1)
                break;
            const float t = src - (float) b;
            const float fine = mixA * (BloomAnalysis::decode (fineA[b]) + t * (BloomAnalysis::decode (fineA[b + 1]) - BloomAnalysis::decode (fineA[b])))
                             + mixB * (BloomAnalysis::decode (fineB[b]) + t * (BloomAnalysis::decode (fineB[b + 1]) - BloomAnalysis::decode (fineB[b])));

            // Tone: harmonics against noise and breath.
            const int nearest = std::min ((int) (src + 0.5f), bins - 2);
            const float weight = peaks[nearest] == BloomAnalysis::noPeak ? noiseWeight : tonalWeight;
            float a = fine * env * weight;
            if (a <= 0.0f)
                continue;
            float p = a * a;
            if (f.harmonicsPerBin > 0.0f)
            {
                // Harmonics: note the power before and after the comb, for the make-up.
                const float c = combGain (f, k);
                const double layerPower = (double) p * (gl2 + gr2);
                powerBeforeComb += layerPower;
                powerAfterComb += layerPower * c * c;
                p *= c * c;
            }
            sl[2 * k] += p * gl2;
            sr[2 * k] += p * gr2;
        }
    }
}

void BloomVoice::runPrefix (int from, int to, int slot) noexcept
{
    // Running sums of power, so any stretch of the spectrum's power is two lookups. Runs in
    // order, piece by piece: prefix[k] is the power of bins below k.
    const float* sl = work->spectrumLeft.data() + slot;
    const float* sr = work->spectrumRight.data() + slot;
    double* pl = work->prefixLeft.data();
    double* pr = work->prefixRight.data();
    if (from == 0)
        pl[0] = pr[0] = 0.0;
    for (int k = from; k < to; ++k)
    {
        pl[k + 1] = pl[k] + (double) sl[2 * k];
        pr[k + 1] = pr[k] + (double) sr[2 * k];
    }
}

namespace
{
// The power of a prefix-summed spectrum from x0 to x1, in bins (bin k spans k - 1/2 to
// k + 1/2), with each bin's power spread evenly across it.
inline double powerBetween (const double* prefix, int bins, double x0, double x1) noexcept
{
    const auto at = [prefix, bins] (double x) noexcept
    {
        const double y = x + 0.5;
        if (y <= 0.0)
            return 0.0;
        const int i = (int) y;
        if (i >= bins)
            return prefix[bins];
        return prefix[i] + (y - (double) i) * (prefix[i + 1] - prefix[i]);
    };
    return at (x1) - at (x0);
}
} // namespace

void BloomVoice::runOctaves (int from, int to) noexcept
{
    // Each copy moved by its ratio keeps the spectrum's power: an output bin takes the power
    // of the stretch of spectrum that lands on it. Into the imaginary half of each bin.
    const int bins = size / 2 + 1;
    const double* pl = work->prefixLeft.data();
    const double* pr = work->prefixRight.data();
    float* sl = work->spectrumLeft.data();
    float* sr = work->spectrumRight.data();
    for (int k = from; k < to; ++k)
    {
        double l = 0.0, r = 0.0;
        for (int i = 0; i < numOctaves; ++i)
        {
            const float w = octaveWeights[(size_t) i];
            if (w <= 0.0f)
                continue;
            const double ratio = (double) octaveRatios[(size_t) i];
            const double x0 = ((double) k - 0.5) / ratio, x1 = ((double) k + 0.5) / ratio;
            l += w * powerBetween (pl, bins, x0, x1);
            r += w * powerBetween (pr, bins, x0, x1);
        }
        sl[2 * k + 1] = (float) l;
        sr[2 * k + 1] = (float) r;
    }
}

void BloomVoice::runSmear (int from, int to, int slot) noexcept
{
    // Smear: each bin takes the average power over a stretch around it as wide as a fixed
    // share of its frequency, so every harmonic widens by the same musical interval into a
    // soft wash (constant-Q smoothing). Reads the prefix sums of slot, writes the other half.
    const int bins = size / 2 + 1;
    const double* pl = work->prefixLeft.data();
    const double* pr = work->prefixRight.data();
    float* sl = work->spectrumLeft.data();
    float* sr = work->spectrumRight.data();
    const int out = 1 - slot;
    for (int k = from; k < to; ++k)
    {
        const double half = 0.5 + (double) smearWidth * (double) k;
        const double x0 = (double) k - half, x1 = (double) k + half;
        const double scale = 1.0 / (2.0 * half);
        sl[2 * k + out] = (float) (powerBetween (pl, bins, x0, x1) * scale);
        sr[2 * k + out] = (float) (powerBetween (pr, bins, x0, x1) * scale);
    }
}

void BloomVoice::runFinal (int from, int to, int slot) noexcept
{
    // Classic: magnitudes from the power, through the Filter, each bin with a new random
    // phase; right runs ahead of or behind left by up to half a turn times Width.
    const float* table = phasors();
    const uint32_t* offsets = stereoOffsets();
    constexpr int phaseShift = 32 - phasorBits;
    float* sl = work->spectrumLeft.data();
    float* sr = work->spectrumRight.data();
    const int first = std::max (from, 1), last = std::min (to, size / 2);
    for (int k = from; k < to; ++k)
    {
        if (k < first || k >= last)
        {
            sl[2 * k] = sl[2 * k + 1] = sr[2 * k] = sr[2 * k + 1] = 0.0f;
            continue;
        }
        float ml = std::sqrt (std::max (0.0f, sl[2 * k + slot]));
        float mr = std::sqrt (std::max (0.0f, sr[2 * k + slot]));
        if (filterOn)
        {
            const float g = filterGain (k);
            ml *= g;
            mr *= g;
        }
        const uint32_t ph = nextRandom();
        const auto offset = (int32_t) offsets[(uint32_t) k & ((1u << offsetBits) - 1u)];
        const uint32_t phR = ph + (uint32_t) (((int64_t) offset * (int64_t) stereoOffset) >> 16);
        const float* p = table + 2 * (ph >> phaseShift);
        const float* q = table + 2 * (phR >> phaseShift);
        sl[2 * k] = ml * p[0];
        sl[2 * k + 1] = ml * p[1];
        sr[2 * k] = mr * q[0];
        sr[2 * k + 1] = mr * q[1];
    }
}

float BloomVoice::windowAt (int index) const noexcept
{
    const float* table = overlap == 2 ? evenPowerTable() : hannTable();
    const float x = (float) index * ((float) windowPoints / (float) size);
    const int i = std::min ((int) x, windowPoints - 1);
    return table[i] + (x - (float) i) * (table[i + 1] - table[i]);
}

void BloomVoice::runWindow (int from, int to) noexcept
{
    float* fl = work->framesLeft[(size_t) building()].data();
    float* fr = work->framesRight[(size_t) building()].data();
    if (from == 0)
    {
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
    }
    // Classic frames' window squares already sum to 1; locked frames' Hann needs scaling.
    const float scale = (overlap == 2 ? 1.0f : windowScale) * std::max (combMakeup, 1.0f) * frameCompressorGain;
    for (int i = from; i < to; ++i)
    {
        const float w = windowAt (i) * scale;
        fl[i] *= w;
        fr[i] *= w;
    }
}

void BloomVoice::runUnit (int unit) noexcept
{
    int stage = 0;
    while (stage < numStages - 1 && unit >= stageStart[(size_t) stage + 1])
        ++stage;
    const int piece = unit - stageStart[(size_t) stage];
    const int count = stagePieces[(size_t) stage];
    const int bins = size / 2 + 1;
    const int from = (int) ((int64_t) bins * piece / count), to = (int) ((int64_t) bins * (piece + 1) / count);

    switch ((Stage) stage)
    {
        case gather:
            if (frameLocked)
                gatherLocked (from, to);
            else
                gatherPower (from, to);
            break;
        case prefixA:    runPrefix (from, to, 0); break;
        case octaveMix:  runOctaves (from, to); break;
        case prefixB:    runPrefix (from, to, smearSlot); break;
        case smearMix:   runSmear (from, to, smearSlot); break;
        case finalPass:  runFinal (from, to, finalSlot); break;
        case inverseLeft:
            fft->inverseChunk (piece, fftChunks, work->spectrumLeft.data(), work->framesLeft[(size_t) building()].data());
            break;
        case inverseRight:
            fft->inverseChunk (piece, fftChunks, work->spectrumRight.data(), work->framesRight[(size_t) building()].data());
            break;
        case windowing:
            runWindow ((int) ((int64_t) size * piece / count), (int) ((int64_t) size * (piece + 1) / count));
            break;
        case numStages: break;
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
    // Random-phase frames (classic Bloom, and breath in every mode) are noise under the
    // synthesis window, so they add in power: the window's squares sum to 1 for classic
    // frames at 2x overlap, and Hann's to 1.5 at 4x. A locked harmonic's frame carries the
    // analysis window as well, so its shape is close to Hann cubed, and those add in
    // amplitude: Hann cubed sums to 1.25.
    const auto* analysis = static_cast<const BloomAnalysis*> (source->analysis (StretchMode::bloom));
    const bool coherent = analysis != nullptr && analysis->locksHarmonics() && overlap == 4;
    const float fullPower = overlap == 2 ? 1.0f : 1.5f;
    for (int i = 0; i < len; ++i)
    {
        float sum = 0.0f, l = 0.0f, r = 0.0f;
        for (int j = 0; j < overlap; ++j)
        {
            const int f = (newest - j + overlap + 1) % (overlap + 1);
            if (! realFrame[(size_t) f])
                continue;
            const int at = j * hop + pos + i;
            const float x = windowAt (at);
            sum += coherent ? x * x * x : x * x;
            l += work->framesLeft[(size_t) f][(size_t) at];
            r += work->framesRight[(size_t) f][(size_t) at];
        }
        const float gain = sum <= 0.0f ? 0.0f : std::min (4.0f, coherent ? 1.25f / sum : std::sqrt (fullPower / sum));
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
        primeCredit += primeRate * numSamples / 128.0;
        while (primeCredit >= 1.0 && unitsDone < units)
        {
            runUnit (unitsDone++);
            primeCredit -= 1.0;
        }
        if (unitsDone < units)
            return;
        priming = false;
        if (overlap == 2)
        {
            // Classic: the first frame starts partway up its rising half (a third to two
            // thirds of the way, staggered so a chord's notes take turns doing their work),
            // so the note sounds at once, with renderFilling making up the level until the
            // next frame comes in.
            pos = std::min (hop - 1, (int) ((0.3f + 0.4f * scheduleFraction) * (float) hop));
            rotate();
            filling = true;
            beginFrame();
        }
        else
        {
            // Each note's frames start at its own point in the hop, so a chord's notes take
            // turns doing their work instead of all working in the same audio blocks.
            pos = std::min (hop - 1, (int) (float) (scheduleFraction * (float) hop));
            rotate();
            // The first frame starts a hop into its window, so the note sounds at once
            // instead of fading in over half a window. The frames that follow fill in the
            // overlap over the next hops, with renderFilling making up the level meanwhile.
            newest = (newest + 1) % (overlap + 1);
            filling = true;
            beginFrame();
            // So the next frame starts two hops after the first one's start, not one, and its
            // harmonics' phases run on that much further.
            phaseHops = 2;
        }
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

        // The newest frame plays its first part (half or quarter), the one before its second,
        // and so on.
        for (int j = 0; j < overlap; ++j)
        {
            const int f = (newest - j + overlap + 1) % (overlap + 1);
            const float* fl = work->framesLeft[(size_t) f].data() + j * hop + pos;
            const float* fr = work->framesRight[(size_t) f].data() + j * hop + pos;
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
