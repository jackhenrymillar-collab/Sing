#include "SourceExchange.h"

namespace sing
{

bool SourceExchange::publish (std::shared_ptr<const LoopSource> source)
{
    if (source == nullptr)
        return false;

    collectGarbage();

    int k = -1;
    for (int i = 0; i < capacity && k < 0; ++i)
        if (slots[(size_t) i].state.load (std::memory_order_acquire) == empty)
            k = i;
    if (k < 0)
        return false;

    auto& slot = slots[(size_t) k];
    slot.source = std::move (source);
    slot.state.store (published, std::memory_order_release);

    const int previous = latest.exchange (k, std::memory_order_acq_rel);
    if (previous >= 0)
    {
        // Replaced before the audio thread picked it up: it will never be used.
        int expected = published;
        slots[(size_t) previous].state.compare_exchange_strong (expected, dead, std::memory_order_acq_rel);
    }

    collectGarbage();
    return true;
}

void SourceExchange::collectGarbage()
{
    const int current = latest.load (std::memory_order_acquire);
    for (int i = 0; i < capacity; ++i)
    {
        auto& slot = slots[(size_t) i];
        const int s = slot.state.load (std::memory_order_acquire);
        if (i != current && (s == retired || s == dead))
        {
            slot.source.reset();
            slot.state.store (empty, std::memory_order_release);
        }
    }
}

std::shared_ptr<const LoopSource> SourceExchange::latestPublished() const
{
    const int k = latest.load (std::memory_order_acquire);
    return k >= 0 ? slots[(size_t) k].source : nullptr;
}

const LoopSource* SourceExchange::acquire() noexcept
{
    for (;;)
    {
        const int k = latest.load (std::memory_order_acquire);
        if (k < 0)
            return nullptr;

        auto& slot = slots[(size_t) k];
        int expected = published;
        if (slot.state.compare_exchange_strong (expected, inUse, std::memory_order_acq_rel)
            || expected == inUse)
            return slot.source.get();

        // A newer loop replaced this one in the meantime: read latest again.
    }
}

void SourceExchange::retire (const LoopSource* source) noexcept
{
    if (source == nullptr)
        return;

    const int current = latest.load (std::memory_order_acquire);
    for (int i = 0; i < capacity; ++i)
    {
        auto& slot = slots[(size_t) i];
        // The latest loop stays in use: new notes will need it.
        if (i != current && slot.state.load (std::memory_order_acquire) == inUse && slot.source.get() == source)
        {
            slot.state.store (retired, std::memory_order_release);
            return;
        }
    }
}

} // namespace sing
