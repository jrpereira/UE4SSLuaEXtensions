#pragma once

// Native function hooks: the UFunction::Func swap and the shared thunk
// (LEX_hooks.md §4.2).
//
// Assumptions:
// - Every engine path that runs a native UFunction goes through
//   UFunction::Invoke, which sets Stack.CurrentNativeFunction to the function
//   (a scoped guard) before calling Func. The thunk is shared by every hooked
//   native function and finds its entry there; UE4SS's own UFunction hooks rely
//   on the same field. If that lookup misses, Stack.Node is tried (equal to the
//   function on the ProcessEvent path); if both miss (or the entry is
//   orphaned), the thunk calls nothing and counts a miss, since it cannot know
//   which Func to run.
// - Stack.Node == the function means the call came through ProcessEvent and
//   Locals is the parameter block (params available, pre and post).
//   Otherwise a Blueprint caller's frame is passing parameters as unevaluated
//   bytecode: pre carries no parameters, post only the Result pointer.
// - Another hooker (UE4SS RegisterHook) may swap Func before or after us. The
//   thunk always calls the Func it saved. Unhook restores only if Func is still
//   the thunk; otherwise the thunk is inside the other hooker's chain, so the
//   entry goes inert with thunk_in_chain set and the thunk passes through for
//   good. A later rehook of such an entry only reactivates it: installing again
//   would make the chain call the thunk twice and recurse.
// - Func is read and written through injected accessors (UE4SS's exported
//   GetFuncPtr/SetFuncPtr in src/HookCore.cpp, a fake in tests).

#include <HookTable.hpp>

#include <atomic>
#include <cstdint>

namespace UE4SSLuaEventBridge::Hooks
{
// Converts between exec pointer types through void (*)(), the generic function
// pointer type, so compilers do not flag the conversion. The real frame never
// converts (Exec is NativeFuncPtr); test frames store their exec type in
// NativeFuncPtr fields and read it back unchanged.
template <typename To, typename From>
To function_cast(From from) noexcept
{
    return reinterpret_cast<To>(reinterpret_cast<void (*)()>(from));
}

// Takes (or reuses) the entry and installs the thunk. GetFunc: NativeFuncPtr();
// SetFunc: void(NativeFuncPtr).
template <typename GetFunc, typename SetFunc>
AttachResult attach_native(HookTable::Writer& writer, const void* function, uint32_t function_flags, GetFunc get_func,
    SetFunc set_func, NativeFuncPtr thunk)
{
    if (auto* existing = writer.find(function); existing != nullptr && existing->state == EntryState::active)
    {
        if (existing->kind == EntryKind::native) return {existing->id, nullptr};
        return {existing->id, "function is hooked as Blueprint"};
    }
    if ((function_flags & func_native) == 0) return {0, "function is not native (no FUNC_Native)"};
    const NativeFuncPtr current = get_func();
    if (current == nullptr) return {0, "function has no native Func"};
    const auto acquired = writer.acquire(function, EntryKind::native);
    if (acquired.entry == nullptr) return {0, acquired.refusal};
    auto& entry = *acquired.entry;
    if (entry.thunk_in_chain || current == thunk)
    {
        // The thunk is still reachable (in another hooker's chain, or put back
        // as Func by that hooker's unhook): reactivate without writing.
        if (entry.saved_func == nullptr) return {entry.id, "Func already points at the hook thunk"};
    }
    else
    {
        entry.saved_func = current;
        set_func(thunk);
    }
    entry.reason.clear();
    entry.state = EntryState::active;
    return {entry.id, nullptr};
}

template <typename GetFunc, typename SetFunc>
AttachResult detach_native(HookTable::Writer& writer, uint32_t id, GetFunc get_func, SetFunc set_func,
    NativeFuncPtr thunk)
{
    auto* entry = writer.entry(id);
    if (entry == nullptr) return {id, "unknown hook entry"};
    if (entry->kind != EntryKind::native) return {id, "entry is not a native hook"};
    if (entry->state != EntryState::active) return {id, nullptr};
    if (get_func() == thunk)
    {
        set_func(entry->saved_func);
        entry->thunk_in_chain = false;
        entry->reason.clear();
    }
    else
    {
        entry->thunk_in_chain = true;
        entry->reason = "another hooker swapped Func after us; the thunk passes through";
    }
    entry->state = EntryState::inert;
    return {id, nullptr};
}

// The thunk body. Context is HookContext<Frame>. The shared lock is released
// around the saved Func, which runs arbitrary game code.
template <typename Context>
void native_thunk(Context& context, RC::Unreal::UObject* object, typename Context::Frame& stack, void* result)
{
    using Access = FrameAccess<typename Context::Frame>;
    using Exec = typename Context::Exec;
    const void* function = Access::current_native_function(stack);
    const void* node = Access::node(stack);
    Exec saved{};
    uint32_t id{};
    bool active{};
    bool params_available{};
    {
        auto reader = context.table.read();
        const auto* entry = reader.find(function);
        if (entry == nullptr || entry->kind != EntryKind::native)
        {
            entry = reader.find(node);
            if (entry != nullptr && entry->kind != EntryKind::native) entry = nullptr;
            function = node;
        }
        // An orphaned entry described a deleted function: never call its Func.
        if (entry == nullptr || entry->saved_func == nullptr || entry->state == EntryState::orphaned)
        {
            context.thunk_misses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        saved = function_cast<Exec>(entry->saved_func);
        id = entry->id;
        params_available = node == function;
        if (entry->state == EntryState::active)
        {
            active = true;
            context.emit(*entry, Phase::pre, object, stack, nullptr, params_available);
        }
    }
    saved(object, stack, result);
    if (!active) return;
    auto reader = context.table.read();
    const auto* entry = reader.entry(id);
    if (entry != nullptr && entry->state == EntryState::active)
        context.emit(*entry, Phase::post, object, stack, result, params_available);
}
}
