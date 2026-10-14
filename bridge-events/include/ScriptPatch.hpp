#pragma once

// Blueprint function hooks: the two-byte bytecode patch and the handlers that
// run in its place (LEX_hooks.md §4.1).
//
// Bytecode assumptions (Kismet compiler output, UE4/UE5 script VM):
// - UStruct::Script is a heap TArray<uint8> holding the function's bytecode.
//   ProcessLocalScriptFunction runs `while (*Code != EX_Return) Step();`, then
//   `Code++` and one more Step for the return expression. Step dispatches
//   GNatives[*Code++](Context, Stack, Result).
// - A compiled body ends in exactly one `EX_Return EX_Nothing EX_EndOfScript`
//   (04 0B 53); early returns jump to that EX_Return. The patch refuses any
//   Script that does not end that way, so a function that breaks the
//   assumption is never patched.
// - Script[0] is an opcode (the first statement). A body that is exactly
//   04 0B 53 is "empty": only B is written and it reports pre and post.
//   EX_Return anywhere else at offset 0 is refused.
// - Both writes are single bytes in heap memory: no page protection change, no
//   torn read, unchanged length and jump offsets. B is written before A and A is
//   restored before B, so a frame that ran A always meets B unless the hook is
//   removed while the frame runs.
// - The VM reads the EX_Return position twice: the loop test, then Step. If B
//   is restored to EX_Return by another thread between those reads, Step
//   dispatches GNatives[EX_Return] and the frame runs on into EX_EndOfScript
//   (fatal outside the editor). Restoring A has no such window (either byte
//   dispatches the original opcode). So B is restored only when the function
//   has run on no thread but the one that patched and is now unhooking it;
//   otherwise it stays (keep_exit) and keeps redirecting to the return buffer,
//   which is harmless for an unhooked entry, and a rehook accepts it. A
//   function's first call on another thread can still meet the window if it
//   started before the hook and reaches its tail during the unhook (and for
//   an empty body, whose first call has no A to report the thread first).
//
// Handler A (entry, Script[0]): Stack.Code already points past the opcode byte,
// which is what the original opcode's handler expects; after the pre-hook it
// calls GNatives[saved first opcode]. Handler B (exit, the EX_Return byte):
// after the post-hook it points Stack.Code at script_return_code; the loop sees
// EX_Return, steps EX_Nothing and the function returns normally. The frame
// never needs the original byte back, so B works the same for an unhooked
// (inert) entry and for an unknown frame.
//
// Frame access: handlers read FFrame::Node and write FFrame::Code through
// FrameAccess<Frame> (src/HookCore.cpp maps it to UE4SS's exported accessors,
// tests/support/FakeFrame.hpp to a fake frame). The handler templates take a
// context (HookTable.hpp's HookContext) as a template parameter, so this header
// does not depend on the table.

#include <HookTypes.hpp>
#include <ReentryGuard.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace UE4SSLuaEventBridge::Hooks
{
// Specialised per frame type. Required members:
//   static uint8_t*& code(Frame&);                       FFrame::Code
//   static const void* node(Frame&);                     FFrame::Node
//   static const void* current_native_function(Frame&); FFrame::CurrentNativeFunction
template <typename Frame>
struct FrameAccess;

// Where handler B sends Code. Starts with {EX_Return, EX_Nothing} (§4.1); the
// trailing EX_EndOfScript mirrors a real tail, so nothing reading past
// EX_Nothing ever sees foreign memory. Never written; not const because
// FFrame::Code is a non-const pointer.
inline uint8_t script_return_code[3] = {ex_return, ex_nothing, ex_end_of_script};

enum class ScriptRefusal : uint8_t
{
    none,
    native_function,  // FUNC_Native: hook it with the Func swap instead
    no_script,        // empty or missing Script
    bad_tail,         // does not end in 04 0B 53
    return_at_entry,  // EX_Return at offset 0 of a longer body
    already_patched,  // Script[0] already holds one of our opcodes, or the tail holds B unexpectedly
    invalid_slots,    // A and B equal, or a reserved opcode
};

inline const char* describe(ScriptRefusal refusal) noexcept
{
    switch (refusal)
    {
    case ScriptRefusal::none: return "none";
    case ScriptRefusal::native_function: return "function is native (FUNC_Native)";
    case ScriptRefusal::no_script: return "function has no bytecode";
    case ScriptRefusal::bad_tail: return "bytecode does not end in EX_Return EX_Nothing EX_EndOfScript";
    case ScriptRefusal::return_at_entry: return "bytecode starts with EX_Return";
    case ScriptRefusal::already_patched: return "bytecode already carries a hook opcode";
    case ScriptRefusal::invalid_slots: return "invalid opcode slots";
    }
    return "unknown";
}

// Everything needed to apply and later restore one function's patch.
struct ScriptPatch
{
    uint8_t* data{};        // Script.GetData() when planned
    std::size_t size{};     // Script.Num() when planned
    std::size_t tail{};     // index of the EX_Return byte (size - 3)
    uint8_t saved_first{};  // original Script[0]; unused for an empty body
    uint8_t slot_a{};
    uint8_t slot_b{};
    bool empty_body{};      // Script is exactly 04 0B 53: only B is written
};

struct ScriptPlan
{
    ScriptRefusal refusal{ScriptRefusal::none};
    ScriptPatch patch{};
};

// exit_kept: an earlier unhook left B over the EX_Return byte (see keep_exit);
// the tail then reads B instead of EX_Return and is accepted as such.
inline ScriptPlan plan_script_patch(std::span<uint8_t> script, uint32_t function_flags, uint8_t slot_a,
    uint8_t slot_b, bool exit_kept = false) noexcept
{
    ScriptPlan plan;
    const auto reserved = [](uint8_t slot) {
        return slot == ex_return || slot == ex_nothing || slot == ex_end_of_script;
    };
    if (slot_a == slot_b || reserved(slot_a) || reserved(slot_b))
    {
        plan.refusal = ScriptRefusal::invalid_slots;
        return plan;
    }
    if ((function_flags & func_native) != 0)
    {
        plan.refusal = ScriptRefusal::native_function;
        return plan;
    }
    if (script.data() == nullptr || script.empty())
    {
        plan.refusal = ScriptRefusal::no_script;
        return plan;
    }
    const std::size_t size = script.size();
    if (size < 3 || script[size - 2] != ex_nothing || script[size - 1] != ex_end_of_script)
    {
        plan.refusal = ScriptRefusal::bad_tail;
        return plan;
    }
    const uint8_t tail = script[size - 3];
    if (tail != ex_return && !(exit_kept && tail == slot_b))
    {
        plan.refusal = tail == slot_b ? ScriptRefusal::already_patched : ScriptRefusal::bad_tail;
        return plan;
    }
    const bool empty_body = size == 3;
    const uint8_t first = empty_body ? ex_return : script[0];
    if (!empty_body && first == ex_return)
    {
        plan.refusal = ScriptRefusal::return_at_entry;
        return plan;
    }
    if (first == slot_a || first == slot_b)
    {
        plan.refusal = ScriptRefusal::already_patched;
        return plan;
    }
    plan.patch = ScriptPatch{script.data(), size, size - 3, first, slot_a, slot_b, empty_body};
    return plan;
}

inline void store_script_byte(uint8_t& byte, uint8_t value) noexcept
{
    std::atomic_ref<uint8_t>(byte).store(value, std::memory_order_release);
}

// B over the EX_Return byte first, then A over Script[0] (not for an empty body).
inline void apply_script_patch(const ScriptPatch& patch) noexcept
{
    store_script_byte(patch.data[patch.tail], patch.slot_b);
    if (!patch.empty_body) store_script_byte(patch.data[0], patch.slot_a);
}

enum class ScriptRestore : uint8_t
{
    restored,  // every byte we wrote is back to its original value
    exit_kept, // A restored, B deliberately left over the EX_Return byte (keep_exit)
    resized,   // Script no longer has the patched length: nothing written
    foreign,   // a patched byte no longer held our opcode: that byte left alone, the others restored
};

// Restores against the function's current Script (its buffer may have moved;
// the length and our opcodes at the patched positions are what identify it).
// A first, then B unless keep_exit. Byte-identical to the original when the
// result is restored.
inline ScriptRestore restore_script_patch(const ScriptPatch& patch, std::span<uint8_t> current,
    bool keep_exit = false) noexcept
{
    if (current.data() == nullptr || current.size() != patch.size) return ScriptRestore::resized;
    bool foreign = false;
    if (!patch.empty_body)
    {
        if (current[0] == patch.slot_a) store_script_byte(current[0], patch.saved_first);
        else foreign = true;
    }
    if (current[patch.tail] != patch.slot_b) foreign = true;
    else if (!keep_exit) store_script_byte(current[patch.tail], ex_return);
    if (foreign) return ScriptRestore::foreign;
    return keep_exit ? ScriptRestore::exit_kept : ScriptRestore::restored;
}

// Marks an entry whose function runs on a thread other than the one that
// patched it (see keep_exit). Under the table's shared lock.
template <typename Entry>
void note_thread(const Entry& entry) noexcept
{
    if (!entry.off_thread.load(std::memory_order_relaxed) && std::this_thread::get_id() != entry.patch_thread)
        entry.off_thread.store(true, std::memory_order_relaxed);
}

// Handler A body. Context is HookContext<Frame> (HookTable.hpp). The table's
// shared lock is held for the lookup, the guard and the pre-hook only, never
// while the original opcode runs: that opcode runs arbitrary game code,
// including other hooked functions and code that may hook or unhook.
template <typename Context>
void script_entry(Context& context, RC::Unreal::UObject* object, typename Context::Frame& stack, void* result)
{
    using Access = FrameAccess<typename Context::Frame>;
    typename Context::Exec next{};
    {
        auto reader = context.table.read();
        const auto* entry = reader.find(Access::node(stack));
        if (entry == nullptr || entry->kind != EntryKind::blueprint || entry->patch.empty_body ||
            entry->state == EntryState::orphaned || context.natives == nullptr)
        {
            // Not a frame we patched: another user of the opcode value, or a
            // new function at a deleted one's address. The original opcode is
            // unknowable; run the free-slot handler (execUndefined: a script
            // error, not a crash).
            context.unknown_nodes.fetch_add(1, std::memory_order_relaxed);
            next = context.undefined;
        }
        else
        {
            note_thread(*entry);
            next = context.natives[entry->patch.saved_first];
            if (entry->state == EntryState::active)
            {
                const auto frame = reinterpret_cast<std::uintptr_t>(&stack);
                if (local_reentry_guard().enter(frame, entry->id) == ReentryGuard::Enter::reentry)
                    context.count_reentry(*entry);
                else
                    context.emit(*entry, Phase::pre, object, stack, nullptr, true);
            }
        }
    }
    if (next != nullptr) next(object, stack, result);
}

// Handler B body: pops the guard, reports (pre and) post, then always redirects
// Code to the return buffer, because B is only ever written over an EX_Return.
template <typename Context>
void script_exit(Context& context, RC::Unreal::UObject* object, typename Context::Frame& stack, void*)
{
    using Access = FrameAccess<typename Context::Frame>;
    {
        auto reader = context.table.read();
        const auto* entry = reader.find(Access::node(stack));
        if (entry == nullptr || entry->kind != EntryKind::blueprint || entry->state == EntryState::orphaned)
        {
            context.unknown_nodes.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            note_thread(*entry);
            local_reentry_guard().exit(reinterpret_cast<std::uintptr_t>(&stack), entry->id);
            if (entry->state == EntryState::active)
            {
                if (entry->patch.empty_body) context.emit(*entry, Phase::pre, object, stack, nullptr, true);
                context.emit(*entry, Phase::post, object, stack, nullptr, true);
            }
        }
    }
    Access::code(stack) = script_return_code;
}
}
