#include "ReducedMotion.h"

#include <cstdlib>
#include <cstring>

namespace sing::ui
{

// Other platforms: no system setting to read yet. SING_REDUCE_MOTION=1 turns
// the animation off, which is also handy for testing.
bool prefersReducedMotion()
{
    const char* env = std::getenv ("SING_REDUCE_MOTION");
    return env != nullptr && std::strcmp (env, "1") == 0;
}

} // namespace sing::ui
