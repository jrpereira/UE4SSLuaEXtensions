// LuaDispatchGate: one decision, passes open only in the decided mode, the
// inert flag, and the bounded wait for a pass on another thread.
#include <dispatch/LuaDispatchGate.hpp>
#include <TestSupport.hpp>

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

using namespace UE4SSXB;
using namespace std::chrono_literals;

namespace
{
void decided_once()
{
    LuaDispatchGate gate;
    CHECK(gate.mode() == LuaDelivery::undecided);
    CHECK(delivery_name(gate.mode()) == nullptr);
    {
        const LuaDispatchGate::Pass pass(gate, LuaDelivery::tick);
        CHECK(!pass); // nothing delivers before the decision
    }
    CHECK(gate.decide(LuaDelivery::tick) == LuaDelivery::tick);
    CHECK(gate.decide(LuaDelivery::loop) == LuaDelivery::tick); // the first decision stands
    CHECK(std::string_view(delivery_name(gate.mode())) == "tick");
    {
        const LuaDispatchGate::Pass tick(gate, LuaDelivery::tick);
        CHECK(static_cast<bool>(tick));
    }
    {
        const LuaDispatchGate::Pass loop(gate, LuaDelivery::loop);
        CHECK(!loop);
    }
    CHECK(gate.passes() == 1);
    gate.set_inert();
    {
        const LuaDispatchGate::Pass late(gate, LuaDelivery::tick);
        CHECK(!late);
    }
    CHECK(gate.inert() && gate.passes() == 1 && gate.wait_idle(0ms));
}

// A pass on another thread holds wait_idle until it ends; the pass's own
// thread never waits for itself.
void waits_for_running_pass()
{
    LuaDispatchGate gate;
    gate.decide(LuaDelivery::loop);
    std::atomic_bool inside{};
    std::atomic_bool release{};
    std::atomic_bool finished{};
    std::atomic_bool own_thread_ok{};
    std::thread consumer([&] {
        const LuaDispatchGate::Pass pass(gate, LuaDelivery::loop);
        own_thread_ok = static_cast<bool>(pass) && gate.wait_idle(0ms);
        inside = true;
        while (!release.load()) std::this_thread::yield();
        finished = true;
    });
    while (!inside.load()) std::this_thread::yield();
    CHECK(own_thread_ok.load());
    CHECK(!gate.wait_idle(5ms)); // still running: the bounded wait gives up
    std::thread releaser([&] {
        std::this_thread::sleep_for(20ms);
        release = true;
    });
    CHECK(gate.wait_idle(10s));
    CHECK(finished.load()); // returned only after the pass ended
    consumer.join();
    releaser.join();
}

// set_inert racing a pass: either the pass is closed or wait_idle sees it.
void inert_against_passes()
{
    for (int round = 0; round < 200; ++round)
    {
        LuaDispatchGate gate;
        gate.decide(LuaDelivery::tick);
        std::atomic_bool stopped{};
        std::atomic<int> after_stop{};
        std::thread consumer([&] {
            for (int i = 0; i < 1000; ++i)
            {
                const LuaDispatchGate::Pass pass(gate, LuaDelivery::tick);
                if (pass && stopped.load()) ++after_stop; // an open pass after the wait returned
            }
        });
        gate.set_inert();
        CHECK(gate.wait_idle(10s));
        stopped = true;
        consumer.join();
        CHECK(after_stop.load() == 0);
    }
}
}

int main()
{
    decided_once();
    waits_for_running_pass();
    inert_against_passes();
    return LexTests::finish("LuaDispatchGateTests");
}
