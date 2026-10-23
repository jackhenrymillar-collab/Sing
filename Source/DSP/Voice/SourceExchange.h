#pragma once

#include "Stretch/LoopSource.h"

#include <array>
#include <atomic>
#include <memory>

namespace sing
{

// Hands rebuilt loops from the message thread to the audio thread without locks, and frees
// them on the message thread once the audio thread has finished with them.
//
// Message thread: publish() a new loop; call collectGarbage() now and then.
// Audio thread:   acquire() the latest loop each block; retire() one when no note uses it.
//
// Each loop sits in a slot whose state moves published -> inUse -> retired (or
// published -> dead if a newer loop replaced it before the audio thread saw it). The two
// threads only ever race on a published slot, and settle it with one compare-and-swap.
class SourceExchange
{
public:
    static constexpr int capacity = 64;

    // Message thread. Returns false if every slot is busy (the audio thread is still using
    // many old loops); try again a little later.
    bool publish (std::shared_ptr<const LoopSource> source);

    // Message thread. Frees loops the audio thread has retired or never picked up.
    void collectGarbage();

    // Message thread: the most recently published loop.
    std::shared_ptr<const LoopSource> latestPublished() const;

    // Audio thread: the latest loop, now marked in use. nullptr until one is published.
    const LoopSource* acquire() noexcept;

    // Audio thread: no note uses this loop any more.
    void retire (const LoopSource* source) noexcept;

private:
    enum State : int { empty, published, inUse, retired, dead };

    struct Slot
    {
        std::shared_ptr<const LoopSource> source;
        std::atomic<int> state { empty };
    };

    std::array<Slot, capacity> slots;
    std::atomic<int> latest { -1 };
};

} // namespace sing
