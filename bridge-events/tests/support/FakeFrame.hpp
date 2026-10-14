#pragma once

// Test doubles for the hook core (Track C), reused by Track I's Windows suite:
// a fake FFrame, a fake GNatives table with a handful of opcodes, a fake
// UFunction (bytecode and Func), a script loop that mirrors
// ProcessLocalScriptFunction, and a rig that wires the real handler bodies
// (ScriptPatch.hpp, NativeThunk.hpp) to a HookContext<FakeFrame>.
//
// Fake opcodes (only meaningful in this table): fake_op_count increments
// FakeFrame::counter; fake_op_loop jumps back to offset 0 while counter <
// loop_limit (a Kismet loop whose first statement is at offset 0); EX_Jump
// (0x06) reads a 4-byte little-endian offset like Kismet's CodeSkipSizeType.

#include <HookTable.hpp>
#include <NativeThunk.hpp>
#include <OpcodeSlots.hpp>
#include <ScriptPatch.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <vector>

namespace UE4SSLuaEventBridge::Hooks::Testing
{
struct FakeFrame
{
    const void* node{};                    // FFrame::Node
    const void* current_native_function{}; // FFrame::CurrentNativeFunction
    uint8_t* code{};                       // FFrame::Code
    uint8_t* script{};                     // Node's Script data; fake jumps are relative to it
    void* locals{};                        // FFrame::Locals
    uint32_t counter{};
    uint32_t loop_limit{};
    uint32_t undefined_hits{};
    uint32_t end_of_script_hits{};
    uint32_t return_steps{};               // an EX_Return reached Step (must never happen)
};
}

namespace UE4SSLuaEventBridge::Hooks
{
template <>
struct FrameAccess<Testing::FakeFrame>
{
    static uint8_t*& code(Testing::FakeFrame& frame) { return frame.code; }
    static const void* node(Testing::FakeFrame& frame) { return frame.node; }
    static const void* current_native_function(Testing::FakeFrame& frame) { return frame.current_native_function; }
};
}

namespace UE4SSLuaEventBridge::Hooks::Testing
{
using FakeExec = void (*)(RC::Unreal::UObject*, FakeFrame&, void*);

inline constexpr uint8_t ex_jump = 0x06;
inline constexpr uint8_t fake_op_count = 0x20;
inline constexpr uint8_t fake_op_loop = 0x21;

inline uint8_t read_opcode(uint8_t* code) noexcept
{
    return std::atomic_ref<uint8_t>(*code).load(std::memory_order_acquire);
}

inline void exec_undefined(RC::Unreal::UObject*, FakeFrame& frame, void*) { ++frame.undefined_hits; }
inline void exec_nothing(RC::Unreal::UObject*, FakeFrame&, void*) {}
inline void exec_return(RC::Unreal::UObject*, FakeFrame& frame, void*) { ++frame.return_steps; }
inline void exec_end_of_script(RC::Unreal::UObject*, FakeFrame& frame, void*) { ++frame.end_of_script_hits; }
inline void exec_count(RC::Unreal::UObject*, FakeFrame& frame, void*) { ++frame.counter; }
inline void exec_loop(RC::Unreal::UObject*, FakeFrame& frame, void*)
{
    if (frame.counter < frame.loop_limit) frame.code = frame.script;
}
inline void exec_jump(RC::Unreal::UObject*, FakeFrame& frame, void*)
{
    uint32_t offset{};
    std::memcpy(&offset, frame.code, sizeof(offset));
    frame.code = frame.script + offset;
}

struct FakeNatives
{
    std::array<FakeExec, gnatives_count> table{};

    FakeNatives() { reset(); }

    void reset()
    {
        table.fill(&exec_undefined);
        table[ex_return] = &exec_return;
        table[ex_nothing] = &exec_nothing;
        table[ex_end_of_script] = &exec_end_of_script;
        table[ex_jump] = &exec_jump;
        table[fake_op_count] = &exec_count;
        table[fake_op_loop] = &exec_loop;
    }

    std::span<FakeExec, gnatives_count> span() { return std::span<FakeExec, gnatives_count>(table); }
};

// Stands in for a UFunction: the hook table keys on its address.
struct FakeFunction
{
    std::vector<uint8_t> script;
    uint32_t flags{};
    std::atomic<NativeFuncPtr> func{};

    FakeFunction() = default;
    explicit FakeFunction(std::initializer_list<uint8_t> code) : script(code) {}
    FakeFunction(const FakeFunction&) = delete;
    FakeFunction& operator=(const FakeFunction&) = delete;

    std::span<uint8_t> bytes() { return std::span<uint8_t>(script); }
    NativeFuncPtr get_func() const { return func.load(std::memory_order_acquire); }
    void set_func(NativeFuncPtr value) { func.store(value, std::memory_order_release); }
};

// Orphans a fake function's entry when it goes out of scope, as the bridge's
// NotifyUObjectDeleted listener does for a real UFunction: test functions are
// locals, so a later test's function may reuse the address.
struct DeleteNotice
{
    HookTable& table;
    const FakeFunction& function;
    ~DeleteNotice() { table.on_deleted(&function); }
};

template <typename Exec>
NativeFuncPtr as_native(Exec exec) noexcept
{
    return function_cast<NativeFuncPtr>(exec);
}

// FFrame::Step.
inline void step(const FakeExec* natives, RC::Unreal::UObject* object, FakeFrame& frame, void* result)
{
    const uint8_t opcode = read_opcode(frame.code);
    ++frame.code;
    natives[opcode](object, frame, result);
}

// ProcessLocalScriptFunction: steps until EX_Return, then steps the return
// expression that follows it.
inline void run_script(const FakeExec* natives, FakeFunction& function, FakeFrame& frame,
    RC::Unreal::UObject* object = nullptr)
{
    frame.node = &function;
    frame.script = function.script.data();
    frame.code = frame.script;
    while (read_opcode(frame.code) != ex_return) step(natives, object, frame, nullptr);
    ++frame.code;
    step(natives, object, frame, nullptr);
}

// UFunction::Invoke: sets CurrentNativeFunction around the call to Func.
inline void invoke(FakeFunction& function, RC::Unreal::UObject* object, FakeFrame& frame, void* result)
{
    const void* saved = frame.current_native_function;
    frame.current_native_function = &function;
    function_cast<FakeExec>(function.get_func())(object, frame, result);
    frame.current_native_function = saved;
}

// One sink record per emitted phase.
struct SinkRecord
{
    uint32_t entry{};
    Phase phase{};
    const void* node{};
    void* result{};
    bool params_available{};
};

struct RecordingSink
{
    std::mutex mutex;
    std::vector<SinkRecord> records;

    static void sink(const HookCall<FakeFrame>& call, void* user)
    {
        auto& self = *static_cast<RecordingSink*>(user);
        std::scoped_lock lock(self.mutex);
        self.records.push_back({call.entry.id, call.phase, call.frame.node, call.result, call.params_available});
    }

    std::size_t count(Phase phase)
    {
        std::scoped_lock lock(mutex);
        std::size_t total = 0;
        for (const auto& record : records) total += record.phase == phase ? 1 : 0;
        return total;
    }

    void clear()
    {
        std::scoped_lock lock(mutex);
        records.clear();
    }
};

// A table, a context and handlers with plain function pointers, as the real
// GNatives and Func need. One instance per Tag type.
template <typename Tag>
struct FakeHookRig
{
    static inline HookTable table{};
    static inline HookContext<FakeFrame> context{table};
    static inline FakeNatives natives{};

    static void entry_handler(RC::Unreal::UObject* object, FakeFrame& frame, void* result)
    {
        script_entry(context, object, frame, result);
    }

    static void exit_handler(RC::Unreal::UObject* object, FakeFrame& frame, void* result)
    {
        script_exit(context, object, frame, result);
    }

    static void thunk(RC::Unreal::UObject* object, FakeFrame& frame, void* result)
    {
        native_thunk(context, object, frame, result);
    }

    static NativeFuncPtr thunk_pointer() { return as_native(&thunk); }

    // Claims a slot pair in the fake GNatives and points the context at it.
    static SlotClaim<FakeExec> claim()
    {
        auto result = claim_free_pair<FakeExec>(natives.span(), &entry_handler, &exit_handler);
        if (result.slots) context.configure(natives.table.data(), result.slots->undefined);
        return result;
    }

    static AttachResult hook(FakeFunction& function, uint8_t slot_a, uint8_t slot_b)
    {
        auto writer = table.write();
        return attach_blueprint(writer, &function, function.bytes(), function.flags, slot_a, slot_b);
    }

    static AttachResult unhook(FakeFunction& function, uint32_t entry)
    {
        auto writer = table.write();
        return detach_blueprint(writer, entry, function.bytes());
    }

    static AttachResult hook_native(FakeFunction& function)
    {
        auto writer = table.write();
        return attach_native(writer, &function, function.flags, [&] { return function.get_func(); },
            [&](NativeFuncPtr value) { function.set_func(value); }, thunk_pointer());
    }

    static AttachResult unhook_native(FakeFunction& function, uint32_t entry)
    {
        auto writer = table.write();
        return detach_native(writer, entry, [&] { return function.get_func(); },
            [&](NativeFuncPtr value) { function.set_func(value); }, thunk_pointer());
    }
};
}
