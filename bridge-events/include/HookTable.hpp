#pragma once

// The process-wide hook table (LEX_hooks.md §6.1) and the context the
// handlers run against.
//
// - One entry per hooked UFunction, keyed by its address. Entries are never
//   freed and never move (the table owns them through unique_ptr), so an
//   entry pointer or id stays valid for the life of the DLL.
// - Entry states: active (patched) → inert (unhooked; saved bytes or Func kept
//   so a frame that already dispatched our opcode or thunk still finds them)
//   → active again on rehook; orphaned when the UFunction object was deleted
//   (nothing is ever written back). An orphaned entry is revived as a fresh
//   entry if a new UFunction appears at the same address and is hooked.
// - std::shared_mutex: handlers take the shared lock (Reader) for a lookup,
//   the guard and the event sink; hook, unhook, subscription changes and
//   orphaning take it exclusively (Writer). No engine call that can run
//   script is ever made under either lock.
// - Capacity max_entries (1,024) distinct functions per process.
// - Subscriptions live in their entry and are never freed either: an
//   unsubscribed one is marked inactive and recycled by a later subscribe once
//   its queued count is back to zero, so a Subscription pointer taken under the
//   lock stays valid while events that count against it are queued.
//
// Lock order for callers (Track I): never take the table lock while holding
// the hook producer queue's mutex. The sink runs under the shared lock and
// takes the producer mutex; the reverse order can deadlock against a waiting
// writer.

#include <HookTypes.hpp>
#include <ScriptPatch.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UE4SSLuaEventBridge::Hooks
{
struct HookEntry
{
    uint32_t id{};                    // index in the table; HookEvent::entry
    const void* function{};           // the UFunction (table key)
    std::uintptr_t owner_class{};     // informational; set by the caller
    std::string owner_name;           // informational; set by the caller
    EntryKind kind{EntryKind::blueprint};
    EntryState state{EntryState::inert};
    ScriptPatch patch{};              // blueprint: bytes saved by the patch
    bool exit_kept{};                 // blueprint, inert: unhook left B in place (ScriptPatch.hpp)
    std::thread::id patch_thread{};   // the thread that last attached the entry
    mutable std::atomic_bool off_thread{}; // a handler ran on another thread since the entry was created
    NativeFuncPtr saved_func{};       // native: Func before the thunk was installed
    bool thunk_in_chain{};            // native: unhook found another hooker on top, so the thunk stays in its chain
    std::shared_ptr<const Schema> schema; // set by the caller at attach; immutable once set
    std::deque<Subscription> subscriptions;
    std::string reason;               // last restore problem, for diagnostics
};

class HookTable
{
public:
    class Reader
    {
    public:
        // Any state; nullptr when the function was never hooked.
        const HookEntry* find(const void* function) const
        {
            const auto found = table_->index_.find(function);
            return found == table_->index_.end() ? nullptr : table_->entries_[found->second].get();
        }

        const HookEntry* entry(uint32_t id) const
        {
            return id < table_->entries_.size() ? table_->entries_[id].get() : nullptr;
        }

        std::size_t size() const { return table_->entries_.size(); }

    private:
        friend class HookTable;
        explicit Reader(const HookTable& table) : lock_(table.mutex_), table_(&table) {}
        std::shared_lock<std::shared_mutex> lock_;
        const HookTable* table_;
    };

    class Writer
    {
    public:
        HookEntry* find(const void* function)
        {
            const auto found = table_->index_.find(function);
            return found == table_->index_.end() ? nullptr : table_->entries_[found->second].get();
        }

        HookEntry* entry(uint32_t id)
        {
            return id < table_->entries_.size() ? table_->entries_[id].get() : nullptr;
        }

        std::size_t size() const { return table_->entries_.size(); }

        struct Acquired
        {
            HookEntry* entry{};
            const char* refusal{};
        };

        // The entry for `function`, created when new. An orphaned entry, or an
        // inert one being taken for the other kind, is reset to a fresh entry
        // (its subscriptions are kept). Refuses an active entry of the other
        // kind, a native entry whose thunk is still in another hooker's chain,
        // and a full table.
        Acquired acquire(const void* function, EntryKind kind)
        {
            if (auto* existing = find(function))
            {
                if (existing->kind == kind && existing->state != EntryState::orphaned) return {existing, nullptr};
                if (existing->state == EntryState::active) return {nullptr, "function is hooked as the other kind"};
                if (existing->state == EntryState::inert && existing->thunk_in_chain)
                    return {nullptr, "function's native thunk is still in another hooker's chain"};
                reset(*existing, kind);
                return {existing, nullptr};
            }
            if (table_->entries_.size() >= max_entries) return {nullptr, "hook table is full"};
            auto created = std::make_unique<HookEntry>();
            created->id = static_cast<uint32_t>(table_->entries_.size());
            created->function = function;
            created->kind = kind;
            auto* raw = created.get();
            table_->entries_.push_back(std::move(created));
            table_->index_.emplace(function, raw->id);
            table_->mark_seen(function);
            table_->count_.store(table_->entries_.size(), std::memory_order_release);
            return {raw, nullptr};
        }

        // A fresh or recycled subscription, active, counters zeroed; the
        // caller fills phases, filter, fields and limits under this writer.
        Subscription* subscribe(uint32_t id, uint64_t session, uint64_t hook)
        {
            auto* target = entry(id);
            if (target == nullptr) return nullptr;
            Subscription* slot = nullptr;
            for (auto& subscription : target->subscriptions)
            {
                if (!subscription.active.load(std::memory_order_acquire) &&
                    subscription.queued.load(std::memory_order_acquire) == 0)
                {
                    slot = &subscription;
                    break;
                }
            }
            if (slot == nullptr) slot = &target->subscriptions.emplace_back();
            slot->session = session;
            slot->hook = hook;
            slot->phases = 0;
            slot->filter = Filter{};
            slot->fields.clear();
            slot->object_paths = false;
            slot->max_queued = default_max_queued_per_hook;
            slot->queued.store(0, std::memory_order_relaxed);
            slot->counters.calls.store(0, std::memory_order_relaxed);
            slot->counters.matched.store(0, std::memory_order_relaxed);
            slot->counters.delivered.store(0, std::memory_order_relaxed);
            slot->counters.dropped.store(0, std::memory_order_relaxed);
            slot->counters.reentries.store(0, std::memory_order_relaxed);
            slot->active.store(true, std::memory_order_release);
            return slot;
        }

        // The active subscription for (session, hook) on the entry.
        Subscription* subscription(uint32_t id, uint64_t session, uint64_t hook)
        {
            auto* target = entry(id);
            if (target == nullptr) return nullptr;
            for (auto& subscription : target->subscriptions)
            {
                if (subscription.active.load(std::memory_order_acquire) && subscription.session == session &&
                    subscription.hook == hook)
                    return &subscription;
            }
            return nullptr;
        }

        bool unsubscribe(uint32_t id, uint64_t session, uint64_t hook)
        {
            auto* subscription = this->subscription(id, session, hook);
            if (subscription == nullptr) return false;
            subscription->active.store(false, std::memory_order_release);
            return true;
        }

        // Deactivates every subscription of the session; returns the ids of the
        // active entries it left without any active subscription (to unhook).
        std::vector<uint32_t> unsubscribe_session(uint64_t session)
        {
            std::vector<uint32_t> idle;
            for (auto& owned : table_->entries_)
            {
                bool touched = false;
                for (auto& subscription : owned->subscriptions)
                {
                    if (subscription.session != session || !subscription.active.load(std::memory_order_acquire))
                        continue;
                    subscription.active.store(false, std::memory_order_release);
                    touched = true;
                }
                if (touched && owned->state == EntryState::active && active_subscriptions(owned->id) == 0)
                    idle.push_back(owned->id);
            }
            return idle;
        }

        std::size_t active_subscriptions(uint32_t id)
        {
            auto* target = entry(id);
            if (target == nullptr) return 0;
            std::size_t count = 0;
            for (const auto& subscription : target->subscriptions)
                count += subscription.active.load(std::memory_order_acquire) ? 1 : 0;
            return count;
        }

        // Active entries without an active subscription (a session stopped off
        // the game thread; the next game-thread pass unhooks them).
        std::vector<uint32_t> idle_entries()
        {
            std::vector<uint32_t> idle;
            for (auto& owned : table_->entries_)
                if (owned->state == EntryState::active && active_subscriptions(owned->id) == 0) idle.push_back(owned->id);
            return idle;
        }

        // The UFunction was deleted: never write to it again.
        std::optional<uint32_t> orphan(const void* function)
        {
            auto* target = find(function);
            if (target == nullptr || target->state == EntryState::orphaned) return std::nullopt;
            target->state = EntryState::orphaned;
            return target->id;
        }

    private:
        friend class HookTable;
        explicit Writer(HookTable& table) : lock_(table.mutex_), table_(&table) {}

        static void reset(HookEntry& entry, EntryKind kind)
        {
            entry.kind = kind;
            entry.state = EntryState::inert;
            entry.patch = ScriptPatch{};
            entry.exit_kept = false;
            entry.patch_thread = std::thread::id{};
            entry.off_thread.store(false, std::memory_order_relaxed);
            entry.saved_func = nullptr;
            entry.thunk_in_chain = false;
            entry.schema.reset();
            entry.reason.clear();
        }

        std::unique_lock<std::shared_mutex> lock_;
        HookTable* table_;
    };

    Reader read() const { return Reader(*this); }
    Writer write() { return Writer(*this); }

    // Lock-free prefilter for the object-delete listener: false means the
    // address was never hooked. Bits are only ever set (entries never leave).
    bool maybe_hooked(const void* function) const noexcept
    {
        if (count_.load(std::memory_order_acquire) == 0) return false;
        const auto [word, bit] = seen_position(function);
        return (seen_[word].load(std::memory_order_acquire) & bit) != 0;
    }

    // NotifyUObjectDeleted fast path: orphans the entry of a deleted hooked
    // function and returns its id.
    std::optional<uint32_t> on_deleted(const void* object)
    {
        if (!maybe_hooked(object)) return std::nullopt;
        return write().orphan(object);
    }

    std::size_t size() const noexcept { return count_.load(std::memory_order_acquire); }

private:
    static constexpr std::size_t seen_words = 64; // 4,096 bits

    static std::pair<std::size_t, uint64_t> seen_position(const void* function) noexcept
    {
        auto bits = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(function));
        bits ^= bits >> 29;
        bits *= 0x9E3779B97F4A7C15ULL;
        const auto index = static_cast<std::size_t>(bits >> 52); // 12 bits
        return {index / 64, uint64_t{1} << (index % 64)};
    }

    void mark_seen(const void* function) noexcept
    {
        const auto [word, bit] = seen_position(function);
        seen_[word].fetch_or(bit, std::memory_order_release);
    }

    mutable std::shared_mutex mutex_;
    std::vector<std::unique_ptr<HookEntry>> entries_;
    std::unordered_map<const void*, uint32_t> index_;
    std::array<std::atomic<uint64_t>, seen_words> seen_{};
    std::atomic<std::size_t> count_{0};
};

// One hooked call, handed to the event sink.
template <typename Frame>
struct HookCall
{
    const HookEntry& entry;    // subscriptions, schema; valid for the sink call only (shared lock held)
    Phase phase;
    RC::Unreal::UObject* object; // the exec handler's Context argument
    Frame& frame;              // Locals, OutParms and Object through the frame's accessors
    void* result;              // native post: where Func wrote the return value (may be null); otherwise null
    bool params_available;     // false on the Blueprint → native path (§4.2)
};

// What the handlers run against: the table, the GNatives-like table A
// dispatches through, the free-slot handler for unknown frames, and the event
// sink. natives, undefined and the sink are written under the table's writer
// lock (configure, set_sink) and read under its reader lock.
template <typename FrameT>
class HookContext
{
public:
    using Frame = FrameT;
    using Exec = void (*)(RC::Unreal::UObject*, Frame&, void*);
    // Runs on the hooked call's thread under the table's shared lock: memory
    // reads and an enqueue only; no Lua, no hook or unhook, no table writes.
    using Sink = void (*)(const HookCall<Frame>& call, void* user);

    explicit HookContext(HookTable& hook_table) : table(hook_table) {}

    void configure(const Exec* gnatives, Exec free_slot_handler)
    {
        auto writer = table.write();
        natives = gnatives;
        undefined = free_slot_handler;
    }

    void set_sink(Sink event_sink, void* user)
    {
        auto writer = table.write();
        sink = event_sink;
        sink_user = user;
    }

    // Under the reader lock. A throwing sink is counted, never propagated into
    // the engine's frames.
    void emit(const HookEntry& entry, Phase phase, RC::Unreal::UObject* object, Frame& frame, void* result,
        bool params_available) noexcept
    {
        if (sink == nullptr) return;
        try
        {
            sink(HookCall<Frame>{entry, phase, object, frame, result, params_available}, sink_user);
        }
        catch (...)
        {
            sink_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Under the reader lock: a loop-back to Script[0] counts on every active
    // subscription of the entry (Status().reentries).
    void count_reentry(const HookEntry& entry) noexcept
    {
        for (const auto& subscription : entry.subscriptions)
        {
            if (subscription.active.load(std::memory_order_acquire))
                const_cast<Subscription&>(subscription).counters.reentries.fetch_add(1, std::memory_order_relaxed);
        }
    }

    HookTable& table;
    const Exec* natives{};
    Exec undefined{};
    Sink sink{};
    void* sink_user{};
    std::atomic<uint64_t> unknown_nodes{0}; // A or B ran for a frame whose Node is not a Blueprint entry
    std::atomic<uint64_t> sink_faults{0};
    std::atomic<uint64_t> thunk_misses{0};  // the native thunk found no entry (nothing was called)
};

struct AttachResult
{
    uint32_t entry{};
    const char* refusal{}; // nullptr on success
    bool ok() const noexcept { return refusal == nullptr; }
};

// Hooks a Blueprint function: plan against its current Script, take the entry,
// then write B and A. Idempotent for an active Blueprint entry. Nothing is
// written and no entry is created when the plan is refused.
inline AttachResult attach_blueprint(HookTable::Writer& writer, const void* function, std::span<uint8_t> script,
    uint32_t function_flags, uint8_t slot_a, uint8_t slot_b)
{
    if (auto* existing = writer.find(function); existing != nullptr && existing->state == EntryState::active)
    {
        if (existing->kind == EntryKind::blueprint) return {existing->id, nullptr};
        return {existing->id, "function is hooked as native"};
    }
    const auto* known = writer.find(function);
    const bool exit_kept = known != nullptr && known->kind == EntryKind::blueprint &&
        known->state == EntryState::inert && known->exit_kept;
    const auto plan = plan_script_patch(script, function_flags, slot_a, slot_b, exit_kept);
    if (plan.refusal != ScriptRefusal::none) return {0, describe(plan.refusal)};
    const auto acquired = writer.acquire(function, EntryKind::blueprint);
    if (acquired.entry == nullptr) return {0, acquired.refusal};
    auto& entry = *acquired.entry;
    entry.patch = plan.patch;
    entry.exit_kept = false;
    if (entry.patch_thread != std::this_thread::get_id())
    {
        if (entry.patch_thread != std::thread::id{}) entry.off_thread.store(true, std::memory_order_relaxed);
        entry.patch_thread = std::this_thread::get_id();
    }
    entry.reason.clear();
    entry.state = EntryState::active;
    apply_script_patch(entry.patch);
    return {entry.id, nullptr};
}

// Unhooks a Blueprint entry against the function's current Script. The entry
// goes inert whatever the restore found; a resized Script or a foreign byte is
// recorded in reason and returned as the refusal text. B is kept (success,
// with a reason) when the function ran on another thread than the one that
// patched it, or when unhooking from another thread (ScriptPatch.hpp).
inline AttachResult detach_blueprint(HookTable::Writer& writer, uint32_t id, std::span<uint8_t> current_script)
{
    auto* entry = writer.entry(id);
    if (entry == nullptr) return {id, "unknown hook entry"};
    if (entry->kind != EntryKind::blueprint) return {id, "entry is not a Blueprint hook"};
    if (entry->state != EntryState::active) return {id, nullptr};
    const bool keep_exit = entry->off_thread.load(std::memory_order_relaxed) ||
        std::this_thread::get_id() != entry->patch_thread;
    const auto restored = restore_script_patch(entry->patch, current_script, keep_exit);
    entry->state = EntryState::inert;
    switch (restored)
    {
    case ScriptRestore::restored: entry->reason.clear(); return {id, nullptr};
    case ScriptRestore::exit_kept:
        entry->exit_kept = true;
        entry->reason = "exit opcode left over EX_Return: the function also runs on other threads";
        return {id, nullptr};
    case ScriptRestore::resized: entry->reason = "bytecode changed length; nothing restored"; break;
    case ScriptRestore::foreign: entry->reason = "a patched byte was overwritten by someone else; left as found"; break;
    }
    return {id, entry->reason.c_str()};
}

// The interface src/HookCore.cpp (Windows only) gives Track I. It binds the
// headers above to GNatives_Internal, UStruct::GetScript, UFunction::
// GetFuncPtr/SetFuncPtr/GetFunctionFlags and the FFrame accessors. Everything
// here runs on the game thread except on_function_deleted (object-delete
// listener, any thread) and status.
namespace Core
{
struct Outcome
{
    bool ok{};
    uint32_t entry{};   // valid when ok, and for unhook
    std::string reason; // why not, or a restore problem on a successful unhook
};

struct CoreStatus
{
    bool blueprint_ready{};      // opcode slots claimed
    std::string blueprint_reason; // why Blueprint hooks are unavailable (empty before the first attempt)
    int32_t slot_a{-1};
    int32_t slot_b{-1};
    uint32_t slot_collisions{};
    std::size_t entries{};
    uint64_t unknown_nodes{};
    uint64_t guard_overflows{};
    uint64_t sink_faults{};
    uint64_t thunk_misses{};
};

using EventSink = void (*)(const HookCall<RC::Unreal::FFrame>& call, void* user);

// Claims the opcode slots on first use. Idempotent per function.
Outcome hook_blueprint(RC::Unreal::UFunction* function);
// Func swap with the shared thunk. Idempotent per function.
Outcome hook_native(RC::Unreal::UFunction* function);
// Restores bytes or Func; the entry goes inert. Already inert or orphaned: ok.
Outcome unhook(uint32_t entry);
// Every active entry; returns how many were unhooked.
std::size_t unhook_all();
// From NotifyUObjectDeleted: orphans a hooked function; returns its entry id.
std::optional<uint32_t> on_function_deleted(const void* object);
void set_event_sink(EventSink sink, void* user);
HookTable& table();
CoreStatus status();
}
}
