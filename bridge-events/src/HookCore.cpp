// Binds the hook core (OpcodeSlots, ScriptPatch, NativeThunk, HookTable) to
// the pinned UE4SS ABI: GNatives_Internal, UStruct::GetScript, UFunction::
// GetFunctionFlags/GetFuncPtr/SetFuncPtr and the FFrame accessors. The logic
// lives in the headers; this file only supplies the real frame, the real
// table and the three handlers with plain function pointers. Interface:
// HookTable.hpp, namespace Hooks::Core (LEX_hooks.md §4, §6.1).
//
// Threads: hook_blueprint, hook_native, unhook and unhook_all are meant for
// the game thread (they write bytecode and Func); on_function_deleted runs on
// whatever thread deletes objects; the handlers run on any thread that
// executes a hooked function.
//
// Inert until used: the core's state is a function-local static created by
// the first call, and GNatives is neither read nor written before the first
// hook_blueprint.

#include <HookABI.hpp>
#include <HookTable.hpp>
#include <NativeThunk.hpp>
#include <OpcodeSlots.hpp>
#include <ReentryGuard.hpp>
#include <ScriptPatch.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace UE4SSLuaEventBridge::Hooks
{
// The real frame, through UE4SS's exported accessors (FFrame is opaque here).
template <>
struct FrameAccess<RC::Unreal::FFrame>
{
    static uint8_t*& code(RC::Unreal::FFrame& frame) { return frame.Code(); }
    static const void* node(RC::Unreal::FFrame& frame) { return frame.Node(); }
    static const void* current_native_function(RC::Unreal::FFrame& frame) { return frame.CurrentNativeFunction(); }
};
}

namespace
{
using namespace UE4SSLuaEventBridge::Hooks;
using RC::Unreal::FFrame;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;

struct CoreState
{
    HookTable table;
    HookContext<FFrame> context{table};
    std::mutex slot_mutex;
    bool slots_tried{};
    std::optional<SlotChoice<NativeFuncPtr>> slots;
    std::string slot_reason;
    uint32_t slot_collisions{};
};

CoreState& core()
{
    static CoreState state;
    return state;
}

void blueprint_entry(UObject* object, FFrame& stack, void* result)
{
    script_entry(core().context, object, stack, result);
}

void blueprint_exit(UObject* object, FFrame& stack, void* result)
{
    script_exit(core().context, object, stack, result);
}

void native_hook_thunk(UObject* object, FFrame& stack, void* result)
{
    native_thunk(core().context, object, stack, result);
}

// Claims the opcode pair once, at the first Blueprint hook. A failure is
// final for the process (the table does not change underneath us).
bool ensure_slots(std::string& reason)
{
    auto& state = core();
    std::scoped_lock lock(state.slot_mutex);
    if (state.slots) return true;
    if (state.slots_tried)
    {
        reason = state.slot_reason;
        return false;
    }
    state.slots_tried = true;
    NativeFuncPtr* natives = RC::Unreal::GNatives_Internal;
    if (natives == nullptr)
    {
        state.slot_reason = "GNatives was not found by UE4SS";
        reason = state.slot_reason;
        return false;
    }
    // The context learns the table and the free-slot pointer right after the
    // claim; no bytecode carries our opcodes before the first patch, so the
    // handlers cannot run in between.
    auto claim = claim_free_pair<NativeFuncPtr>(std::span<NativeFuncPtr, gnatives_count>(natives, gnatives_count),
        &blueprint_entry, &blueprint_exit);
    state.slot_collisions = claim.collisions;
    if (!claim.slots)
    {
        state.slot_reason = claim.reason != nullptr ? claim.reason : "no opcode slots";
        reason = state.slot_reason;
        return false;
    }
    state.context.configure(natives, claim.slots->undefined);
    state.slots = claim.slots;
    return true;
}

Core::Outcome outcome(const AttachResult& result)
{
    Core::Outcome out;
    out.ok = result.ok();
    out.entry = result.entry;
    if (result.refusal != nullptr) out.reason = result.refusal;
    return out;
}

Core::Outcome refused(std::string reason)
{
    Core::Outcome out;
    out.reason = std::move(reason);
    return out;
}

std::span<uint8_t> script_of(UFunction* function)
{
    auto& script = function->GetScript();
    if (script.data == nullptr || script.num <= 0) return {};
    return std::span<uint8_t>(script.data, static_cast<std::size_t>(script.num));
}

UFunction* function_of(const HookEntry& entry)
{
    return static_cast<UFunction*>(const_cast<void*>(entry.function));
}

AttachResult detach(HookTable::Writer& writer, HookEntry& entry)
{
    UFunction* function = function_of(entry);
    if (entry.kind == EntryKind::blueprint) return detach_blueprint(writer, entry.id, script_of(function));
    return detach_native(writer, entry.id, [function] { return function->GetFuncPtr(); },
        [function](NativeFuncPtr value) { function->SetFuncPtr(value); }, &native_hook_thunk);
}
}

namespace UE4SSLuaEventBridge::Hooks::Core
{
Outcome hook_blueprint(UFunction* function)
{
    if (function == nullptr) return refused("no function");
    std::string reason;
    if (!ensure_slots(reason)) return refused("Blueprint hooks unavailable: " + reason);
    auto& state = core();
    const uint32_t flags = function->GetFunctionFlags();
    auto writer = state.table.write();
    return outcome(attach_blueprint(writer, function, script_of(function), flags, state.slots->a, state.slots->b));
}

Outcome hook_native(UFunction* function)
{
    if (function == nullptr) return refused("no function");
    const uint32_t flags = function->GetFunctionFlags();
    auto writer = core().table.write();
    return outcome(attach_native(writer, function, flags, [function] { return function->GetFuncPtr(); },
        [function](NativeFuncPtr value) { function->SetFuncPtr(value); }, &native_hook_thunk));
}

Outcome unhook(uint32_t entry)
{
    auto writer = core().table.write();
    HookEntry* target = writer.entry(entry);
    if (target == nullptr) return refused("unknown hook entry");
    if (target->state != EntryState::active)
    {
        Outcome out;
        out.ok = true;
        out.entry = entry;
        return out;
    }
    auto out = outcome(detach(writer, *target));
    // A kept exit opcode or a pass-through thunk is a success with a note.
    if (out.ok && !target->reason.empty()) out.reason = target->reason;
    return out;
}

std::size_t unhook_all()
{
    auto writer = core().table.write();
    std::size_t count = 0;
    for (uint32_t id = 0; id < writer.size(); ++id)
    {
        HookEntry* target = writer.entry(id);
        if (target == nullptr || target->state != EntryState::active) continue;
        detach(writer, *target);
        ++count;
    }
    return count;
}

std::optional<uint32_t> on_function_deleted(const void* object)
{
    return core().table.on_deleted(object);
}

void set_event_sink(EventSink sink, void* user)
{
    core().context.set_sink(sink, user);
}

HookTable& table()
{
    return core().table;
}

CoreStatus status()
{
    auto& state = core();
    CoreStatus out;
    {
        std::scoped_lock lock(state.slot_mutex);
        out.blueprint_ready = state.slots.has_value();
        out.blueprint_reason = state.slot_reason;
        if (state.slots)
        {
            out.slot_a = state.slots->a;
            out.slot_b = state.slots->b;
        }
        out.slot_collisions = state.slot_collisions;
    }
    out.entries = state.table.size();
    out.unknown_nodes = state.context.unknown_nodes.load(std::memory_order_relaxed);
    out.guard_overflows = guard_overflow_count.load(std::memory_order_relaxed);
    out.sink_faults = state.context.sink_faults.load(std::memory_order_relaxed);
    out.thunk_misses = state.context.thunk_misses.load(std::memory_order_relaxed);
    return out;
}
}
