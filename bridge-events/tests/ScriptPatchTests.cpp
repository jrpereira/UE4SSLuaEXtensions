#include "support/FakeFrame.hpp"

#include <TestSupport.hpp>
#include <string_view>
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

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values) { return std::vector<uint8_t>(values); }

void validation_refusals()
{
    auto plan = [](std::vector<uint8_t> script, uint32_t flags = 0) {
        return plan_script_patch(std::span<uint8_t>(script), flags, 0xFF, 0xFE).refusal;
    };
    CHECK(plan({}) == ScriptRefusal::no_script);
    CHECK(plan(bytes({0x04, 0x0B})) == ScriptRefusal::bad_tail);
    CHECK(plan(bytes({0x20, 0x04, 0x0B})) == ScriptRefusal::bad_tail);
    CHECK(plan(bytes({0x20, 0x04, 0x0B, 0x53, 0x00})) == ScriptRefusal::bad_tail);
    CHECK(plan(bytes({0x20, 0x04, 0x53, 0x53})) == ScriptRefusal::bad_tail);
    CHECK(plan(bytes({0x20, 0x04, 0x0B, 0x53}), func_native) == ScriptRefusal::native_function);
    CHECK(plan(bytes({0x04, 0x20, 0x04, 0x0B, 0x53})) == ScriptRefusal::return_at_entry);
    CHECK(plan(bytes({0xFF, 0x20, 0x04, 0x0B, 0x53})) == ScriptRefusal::already_patched);
    CHECK(plan(bytes({0xFE, 0x20, 0x04, 0x0B, 0x53})) == ScriptRefusal::already_patched);
    std::vector<uint8_t> ok = bytes({0x20, 0x04, 0x0B, 0x53});
    CHECK(plan_script_patch(std::span<uint8_t>(ok), 0, 0xFF, 0xFF).refusal == ScriptRefusal::invalid_slots);
    CHECK(plan_script_patch(std::span<uint8_t>(ok), 0, ex_return, 0xFE).refusal == ScriptRefusal::invalid_slots);
    CHECK(std::string_view(describe(ScriptRefusal::bad_tail)).find("EX_Return") != std::string_view::npos);

    // A null data pointer with a size (TArray never allocated) is no script.
    CHECK(plan_script_patch(std::span<uint8_t>(), 0, 0xFF, 0xFE).refusal == ScriptRefusal::no_script);
}

void plan_apply_restore_round_trip()
{
    std::vector<uint8_t> script = bytes({0x20, 0x21, 0x06, 0x09, 0x00, 0x00, 0x00, 0x20, 0x04, 0x0B, 0x53});
    const auto original = script;
    const auto plan = plan_script_patch(std::span<uint8_t>(script), 0, 0xFF, 0xFE);
    CHECK(plan.refusal == ScriptRefusal::none);
    CHECK(!plan.patch.empty_body && plan.patch.tail == 8 && plan.patch.saved_first == 0x20);
    apply_script_patch(plan.patch);
    CHECK(script[0] == 0xFF && script[8] == 0xFE);
    for (std::size_t index = 1; index < script.size(); ++index)
        if (index != 8) CHECK(script[index] == original[index]); // length and every jump operand unchanged
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(script)) == ScriptRestore::restored);
    CHECK(script == original);

    // Restore against a moved buffer with the same content works: the length
    // and our opcodes identify the patch, not the address.
    apply_script_patch(plan.patch);
    std::vector<uint8_t> moved = script;
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(moved)) == ScriptRestore::restored);
    CHECK(moved == original);
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(script)) == ScriptRestore::restored);
    CHECK(script == original);

    // A resized Script is left alone.
    apply_script_patch(plan.patch);
    std::vector<uint8_t> grown = script;
    grown.push_back(0x53);
    const auto grown_before = grown;
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(grown)) == ScriptRestore::resized);
    CHECK(grown == grown_before);

    // A byte someone else overwrote is left alone; ours is restored.
    script[8] = 0x77;
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(script)) == ScriptRestore::foreign);
    CHECK(script[0] == 0x20 && script[8] == 0x77);
}

void empty_body_patch()
{
    std::vector<uint8_t> script = bytes({0x04, 0x0B, 0x53});
    const auto plan = plan_script_patch(std::span<uint8_t>(script), 0, 0xFF, 0xFE);
    CHECK(plan.refusal == ScriptRefusal::none && plan.patch.empty_body && plan.patch.tail == 0);
    apply_script_patch(plan.patch);
    CHECK(script == bytes({0xFE, 0x0B, 0x53})); // only B
    CHECK(restore_script_patch(plan.patch, std::span<uint8_t>(script)) == ScriptRestore::restored);
    CHECK(script == bytes({0x04, 0x0B, 0x53}));
}

void straight_body_reports_pre_and_post()
{
    sink.clear();
    FakeFunction function{0x20, 0x20, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto original = function.script;
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    CHECK(function.script[0] == slot_a && function.script[2] == slot_b);

    FakeFrame frame;
    run_script(Rig::natives.table.data(), function, frame);
    CHECK(frame.counter == 2);                 // the saved first opcode ran through the fake GNatives
    CHECK(frame.code == script_return_code + 2); // B redirected Code; the loop stepped EX_Nothing
    CHECK(frame.end_of_script_hits == 0 && frame.undefined_hits == 0 && frame.return_steps == 0);
    CHECK(sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);
    CHECK(sink.records.size() == 2 && sink.records[0].phase == Phase::pre && sink.records[1].phase == Phase::post);
    CHECK(sink.records[0].entry == hooked.entry && sink.records[0].params_available);
    CHECK(local_reentry_guard().size() == 0);

    // Idempotent hook.
    const auto again = Rig::hook(function, slot_a, slot_b);
    CHECK(again.ok() && again.entry == hooked.entry);

    CHECK(Rig::unhook(function, hooked.entry).ok());
    CHECK(function.script == original);
    sink.clear();
    FakeFrame plain;
    run_script(Rig::natives.table.data(), function, plain);
    CHECK(plain.counter == 2 && sink.records.empty());
}

void loop_back_to_offset_zero_is_one_call()
{
    sink.clear();
    // count; loop back to 0 while counter < 3; return.
    FakeFunction function{0x20, 0x21, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    FakeFrame frame;
    frame.loop_limit = 3;
    run_script(Rig::natives.table.data(), function, frame);
    CHECK(frame.counter == 3); // A ran three times and dispatched count each time
    CHECK(sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);
    {
        auto writer = Rig::table.write();
        auto* subscription = writer.subscribe(hooked.entry, 1, 1);
        CHECK(subscription != nullptr);
    }
    frame = FakeFrame{};
    frame.loop_limit = 3;
    run_script(Rig::natives.table.data(), function, frame);
    {
        auto writer = Rig::table.write();
        auto* subscription = writer.subscription(hooked.entry, 1, 1);
        CHECK(subscription != nullptr && subscription->counters.reentries.load() == 2);
        CHECK(writer.unsubscribe(hooked.entry, 1, 1));
    }
    CHECK(local_reentry_guard().size() == 0);
    CHECK(Rig::unhook(function, hooked.entry).ok());
}

void early_return_jump_reaches_b()
{
    sink.clear();
    // EX_Jump to the EX_Return at offset 6, skipping count.
    FakeFunction function{0x06, 0x06, 0x00, 0x00, 0x00, 0x20, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    FakeFrame frame;
    run_script(Rig::natives.table.data(), function, frame);
    CHECK(frame.counter == 0 && frame.end_of_script_hits == 0);
    CHECK(sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);
    CHECK(Rig::unhook(function, hooked.entry).ok());
}

void empty_body_fires_pre_and_post_from_b()
{
    sink.clear();
    FakeFunction function{0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    CHECK(function.script[0] == slot_b);
    FakeFrame frame;
    run_script(Rig::natives.table.data(), function, frame);
    CHECK(frame.code == script_return_code + 2 && frame.end_of_script_hits == 0);
    CHECK(sink.records.size() == 2 && sink.records[0].phase == Phase::pre && sink.records[1].phase == Phase::post);
    CHECK(Rig::unhook(function, hooked.entry).ok());
    CHECK(function.script == bytes({0x04, 0x0B, 0x53}));
}

void b_redirects_code_to_the_return_buffer()
{
    FakeFunction function{0x20, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    FakeFrame frame;
    frame.node = &function;
    frame.script = function.script.data();
    frame.code = frame.script + 2; // just past B, as Step leaves it
    Rig::exit_handler(nullptr, frame, nullptr);
    CHECK(frame.code == script_return_code);
    CHECK(script_return_code[0] == ex_return && script_return_code[1] == ex_nothing);
    CHECK(Rig::unhook(function, hooked.entry).ok());
}

void inert_entry_keeps_racing_frames_correct()
{
    sink.clear();
    FakeFunction function{0x20, 0x20, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());
    CHECK(Rig::unhook(function, hooked.entry).ok());

    // A frame that read A before the unhook: the inert entry still dispatches
    // the saved opcode, without events.
    FakeFrame frame;
    frame.node = &function;
    frame.script = function.script.data();
    frame.code = frame.script + 1;
    Rig::entry_handler(nullptr, frame, nullptr);
    CHECK(frame.counter == 1 && frame.undefined_hits == 0);
    // ... and one that read B still returns.
    Rig::exit_handler(nullptr, frame, nullptr);
    CHECK(frame.code == script_return_code);
    CHECK(sink.records.empty());

    // Rehook recomputes the patch from the restored bytes.
    const auto again = Rig::hook(function, slot_a, slot_b);
    CHECK(again.ok() && again.entry == hooked.entry);
    FakeFrame rerun;
    run_script(Rig::natives.table.data(), function, rerun);
    CHECK(rerun.counter == 2 && sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);
    CHECK(Rig::unhook(function, hooked.entry).ok());
}

void off_thread_function_keeps_its_exit_opcode()
{
    sink.clear();
    FakeFunction function{0x20, 0x20, 0x04, 0x0B, 0x53};
    DeleteNotice function_deleted{Rig::table, function};
    const auto original = function.script;
    const auto hooked = Rig::hook(function, slot_a, slot_b);
    CHECK(hooked.ok());

    // Unhooked before any other thread ran it: byte-identical.
    CHECK(Rig::unhook(function, hooked.entry).ok());
    CHECK(function.script == original);

    // Once it ran on another thread, B stays over EX_Return on unhook.
    CHECK(Rig::hook(function, slot_a, slot_b).ok());
    uint32_t worker_counter{};
    std::thread worker([&] {
        FakeFrame frame;
        run_script(Rig::natives.table.data(), function, frame);
        worker_counter = frame.counter;
    });
    worker.join();
    CHECK(worker_counter == 2);
    CHECK(Rig::unhook(function, hooked.entry).ok());
    CHECK(function.script[0] == 0x20 && function.script[2] == slot_b);
    {
        auto reader = Rig::table.read();
        const auto* entry = reader.entry(hooked.entry);
        CHECK(entry->exit_kept && entry->state == EntryState::inert && !entry->reason.empty());
    }
    // The kept B on an inert entry returns normally and reports nothing.
    sink.clear();
    FakeFrame frame;
    run_script(Rig::natives.table.data(), function, frame);
    CHECK(frame.counter == 2 && frame.end_of_script_hits == 0 && sink.records.empty());

    // Rehook accepts the kept B; unhook keeps it again.
    CHECK(Rig::hook(function, slot_a, slot_b).ok());
    CHECK(function.script[0] == slot_a && function.script[2] == slot_b);
    FakeFrame hooked_frame;
    run_script(Rig::natives.table.data(), function, hooked_frame);
    CHECK(hooked_frame.counter == 2 && sink.count(Phase::pre) == 1 && sink.count(Phase::post) == 1);
    CHECK(Rig::unhook(function, hooked.entry).ok());
    CHECK(function.script[0] == 0x20 && function.script[2] == slot_b);

    // Without the entry's record, a B in the tail is refused as already patched.
    FakeFunction copy{0x20, 0x20, 0x00, 0x0B, 0x53};
    DeleteNotice copy_deleted{Rig::table, copy};
    copy.script[2] = slot_b;
    CHECK(!Rig::hook(copy, slot_a, slot_b).ok());
}

void unknown_frames()
{
    sink.clear();
    const auto unknown_before = Rig::context.unknown_nodes.load();
    // Our opcodes in a function we never patched (another user of the value).
    FakeFunction stranger{0x00, 0x20, 0x00, 0x0B, 0x53};
    DeleteNotice stranger_deleted{Rig::table, stranger};
    stranger.script[0] = slot_a;
    stranger.script[2] = slot_b;
    FakeFrame frame;
    run_script(Rig::natives.table.data(), stranger, frame);
    CHECK(frame.undefined_hits == 1);   // A ran execUndefined (a script error, not a crash)
    CHECK(frame.counter == 1);
    CHECK(frame.end_of_script_hits == 0); // B still returned through the buffer
    CHECK(Rig::context.unknown_nodes.load() == unknown_before + 2);
    CHECK(sink.records.empty());
}

void refused_hooks_write_nothing()
{
    FakeFunction native{0x20, 0x04, 0x0B, 0x53};
    DeleteNotice native_deleted{Rig::table, native};
    native.flags = func_native;
    const auto before = native.script;
    const auto refused = Rig::hook(native, slot_a, slot_b);
    CHECK(!refused.ok() && native.script == before);
    FakeFunction bad_tail{0x20, 0x04, 0x0B};
    DeleteNotice bad_tail_deleted{Rig::table, bad_tail};
    CHECK(!Rig::hook(bad_tail, slot_a, slot_b).ok());
    // No entry was created (an orphaned one from an earlier test's function at
    // the same address may exist; it is never activated by a refusal).
    auto reader = Rig::table.read();
    for (const void* function : {static_cast<const void*>(&native), static_cast<const void*>(&bad_tail)})
    {
        const auto* entry = reader.find(function);
        CHECK(entry == nullptr || entry->state == EntryState::orphaned);
    }
}
}

int main()
{
    const auto claim = Rig::claim();
    CHECK(claim.slots.has_value());
    if (!claim.slots) return LexTests::finish("ScriptPatchTests");
    slot_a = claim.slots->a;
    slot_b = claim.slots->b;
    CHECK(slot_a == 0xFF && slot_b == 0xFE);
    CHECK(Rig::natives.table[slot_a] == &Rig::entry_handler && Rig::natives.table[slot_b] == &Rig::exit_handler);
    Rig::context.set_sink(&RecordingSink::sink, &sink);

    validation_refusals();
    plan_apply_restore_round_trip();
    empty_body_patch();
    straight_body_reports_pre_and_post();
    loop_back_to_offset_zero_is_one_call();
    early_return_jump_reaches_b();
    empty_body_fires_pre_and_post_from_b();
    b_redirects_code_to_the_return_buffer();
    inert_entry_keeps_racing_frames_correct();
    off_thread_function_keeps_its_exit_opcode();
    unknown_frames();
    refused_hooks_write_nothing();
    return LexTests::finish("ScriptPatchTests");
}
