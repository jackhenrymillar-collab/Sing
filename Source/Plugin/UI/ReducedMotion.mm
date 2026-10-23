#include "ReducedMotion.h"

#import <AppKit/AppKit.h>

namespace sing::ui
{

bool prefersReducedMotion()
{
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}

} // namespace sing::ui
