// Tail delivery from the engine tick (FileBridgeTailDelivery.hpp with
// dispatch/LuaDispatchGate.hpp): on_update polls and offers batches on one
// thread, an injected tick delivers them on another. Every line arrives once,
// in order, from the tick thread only, and a stop waits for a running pass.

#include <FileBridgeTailDelivery.hpp>
#include <TestSupport.hpp>
#include <dispatch/LuaDispatchGate.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace UE4SSLuaFileBridge::Core;
using namespace std::chrono_literals;
using UE4SSXB::DispatchBudget;
using UE4SSXB::LuaDelivery;
using UE4SSXB::LuaDispatchGate;

namespace
{
struct Session
{
    std::atomic_bool active{true};
    std::atomic_bool torn_down{};
};

struct Pending
{
    std::shared_ptr<Session> session;
    int64_t line{};
};

// The session's dispatcher: one entry at a time, from the expected thread.
struct Lua
{
    std::atomic<int> depth{};
    std::atomic<int> overlaps{};
    std::atomic<int> wrong_thread{};
    std::atomic<int> torn{};
    std::atomic<std::thread::id> consumer{};
    std::vector<int64_t> lines; // consumer thread only
    std::atomic_bool block{};
    std::atomic_bool inside{};
    std::atomic_bool release{};
    std::atomic_bool finished{};

    bool deliver(Pending& pending)
    {
        if (!pending.session->active.load()) return false;
        if (depth.fetch_add(1) != 0) ++overlaps;
        if (pending.session->torn_down.load()) ++torn;
        if (consumer.load() != std::this_thread::get_id()) ++wrong_thread;
        lines.push_back(pending.line);
        if (block.load())
        {
            inside = true;
            while (!release.load()) std::this_thread::yield();
            finished = true;
        }
        depth.fetch_sub(1);
        return true;
    }
};

DispatchBudget budget(std::size_t count)
{
    return DispatchBudget(DispatchBudget::Clock::now(), count, std::chrono::microseconds(0));
}

void handoff()
{
    TailHandoff<Pending> tail;
    auto session = std::make_shared<Session>();
    CHECK(tail.wants_batch());
    CHECK(tail.offer({}));
    CHECK(tail.offer({Pending{session, 1}, Pending{session, 2}, Pending{session, 3}}));
    CHECK(!tail.wants_batch());
    CHECK(!tail.offer({Pending{session, 4}})); // the files stay the buffer
    Lua lua;
    lua.consumer = std::this_thread::get_id();
    bool exhausted = false;
    const auto now = [] { return DispatchBudget::Clock::now(); };
    CHECK(tail.deliver(budget(2), [&](Pending& pending) { return lua.deliver(pending); }, now, exhausted) == 2);
    CHECK(exhausted && tail.size() == 1);
    CHECK(tail.deliver(budget(2), [&](Pending& pending) { return lua.deliver(pending); }, now, exhausted) == 1);
    CHECK(!exhausted && tail.wants_batch());
    CHECK((lua.lines == std::vector<int64_t>{1, 2, 3}));
    // A stopped session's entries are discarded and do not count.
    session->active = false;
    CHECK(tail.offer({Pending{session, 5}, Pending{session, 6}}));
    CHECK(tail.deliver(budget(1), [&](Pending& pending) { return lua.deliver(pending); }, now, exhausted) == 0);
    CHECK(!exhausted && lua.lines.size() == 3);
}

// on_update offers, the tick delivers; on_update never calls Lua in tick mode.
void tick_delivery()
{
    LuaDispatchGate gate;
    gate.decide(LuaDelivery::tick);
    TailHandoff<Pending> tail;
    auto session = std::make_shared<Session>();
    Lua lua;
    constexpr int64_t total = 20000;
    std::atomic<int64_t> offered{};
    std::atomic_bool polling{true};
    std::atomic<int> loop_passes{};

    std::thread update([&] {
        int64_t next = 0;
        while (next < total)
        {
            if (tail.wants_batch())
            {
                std::vector<Pending> batch;
                for (int i = 0; i < 37 && next < total; ++i) batch.push_back(Pending{session, next++});
                CHECK(tail.offer(std::move(batch)));
                offered = next;
            }
            // Loop-mode delivery path: closed in tick mode.
            const LuaDispatchGate::Pass pass(gate, LuaDelivery::loop);
            if (pass) ++loop_passes;
            std::this_thread::yield();
        }
        polling = false;
    });
    std::thread tick([&] {
        lua.consumer = std::this_thread::get_id();
        const auto now = [] { return DispatchBudget::Clock::now(); };
        while (polling.load() || tail.size() != 0)
        {
            const LuaDispatchGate::Pass pass(gate, LuaDelivery::tick);
            if (!pass) continue;
            bool exhausted = false;
            tail.deliver(budget(16), [&](Pending& pending) { return lua.deliver(pending); }, now, exhausted);
        }
    });
    update.join();
    tick.join();
    CHECK(loop_passes.load() == 0);
    CHECK(lua.overlaps.load() == 0 && lua.wrong_thread.load() == 0);
    CHECK(static_cast<int64_t>(lua.lines.size()) == total);
    bool ordered = true;
    for (std::size_t i = 0; i < lua.lines.size(); ++i) ordered = ordered && lua.lines[i] == static_cast<int64_t>(i);
    CHECK(ordered);
}

// on_lua_stop off the game thread: inactive, then the bounded wait.
void stop_waits_for_pass()
{
    LuaDispatchGate gate;
    gate.decide(LuaDelivery::tick);
    TailHandoff<Pending> tail;
    auto session = std::make_shared<Session>();
    Lua lua;
    lua.block = true;
    CHECK(tail.offer({Pending{session, 1}, Pending{session, 2}}));
    std::thread tick([&] {
        lua.consumer = std::this_thread::get_id();
        const LuaDispatchGate::Pass pass(gate, LuaDelivery::tick);
        bool exhausted = false;
        tail.deliver(budget(0), [&](Pending& pending) { return lua.deliver(pending); },
            [] { return DispatchBudget::Clock::now(); }, exhausted);
    });
    while (!lua.inside.load()) std::this_thread::yield();
    session->active = false;
    CHECK(!gate.wait_idle(5ms));
    std::thread releaser([&] {
        std::this_thread::sleep_for(20ms);
        lua.release = true;
    });
    CHECK(gate.wait_idle(10s));
    CHECK(lua.finished.load());
    session->torn_down = true;
    tick.join();
    releaser.join();
    CHECK((lua.lines == std::vector<int64_t>{1})); // line 2 met an inactive session
    CHECK(lua.torn.load() == 0 && tail.size() == 0);

    // Unload: the inert gate closes every later pass.
    gate.set_inert();
    CHECK(gate.wait_idle(0ms));
    const LuaDispatchGate::Pass late(gate, LuaDelivery::tick);
    CHECK(!late);
}
}

int main()
{
    handoff();
    tick_delivery();
    stop_waits_for_pass();
    return LexTests::finish("TailDeliveryTests");
}
