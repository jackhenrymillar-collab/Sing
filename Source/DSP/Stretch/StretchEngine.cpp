#include "StretchEngine.h"

#include "GranularEngine.h"
#include "Bloom/BloomAnalysis.h"

namespace sing
{

std::unique_ptr<StretchEngine> createStretchEngine (StretchMode mode)
{
    switch (mode)
    {
        case StretchMode::granular:
            return std::make_unique<GranularEngine>();

        case StretchMode::bloom:   // Bloom plays whole notes (BloomVoice), not per-copy engines
        case StretchMode::count:
            break;
    }

    return std::make_unique<GranularEngine>();
}

std::shared_ptr<const SourceAnalysis> analyseSource (StretchMode mode, const LoopSource& source)
{
    switch (mode)
    {
        case StretchMode::bloom:      // the loop's magnitude spectrogram
            return BloomAnalysis::build (source);
        case StretchMode::granular:   // reads the play buffer directly
        case StretchMode::count:
            break;
    }

    return nullptr;
}

} // namespace sing
