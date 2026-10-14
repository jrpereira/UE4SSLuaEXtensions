#include "support/FakeFrame.hpp"

#include <TestSupport.hpp>
#include <atomic>
#include <thread>
#include <vector>

using namespace UE4SSLuaEventBridge::Hooks;
using namespace UE4SSLuaEventBridge::Hooks::Testing;

namespace
{
struct Tag
{
};
using Rig = FakeHookRig<Tag>;

uint8_t slot_a{};
uint8_t slot_b{};
RecordingSink sink;

const void* fake_address(std::uintptr_t value) { return reinterpret_cast<const void*>(value); }

// The native function's original Func: counts and writes 42 to the result.
void original_func(RC::Unreal::UObject*, FakeFrame& frame, void* result)
{
    ++frame.counter;
    if (result != nullptr) *static_cast<int32_t*>(result) = 42;
}

// Another hooker (UE4SS RegisterHook) that wraps whatever Func it found.
NativeFuncPtr other_saved{};
std::atomic<uint32_t> other_calls{0};
void other_hooker(RC::Unreal::UObject* object, FakeFrame& frame, void* result)
{
    ++other_calls;
    function_cast<FakeExec>(other_saved)(object, frame, result);
}

void entries_and_states()
{
    HookTable table;
    CHECK(!table.maybe_hooked(fake_address(0x1000)));
    uint32_t first{};
    {
        auto writer = table.write();
        const auto acquired = writer.acquire(fake_address(0x1000), EntryKind::blueprint);
        CHECK(acquired.entry != nullptr && acquired.entry->id == 0 && acquired.entry->state == EntryState::inert);
        first = acquired.entry->id;
        acquired.entry->state = EntryState::active;
        acquired.entry->patch.saved_first = 0x20;
        const auto second = writer.acquire(fake_address(0x2000), EntryKind::native);
        CHECK(second.entry != nullptr && second.entry->id == 1);
        // Same function, other kind, while active: refused.
        CHECK(writer.acquire(fake_address(0x1000), EntryKind::native).entry == nullptr);
        CHECK(writer.acquire(fake_address(0x1000), EntryKind::blueprint).entry == acquired.entry);
    }
    CHECK(table.size() == 2 && table.maybe_hooked(fake_address(0x1000)));

    // Inert entries keep answering lookups with their saved bytes.
    {
        auto writer = table.write();
        writer.entry(first)->state = EntryState::inert;
    }
    {
        auto reader = table.read();
        const auto* entry = reader.find(fake_address(0x1000));
        CHECK(entry != nullptr && entry->state == EntryState::inert && entry->patch.saved_first == 0x20);
        CHECK(reader.entry(first) == entry && reader.entry(99) == nullptr && reader.find(fake_address(0x3000)) == nullptr);
    }

    // Deletion orphans; a second notice and unknown objects do nothing.
    CHECK(table.on_deleted(fake_address(0x1000)) == std::optional<uint32_t>(first));
    CHECK(!table.on_deleted(fake_address(0x1000)).has_value());
    CHECK(!table.on_deleted(fake_address(0x7777)).has_value());
    {
        auto reader = table.read();
        CHECK(reader.find(fake_address(0x1000))->state == EntryState::orphaned);
    }
    // A new function at the same address revives the entry as a fresh one, of
    // either kind; the id stays.
    {
        auto writer = table.write();
        const auto revived = writer.acquire(fake_address(0x1000), EntryKind::native);
        CHECK(revived.entry != nullptr && revived.entry->id == first && revived.entry->kind == EntryKind::native);
        CHECK(revived.entry->state == EntryState::inert && revived.entry->patch.saved_first == 0);
        // An inert native entry whose thunk is still in a chain cannot change kind.
        revived.entry->thunk_in_chain = true;
        CHECK(writer.acquire(fake_address(0x1000), EntryKind::blueprint).entry == nullptr);
        CHECK(writer.acquire(fake_address(0x1000), EntryKind::native).entry == revived.entry);
    }
}

void capacity()
{
    HookTable table;
    auto writer = table.write();
    for (std::uintptr_t index = 0; index < max_entries; ++index)
        CHECK(writer.acquire(fake_address(0x10000 + index * 16), EntryKind::blueprint).entry != nullptr);
    const auto full = writer.acquire(fake_address(0x900000), EntryKind::blueprint);
    CHECK(full.entry == nullptr && full.refusal != nullptr);
    // Existing functions are still found.
    CHECK(writer.acquire(fake_address(0x10000), EntryKind::blueprint).entry != nullptr);
    CHECK(writer.size() == max_entries);
}

void subscriptions()
{
    HookTable table;
    auto writer = table.write();
    const auto id = writer.acquire(fake_address(0x1000), EntryKind::blueprint).entry->id;
    writer.entry(id)->state = EntryState::active;
    auto* one = writer.subscribe(id, 1, 10);
    auto* two = writer.subscribe(id, 2, 20);
    CHECK(one != nullptr && two != nullptr && one != two);
    one->phases = static_cast<uint8_t>(Phase::pre);
    one->counters.calls = 5;
    CHECK(writer.subscription(id, 1, 10) == one && writer.subscription(id, 1, 11) == nullptr);
    CHECK(writer.active_subscriptions(id) == 2);
    CHECK(writer.subscribe(99, 1, 1) == nullptr);

    // An unsubscribed slot with queued events is not recycled...
    one->queued = 3;
    CHECK(writer.unsubscribe(id, 1, 10) && !writer.unsubscribe(id, 1, 10));
    auto* three = writer.subscribe(id, 3, 30);
    CHECK(three != one);
    // ... until its queue drained; then it is, reset.
    CHECK(writer.unsubscribe(id, 3, 30));
    one->queued = 0;
    auto* four = writer.subscribe(id, 4, 40);
    CHECK(four == one && four->session == 4 && four->hook == 40 && four->phases == 0);
    CHECK(four->counters.calls.load() == 0 && four->active.load());

    // Session stop: entries left without subscribers are reported.
    const auto other = writer.acquire(fake_address(0x2000), EntryKind::native).entry->id;
    writer.entry(other)->state = EntryState::active;
    writer.subscribe(other, 2, 21);
    const auto idle = writer.unsubscribe_session(2);
    CHECK(idle == std::vector<uint32_t>{other}); // entry `id` still has session 4
    CHECK(writer.idle_entries() == std::vector<uint32_t>{other});
    CHECK(writer.unsubscribe(id, 4, 40));
    CHECK(writer.idle_entries() == (std::vector<uint32_t>{id, other}));
}

void native_paths()
{
    sink.clear();
    FakeFunction function;
    DeleteNotice deleted{Rig::table, function};
    function.flags = func_native;
    function.set_func(as_native(&original_func));

    const auto hooked = Rig::hook_native(function);
    CHECK(hooked.ok() && function.get_func() == Rig::thunk_pointer());
    CHECK(Rig::hook_native(function).entry == hooked.entry); // idempotent

    // ProcessEvent path: Node is the function, params available.
    int32_t value{};
    FakeFrame event_frame;
    event_frame.node = &function;
    invoke(function, nullptr, event_frame, &value);
    CHECK(event_frame.counter == 1 && value == 42);
    CHECK(sink.records.size() == 2);
    if (sink.records.size() == 2)
    {
        CHECK(sink.records[0].phase == Phase::pre && sink.records[0].params_available && sink.records[0].result == nullptr);
        CHECK(sink.records[1].phase == Phase::post && sink.records[1].params_available && sink.records[1].result == &value);
    }

    // Blueprint caller: Node is the calling script function.
    sink.clear();
    FakeFunction caller{0x20, 0x04, 0x0B, 0x53};
    FakeFrame bp_frame;
    bp_frame.node = &caller;
    value = 0;
    invoke(function, nullptr, bp_frame, &value);
    CHECK(bp_frame.counter == 1 && value == 42);
    CHECK(sink.records.size() == 2);
    if (sink.records.size() == 2)
        CHECK(!sink.records[0].params_available && !sink.records[1].params_available && sink.records[1].result == &value);

    // Plain unhook restores Func.
    CHECK(Rig::unhook_native(function, hooked.entry).ok());
    CHECK(function.get_func() == as_native(&original_func));
    sink.clear();
    invoke(function, nullptr, event_frame, &value);
    CHECK(sink.records.empty());

    // Refusals.
    FakeFunction script_function{0x20, 0x04, 0x0B, 0x53};
    DeleteNotice script_deleted{Rig::table, script_function};
    CHECK(!Rig::hook_native(script_function).ok());
    FakeFunction no_func;
    DeleteNotice no_func_deleted{Rig::table, no_func};
    no_func.flags = func_native;
    CHECK(!Rig::hook_native(no_func).ok());

    // A thunk call for an unknown function calls nothing and is counted.
    const auto misses = Rig::context.thunk_misses.load();
    FakeFrame stray;
    stray.current_native_function = &no_func;
    stray.node = &caller;
    Rig::thunk(nullptr, stray, nullptr);
    CHECK(Rig::context.thunk_misses.load() == misses + 1 && stray.counter == 0);
}

void chain_safe_restore()
{
    sink.clear();
    FakeFunction function;
    DeleteNotice deleted{Rig::table, function};
    function.flags = func_native;
    function.set_func(as_native(&original_func));
    const auto hooked = Rig::hook_native(function);
    CHECK(hooked.ok());

    // Another hooker swaps Func after us, saving our thunk.
    other_saved = function.get_func();
    function.set_func(as_native(&other_hooker));
    FakeFrame frame;
    frame.node = &function;
    invoke(function, nullptr, frame, nullptr);
    CHECK(frame.counter == 1 && other_calls.load() == 1 && sink.count(Phase::pre) == 1);

    // Our unhook must not touch Func: the entry goes inert, the thunk passes through.
    CHECK(Rig::unhook_native(function, hooked.entry).ok());
    CHECK(function.get_func() == as_native(&other_hooker));
    {
        auto reader = Rig::table.read();
        const auto* entry = reader.entry(hooked.entry);
        CHECK(entry->state == EntryState::inert && entry->thunk_in_chain);
    }
    sink.clear();
    invoke(function, nullptr, frame, nullptr);
    CHECK(frame.counter == 2 && other_calls.load() == 2 && sink.records.empty());

    // Rehook reactivates without writing (writing would put the thunk in the chain twice).
    CHECK(Rig::hook_native(function).ok());
    CHECK(function.get_func() == as_native(&other_hooker));
    invoke(function, nullptr, frame, nullptr);
    CHECK(frame.counter == 3 && sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);

    // The other hooker leaves, putting our thunk back as Func; our unhook now restores the original.
    function.set_func(other_saved);
    CHECK(Rig::unhook_native(function, hooked.entry).ok());
    CHECK(function.get_func() == as_native(&original_func));
    {
        auto reader = Rig::table.read();
        CHECK(!reader.entry(hooked.entry)->thunk_in_chain);
    }

    // Hooked first by the other hooker, then by us: our thunk calls it.
    other_saved = function.get_func();
    function.set_func(as_native(&other_hooker));
    CHECK(Rig::hook_native(function).ok());
    const auto before = other_calls.load();
    invoke(function, nullptr, frame, nullptr);
    CHECK(other_calls.load() == before + 1 && frame.counter == 4);
    CHECK(Rig::unhook_native(function, hooked.entry).ok());
    CHECK(function.get_func() == as_native(&other_hooker));
}

struct CountingSink
{
    std::atomic<uint64_t> pre{0};
    std::atomic<uint64_t> post{0};
    static void sink(const HookCall<FakeFrame>& call, void* user)
    {
        auto& self = *static_cast<CountingSink*>(user);
        (call.phase == Phase::pre ? self.pre : self.post).fetch_add(1, std::memory_order_relaxed);
    }
};

void concurrent_calls_while_hooking_and_unhooking()
{
    CountingSink counting;
    Rig::context.set_sink(&CountingSink::sink, &counting);
    FakeFunction script{0x20, 0x20, 0x21, 0x04, 0x0B, 0x53};
    DeleteNotice script_deleted{Rig::table, script};
    const auto original_script = script.script;
    FakeFunction native;
    DeleteNotice native_deleted{Rig::table, native};
    native.flags = func_native;
    native.set_func(as_native(&original_func));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> failures{0};
    std::atomic<uint64_t> calls{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                FakeFrame frame;
                frame.loop_limit = 4; // two counts per pass, loops back to offset 0 once
                run_script(Rig::natives.table.data(), script, frame);
                if (frame.counter != 4 || frame.undefined_hits != 0 || frame.end_of_script_hits != 0 ||
                    frame.return_steps != 0)
                    failures.fetch_add(1);
                FakeFrame native_frame;
                native_frame.node = &native;
                int32_t value{};
                invoke(native, nullptr, native_frame, &value);
                if (native_frame.counter != 1 || value != 42) failures.fetch_add(1);
                {
                    auto reader = Rig::table.read();
                    const auto* entry = reader.find(&script);
                    if (entry != nullptr && (entry->function != &script || entry->kind != EntryKind::blueprint))
                        failures.fetch_add(1);
                }
                calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    uint32_t script_entry_id{};
    uint32_t native_entry_id{};
    for (int round = 0; round < 2000; ++round)
    {
        const auto bp = Rig::hook(script, slot_a, slot_b);
        const auto nat = Rig::hook_native(native);
        if (!bp.ok() || !nat.ok()) failures.fetch_add(1);
        script_entry_id = bp.entry;
        native_entry_id = nat.entry;
        {
            auto writer = Rig::table.write();
            auto* subscription = writer.subscribe(bp.entry, 1, static_cast<uint64_t>(round));
            if (subscription == nullptr) failures.fetch_add(1);
            else subscription->phases = static_cast<uint8_t>(Phase::pre) | static_cast<uint8_t>(Phase::post);
        }
        std::this_thread::yield();
        {
            auto writer = Rig::table.write();
            writer.unsubscribe_session(1);
        }
        if (!Rig::unhook(script, bp.entry).ok() || !Rig::unhook_native(native, nat.entry).ok()) failures.fetch_add(1);
    }
    // Let the workers run hooked for a while too.
    Rig::hook(script, slot_a, slot_b);
    Rig::hook_native(native);
    const auto settled = calls.load();
    while (calls.load() < settled + 2000) std::this_thread::yield();
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    CHECK(failures.load() == 0);
    CHECK(calls.load() > 2000);
    CHECK(counting.pre.load() > 0 && counting.post.load() > 0);
    CHECK(Rig::unhook(script, script_entry_id).ok());
    CHECK(Rig::unhook_native(native, native_entry_id).ok());
    // The workers ran the function on other threads, so unhook restored A and
    // left B over the EX_Return byte (ScriptPatch.hpp, keep_exit).
    auto expected = original_script;
    expected[3] = slot_b;
    CHECK(script.script == expected);
    CHECK(native.get_func() == as_native(&original_func));
    {
        auto reader = Rig::table.read();
        const auto* entry = reader.entry(script_entry_id);
        CHECK(entry->state == EntryState::inert && entry->exit_kept && entry->off_thread.load() && !entry->reason.empty());
    }
    FakeFrame after;
    after.loop_limit = 4;
    run_script(Rig::natives.table.data(), script, after);
    CHECK(after.counter == 4 && after.end_of_script_hits == 0 && after.return_steps == 0);
    Rig::context.set_sink(&RecordingSink::sink, &sink);
}
}

int main()
{
    const auto claim = Rig::claim();
    CHECK(claim.slots.has_value());
    if (!claim.slots) return LexTests::finish("HookTableTests");
    slot_a = claim.slots->a;
    slot_b = claim.slots->b;
    Rig::context.set_sink(&RecordingSink::sink, &sink);

    entries_and_states();
    capacity();
    subscriptions();
    native_paths();
    chain_safe_restore();
    concurrent_calls_while_hooking_and_unhooking();
    return LexTests::finish("HookTableTests");
}
