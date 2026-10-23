#pragma once

#include "Sinc.h"

#include <vector>

namespace sing::dsp
{

// Brings the engine's internal 2x rate back to the host rate. A steep linear-phase
// half-band filter: flat to 0.45 of the host rate, 140 dB down from 0.55, so whatever
// the pitch engine leaves above the audible band is removed before it can fold back.
// Half the taps of a half-band filter are zero, so only the others are computed.
class HalfbandDecimator
{
public:
    void prepare()
    {
        const auto h = designLowpass (0.25, 0.05, 140.0);
        length = static_cast<int> (h.size());
        centre = length / 2;
        taps.clear();
        offsets.clear();
        for (int i = 0; i < length; ++i)
            if (i == centre || (i - centre) % 2 != 0)
            {
                taps.push_back (h[static_cast<std::size_t> (i)]);
                offsets.push_back (i);
            }
        history.assign (static_cast<std::size_t> (2 * length + 4096), 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        filled = length;
    }

    // Latency in host-rate samples (the filter's centre, halved).
    int latency() const noexcept { return centre / 2; }

    // in: 2 * numOut samples at the internal rate.
    void process (const float* in, float* out, int numOut) noexcept
    {
        int done = 0;
        while (done < numOut)
        {
            // Work in chunks that fit the history buffer.
            const int chunk = std::min (numOut - done, static_cast<int> (history.size() - static_cast<std::size_t> (length)) / 2 - 1);
            float* h = history.data();
            std::copy (in + 2 * done, in + 2 * (done + chunk), h + filled);
            for (int m = 0; m < chunk; ++m)
            {
                // Newest sample of this output is at filled + 2m + 1; tap i reaches back i samples.
                const float* x = h + filled + 2 * m + 1;
                float acc = 0.0f;
                for (std::size_t t = 0; t < taps.size(); ++t)
                    acc += taps[t] * x[-offsets[t]];
                out[done + m] = acc;
            }
            // Keep the last `length` samples for the next call.
            const int total = filled + 2 * chunk;
            std::copy (h + total - length, h + total, h);
            filled = length;
            done += chunk;
        }
    }

private:
    std::vector<float> taps;
    std::vector<int> offsets;
    std::vector<float> history;
    int length = 0, centre = 0, filled = 0;
};

} // namespace sing::dsp
