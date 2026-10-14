#include <ReentryGuard.hpp>

#include <TestSupport.hpp>
#include <thread>

using UE4SSLuaEventBridge::Hooks::guard_overflow_count;
using UE4SSLuaEventBridge::Hooks::local_reentry_guard;
using UE4SSLuaEventBridge::Hooks::max_guard_records;
using UE4SSLuaEventBridge::Hooks::ReentryGuard;
using Enter = ReentryGuard::Enter;

namespace
{
// Addresses as on a downward-growing stack: a callee's frame is lower.
constexpr std::uintptr_t outer = 0x10000;
constexpr std::uintptr_t inner = 0x0F000;
constexpr std::uintptr_t innermost = 0x0E000;

void detects_loop_back_to_offset_zero()
{
    ReentryGuard guard;
    CHECK(guard.enter(outer, 1) == Enter::genuine);
    CHECK(guard.enter(outer, 1) == Enter::reentry); // the loop jumped back to Script[0]
    CHECK(guard.enter(outer, 1) == Enter::reentry);
    CHECK(guard.size() == 1);
    CHECK(guard.exit(outer, 1));
    CHECK(guard.size() == 0);
    CHECK(guard.enter(outer, 1) == Enter::genuine); // the next call is genuine again
}

void nested_and_recursive_calls_are_genuine()
{
    ReentryGuard guard;
    CHECK(guard.enter(outer, 1) == Enter::genuine);
    CHECK(guard.enter(inner, 2) == Enter::genuine);     // callee
    CHECK(guard.enter(innermost, 1) == Enter::genuine); // recursion: same entry, new frame
    CHECK(guard.size() == 3);
    CHECK(guard.exit(innermost, 1));
    CHECK(guard.exit(inner, 2));
    CHECK(guard.enter(outer, 1) == Enter::reentry); // still inside the outer call
    CHECK(guard.exit(outer, 1));
    CHECK(guard.size() == 0);
}

void prunes_dead_callees()
{
    ReentryGuard guard;
    CHECK(guard.enter(outer, 1) == Enter::genuine);
    CHECK(guard.enter(inner, 2) == Enter::genuine);
    CHECK(guard.enter(innermost, 3) == Enter::genuine);
    // inner and innermost never reached B (aborted). The outer frame loops back:
    // records below it are dead callees and go.
    CHECK(guard.enter(outer, 1) == Enter::reentry);
    CHECK(guard.size() == 1);

    // A new callee at the address a dead one used is genuine (different entry).
    CHECK(guard.enter(inner, 4) == Enter::genuine);
    CHECK(guard.size() == 2);
    // B of the outer frame prunes the callee record as well.
    CHECK(guard.exit(outer, 1));
    CHECK(guard.size() == 0);
}

void exit_without_record()
{
    ReentryGuard guard;
    CHECK(!guard.exit(outer, 1)); // hooked after the frame passed Script[0]
    CHECK(guard.enter(outer, 1) == Enter::genuine);
    CHECK(!guard.exit(outer, 2)); // another entry's frame at the same address: not ours
    CHECK(guard.size() == 1);
    CHECK(guard.exit(outer, 1));
}

void overflow_at_the_bound()
{
    ReentryGuard guard;
    const auto before = guard_overflow_count.load();
    for (std::size_t depth = 0; depth < max_guard_records; ++depth)
        CHECK(guard.enter(outer - depth * 0x100, static_cast<uint32_t>(depth)) == Enter::genuine);
    CHECK(guard.size() == max_guard_records);
    const std::uintptr_t deeper = outer - max_guard_records * 0x100;
    CHECK(guard.enter(deeper, 999) == Enter::overflow);
    CHECK(guard.enter(deeper, 999) == Enter::overflow); // not recorded, so no re-entry detection
    CHECK(guard_overflow_count.load() == before + 2);
    CHECK(guard.size() == max_guard_records);
    CHECK(!guard.exit(deeper, 999));
    // The deepest recorded frame returns; there is room again.
    CHECK(guard.exit(outer - (max_guard_records - 1) * 0x100, static_cast<uint32_t>(max_guard_records - 1)));
    CHECK(guard.enter(deeper, 999) == Enter::genuine);
    // Returning to the outermost frame prunes everything below it.
    CHECK(guard.enter(outer, 0) == Enter::reentry);
    CHECK(guard.size() == 1);
}

void per_thread_guards()
{
    auto& main_guard = local_reentry_guard();
    main_guard.clear();
    CHECK(main_guard.enter(outer, 7) == Enter::genuine);
    Enter other{};
    std::size_t other_size{};
    std::thread worker([&] {
        auto& guard = local_reentry_guard();
        other = guard.enter(outer, 7); // same address and entry, another thread's stack
        other_size = guard.size();
    });
    worker.join();
    CHECK(other == Enter::genuine && other_size == 1);
    CHECK(&local_reentry_guard() == &main_guard && main_guard.size() == 1);
    CHECK(main_guard.exit(outer, 7));
}
}

int main()
{
    detects_loop_back_to_offset_zero();
    nested_and_recursive_calls_are_genuine();
    prunes_dead_callees();
    exit_without_record();
    overflow_at_the_bound();
    per_thread_guards();
    return LexTests::finish("ReentryGuardTests");
}
