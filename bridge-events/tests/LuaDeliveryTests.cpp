// One Lua consumer thread (LuaDelivery.hpp): an injected engine tick and
// on_update run on two std::threads against fake sessions whose Lua side
// counts its entry depth. Input events, delivery faults, traces, loop-start
// callbacks and function-hook work (the real HookRuntime's notices) all reach
// Lua from one thread: the tick in tick mode, on_update in loop mode.

#include <HookRuntime.hpp>
#include <LuaDelivery.hpp>
#include <TestSupport.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace UE4SSLuaEventBridge;
using namespace std::chrono_literals;

namespace
{
using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------ fake backend

enum class FakeFault : uint8_t { None, CallbackError };

struct FakeSession
{
    uint64_t id{};
    std::atomic_bool active{true};
    std::atomic_bool torn_down{}; // the Lua state is gone (after on_lua_stop returned)
    LoopStart loop_start;         // guarded by Bridge::session_mutex
};

struct FakeDebug
{
};

struct FakeSubscription
{
    uint64_t id{};
    uint64_t target{};
    FakeSession* session{};
    FakeDebug debug;
    std::string action_path_utf8{"/Engine/Transient.IA_Test"};
    std::string phase_name{"Triggered"};
    std::atomic_bool active{true};
    bool fail{};  // the Lua callback raises
    bool block{}; // the Lua callback waits for Sink::release
    bool delivery_valid() const { return true; }
};

struct FakeEvent
{
    uint64_t subscription{};
    uint64_t sequence{};
    std::shared_ptr<FakeSubscription> owner;
};

struct FakeTrace
{
    FakeSession* session{};
    std::string line;
};

struct FakeFaultRecord
{
    FakeSession* session{};
    uint64_t target{};
    FakeFault reason{};
};

// Producers on any thread; the consumer takes whole batches, as
// EnhancedInputBackend does.
class FakeBackend
{
public:
    void push_event(const std::shared_ptr<FakeSubscription>& owner)
    {
        std::scoped_lock lock(mutex_);
        events_.push_back(FakeEvent{owner->id, ++sequence_, owner});
    }
    void push_trace(FakeSession& session, std::string line)
    {
        std::scoped_lock lock(mutex_);
        traces_.push_back(FakeTrace{&session, std::move(line)});
    }
    void push_fault(FakeSession& session, uint64_t target)
    {
        std::scoped_lock lock(mutex_);
        faults_.push_back(FakeFaultRecord{&session, target, FakeFault::CallbackError});
    }

    std::vector<FakeEvent> take_events()
    {
        std::scoped_lock lock(mutex_);
        return std::exchange(events_, {});
    }
    void recycle_events(std::vector<FakeEvent>) {}
    void consumer_events_remaining(std::size_t) {}
    std::vector<FakeTrace> take_traces()
    {
        std::scoped_lock lock(mutex_);
        return std::exchange(traces_, {});
    }
    void recycle_traces(std::vector<FakeTrace>) {}
    std::optional<FakeFaultRecord> take_delivery_fault()
    {
        std::scoped_lock lock(mutex_);
        if (faults_.empty()) return std::nullopt;
        auto fault = faults_.front();
        faults_.pop_front();
        return fault;
    }
    void report_delivery_fault(const FakeSubscription& subscription, FakeFault reason)
    {
        std::scoped_lock lock(mutex_);
        faults_.push_back(FakeFaultRecord{subscription.session, subscription.target, reason});
    }
    void deactivate(FakeSession&, uint64_t) { ++deactivated; }
    void trace(FakeSession&, const FakeDebug&, std::string_view, std::string_view = {}, uint64_t = 0,
        std::string_view = {})
    {
    }

    std::atomic<int> deactivated{};

private:
    std::mutex mutex_;
    uint64_t sequence_{};
    std::vector<FakeEvent> events_;
    std::vector<FakeTrace> traces_;
    std::deque<FakeFaultRecord> faults_;
};

// ------------------------------------------------------------ fake Lua side

// Every entry checks that no other call is inside a Lua state, that the
// session's state still exists, and which thread made it.
struct Sink
{
    std::atomic<int> depth{};
    std::atomic<int> overlaps{};
    std::atomic<int> torn{};
    std::atomic<int> wrong_thread{};
    std::atomic<std::thread::id> consumer{};
    std::atomic<int> inputs{};
    std::atomic<int> faults{};
    std::atomic<int> traces{};
    std::atomic<int> hooks{};
    std::atomic<int> reports{};
    std::atomic_bool inside{};
    std::atomic_bool release{};
    std::atomic_bool finished{};
    std::mutex order_mutex;
    std::vector<uint64_t> loop_starts;

    class Entry
    {
    public:
        Entry(Sink& sink, FakeSession& session) : sink_(sink)
        {
            if (sink_.depth.fetch_add(1) != 0) ++sink_.overlaps;
            if (session.torn_down.load()) ++sink_.torn;
            const auto expected = sink_.consumer.load();
            if (expected != std::thread::id{} && expected != std::this_thread::get_id()) ++sink_.wrong_thread;
            for (int i = 0; i < 20; ++i) std::this_thread::yield(); // widen any overlap window
        }
        ~Entry() { sink_.depth.fetch_sub(1); }

    private:
        Sink& sink_;
    };

    bool live(const FakeSession* session) const { return session && session->active.load(); }

    void input(FakeSession& session, const FakeSubscription& subscription, const FakeEvent&)
    {
        const Entry entry(*this, session);
        ++inputs;
        if (subscription.fail) throw std::runtime_error("callback failed");
        if (subscription.block)
        {
            inside = true;
            while (!release.load()) std::this_thread::yield();
            finished = true;
        }
    }
    void fault(FakeSession& session, uint64_t, FakeFault)
    {
        const Entry entry(*this, session);
        ++faults;
    }
    void trace(FakeSession& session, const std::string&)
    {
        const Entry entry(*this, session);
        ++traces;
    }
    void loop_start(FakeSession& session, uint64_t token)
    {
        const Entry entry(*this, session);
        std::scoped_lock lock(order_mutex);
        loop_starts.push_back(token);
    }
    void hook(FakeSession& session)
    {
        const Entry entry(*this, session);
        ++hooks;
    }
    void report(std::string_view, std::string_view) { ++reports; }

    std::vector<uint64_t> taken_loop_starts()
    {
        std::scoped_lock lock(order_mutex);
        return loop_starts;
    }
};

// ------------------------------------------------------------ real hook runtime

struct NoFrame
{
};

class NoPlatform final : public Hooks::HookPlatform<NoFrame>
{
public:
    std::byte* locals(NoFrame&) override { return nullptr; }
    bool has_out_parms(NoFrame&) override { return false; }
    const void* out_param_address(NoFrame&, std::uintptr_t) override { return nullptr; }
    bool passes(const Hooks::Filter&, std::uintptr_t) override { return true; }
    Hooks::ObjectIdentity context_identity(std::uintptr_t object) override { return {object, -1, 0}; }
    bool is_game_thread() override { return true; }
    Hooks::ObjectIdentity identity(std::uintptr_t address) override { return {address, -1, 0}; }
};

// ------------------------------------------------------------ the bridge glue

// BridgeMod.cpp's consumer glue around LuaConsumer and LuaPass: on_engine_tick,
// on_update, deliver_lua_pass, mark_sessions_ready and take_due_loop_starts.
struct Bridge
{
    explicit Bridge(LuaDelivery mode)
    {
        consumer.gate().decide(mode);
        hooks.set_delivery(mode == LuaDelivery::tick ? Hooks::HookDelivery::tick : Hooks::HookDelivery::loop);
    }

    FakeSession& add_session()
    {
        auto session = std::make_unique<FakeSession>();
        std::scoped_lock lock(session_mutex);
        session->id = sessions.size() + 1;
        sessions.push_back(std::move(session));
        return *sessions.back();
    }

    FakeSession* find(uint64_t id)
    {
        std::scoped_lock lock(session_mutex);
        return id >= 1 && id <= sessions.size() ? sessions[id - 1].get() : nullptr;
    }

    void register_loop_start(FakeSession& session, uint64_t token)
    {
        std::scoped_lock lock(session_mutex);
        if (session.loop_start.accepts()) session.loop_start.callbacks.insert(token);
    }

    void mark_sessions_ready()
    {
        std::scoped_lock lock(session_mutex);
        for (auto& session : sessions)
            if (!session->loop_start.ready) session->loop_start.mark_ready();
    }

    std::vector<std::pair<FakeSession*, uint64_t>> take_due_loop_starts()
    {
        std::vector<std::pair<FakeSession*, uint64_t>> due;
        std::scoped_lock lock(session_mutex);
        for (auto& session : sessions)
        {
            if (!session->active.load()) continue;
            for (const auto token : session->loop_start.take_due()) due.emplace_back(session.get(), token);
        }
        return due;
    }

    void deliver_lua_pass(bool input_due)
    {
        const auto now = [] { return DispatchBudget::Clock::now(); };
        pass.loop_start(take_due_loop_starts());
        if (!input_due) return;
        pass.faults();
        pass.input(now);
        pass.traces(now);
    }

    void deliver_notice(const Hooks::HookNotice& notice)
    {
        auto* session = find(notice.session);
        if (sink.live(session)) sink.hook(*session);
    }

    bool tick()
    {
        return consumer.tick(
            [this] {
                hooks.tick(DispatchBudget(DispatchBudget::Clock::now(), 64, std::chrono::microseconds(0)), [] {},
                    [](Hooks::QueuedHookEvent&) { return Hooks::EventOutcome::discarded; },
                    [this](const Hooks::HookNotice& notice) { deliver_notice(notice); },
                    [] { return DispatchBudget::Clock::now(); });
            },
            [this](bool input_due) { deliver_lua_pass(input_due); });
    }

    bool update()
    {
        return consumer.update(
            Clock::now(), [this] { mark_sessions_ready(); },
            [this] {
                hooks.update(Clock::now(), DispatchBudget(DispatchBudget::Clock::now(), 64, std::chrono::microseconds(0)),
                    [](Hooks::QueuedHookEvent&) { return Hooks::EventOutcome::discarded; },
                    [this](const Hooks::HookNotice& notice) { deliver_notice(notice); },
                    [] { return DispatchBudget::Clock::now(); });
            },
            [this](bool input_due) { deliver_lua_pass(input_due); });
    }

    void hook_work(FakeSession& session)
    {
        hooks.notify(Hooks::HookNotice{Hooks::message_hook_wakeup, session.id, 1, {}});
    }

    LuaConsumer consumer{1000};
    FakeBackend backend;
    Sink sink;
    LuaPass<FakeBackend, Sink> pass{backend, sink, 64, 0};
    NoPlatform platform;
    Hooks::HookRuntime<NoFrame> hooks{platform, 100000, 1000};
    std::mutex session_mutex;
    std::vector<std::unique_ptr<FakeSession>> sessions;
};

std::shared_ptr<FakeSubscription> subscription(FakeSession& session, uint64_t id, uint64_t target)
{
    auto owner = std::make_shared<FakeSubscription>();
    owner->id = id;
    owner->target = target;
    owner->session = &session;
    return owner;
}

// Calls `step` until `done` or the deadline.
template <typename Step, typename Done>
bool settle(Step step, Done done)
{
    const auto deadline = Clock::now() + 10s;
    while (!done())
    {
        if (Clock::now() >= deadline) return false;
        step();
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// ---- tick mode: every Lua call from the tick thread, none from on_update
void tick_mode()
{
    Bridge bridge(LuaDelivery::tick);
    auto& first = bridge.add_session();
    auto& second = bridge.add_session();
    for (const uint64_t token : {3, 1, 2}) bridge.register_loop_start(first, token);
    auto input_a = subscription(first, 1, 10);
    auto input_b = subscription(second, 2, 20);
    auto failing = subscription(second, 3, 30);
    failing->fail = true;

    // Loop start waits for on_update's barrier.
    for (int i = 0; i < 5; ++i) CHECK(bridge.tick());
    CHECK(bridge.sink.taken_loop_starts().empty());

    constexpr int frames = 400;
    std::atomic_bool ticking{true};
    std::atomic<int> update_passes{};
    std::atomic<int> updates{};
    std::thread tick([&] {
        bridge.sink.consumer = std::this_thread::get_id();
        for (int frame = 0; frame < frames; ++frame)
        {
            // Native Execute and hook handlers run on the game thread too.
            bridge.backend.push_event(input_a);
            bridge.backend.push_event(input_b);
            if (frame % 10 == 0) bridge.backend.push_trace(first, "trace");
            if (frame % 50 == 0) bridge.backend.push_fault(second, 20);
            if (frame == 100) bridge.backend.push_event(failing);
            bridge.hook_work(frame % 2 == 0 ? first : second);
            CHECK(bridge.tick());
        }
        ticking = false;
    });
    std::thread update([&] {
        while (ticking.load())
        {
            if (bridge.update()) ++update_passes;
            ++updates;
            std::this_thread::yield();
        }
    });
    tick.join();
    update.join();
    bridge.sink.consumer = std::this_thread::get_id();
    CHECK(settle([&] { bridge.tick(); }, [&] {
        return bridge.sink.inputs.load() == 2 * frames + 1 && bridge.sink.hooks.load() == frames &&
            bridge.sink.traces.load() == frames / 10 && bridge.sink.faults.load() == frames / 50 + 1;
    }));

    CHECK(updates.load() > 0 && update_passes.load() == 0); // on_update never delivered
    CHECK(bridge.sink.overlaps.load() == 0 && bridge.sink.wrong_thread.load() == 0 && bridge.sink.torn.load() == 0);
    CHECK(bridge.sink.inputs.load() == 2 * frames + 1);
    CHECK(bridge.sink.hooks.load() == frames);
    CHECK(bridge.sink.traces.load() == frames / 10);
    CHECK(bridge.sink.faults.load() == frames / 50 + 1); // pushed faults and the failed callback's
    CHECK(bridge.backend.deactivated.load() == 1 && bridge.sink.reports.load() == 1);
    CHECK((bridge.sink.taken_loop_starts() == std::vector<uint64_t>{1, 2, 3})); // once, in registration order
    {
        std::scoped_lock lock(bridge.session_mutex);
        CHECK(!first.loop_start.accepts() && first.loop_start.callbacks.empty());
    }
}

// ---- loop mode: on_update delivers everything; the tick never does
void loop_mode()
{
    Bridge bridge(LuaDelivery::loop);
    auto& session = bridge.add_session();
    for (const uint64_t token : {5, 4}) bridge.register_loop_start(session, token);
    auto input = subscription(session, 1, 10);

    constexpr int rounds = 200;
    std::atomic_bool producing{true};
    std::atomic<int> tick_passes{};
    std::thread game([&] {
        for (int round = 0; round < rounds; ++round)
        {
            bridge.backend.push_event(input);
            if (round % 20 == 0) bridge.backend.push_trace(session, "trace");
            if (round % 40 == 0) bridge.backend.push_fault(session, 10);
            bridge.hook_work(session);
            if (bridge.tick()) ++tick_passes; // an engine tick in loop mode delivers nothing
            std::this_thread::yield();
        }
        producing = false;
    });
    std::thread update([&] {
        bridge.sink.consumer = std::this_thread::get_id();
        while (producing.load()) bridge.update();
        CHECK(settle([&] { bridge.update(); }, [&] {
            return bridge.sink.inputs.load() == rounds && bridge.sink.hooks.load() == rounds &&
                bridge.sink.traces.load() == rounds / 20 && bridge.sink.faults.load() == rounds / 40;
        }));
    });
    game.join();
    update.join();
    CHECK(tick_passes.load() == 0);
    CHECK(bridge.sink.overlaps.load() == 0 && bridge.sink.wrong_thread.load() == 0);
    CHECK(bridge.sink.inputs.load() == rounds && bridge.sink.hooks.load() == rounds);
    CHECK(bridge.sink.traces.load() == rounds / 20 && bridge.sink.faults.load() == rounds / 40);
    CHECK((bridge.sink.taken_loop_starts() == std::vector<uint64_t>{4, 5}));
}

// ---- a stop issued during a running pass waits for it
void stop_waits_for_pass()
{
    Bridge bridge(LuaDelivery::tick);
    auto& session = bridge.add_session();
    auto blocking = subscription(session, 1, 10);
    blocking->block = true;
    auto later = subscription(session, 2, 10);
    bridge.backend.push_event(blocking);
    bridge.backend.push_event(later);
    std::thread tick([&] { bridge.tick(); });
    while (!bridge.sink.inside.load()) std::this_thread::yield();

    // on_lua_stop on the event-loop thread: inactive first, then the bounded wait.
    session.active = false;
    CHECK(!bridge.consumer.gate().wait_idle(5ms));
    std::thread releaser([&] {
        std::this_thread::sleep_for(20ms);
        bridge.sink.release = true;
    });
    CHECK(bridge.consumer.gate().wait_idle(10s));
    CHECK(bridge.sink.finished.load()); // the stop returned only after the callback
    session.torn_down = true;           // UE4SS now tears the Lua state down
    tick.join();
    releaser.join();

    bridge.backend.push_event(later);
    bridge.hook_work(session);
    bridge.backend.push_trace(session, "after stop");
    CHECK(bridge.tick());
    CHECK(bridge.sink.inputs.load() == 1); // `later` was skipped: the session is inactive
    CHECK(bridge.sink.hooks.load() == 0 && bridge.sink.traces.load() == 0);
    CHECK(bridge.sink.torn.load() == 0 && bridge.sink.overlaps.load() == 0);
}

// ---- unload: an inert gate ends every pass, in both modes
void unload()
{
    for (const auto mode : {LuaDelivery::tick, LuaDelivery::loop})
    {
        Bridge bridge(mode);
        auto& session = bridge.add_session();
        bridge.backend.push_event(subscription(session, 1, 10));
        bridge.consumer.gate().set_inert();
        CHECK(bridge.consumer.gate().wait_idle(0ms));
        CHECK(!bridge.tick() && !bridge.update());
        CHECK(bridge.sink.inputs.load() == 0);
    }
}

// ---- undecided (a C++ mod reinstall starts Lua mods before Unreal init): no delivery yet
void undecided()
{
    Bridge bridge(LuaDelivery::undecided);
    auto& session = bridge.add_session();
    bridge.register_loop_start(session, 1);
    bridge.backend.push_event(subscription(session, 1, 10));
    CHECK(!bridge.tick() && !bridge.update());
    CHECK(bridge.sink.inputs.load() == 0 && bridge.sink.taken_loop_starts().empty());
    {
        std::scoped_lock lock(bridge.session_mutex);
        CHECK(session.loop_start.ready); // on_update still marked the barrier
    }
    bridge.consumer.gate().decide(LuaDelivery::tick);
    CHECK(bridge.tick());
    CHECK(bridge.sink.inputs.load() == 1 && (bridge.sink.taken_loop_starts() == std::vector<uint64_t>{1}));
}
}

int main()
{
    tick_mode();
    loop_mode();
    stop_waits_for_pass();
    unload();
    undecided();
    return LexTests::finish("LuaDeliveryTests");
}
