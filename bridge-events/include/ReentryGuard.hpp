#pragma once

// Per-thread re-entry guard for the Blueprint entry handler (LEX_hooks.md §4.3).
//
// Handler A replaces Script[0]. A Kismet loop whose first statement sits at
// offset 0 jumps back there and would run A again in the same frame; the guard
// tells that loop-back (a re-entry) apart from a genuine call.
//
// Assumptions:
// - Every FFrame that runs bytecode is a local of the native function that
//   executes it (ProcessEvent, ProcessScriptFunction), so its address lies on
//   the executing thread's stack.
// - The stack grows downward (x64 Windows, and every platform the tests run
//   on): a callee's frame has a lower address than its caller's. A record below
//   the frame being entered or left therefore belongs to a callee that has
//   already returned, and is dropped ("pruned").
// - A frame that never reaches handler B (bAbortingExecution, or a hook
//   attached mid-frame) leaves its record behind; the next handler call above
//   it prunes it. A stale record exactly at a reused frame address with the same
//   entry suppresses one pre-hook; that needs an aborted frame first, and is the
//   accepted cost of keeping the guard allocation-free.
//
// Storage is a fixed array: the handlers also run on animation worker threads,
// where they must not allocate. A full guard does not record the call
// (Enter::overflow: the caller treats it as genuine, so a later loop-back may
// fire a spurious pre-hook) and counts it in guard_overflow_count.

#include <HookTypes.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace UE4SSLuaEventBridge::Hooks
{
// Process-wide count of calls the guard could not record (all threads).
inline std::atomic<uint64_t> guard_overflow_count{0};

class ReentryGuard
{
public:
    enum class Enter : uint8_t { genuine, reentry, overflow };

    // Handler A: is this frame entering the entry's function, or looping back
    // to Script[0] inside a call already recorded?
    Enter enter(std::uintptr_t frame, uint32_t entry) noexcept
    {
        prune_below(frame);
        for (std::size_t index = count_; index-- > 0;)
        {
            if (records_[index].frame == frame && records_[index].entry == entry) return Enter::reentry;
        }
        if (count_ == records_.size())
        {
            guard_overflow_count.fetch_add(1, std::memory_order_relaxed);
            return Enter::overflow;
        }
        records_[count_++] = Record{frame, entry};
        return Enter::genuine;
    }

    // Handler B: the frame is returning. Drops its record; false when there was
    // none (overflowed, or hooked after the frame had passed Script[0]).
    bool exit(std::uintptr_t frame, uint32_t entry) noexcept
    {
        prune_below(frame);
        for (std::size_t index = count_; index-- > 0;)
        {
            if (records_[index].frame != frame || records_[index].entry != entry) continue;
            for (std::size_t next = index + 1; next < count_; ++next) records_[next - 1] = records_[next];
            --count_;
            return true;
        }
        return false;
    }

    std::size_t size() const noexcept { return count_; }
    void clear() noexcept { count_ = 0; }

private:
    struct Record
    {
        std::uintptr_t frame{};
        uint32_t entry{};
    };

    void prune_below(std::uintptr_t frame) noexcept
    {
        std::size_t kept = 0;
        for (std::size_t index = 0; index < count_; ++index)
        {
            if (records_[index].frame >= frame) records_[kept++] = records_[index];
        }
        count_ = kept;
    }

    std::array<Record, max_guard_records> records_{};
    std::size_t count_{};
};

// The calling thread's guard. Trivially constructible, so the thread_local
// needs no dynamic initialisation (implicit TLS of a DLL loaded after the
// thread started is supported on Windows Vista and later).
inline ReentryGuard& local_reentry_guard() noexcept
{
    thread_local ReentryGuard guard;
    return guard;
}
}
