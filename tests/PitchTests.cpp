// The pitch engine: clean pitching up and down.
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sing;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr float kAmp = 0.5f;
    const double kRef = kAmp / std::sqrt (2.0); // RMS of the test tone

    std::unique_ptr<dsp::VoiceData> tone (double hz, double fs = kFs)
    {
        const auto s = test::sine (hz, fs, 8.0, kAmp);
        return dsp::VoiceData::build (s.data(), s.size(), fs, 60.0f);
    }
}

TEST_CASE ("Pitched tones come out at the right level with almost nothing else", "[pitch]")
{
    // A 1 kHz tone played from three octaves down to three octaves up, including awkward
    // in-between ratios.
    const double semis = GENERATE (-36.0, -24.0, -13.7, -7.0, -1.0, -0.03, 0.0, 0.5, 3.0, 7.0, 11.9, 12.0, 19.3, 24.0, 31.0, 36.0);
    const double source = semis > 20.0 ? 400.0 : 1000.0; // stay in the audible band at the top
    const auto v = tone (source);
    const double ratio = std::exp2 (semis / 12.0);
    const auto out = test::playAtPitch (*v, ratio, kFs, 0.2, 12000);
    const auto fit = test::fitSine (out, source * ratio, kFs, 2000);

    INFO ("semitones " << semis << ", residual " << fit.residualDb (kAmp) << " dB");
    CHECK (std::abs (20.0 * std::log10 (fit.amplitude / kAmp)) < 0.01); // level within 0.01 dB
    CHECK (fit.residualDb (kAmp) < -125.0);                             // everything else 125 dB down
}

TEST_CASE ("High tones flat to 20 kHz at any pitch", "[pitch]")
{
    // A 9 kHz tone pitched up so it lands near the top of the band.
    const double semis = GENERATE (0.0, 3.0, 6.0, 8.0);
    const auto v = tone (9000.0);
    const double ratio = std::exp2 (semis / 12.0);
    INFO ("lands at " << 9000.0 * ratio << " Hz");
    const auto out = test::playAtPitch (*v, ratio, kFs, 0.2, 24000);
    const auto fit = test::fitSine (out, 9000.0 * ratio, kFs, 2000);
    CHECK (std::abs (20.0 * std::log10 (fit.amplitude / kAmp)) < 0.05);
    CHECK (fit.residualDb (kAmp) < -125.0);
}

TEST_CASE ("Pitching up never folds tones back into the audible band (no aliasing)", "[pitch]")
{
    // Tones pitched above the output's Nyquist limit must vanish rather than fold back
    // down. What's left is measured below 20 kHz, where folded tones would be heard.
    struct Case { double hz, semis; };
    const auto c = GENERATE (Case { 15000.0, 12.0 }, Case { 20000.0, 7.0 }, Case { 8000.0, 24.0 }, Case { 6000.0, 36.0 },
                             Case { 11000.0, 14.5 }, Case { 13000.0, 19.0 }, Case { 4000.0, 31.0 });
    const auto v = tone (c.hz);
    const auto out = test::playAtPitch (*v, std::exp2 (c.semis / 12.0), kFs, 0.2, 12000);
    const auto audible = test::lowpass20k (out, kFs);
    const double db = 20.0 * std::log10 (std::max (1.0e-12, test::rms (audible, 2000)) / kRef);
    INFO (c.hz << " Hz up " << c.semis << " semitones leaves " << db << " dB below 20 kHz");
    CHECK (db < -125.0);
}

TEST_CASE ("Pitching down adds no images", "[pitch]")
{
    // A bright tone pitched far down: only the lowered tone may remain.
    const double semis = GENERATE (-5.0, -12.0, -24.0, -36.0);
    const auto v = tone (18000.0);
    const double ratio = std::exp2 (semis / 12.0);
    const auto out = test::playAtPitch (*v, ratio, kFs, 0.2, 24000);
    const auto fit = test::fitSine (out, 18000.0 * ratio, kFs, 2000);
    INFO ("residual " << fit.residualDb (kAmp) << " dB");
    CHECK (std::abs (20.0 * std::log10 (fit.amplitude / kAmp)) < 0.05);
    CHECK (fit.residualDb (kAmp) < -125.0);
}

TEST_CASE ("Voices recorded at other sample rates play at the right pitch", "[pitch]")
{
    // A 44.1 kHz and a 96 kHz recording in a 48 kHz session, and a 48 kHz one at 44.1 kHz.
    struct Case { double sourceRate, outRate; };
    const auto c = GENERATE (Case { 44100.0, 48000.0 }, Case { 96000.0, 48000.0 }, Case { 48000.0, 44100.0 }, Case { 48000.0, 96000.0 });
    const auto v = tone (2000.0, c.sourceRate);
    const auto out = test::playAtPitch (*v, std::exp2 (5.0 / 12.0), c.outRate, 0.2, 20000);
    const auto fit = test::fitSine (out, 2000.0 * std::exp2 (5.0 / 12.0), c.outRate, 2000);
    CHECK (std::abs (20.0 * std::log10 (fit.amplitude / kAmp)) < 0.02);
    CHECK (fit.residualDb (kAmp) < -125.0);
}

TEST_CASE ("Sweeping the pitch stays clean", "[pitch]")
{
    // A glide from two octaves down to two octaves up through every level switch. The
    // output is compared with the ideal swept sine.
    const double source = 700.0;
    const auto v = tone (source);
    const int n = 48000;
    std::vector<float> inter (static_cast<std::size_t> (2 * n));
    double pos = 0.1 * 2.0 * kFs, phase = 2.0 * std::numbers::pi * source * 0.1;
    std::vector<double> idealPhase (inter.size());
    for (std::size_t i = 0; i < inter.size(); ++i)
    {
        const double semis = -24.0 + 48.0 * static_cast<double> (i) / static_cast<double> (inter.size());
        const double ratio = (v->getSourceRate() / kFs) * std::exp2 (semis / 12.0);
        inter[i] = v->read (pos, ratio);
        idealPhase[i] = phase;
        pos += ratio;
        phase += 2.0 * std::numbers::pi * source * ratio / (2.0 * v->getSourceRate());
    }
    // Compare at the internal rate (no decimation), away from the ends.
    double err = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 1000; i + 1000 < inter.size(); ++i)
    {
        const double e = inter[i] - kAmp * std::sin (idealPhase[i]);
        err += e * e;
        ++count;
    }
    const double db = 20.0 * std::log10 (std::sqrt (err / static_cast<double> (count)) / kAmp);
    INFO ("sweep error " << db << " dB");
    CHECK (db < -125.0);
}
