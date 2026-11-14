#pragma once

// Bridge-to-Lua delivery (docs/BUILD.md, "Ownership and event delivery"): one
// consumer thread makes every call into a session's Lua state.
//
// - Tick mode (UE4SS's engine-tick detour exists): one engine-tick
//   post-callback on the game thread drains function hooks, then runs the Lua
//   pass (loop-start callbacks, delivery faults, input events, traces). The
//   event-loop thread (on_update) makes no Lua call; it only marks new sessions
//   ready for their loop-start callbacks, because Lua mods start on that
//   thread and the first on_update after a session started is the barrier
//   "the whole Lua-mod batch has started".
// - Loop mode (no engine tick): on_update does all of it, as before.
//
// Portable: BridgeMod.cpp supplies the input backend and a Lua sink that
// pushes onto UE4SS's Lua; tests supply fakes.
//
// Backend: take_events, recycle_events, consumer_events_remaining, trace,
// report_delivery_fault, deactivate, take_delivery_fault, take_traces,
// recycle_traces (EnhancedInputBackend.hpp).
// Sink: live(session*), input(session, subscription, event),
// fault(session, target, reason), trace(session, line),
// loop_start(session, token), report(message, detail). The four calls raise on
// a Lua error.

#include <SharedDispatch.hpp>
#include <dispatch/LuaDispatchGate.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace UE4SSLuaEventBridge
{
using UE4SSXB::LuaDelivery;
using UE4SSXB::LuaDispatchGate;

inline constexpr uint32_t default_max_events_per_pass = 256;
inline constexpr uint32_t default_max_dispatch_us = 500;

// A session's onLoopStart callbacks. Guarded by the bridge's session lock.
struct LoopStart
{
    static constexpr std::size_t capacity = 4096;

    std::set<uint64_t> callbacks; // tokens ascend: registration order
    bool ready{};                 // the event-loop thread passed once since the session started
    bool delivered{};             // taken, or the session stopped: no more registrations

    bool accepts() const { return !delivered; }
    // Event-loop thread (on_update).
    void mark_ready() { ready = true; }
    // Consumer thread: the tokens due now, once per session, after mark_ready.
    std::vector<uint64_t> take_due()
    {
        if (!ready || delivered) return {};
        delivered = true;
        std::vector<uint64_t> due(callbacks.begin(), callbacks.end());
        callbacks.clear();
        return due;
    }
    void stop()
    {
        callbacks.clear();
        delivered = true;
    }
};

// Which thread runs a delivery pass. The gate and the schedule have static
// storage in BridgeMod.cpp: the engine-tick callback outlives the mod object.
class LuaConsumer
{
public:
    using Clock = UE4SSXB::QueueDispatchSchedule::Clock;

    explicit LuaConsumer(uint32_t queue_checks_per_second) : schedule_(queue_checks_per_second) {}

    LuaDispatchGate& gate() { return gate_; }
    const LuaDispatchGate& gate() const { return gate_; }

    // Engine-tick post-callback body (game thread): the hook drain, then the
    // Lua pass with input due every frame. Returns whether the pass ran.
    template <typename Hooks, typename Lua>
    bool tick(Hooks hooks, Lua lua)
    {
        const LuaDispatchGate::Pass pass(gate_, LuaDelivery::tick);
        if (!pass) return false;
        hooks();
        lua(true);
        return true;
    }

    // on_update body (event-loop thread). Marks new sessions ready in every
    // mode; delivers only in loop mode, input at the queue-check rate.
    template <typename MarkReady, typename Hooks, typename Lua>
    bool update(Clock::time_point now, MarkReady mark_ready, Hooks hooks, Lua lua)
    {
        mark_ready();
        if (gate_.mode() != LuaDelivery::loop) return false;
        const LuaDispatchGate::Pass pass(gate_, LuaDelivery::loop);
        if (!pass) return false;
        hooks();
        lua(schedule_.due(now));
        return true;
    }

private:
    LuaDispatchGate gate_;
    UE4SSXB::QueueDispatchSchedule schedule_; // loop mode, event-loop thread only
};

// The Lua pass's steps over the input backend. Consumer thread only.
template <typename Backend, typename Sink>
class LuaPass
{
public:
    using Event = typename decltype(std::declval<Backend&>().take_events())::value_type;
    using Trace = typename decltype(std::declval<Backend&>().take_traces())::value_type;
    using Fault = std::remove_cvref_t<decltype(std::declval<Backend&>().take_delivery_fault()->reason)>;

    LuaPass(Backend& backend, Sink& sink, uint32_t max_events, uint32_t max_dispatch_us)
        : backend_(backend), sink_(sink), max_events_(max_events), max_dispatch_us_(max_dispatch_us)
    {
    }

    // Loop-start callbacks due now, as (session, token) in registration order.
    template <typename Session>
    void loop_start(const std::vector<std::pair<Session*, uint64_t>>& due)
    {
        for (const auto& [session, token] : due)
        {
            if (!sink_.live(session)) continue;
            try
            {
                sink_.loop_start(*session, token);
            }
            catch (const std::exception& error)
            {
                sink_.report("loop-start callback failed: ", error.what());
            }
            catch (...)
            {
                sink_.report("loop-start callback failed: unknown exception", {});
            }
        }
    }

    void faults()
    {
        while (auto fault = backend_.take_delivery_fault())
        {
            auto* session = fault->session;
            if (!sink_.live(session)) continue;
            try
            {
                sink_.fault(*session, fault->target, fault->reason);
            }
            catch (...)
            {
                sink_.report("delivery fault handler failed; target remains disabled", {});
            }
        }
    }

    // Input events within the budget (UE4SSLEB_MAX_EVENTS_PER_PASS,
    // UE4SSLEB_MAX_DISPATCH_US). Returns the callbacks attempted.
    template <typename Now>
    std::size_t input(Now now)
    {
        const DispatchBudget budget(now(), max_events_, std::chrono::microseconds(max_dispatch_us_));
        if (events_.empty())
        {
            backend_.recycle_events(events_.release_buffer());
            events_.load(backend_.take_events());
        }
        return dispatch_budgeted(events_, budget, [&](Event event) { return deliver(event); }, now);
    }

    template <typename Now>
    void traces(Now now)
    {
        const DispatchBudget budget(now(), 32, std::chrono::microseconds(250));
        if (traces_.empty())
        {
            backend_.recycle_traces(traces_.release_buffer());
            traces_.load(backend_.take_traces());
        }
        std::size_t processed{};
        while (!traces_.empty() && budget.permits(processed, now()))
        {
            auto trace = traces_.pop();
            ++processed;
            auto* session = trace.session;
            if (!sink_.live(session)) continue;
            try
            {
                sink_.trace(*session, trace.line);
            }
            catch (...)
            {
                // The native trace writer already emitted the same line to
                // OutputDebugString. Logging must not affect input dispatch.
            }
        }
    }

private:
    bool deliver(Event& event)
    {
        backend_.consumer_events_remaining(events_.size());
        faults();
        const auto& subscription = event.owner;
        if (!subscription) return false;
        auto* session = subscription->session;
        if (session) backend_.trace(*session, subscription->debug, "event_dequeued", subscription->phase_name, event.sequence);
        if (!subscription->active.load() || !subscription->delivery_valid())
        {
            if (session)
            {
                backend_.trace(*session, subscription->debug, "callback_skipped", subscription->phase_name,
                    event.sequence, "binding_inactive");
            }
            return false;
        }
        if (!sink_.live(session))
        {
            if (session)
            {
                backend_.trace(*session, subscription->debug, "callback_skipped", subscription->phase_name,
                    event.sequence, "lua_session_inactive");
            }
            return false;
        }
        try
        {
            backend_.trace(*session, subscription->debug, "lua_callback_started", subscription->phase_name, event.sequence);
            sink_.input(*session, *subscription, event);
            backend_.trace(*session, subscription->debug, "lua_callback_completed", subscription->phase_name, event.sequence);
        }
        catch (const std::exception& error)
        {
            failed(*session, *subscription, event.sequence, error.what());
        }
        catch (...)
        {
            failed(*session, *subscription, event.sequence, {});
        }
        return true; // Failed callback attempts also consume the count allowance.
    }

    // The binding is disabled; say so even without tracing. Deactivate now and
    // let the next game-thread bridge operation detach the native binding.
    template <typename Session, typename Subscription>
    void failed(Session& session, Subscription& subscription, uint64_t sequence, const char* what)
    {
        backend_.trace(session, subscription.debug, "lua_callback_failed", subscription.phase_name, sequence,
            what ? what : "unknown_lua_exception");
        sink_.report("input callback for " + subscription.action_path_utf8 + " (" + subscription.phase_name +
                ") failed; binding disabled: ",
            what ? what : "unknown exception");
        backend_.report_delivery_fault(subscription, Fault::CallbackError);
        backend_.deactivate(session, subscription.id);
        faults();
    }

    Backend& backend_;
    Sink& sink_;
    const uint32_t max_events_;
    const uint32_t max_dispatch_us_;
    DispatchBacklog<Event> events_;
    DispatchBacklog<Trace> traces_;
};
}
