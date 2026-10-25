#include "GranularEngine.h"

#include <algorithm>
#include <cmath>

namespace sing
{

namespace
{
constexpr double twoPi = 6.28318530717958647692;
constexpr int windowSize = 2048;
constexpr int controlStep = 32;           // samples between parameter smoothing updates
constexpr double smoothingSeconds = 0.05;
constexpr float decorrelationSeconds = 0.004f;

// 4-point, 3rd-order Hermite interpolation. p points at sample 0; p[-1] to p[2] are read.
inline float hermite (const float* p, float f) noexcept
{
    const float xm1 = p[-1], x0 = p[0], x1 = p[1], x2 = p[2];
    const float c = 0.5f * (x1 - xm1);
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + 0.5f * (x2 - x0);
    const float b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

inline double wrap (double x, double n) noexcept
{
    x = std::fmod (x, n);
    if (x < 0.0)
        x += n;
    return x < n ? x : 0.0;   // x + n can round up to n
}
} // namespace

GranularEngine::GranularEngine()
{
    window.resize ((size_t) windowSize + 1);
    for (int i = 0; i <= windowSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * (double) i / (double) windowSize));
}

void GranularEngine::prepare (double hostSampleRate, int)
{
    hostRate = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
    smoothingCoeff = (float) std::exp (-(double) controlStep / (smoothingSeconds * hostRate));
    setSettings (settings);
    stop();
}

void GranularEngine::setSettings (const Settings& s) noexcept
{
    settings = s;
    settings.grainSeconds = std::clamp (settings.grainSeconds, 0.01f, 1.0f);
    settings.overlap = std::clamp (settings.overlap, 2, 8);
    settings.jitterSeconds = std::clamp (settings.jitterSeconds, 0.0f, 2.0f);
    settings.timingJitter = std::clamp (settings.timingJitter, 0.0f, 1.0f);
    settings.stereoWidth = std::clamp (settings.stereoWidth, 0.0f, 1.0f);

    // A whole number of grain intervals per grain keeps the Hann windows summing flat.
    const int o = settings.overlap;
    const int interval = std::max (4, (int) std::lround (settings.grainSeconds * hostRate / (double) o));
    grainLength = interval * o;
}

void GranularEngine::setParameters (const StretchParameters& p) noexcept
{
    params = p;
    params.speed = std::clamp (params.speed, 0.0f, 4.0f);
    params.pitchRatio = std::clamp (params.pitchRatio, 1.0f / 64.0f, 16.0f);
}

void GranularEngine::start (const LoopSource& source, double bufferPosition) noexcept
{
    stop();

    current = &source;
    sourceScale = source.sampleRate() / hostRate;
    head = wrap (bufferPosition, (double) source.size());
    speed = params.hold ? 0.0f : params.speed;
    rate = params.pitchRatio;
    phaseReference = head;

    // Start with a full cloud, laid out as if it had been playing up to now, so the note
    // speaks at once. The voice's Attack shapes the onset from there.
    const int interval = grainLength / settings.overlap;
    for (int i = settings.overlap - 1; i >= 0; --i)
        spawnGrain (i * interval);

    samplesToNextGrain = (double) interval;
}

void GranularEngine::setSource (const LoopSource& source) noexcept
{
    if (current == nullptr || current == &source)
        return;

    // Keep the play light in the same place in the voice. Grains already sounding finish on
    // the old loop, so the change crossfades over one grain.
    const double voicePosition = current->toVoicePosition (head);
    current = &source;
    sourceScale = source.sampleRate() / hostRate;
    head = wrap (source.fromVoicePosition (voicePosition), (double) source.size());
    phaseReference = head;
}

void GranularEngine::stop() noexcept
{
    current = nullptr;
    for (auto& g : grains)
        g.source = nullptr;
}

bool GranularEngine::isUsing (const LoopSource& source) const noexcept
{
    if (current == &source)
        return true;

    return std::any_of (grains.begin(), grains.end(), [&] (const Grain& g) { return g.source == &source; });
}

int GranularEngine::activeGrainCount() const noexcept
{
    return (int) std::count_if (grains.begin(), grains.end(), [] (const Grain& g) { return g.source != nullptr; });
}

double GranularEngine::playPosition() const noexcept
{
    return current != nullptr ? current->toVoicePosition (head) : 0.0;
}

float GranularEngine::nextRandom() noexcept
{
    // xorshift32
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return (float) ((double) rngState * (2.0 / 4294967296.0) - 1.0);
}

double GranularEngine::alignToPhase (double target) const noexcept
{
    // Pitch-synchronous placement: move the grain by whole pitch periods so it starts in phase
    // with the rest of the cloud. Overlapping grains of a steady vowel then add up cleanly
    // instead of beating against each other, while jitter still varies what each grain reads.
    // The period where the grain reads, so a voice that moves between notes stays in phase
    // with the note it is actually singing there.
    const double period = (double) current->periodAt (target);
    if (! settings.phaseAlign || period < 2.0)
        return target;

    const double n = (double) current->size();
    double diff = std::fmod (target - phaseReference, n);
    if (diff > 0.5 * n) diff -= n;
    if (diff < -0.5 * n) diff += n;
    return phaseReference + std::round (diff / period) * period;
}

void GranularEngine::followHead() noexcept
{
    // The phase reference runs at the grains' rate and the head at Stretch speed, so they part
    // whenever the two differ (any key but the root, any Stretch above 0). Keep the reference
    // within a period of the head by stepping it one local pitch period at a time: it stays
    // on the same point of the voice's cycle, and alignToPhase only ever measures a short
    // distance. Measured across many periods of a voice whose pitch moves (vibrato), the
    // rounding would put each grain at a different point of the cycle, and the overlapping
    // grains would phase against each other.
    if (! settings.phaseAlign)
        return;
    const double n = (double) current->size();
    for (int guard = 0; guard < 4096; ++guard)
    {
        double diff = std::fmod (head - phaseReference, n);
        if (diff > 0.5 * n) diff -= n;
        if (diff < -0.5 * n) diff += n;
        const double period = (double) current->periodAt (phaseReference);
        if (period < 2.0)
        {
            // Unpitched here: nothing to keep in phase with, so just follow the head.
            phaseReference = head;
            return;
        }
        if (diff > period)
            phaseReference = wrap (phaseReference + period, n);
        else if (diff < -period)
            phaseReference = wrap (phaseReference - period, n);
        else
            return;
    }
    phaseReference = head;
}

float GranularEngine::effectiveJitterSamples() const noexcept
{
    const float sourceRate = (float) current->sampleRate();
    const float userJitter = settings.jitterSeconds * sourceRate;

    // Near a standstill, overlapping grains read almost the same audio a fixed time apart,
    // which rings like a comb and flutters at the grain rate. Spread them over at least half
    // a grain there, so the held breath stays smooth.
    const float slow = 1.0f - std::min (speed * 4.0f, 1.0f);
    const float grainSpan = (float) grainLength * rate * (float) sourceScale;
    return std::max (userJitter, slow * 0.5f * grainSpan);
}

float GranularEngine::levelCompensation (float jitterSamples) const noexcept
{
    // Grains reading the same audio in step add up in amplitude (Hann windows at overlap O sum
    // to O / 2). Grains reading unrelated audio add up in power (the squares sum to 3 O / 8).
    // Estimate how far apart neighbouring grains read, and blend between the two.
    const float o = (float) settings.overlap;
    const float interval = (float) grainLength / o;
    const bool aligned = settings.phaseAlign && current->periodAt (head) >= 2.0f;
    const float apart = std::abs (speed - rate) * (float) sourceScale * interval + 0.667f * jitterSamples;
    float d = std::clamp (apart / (decorrelationSeconds * (float) current->sampleRate()), 0.0f, 1.0f);

    // Grains placed in phase add up mostly in amplitude. A real voice drifts in pitch (vibrato),
    // so they never line up perfectly: 0.3 is a measured middle between a pure tone and a voice.
    if (aligned)
        d *= 0.3f;
    const float sum = (1.0f - d) * (0.5f * o) + d * std::sqrt (0.375f * o);
    return 1.0f / sum;
}

void GranularEngine::spawnGrain (int age) noexcept
{
    auto it = std::find_if (grains.begin(), grains.end(), [] (const Grain& g) { return g.source == nullptr; });
    if (it == grains.end())
        return;

    const float jitter = effectiveJitterSamples();
    const double n = (double) current->size();
    auto& g = *it;
    g.source = current;
    g.length = grainLength;
    g.age = std::min (age, grainLength - 1);
    // Where a grain started g.age samples ago would be reading now. When the read head and the
    // grains move at the same rate this is just the head, so a cloud with no jitter plays the
    // loop back exactly.
    const double target = head + (double) (jitter * nextRandom())
                        + (double) (rate - speed) * sourceScale * (double) g.age;
    g.position = wrap (alignToPhase (target), n);

    const float norm = levelCompensation (jitter);
    const float theta = (settings.stereoWidth * nextRandom() + 1.0f) * 0.78539816f;   // pi / 4
    g.gainLeft = std::cos (theta) * 1.41421356f * norm;
    g.gainRight = std::sin (theta) * 1.41421356f * norm;
}

void GranularEngine::renderGrain (Grain& g, float* left, float* right, int numSamples) noexcept
{
    const LoopSource& src = *g.source;
    const float* inL = src.channel (0);
    const float* inR = src.channel (1);
    const bool stereo = src.numChannels() > 1;
    const double n = (double) src.size();
    const double step = (double) rate * src.sampleRate() / hostRate;
    const float toWindow = (float) windowSize / (float) g.length;

    const int count = std::min (numSamples, g.length - g.age);
    double pos = g.position;

    for (int i = 0; i < count; ++i)
    {
        const float wp = (float) (g.age + i) * toWindow;
        const int wi = (int) wp;
        const float w = window[(size_t) wi] + (wp - (float) wi) * (window[(size_t) wi + 1] - window[(size_t) wi]);

        const int idx = (int) pos;
        const float f = (float) (pos - (double) idx);
        const float sL = hermite (inL + idx, f);
        const float sR = stereo ? hermite (inR + idx, f) : sL;

        left[i] += sL * w * g.gainLeft;
        right[i] += sR * w * g.gainRight;

        pos += step;
        if (pos >= n)
            pos -= n;
    }

    g.position = pos;
    g.age += count;
    if (g.age >= g.length)
        g.source = nullptr;
}

void GranularEngine::process (float* left, float* right, int numSamples) noexcept
{
    if (current == nullptr)
        return;

    const double n = (double) current->size();
    const double interval = (double) (grainLength / settings.overlap);
    const float targetSpeed = params.hold ? 0.0f : params.speed;
    const float targetRate = params.pitchRatio;

    int done = 0;
    while (done < numSamples)
    {
        // Parameter smoothing, once per control step.
        speed = targetSpeed + smoothingCoeff * (speed - targetSpeed);
        rate = targetRate + smoothingCoeff * (rate - targetRate);
        if (std::abs (speed - targetSpeed) < 1.0e-6f) speed = targetSpeed;
        if (std::abs (rate - targetRate) < 1.0e-6f) rate = targetRate;

        const int stepEnd = std::min (numSamples, done + controlStep);
        while (done < stepEnd)
        {
            const int untilGrain = std::max (1, (int) std::ceil (samplesToNextGrain));
            const int len = std::min (stepEnd - done, untilGrain);

            for (auto& g : grains)
                if (g.source != nullptr)
                    renderGrain (g, left + done, right + done, len);

            head = wrap (head + (double) speed * sourceScale * (double) len, n);
            phaseReference = wrap (phaseReference + (double) rate * sourceScale * (double) len, n);
            followHead();
            samplesToNextGrain -= (double) len;
            done += len;

            if (samplesToNextGrain <= 0.0)
            {
                spawnGrain (0);
                samplesToNextGrain += interval * (1.0 + 0.5 * (double) (settings.timingJitter * nextRandom()));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------

float StretchKnob::toSpeed (float knob) noexcept
{
    knob = std::clamp (knob, 0.0f, 1.0f);
    if (knob >= freezeAt)
        return 0.0f;

    return std::exp2 (-std::log2 (maxSlowdown) * knob / freezeAt);
}

GranularEngine::Settings StretchKnob::toGranularSettings (float knob, GranularEngine::Settings base) noexcept
{
    const float t = std::clamp (knob, 0.0f, 1.0f);
    auto lerp = [t] (float a, float b) { return a + (b - a) * t; };

    // At 0 the grains read exactly where the head is, so Stretch 0 plays the voice as
    // recorded. Jitter and width come in over the first tenth of the knob, as the read
    // head starts to slow and the grains begin to repeat.
    const float onset = std::min (t * 10.0f, 1.0f);
    base.grainSeconds = lerp (0.09f, 0.24f);
    base.jitterSeconds = 0.008f * onset + (0.18f - 0.008f) * t * std::sqrt (t);
    base.stereoWidth = onset * lerp (0.25f, 0.6f);
    base.timingJitter *= onset;
    return base;
}

} // namespace sing
