#pragma once

// Tail deliveries handed from file IO to the Lua consumer thread
// (FileBridgeNativeContract.hpp section 7). on_update polls the files and
// offers each batch; the consumer (the engine-tick callback in tick mode,
// on_update itself in loop mode) delivers it within the per-pass budget. One
// entry is taken under the lock at a time, so neither side holds the lock
// while the other does file IO or calls Lua. Portable; tested in
// tests/TailDeliveryTests.cpp.

#include <dispatch/DispatchBacklog.hpp>
#include <dispatch/DispatchBudget.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace UE4SSLuaFileBridge::Core
{
template <typename Pending>
class TailHandoff
{
public:
    // Event-loop thread: a new batch is wanted only once the last one was
    // delivered, so the files stay the buffer while delivery lags.
    bool wants_batch() const
    {
        std::scoped_lock lock(mutex_);
        return backlog_.empty();
    }

    // Event-loop thread. False (batch dropped) if the backlog is not empty.
    bool offer(std::vector<Pending> batch)
    {
        if (batch.empty()) return true;
        std::scoped_lock lock(mutex_);
        if (!backlog_.empty()) return false;
        (void)backlog_.release_buffer();
        backlog_.load(std::move(batch));
        return true;
    }

    // Consumer thread: delivers within the budget. deliver(Pending&) returns
    // whether a callback was attempted (counts against the budget). Returns
    // the attempts; `exhausted` is set when entries remain.
    template <typename Deliver, typename Now>
    std::size_t deliver(const UE4SSXB::DispatchBudget& budget, Deliver deliver, Now now, bool& exhausted)
    {
        std::size_t processed{};
        bool made_progress = false;
        for (;;)
        {
            std::optional<Pending> next;
            {
                std::scoped_lock lock(mutex_);
                if (backlog_.empty())
                {
                    exhausted = false;
                    return processed;
                }
                if (!budget.permits(processed, now(), made_progress))
                {
                    exhausted = true;
                    return processed;
                }
                next.emplace(backlog_.pop());
            }
            made_progress = true;
            if (deliver(*next)) ++processed;
        }
    }

    std::size_t size() const
    {
        std::scoped_lock lock(mutex_);
        return backlog_.size();
    }

private:
    mutable std::mutex mutex_;
    UE4SSXB::DispatchBacklog<Pending> backlog_;
};
}
