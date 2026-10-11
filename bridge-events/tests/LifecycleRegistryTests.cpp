#include <LifecycleRegistry.hpp>

#include <TestSupport.hpp>
#include <thread>
#include <vector>

using UE4SSLuaEventBridge::LifecycleRegistry;
using UE4SSLuaEventBridge::ObjectLifetimeIdentity;

static void reported_losses_and_faults()
{
    LifecycleRegistry registry(2, 1);
    registry.start_session(1);
    registry.start_session(2);
    CHECK(registry.report_losses(1) && registry.report_losses(2));
    CHECK(!registry.report_losses(9));
    const ObjectLifetimeIdentity first{0x1000, 7, 40};
    const ObjectLifetimeIdentity second{0x2000, 8, 50};
    const ObjectLifetimeIdentity replacement{0x1000, 7, 41};

    const auto one = registry.capture(1, first);
    CHECK(one.token > 0 && registry.capture(1, first).token == one.token);
    const auto other_session = registry.capture(2, first);
    CHECK(other_session.token > one.token);
    CHECK(registry.valid(1, one.token, first));
    CHECK(!registry.valid(2, one.token, first));

    registry.invalidate(first.address, first.index);
    CHECK(!registry.valid(1, one.token, first));
    CHECK(registry.take_lost(1).token == one.token);
    CHECK(registry.take_lost(2).token == other_session.token);
    const auto replacement_token = registry.capture(1, replacement);
    CHECK(replacement_token.token > other_session.token);

    const auto two = registry.capture(1, second);
    CHECK(two.token > 0);
    const auto over_capacity = registry.capture(1, {0x3000, 9, 60});
    CHECK(over_capacity.failure == LifecycleRegistry::CaptureFailure::capacity);

    registry.invalidate(replacement.address, replacement.index);
    registry.invalidate(second.address, second.index);
    const auto overflow = registry.take_lost(1);
    CHECK(overflow.overflow && !registry.valid(1, two.token, second));
    CHECK(registry.capture(1, first).failure == LifecycleRegistry::CaptureFailure::faulted);

    registry.stop_session(1);
    CHECK(registry.take_lost(1).unknown_session);
    registry.start_session(1);
    CHECK(registry.capture(1, first).token > two.token);
}

static void unreported_losses_never_fault()
{
    // A session that never asks for losses keeps validating however many of
    // its objects die.
    LifecycleRegistry registry(4, 1);
    registry.start_session(1);
    const ObjectLifetimeIdentity kept{0x5000, 20, 1};
    const auto kept_token = registry.capture(1, kept).token;
    for (int32_t index = 0; index < 100; ++index)
    {
        const ObjectLifetimeIdentity dying{0x6000 + static_cast<std::uintptr_t>(index), 30 + index, 1};
        CHECK(registry.capture(1, dying).token > 0);
        registry.invalidate(dying.address, dying.index);
    }
    CHECK(registry.valid(1, kept_token, kept) && registry.observed() == 1);
    // take_lost opts in: later losses are reported, earlier ones were never queued.
    CHECK(!registry.take_lost(1).token && !registry.take_lost(1).overflow);
    registry.invalidate(kept.address, kept.index);
    CHECK(registry.take_lost(1).token == kept_token);
}

static void indexed_invalidation()
{
    LifecycleRegistry registry;
    registry.start_session(1);
    registry.start_session(2);
    CHECK(registry.observed() == 0);
    registry.invalidate(0x1, 1); // Fast path with nothing observed.

    const ObjectLifetimeIdentity a{0x7000, 40, 3};
    const ObjectLifetimeIdentity b{0x8000, 41, 3};
    const auto a1 = registry.capture(1, a).token;
    const auto a2 = registry.capture(2, a).token;
    const auto b1 = registry.capture(1, b).token;
    CHECK(registry.observed() == 3);

    // Matching by address alone or by index alone invalidates the observation.
    registry.invalidate(a.address, 999);
    CHECK(!registry.valid(1, a1, a) && !registry.valid(2, a2, a) && registry.valid(1, b1, b));
    registry.invalidate(0x9999, b.index);
    CHECK(!registry.valid(1, b1, b) && registry.observed() == 0);

    // A reused slot gets a new token, and the dead one stays dead.
    const ObjectLifetimeIdentity reused{0x7000, 40, 4};
    const auto r1 = registry.capture(1, reused).token;
    CHECK(r1 > b1 && registry.valid(1, r1, reused) && !registry.valid(1, a1, a));

    // Stopping a session drops its watches; another session's survive.
    const auto r2 = registry.capture(2, reused).token;
    registry.stop_session(1);
    CHECK(registry.observed() == 1 && registry.valid(2, r2, reused));
    registry.invalidate(reused.address, reused.index);
    CHECK(!registry.valid(2, r2, reused) && registry.observed() == 0);

    registry.capture(2, a);
    registry.shutdown();
    CHECK(registry.observed() == 0);
}

static void reference_counted_release()
{
    LifecycleRegistry registry;
    registry.start_session(1);
    CHECK(registry.report_losses(1));
    const ObjectLifetimeIdentity object{0xA000, 60, 2};
    const auto token = registry.capture(1, object).token;
    CHECK(registry.capture(1, object).token == token); // Second reference.
    CHECK(registry.release(1, token) && registry.valid(1, token, object));
    CHECK(registry.release(1, token) && !registry.valid(1, token, object));
    CHECK(registry.observed() == 0 && !registry.release(1, token));
    registry.invalidate(object.address, object.index);
    CHECK(!registry.take_lost(1).token && "a released observation reports no loss");
    const auto again = registry.capture(1, object).token;
    CHECK(again > token && registry.valid(1, again, object));
    CHECK(!registry.release(2, again) && !registry.release(1, 0));
}

static void concurrent_capture_and_invalidation()
{
    // Engine listeners invalidate from other threads while Lua captures.
    LifecycleRegistry registry(100000, 100000);
    registry.start_session(1);
    registry.report_losses(1);
    constexpr int32_t count = 20000;
    std::thread engine([&] {
        for (int32_t index = 0; index < count; ++index)
            registry.invalidate(0x100000 + static_cast<std::uintptr_t>(index), index);
    });
    std::vector<uint64_t> tokens;
    for (int32_t index = 0; index < count; ++index)
    {
        const ObjectLifetimeIdentity identity{0x100000 + static_cast<std::uintptr_t>(index), index, 1};
        tokens.push_back(registry.capture(1, identity).token);
    }
    engine.join();
    for (int32_t index = 0; index < count; ++index)
        registry.invalidate(0x100000 + static_cast<std::uintptr_t>(index), index);
    CHECK(registry.observed() == 0);
    std::size_t lost{};
    while (registry.take_lost(1).token) ++lost;
    CHECK(lost == static_cast<std::size_t>(count) && "every observation ends exactly once");
}

int main()
{
    reported_losses_and_faults();
    unreported_losses_never_fault();
    indexed_invalidation();
    reference_counted_release();
    concurrent_capture_and_invalidation();
    return LexTests::finish("LifecycleRegistryTests");
}
