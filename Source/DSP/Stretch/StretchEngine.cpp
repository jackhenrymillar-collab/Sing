#include "StretchEngine.h"

#include "GranularEngine.h"

namespace sing
{

std::unique_ptr<StretchEngine> createStretchEngine (StretchMode mode)
{
    switch (mode)
    {
        case StretchMode::granular:
            return std::make_unique<GranularEngine>();

        case StretchMode::bloom:   // not written yet: fall back to granular
        case StretchMode::count:
            break;
    }

    return std::make_unique<GranularEngine>();
}

std::shared_ptr<const SourceAnalysis> analyseSource (StretchMode mode, const LoopSource&)
{
    switch (mode)
    {
        case StretchMode::granular:   // reads the play buffer directly
        case StretchMode::bloom:      // will build its magnitude spectrogram here
        case StretchMode::count:
            break;
    }

    return nullptr;
}

} // namespace sing
