#pragma once
// Which thread makes a bridge's calls into Lua, and a marker for the pass that
// is making them (bridge-events/docs/BUILD.md, "Ownership and event delivery").
//
// A bridge decides its delivery once, at Unreal initialization: `tick` when
// UE4SS's engine-tick detour exists (one engine-tick post-callback on the game
// thread makes every Lua call), otherwise `loop` (on_update, on UE4SS's
// event-loop thread). Every delivery pass holds a Pass for its whole length.
// Session stop and unload wait for a running pass with wait_idle, bounded and
// never under a lock: UE4SS fires on_lua_stop while holding its own Lua action
// mutex, which a callback inside the pass may be waiting for.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace UE4SSXB
{
enum class LuaDelivery : uint8_t { undecided, tick, loop };

inline const char* delivery_name(LuaDelivery delivery)
{
    switch (delivery)
    {
    case LuaDelivery::tick: return "tick";
    case LuaDelivery::loop: return "loop";
    case LuaDelivery::undecided: break;
    }
    return nullptr;
}

class LuaDispatchGate
{
public:
    using Clock = std::chrono::steady_clock;

    // Decided once; a later call keeps the first decision. Returns the mode in force.
    LuaDelivery decide(LuaDelivery mode)
    {
        auto expected = LuaDelivery::undecided;
        mode_.compare_exchange_strong(expected, mode, std::memory_order_acq_rel);
        return mode_.load(std::memory_order_acquire);
    }
    LuaDelivery mode() const { return mode_.load(std::memory_order_acquire); }

    // A delivery pass on the calling thread. False (closed) when the gate is
    // inert or `mode` is not the decided delivery: the pass then makes no Lua
    // call. Marked in flight either way, sequentially consistent with
    // set_inert: either the pass sees the inert flag or wait_idle sees the pass.
    class Pass
    {
    public:
        Pass(LuaDispatchGate& gate, LuaDelivery mode) : gate_(gate)
        {
            gate_.in_flight_.fetch_add(1);
            gate_.owner_.store(std::this_thread::get_id());
            open_ = !gate_.inert_.load() && gate_.mode_.load() == mode;
            if (open_) gate_.passes_.fetch_add(1, std::memory_order_relaxed);
        }
        ~Pass()
        {
            gate_.owner_.store(std::thread::id{});
            gate_.in_flight_.fetch_sub(1);
        }
        Pass(const Pass&) = delete;
        Pass& operator=(const Pass&) = delete;
        explicit operator bool() const { return open_; }

    private:
        LuaDispatchGate& gate_;
        bool open_{};
    };

    // Waits until no pass runs on another thread, or the timeout. True when
    // none does (or the caller is the pass's own thread, as when a callback
    // inside the pass stops its own session).
    bool wait_idle(std::chrono::milliseconds timeout) const
    {
        const auto deadline = Clock::now() + timeout;
        while (in_flight_.load() != 0)
        {
            if (owner_.load() == std::this_thread::get_id()) return true;
            if (Clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    // Unload: no more open passes. Follow with wait_idle for a pass already running.
    void set_inert() { inert_.store(true); }
    bool inert() const { return inert_.load(); }

    // Open passes so far.
    uint64_t passes() const { return passes_.load(std::memory_order_relaxed); }

private:
    std::atomic<LuaDelivery> mode_{LuaDelivery::undecided};
    std::atomic<int> in_flight_{0};
    std::atomic<std::thread::id> owner_{};
    std::atomic_bool inert_{};
    std::atomic<uint64_t> passes_{0};
};
}
