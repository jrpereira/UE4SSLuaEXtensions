#pragma once

// Function-hook delivery (COORDINATOR/LEX/LEX_hooks.md §5–§7): what happens
// between the hook core's handlers and the session's Lua dispatcher.
//
// - The options a hook carries (parse_hook_options) and its parameter
//   selection (select_fields), decoded at Hook() time on the game thread.
// - SubscriptionPlan: everything the sink derives once per subscription (the
//   copiers for pre, post and the Blueprint → native return-only case, the
//   out-parameter fields read through FFrame::OutParms, the flattener).
// - HookRuntime: the sink (filters, per-hook limit, copy, enqueue), the hook
//   event queue with its own capacity, budget and counters, the notices
//   (wake-up -3, pending -4, fault -5), pending wake-up names, the hook
//   records the natives look up, and the two drains (engine tick, on_update).
//
// Portable: the platform (UE4SS in BridgeMod.cpp, a fake in tests) supplies
// frame access, object reads and filters through HookPlatform.
//
// Threads and locks:
// - on_call runs on the hooked call's thread under the hook table's shared
//   lock: memory reads, a copy and an enqueue only. It takes queue_mutex_.
// - set_plan runs under the table's writer lock; plans are read under the
//   shared lock, so the plan map needs no lock of its own.
// - Lock order: table → notice_mutex_ / records_mutex_ / queue_mutex_. Nothing
//   takes the table while holding one of the runtime's mutexes.
// - drain, tick and update run on one consumer thread at a time (the game
//   thread for the tick drain, the event-loop thread for the fallback). A pass
//   is marked in flight; session stop and unload wait for it with a bounded
//   wait_for_drain, never a lock: UE4SS fires on_lua_stop while holding its
//   own Lua action mutex, which a callback inside the pass may be waiting for.

#include <HookFilters.hpp>
#include <HookTable.hpp>
#include <HookTypes.hpp>
#include <ParamCopier.hpp>
#include <SharedDispatch.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UE4SSLuaEventBridge::Hooks
{
inline constexpr std::size_t default_max_queued_hook_events = 8192;
inline constexpr uint32_t default_max_hook_events_per_pass = 64;
inline constexpr uint32_t default_max_hook_dispatch_us = 500;
inline constexpr uint32_t default_hook_checks_per_second = 120;
inline constexpr uint32_t max_hook_queued_option = 1000000;

// ------------------------------------------------------------------ options

// The packed options text of HookFunction (§9.2, contract decision 3):
// `names=a|b|c; outer=<addr>; exact=0|1; obj=<address>:<token>; paths=0|1;
// maxq=n; params=A.B|C`, plus `sync=1`, which 1.1 refuses. Unknown keys are
// ignored; a malformed known key is an error.
struct HookOptions
{
    std::vector<std::string> names;
    std::uintptr_t outer{};
    bool exact{};
    bool has_object{};
    std::uintptr_t object_address{};
    uint64_t object_token{};
    bool paths{};
    uint32_t max_queued{default_max_queued_per_hook};
    std::vector<std::string> params;
    bool sync{};
};

namespace detail
{
inline std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

template <typename T>
bool parse_number(std::string_view text, T& value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

inline bool parse_flag(std::string_view text, bool& value)
{
    if (text == "0") value = false;
    else if (text == "1") value = true;
    else return false;
    return true;
}

inline bool split_list(std::string_view text, std::vector<std::string>& out)
{
    out.clear();
    while (true)
    {
        const auto bar = text.find('|');
        const auto item = trim(text.substr(0, bar));
        if (item.empty()) return false;
        out.emplace_back(item);
        if (bar == std::string_view::npos) return true;
        text.remove_prefix(bar + 1);
    }
}
}

// Returns an error, or nothing when `out` holds the options.
inline std::optional<std::string> parse_hook_options(std::string_view text, HookOptions& out)
{
    out = HookOptions{};
    while (!text.empty())
    {
        const auto semicolon = text.find(';');
        const auto part = detail::trim(text.substr(0, semicolon));
        text = semicolon == std::string_view::npos ? std::string_view{} : text.substr(semicolon + 1);
        if (part.empty()) continue;
        const auto equals = part.find('=');
        if (equals == std::string_view::npos) return "malformed hook option: " + std::string(part);
        const auto key = detail::trim(part.substr(0, equals));
        const auto value = detail::trim(part.substr(equals + 1));
        if (key == "names")
        {
            if (!detail::split_list(value, out.names)) return std::string("names must be a |-separated list");
        }
        else if (key == "outer")
        {
            uint64_t address{};
            if (!detail::parse_number(value, address) || address == 0) return std::string("outer must be an address");
            out.outer = static_cast<std::uintptr_t>(address);
        }
        else if (key == "exact")
        {
            if (!detail::parse_flag(value, out.exact)) return std::string("exact must be 0 or 1");
        }
        else if (key == "obj")
        {
            const auto colon = value.find(':');
            uint64_t address{};
            uint64_t token{};
            if (colon == std::string_view::npos || !detail::parse_number(value.substr(0, colon), address) ||
                !detail::parse_number(value.substr(colon + 1), token) || address == 0 || token == 0)
                return std::string("obj must be <address>:<token>");
            out.has_object = true;
            out.object_address = static_cast<std::uintptr_t>(address);
            out.object_token = token;
        }
        else if (key == "paths")
        {
            if (!detail::parse_flag(value, out.paths)) return std::string("paths must be 0 or 1");
        }
        else if (key == "maxq")
        {
            if (!detail::parse_number(value, out.max_queued) || out.max_queued < 1 ||
                out.max_queued > max_hook_queued_option)
                return std::string("maxq must be an integer in 1..1000000");
        }
        else if (key == "params")
        {
            if (!detail::split_list(value, out.params)) return std::string("params must be a |-separated list");
        }
        else if (key == "sync")
        {
            if (!detail::parse_flag(value, out.sync)) return std::string("sync must be 0 or 1");
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- selection

// "Param", "Struct.Field": the names `params = {...}` uses (§5.1).
inline std::string field_path(const Schema& schema, uint16_t index)
{
    std::string path = schema.fields[index].name;
    for (uint16_t parent = schema.fields[index].parent; parent != no_parent; parent = schema.fields[parent].parent)
        path = schema.fields[parent].name + "." + path;
    return path;
}

// Schema indices for `params`. A request for an unsupported kind fails with
// the kind named; struct leaves of an unsupported kind are delivered as nil.
inline std::optional<std::string> select_fields(const Schema& schema, const std::vector<std::string>& params,
    std::vector<uint16_t>& out)
{
    out.clear();
    for (const auto& wanted : params)
    {
        std::optional<uint16_t> found;
        for (std::size_t index = 0; index < schema.fields.size() && !found; ++index)
        {
            if (field_path(schema, static_cast<uint16_t>(index)) == wanted) found = static_cast<uint16_t>(index);
        }
        if (!found) return "unknown parameter: " + wanted;
        const Field& field = schema.fields[*found];
        if (field.kind == FieldKind::unsupported)
            return "parameter " + wanted + " has an unsupported kind (" + field.class_name + ")";
        out.push_back(*found);
    }
    return std::nullopt;
}

inline uint16_t top_level_of(const Schema& schema, uint16_t index)
{
    while (schema.fields[index].parent != no_parent) index = schema.fields[index].parent;
    return index;
}

// Pre-phase copies inputs only: no return value, and no out parameter the
// flattener would name "out:" (non-const out), which Lua ignores before the call.
inline bool is_input(const Field& field)
{
    if ((field.flags & cpf_parm) == 0 || (field.flags & cpf_return_parm) != 0) return false;
    return (field.flags & cpf_out_parm) == 0 || (field.flags & cpf_const_parm) != 0;
}

inline std::vector<uint16_t> pre_selection(const Schema& schema, const std::vector<uint16_t>& selection)
{
    std::vector<uint16_t> out;
    if (selection.empty())
    {
        for (std::size_t index = 0; index < schema.fields.size(); ++index)
        {
            const Field& field = schema.fields[index];
            if (field.depth == 0 && is_input(field)) out.push_back(static_cast<uint16_t>(index));
        }
        return out;
    }
    for (const uint16_t index : selection)
        if (is_input(schema.fields[top_level_of(schema, index)])) out.push_back(index);
    return out;
}

// Post-phase copies the selection (every parameter when empty) and the return value.
inline std::vector<uint16_t> post_selection(const Schema& schema, const std::vector<uint16_t>& selection)
{
    std::vector<uint16_t> out;
    if (selection.empty())
    {
        for (std::size_t index = 0; index < schema.fields.size(); ++index)
        {
            const Field& field = schema.fields[index];
            if (field.depth == 0 && (field.flags & cpf_parm) != 0) out.push_back(static_cast<uint16_t>(index));
        }
        return out;
    }
    out = selection;
    if (schema.return_field)
    {
        bool listed = false;
        for (const uint16_t index : out) listed = listed || index == *schema.return_field;
        if (!listed) out.push_back(*schema.return_field);
    }
    return out;
}

inline std::vector<uint16_t> return_selection(const Schema& schema)
{
    return schema.return_field ? std::vector<uint16_t>{*schema.return_field} : std::vector<uint16_t>{};
}

// Everything the sink needs for one subscription. Immutable once built; the
// queue keeps it alive for the events that use it.
struct SubscriptionPlan
{
    // properties[i]: the FProperty address of schema field i (Lua's list), for
    // FindOutParamValueAddress. selection: select_fields' result.
    SubscriptionPlan(std::shared_ptr<const Schema> built, std::vector<std::uintptr_t> property_addresses,
        const std::vector<uint16_t>& selection, bool object_paths)
        : schema(std::move(built)),
          properties(std::move(property_addresses)),
          overwrite(out_fields(*schema, properties)),
          copies_pre(!pre_selection(*schema, selection).empty()),
          copies_post(!post_selection(*schema, selection).empty()),
          copies_return(schema->return_field.has_value()),
          pre(*schema, pre_selection(*schema, selection), false, object_paths),
          post(*schema, post_selection(*schema, selection), true, object_paths),
          ret(*schema, return_selection(*schema), true, object_paths),
          flattener(*schema)
    {
    }

    SubscriptionPlan(const SubscriptionPlan&) = delete;
    SubscriptionPlan& operator=(const SubscriptionPlan&) = delete;

    std::shared_ptr<const Schema> schema;
    std::vector<std::uintptr_t> properties;
    std::vector<uint16_t> overwrite; // top-level out parameters and the return value
    bool copies_pre;
    bool copies_post;
    bool copies_return;
    ParamCopier pre;
    ParamCopier post;
    ParamCopier ret;
    Flattener flattener;
    std::string note; // a diagnostic for Status().reason ("name not found: X")

private:
    static std::vector<uint16_t> out_fields(const Schema& built, const std::vector<std::uintptr_t>& addresses)
    {
        std::vector<uint16_t> out;
        for (std::size_t index = 0; index < built.fields.size() && index < addresses.size(); ++index)
        {
            const Field& field = built.fields[index];
            if (field.depth == 0 && (field.flags & cpf_out_parm) != 0 && addresses[index] != 0)
                out.push_back(static_cast<uint16_t>(index));
        }
        return out;
    }
};

// ------------------------------------------------------------------ runtime

// What the runtime needs from the engine. ObjectReader (identity, path) reads
// parameter objects defensively: a parameter may hold any pointer.
template <typename Frame>
class HookPlatform : public ObjectReader
{
public:
    virtual std::byte* locals(Frame& frame) = 0;
    virtual bool has_out_parms(Frame& frame) = 0;
    virtual const void* out_param_address(Frame& frame, std::uintptr_t property) = 0;
    // The call's object: live for the duration of the call.
    virtual bool passes(const Filter& filter, std::uintptr_t object) = 0;
    virtual ObjectIdentity context_identity(std::uintptr_t object) = 0;
    virtual bool is_game_thread() = 0;
};

struct HookNotice
{
    int64_t code{};     // message_hook_wakeup, message_hook_pending, message_hook_fault
    uint64_t session{};
    uint64_t id{};      // the pending token (-3) or the hook id (-4, -5)
    std::string reason; // -4 and -5
};

struct QueuedHookEvent
{
    HookEvent event;
    Subscription* subscription{};
    std::shared_ptr<const SubscriptionPlan> plan;
};

struct HookQueueStats
{
    std::size_t queued{};
    std::size_t high_water{};
    uint64_t rejected{};
};

enum class HookDelivery : uint8_t { undecided, tick, loop };
enum class EventOutcome : uint8_t { discarded, delivered };

// The natives' view of one Lua hook.
struct HookRecord
{
    uint64_t session{};
    uint32_t entry{};
    bool orphaned{};
};

template <typename Frame>
class HookRuntime
{
public:
    using Clock = std::chrono::steady_clock;

    explicit HookRuntime(HookPlatform<Frame>& platform, std::size_t queue_limit = default_max_queued_hook_events,
        uint32_t checks_per_second = default_hook_checks_per_second)
        : platform_(platform), schedule_(checks_per_second)
    {
        capacity_.limit = queue_limit;
    }

    HookRuntime(const HookRuntime&) = delete;
    HookRuntime& operator=(const HookRuntime&) = delete;

    // HookContext sink: `user` is the runtime.
    static void sink(const HookCall<Frame>& call, void* user) { static_cast<HookRuntime*>(user)->on_call(call); }

    // ---------------------------------------------------------- plans

    // Caller holds the table's writer lock.
    void set_plan(const Subscription* subscription, std::shared_ptr<const SubscriptionPlan> plan)
    {
        plans_[subscription] = std::move(plan);
    }

    // Caller holds the table's reader or writer lock.
    const std::shared_ptr<const SubscriptionPlan>* plan_of(const Subscription* subscription) const
    {
        const auto found = plans_.find(subscription);
        return found == plans_.end() ? nullptr : &found->second;
    }

    // ----------------------------------------------------------- sink

    void on_call(const HookCall<Frame>& call)
    {
        if (inert_.load(std::memory_order_acquire)) return;
        const auto object = reinterpret_cast<std::uintptr_t>(call.object);
        const auto phase_bit = static_cast<uint8_t>(call.phase);
        std::optional<ObjectIdentity> identity;
        std::optional<bool> game_thread;
        for (const auto& item : call.entry.subscriptions)
        {
            auto& subscription = const_cast<Subscription&>(item); // counters and queued are atomics
            if (!subscription.active.load(std::memory_order_acquire) || (subscription.phases & phase_bit) == 0) continue;
            // calls and matched count each call once: in its first subscribed phase.
            const bool first = call.phase == Phase::pre ||
                (subscription.phases & static_cast<uint8_t>(Phase::pre)) == 0;
            if (first) subscription.counters.calls.fetch_add(1, std::memory_order_relaxed);
            if (!platform_.passes(subscription.filter, object)) continue;
            if (first) subscription.counters.matched.fetch_add(1, std::memory_order_relaxed);
            const auto* plan = plan_of(&subscription);
            if (plan == nullptr || !*plan) continue;
            if (subscription.queued.load(std::memory_order_acquire) >= subscription.max_queued)
            {
                subscription.counters.dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!identity) identity = platform_.context_identity(object);
            if (!game_thread) game_thread = platform_.is_game_thread();

            QueuedHookEvent queued;
            queued.event.payload = take_buffer();
            fill_payload(call, **plan, queued.event.payload);
            queued.event.entry = call.entry.id;
            queued.event.session = subscription.session;
            queued.event.hook = subscription.hook;
            queued.event.phase = call.phase;
            queued.event.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            queued.event.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count();
            queued.event.game_thread = *game_thread;
            queued.event.params_available = call.params_available;
            queued.event.object = *identity;
            queued.event.object.address = object;
            if (subscription.object_paths && object != 0) queued.event.object_path = platform_.path(object);
            queued.subscription = &subscription;
            queued.plan = *plan;
            enqueue(std::move(queued));
        }
    }

    // ---------------------------------------------------------- queue

    std::vector<QueuedHookEvent> take_events()
    {
        std::scoped_lock lock(queue_mutex_);
        ready_.drained();
        return drain_queue(queue_, spare_);
    }

    HookQueueStats stats() const
    {
        std::scoped_lock lock(queue_mutex_);
        return {backlog_count_.total(queue_.size()), capacity_.high_water, capacity_.rejected};
    }

    // -------------------------------------------------------- notices

    void notify(HookNotice notice)
    {
        std::scoped_lock lock(notice_mutex_);
        notices_.push_back(std::move(notice));
        notices_ready_.store(true, std::memory_order_release);
    }

    std::vector<HookNotice> take_notices()
    {
        if (!notices_ready_.load(std::memory_order_acquire)) return {};
        std::scoped_lock lock(notice_mutex_);
        notices_ready_.store(false, std::memory_order_release);
        wakeups_queued_.clear();
        return std::exchange(notices_, {});
    }

    // -------------------------------------------------- pending names

    // A pending hook's owner-class short name, as an FName comparison index.
    void add_pending_name(uint64_t session, uint64_t token, uint32_t name_index)
    {
        std::scoped_lock lock(notice_mutex_);
        const auto key = std::make_pair(session, token);
        if (const auto found = pending_by_token_.find(key); found != pending_by_token_.end())
            remove_name_locked(found->second, key);
        pending_by_token_[key] = name_index;
        pending_by_name_[name_index].push_back(key);
        name_bits_[(name_index >> 6) % name_bits_.size()].fetch_or(uint64_t{1} << (name_index & 63),
            std::memory_order_release);
        pending_count_.store(pending_by_token_.size(), std::memory_order_release);
    }

    bool forget_pending_name(uint64_t session, uint64_t token)
    {
        std::scoped_lock lock(notice_mutex_);
        const auto key = std::make_pair(session, token);
        const auto found = pending_by_token_.find(key);
        if (found == pending_by_token_.end()) return false;
        remove_name_locked(found->second, key);
        pending_by_token_.erase(found);
        pending_count_.store(pending_by_token_.size(), std::memory_order_release);
        return true;
    }

    void forget_session(uint64_t session)
    {
        std::scoped_lock lock(notice_mutex_);
        for (auto it = pending_by_token_.begin(); it != pending_by_token_.end();)
        {
            if (it->first.first != session)
            {
                ++it;
                continue;
            }
            remove_name_locked(it->second, it->first);
            it = pending_by_token_.erase(it);
        }
        pending_count_.store(pending_by_token_.size(), std::memory_order_release);
    }

    std::size_t pending_names() const { return pending_count_.load(std::memory_order_acquire); }

    // NotifyUObjectCreated fast path: the created object's name index.
    void on_object_created(uint32_t name_index)
    {
        if (pending_count_.load(std::memory_order_acquire) == 0) return;
        const uint64_t bits = name_bits_[(name_index >> 6) % name_bits_.size()].load(std::memory_order_acquire);
        if ((bits & (uint64_t{1} << (name_index & 63))) == 0) return;
        std::scoped_lock lock(notice_mutex_);
        const auto found = pending_by_name_.find(name_index);
        if (found == pending_by_name_.end()) return;
        for (const auto& key : found->second)
        {
            if (!wakeups_queued_.insert(key).second) continue;
            notices_.push_back(HookNotice{message_hook_wakeup, key.first, key.second, {}});
            notices_ready_.store(true, std::memory_order_release);
        }
    }

    // ------------------------------------------------------- records

    uint64_t next_hook_id() { return next_hook_.fetch_add(1, std::memory_order_relaxed) + 1; }

    void add_record(uint64_t hook, HookRecord value)
    {
        std::scoped_lock lock(records_mutex_);
        records_[hook] = value;
    }

    std::optional<HookRecord> record(uint64_t hook) const
    {
        std::scoped_lock lock(records_mutex_);
        const auto found = records_.find(hook);
        if (found == records_.end()) return std::nullopt;
        return found->second;
    }

    bool erase_record(uint64_t hook)
    {
        std::scoped_lock lock(records_mutex_);
        return records_.erase(hook) != 0;
    }

    std::vector<uint64_t> session_hooks(uint64_t session) const
    {
        std::scoped_lock lock(records_mutex_);
        std::vector<uint64_t> out;
        for (const auto& [hook, item] : records_)
            if (item.session == session) out.push_back(hook);
        return out;
    }

    // After Core::on_function_deleted orphaned `entry`: its subscriptions go
    // inactive, their records orphaned, and each session gets a -4 notice.
    std::size_t on_orphaned(HookTable& table, uint32_t entry)
    {
        std::vector<std::pair<uint64_t, uint64_t>> lost;
        {
            auto writer = table.write();
            auto* target = writer.entry(entry);
            if (target == nullptr) return 0;
            for (auto& subscription : target->subscriptions)
            {
                if (!subscription.active.load(std::memory_order_acquire)) continue;
                lost.emplace_back(subscription.session, subscription.hook);
                subscription.active.store(false, std::memory_order_release);
            }
        }
        {
            std::scoped_lock lock(records_mutex_);
            for (const auto& item : lost)
                if (auto found = records_.find(item.second); found != records_.end()) found->second.orphaned = true;
        }
        for (const auto& [session, hook] : lost)
            notify(HookNotice{message_hook_pending, session, hook, "function was deleted"});
        return lost.size();
    }

    // ------------------------------------------------------- drains

    // Notices first, then events within the budget. deliver_event returns
    // whether the event reached Lua; deliver_notice is called for each notice.
    // Consumer thread only.
    template <typename DeliverEvent, typename DeliverNotice, typename Now>
    std::size_t drain(const DispatchBudget& budget, DeliverEvent deliver_event, DeliverNotice deliver_notice, Now now)
    {
        for (auto& notice : take_notices()) deliver_notice(notice);
        if (backlog_.empty())
        {
            auto spent = backlog_.release_buffer();
            spent.clear();
            {
                std::scoped_lock lock(queue_mutex_);
                retain_empty_buffer(spare_, spent);
            }
            backlog_.load(take_events());
        }
        return dispatch_budgeted(backlog_, budget, [&](QueuedHookEvent queued) {
            backlog_count_.remaining(backlog_.size());
            bool attempted = false;
            auto* subscription = queued.subscription;
            if (subscription != nullptr && subscription->active.load(std::memory_order_acquire))
            {
                attempted = deliver_event(queued) == EventOutcome::delivered;
                if (attempted) subscription->counters.delivered.fetch_add(1, std::memory_order_relaxed);
            }
            // delivered is counted before queued drops, so a recycled
            // subscription never receives this event's count.
            if (subscription != nullptr) subscription->queued.fetch_sub(1, std::memory_order_acq_rel);
            give_buffer(std::move(queued.event.payload));
            return attempted;
        }, now);
    }

    // Engine-tick post-callback body (game thread): `before` (the idle sweep),
    // the drain, then the frame count (rule 9: every callback, drained or not,
    // after the drain). Nothing runs once set_inert was called.
    template <typename Before, typename DeliverEvent, typename DeliverNotice, typename Now>
    void tick(const DispatchBudget& budget, Before before, DeliverEvent deliver_event, DeliverNotice deliver_notice,
        Now now)
    {
        const InFlight pass(*this);
        if (tick_inert_.load()) return;
        if (!inert_.load() && delivery_.load() == HookDelivery::tick)
        {
            before();
            drain(budget, deliver_event, deliver_notice, now);
        }
        ticks_.fetch_add(1, std::memory_order_release);
    }

    // on_update fallback (event-loop thread), only when the engine tick is
    // not available. Returns whether a pass ran.
    template <typename DeliverEvent, typename DeliverNotice, typename Now>
    bool update(Clock::time_point at, const DispatchBudget& budget, DeliverEvent deliver_event,
        DeliverNotice deliver_notice, Now now)
    {
        const InFlight pass(*this);
        if (delivery_.load() != HookDelivery::loop || inert_.load()) return false;
        if (!schedule_.due(at)) return false;
        drain(budget, deliver_event, deliver_notice, now);
        update_calls_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Waits until no drain pass runs on another thread, or the timeout. True
    // when none is (or the caller is the pass's own thread).
    bool wait_for_drain(std::chrono::milliseconds timeout) const
    {
        const auto deadline = Clock::now() + timeout;
        while (draining_.load() != 0)
        {
            if (drain_thread_.load() == std::this_thread::get_id()) return true;
            if (Clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    // Decided once, at the first hook.
    void set_delivery(HookDelivery mode) { delivery_.store(mode, std::memory_order_release); }
    HookDelivery delivery() const { return delivery_.load(std::memory_order_acquire); }
    uint64_t ticks() const { return ticks_.load(std::memory_order_acquire); }
    uint64_t update_calls() const { return update_calls_.load(std::memory_order_acquire); }

    // Unload: no new events, no more passes; handlers pass through. Follow
    // with wait_for_drain for a pass already running.
    void set_inert()
    {
        inert_.store(true);
        tick_inert_.store(true);
    }
    bool inert() const { return inert_.load(std::memory_order_acquire); }

    // A session stopped or a hook was removed off the game thread: the next
    // game-thread pass unhooks entries left without subscriptions.
    void request_idle_sweep() { idle_sweep_.store(true, std::memory_order_release); }
    bool take_idle_sweep() { return idle_sweep_.exchange(false, std::memory_order_acq_rel); }

private:
    static constexpr std::size_t max_pooled_buffers = 1024;

    // Marks a drain pass in flight (sequentially consistent with set_inert:
    // either the pass sees the inert flag or wait_for_drain sees the pass).
    class InFlight
    {
    public:
        explicit InFlight(HookRuntime& runtime) : runtime_(runtime)
        {
            runtime_.draining_.fetch_add(1);
            runtime_.drain_thread_.store(std::this_thread::get_id());
        }
        ~InFlight()
        {
            runtime_.drain_thread_.store(std::thread::id{});
            runtime_.draining_.fetch_sub(1);
        }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;

    private:
        HookRuntime& runtime_;
    };

    // A scratch parameter block, per thread: handlers may run on worker threads.
    static std::vector<std::byte>& scratch(std::size_t size)
    {
        thread_local std::vector<std::byte> buffer;
        if (buffer.size() < size) buffer.resize(size);
        return buffer;
    }

    void fill_payload(const HookCall<Frame>& call, const SubscriptionPlan& plan, std::vector<std::byte>& payload)
    {
        payload.clear();
        const Schema& schema = *plan.schema;
        const bool post = call.phase == Phase::post;
        const Field* ret = schema.return_field ? &schema.fields[*schema.return_field] : nullptr;
        if (!call.params_available)
        {
            // Blueprint → native: post carries the return value only, from Result.
            if (!post || !plan.copies_return || call.result == nullptr || ret == nullptr) return;
            auto& block = scratch(schema.parms_size);
            std::memcpy(block.data() + ret->offset, call.result, static_cast<std::size_t>(ret->size) * ret->dim);
            plan.ret.copy(block.data(), &platform_, payload);
            return;
        }
        if (post ? !plan.copies_post : !plan.copies_pre) return;
        std::byte* locals = platform_.locals(call.frame);
        if (locals == nullptr) return;
        const std::byte* block = locals;
        const bool redirect = !plan.overwrite.empty() && platform_.has_out_parms(call.frame);
        const bool native_result = post && call.entry.kind == EntryKind::native && call.result != nullptr &&
            ret != nullptr && static_cast<const void*>(locals + ret->offset) != call.result;
        if (redirect || native_result)
        {
            auto& copy = scratch(schema.parms_size);
            if (schema.parms_size != 0) std::memcpy(copy.data(), locals, schema.parms_size);
            if (redirect)
            {
                for (const uint16_t index : plan.overwrite)
                {
                    const Field& field = schema.fields[index];
                    const void* source = platform_.out_param_address(call.frame, plan.properties[index]);
                    if (source != nullptr && source != static_cast<const void*>(locals + field.offset))
                        std::memcpy(copy.data() + field.offset, source,
                            static_cast<std::size_t>(field.size) * field.dim);
                }
            }
            if (native_result)
                std::memcpy(copy.data() + ret->offset, call.result, static_cast<std::size_t>(ret->size) * ret->dim);
            block = copy.data();
        }
        (post ? plan.post : plan.pre).copy(block, &platform_, payload);
    }

    std::vector<std::byte> take_buffer()
    {
        std::scoped_lock lock(queue_mutex_);
        if (pool_.empty()) return {};
        auto buffer = std::move(pool_.back());
        pool_.pop_back();
        return buffer;
    }

    void give_buffer(std::vector<std::byte> buffer)
    {
        if (buffer.capacity() == 0 || buffer.capacity() > max_payload_bytes) return;
        buffer.clear();
        std::scoped_lock lock(queue_mutex_);
        if (pool_.size() < max_pooled_buffers) pool_.push_back(std::move(buffer));
    }

    void enqueue(QueuedHookEvent queued)
    {
        auto* subscription = queued.subscription;
        std::scoped_lock lock(queue_mutex_);
        if (!capacity_.admit(queue_.size()))
        {
            subscription->counters.dropped.fetch_add(1, std::memory_order_relaxed);
            auto buffer = std::move(queued.event.payload);
            buffer.clear();
            if (pool_.size() < max_pooled_buffers && buffer.capacity() <= max_payload_bytes)
                pool_.push_back(std::move(buffer));
            return;
        }
        subscription->queued.fetch_add(1, std::memory_order_acq_rel);
        queue_.push_back(std::move(queued));
        capacity_.accepted(queue_.size());
        ready_.publish();
    }

    void remove_name_locked(uint32_t name_index, const std::pair<uint64_t, uint64_t>& key)
    {
        const auto found = pending_by_name_.find(name_index);
        if (found == pending_by_name_.end()) return;
        auto& keys = found->second;
        for (auto it = keys.begin(); it != keys.end(); ++it)
        {
            if (*it != key) continue;
            keys.erase(it);
            break;
        }
        if (keys.empty()) pending_by_name_.erase(found);
    }

    HookPlatform<Frame>& platform_;
    std::unordered_map<const Subscription*, std::shared_ptr<const SubscriptionPlan>> plans_;

    mutable std::mutex queue_mutex_;
    std::vector<QueuedHookEvent> queue_;
    std::vector<QueuedHookEvent> spare_;
    std::vector<std::vector<std::byte>> pool_;
    QueueCapacity capacity_{};
    QueueReadiness ready_;
    ConsumerBacklogCount backlog_count_;
    DispatchBacklog<QueuedHookEvent> backlog_;
    std::atomic<uint64_t> sequence_{0};

    std::mutex notice_mutex_;
    std::vector<HookNotice> notices_;
    std::atomic_bool notices_ready_{};
    std::set<std::pair<uint64_t, uint64_t>> wakeups_queued_;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> pending_by_token_;
    std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> pending_by_name_;
    std::array<std::atomic<uint64_t>, 64> name_bits_{};
    std::atomic<std::size_t> pending_count_{0};

    mutable std::mutex records_mutex_;
    std::unordered_map<uint64_t, HookRecord> records_;
    std::atomic<uint64_t> next_hook_{0};

    std::atomic<int> draining_{0};
    std::atomic<std::thread::id> drain_thread_{};
    std::atomic<HookDelivery> delivery_{HookDelivery::undecided};
    QueueDispatchSchedule schedule_;
    std::atomic<uint64_t> ticks_{0};
    std::atomic<uint64_t> update_calls_{0};
    std::atomic_bool inert_{};
    std::atomic_bool tick_inert_{};
    std::atomic_bool idle_sweep_{};
};
}
