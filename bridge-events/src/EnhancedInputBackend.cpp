#include <EnhancedInputBackend.hpp>

#ifdef _WIN32

#include <algorithm>
#include <SharedDispatch.hpp>
#include <sstream>
#include <BindingSnapshot.hpp>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#define UE4SSLEB_WINAPI __stdcall
#else
#define UE4SSLEB_WINAPI
#endif

extern "C" __declspec(dllimport) unsigned long UE4SSLEB_WINAPI GetCurrentThreadId();
extern "C" __declspec(dllimport) void UE4SSLEB_WINAPI OutputDebugStringA(const char* output);

extern "C" __declspec(dllimport) void* UE4SSLEB_WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) int UE4SSLEB_WINAPI ReadProcessMemory(void*, const void*, void*, std::size_t, std::size_t*);

#undef UE4SSLEB_WINAPI

namespace UE4SSLuaEventBridge
{
namespace ABI = EnhancedInputABI;
using RC::Unreal::FMemory;
using RC::Unreal::FWeakObjectPtr;
using RC::Unreal::UClass;
using RC::Unreal::UObject;

namespace
{
template <typename T>
T read_at(const ABI::InputActionInstanceView& view, std::size_t offset)
{
    T value{};
    std::memcpy(&value, view.storage + offset, sizeof(T));
    return value;
}

std::wstring widen_ascii(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

UClass* object_class(UObject* object)
{
    UClass* result{};
    std::memcpy(
        &result,
        reinterpret_cast<std::byte*>(object) + ABI::uobject_class_offset,
        sizeof(result));
    return result;
}

void append_debug_text(std::string& output, std::string_view value)
{
    output.push_back('"');
    if (value.empty())
    {
        output.push_back('-');
    }
    else
    {
        for (const unsigned char character : value)
        {
            if (character == '\\' || character == '"')
            {
                output.push_back('\\');
            }
            output.push_back(character >= 0x20 ? static_cast<char>(character) : '?');
        }
    }
    output.push_back('"');
}
}

const char* delivery_fault_reason(DeliveryFault fault)
{
    switch (fault) {
    case DeliveryFault::QueueCapacity: return "queue_capacity_exceeded";
    case DeliveryFault::QueueException: return "queue_exception";
    case DeliveryFault::CallbackError: return "callback_error";
    default: return "healthy";
    }
}

static void latch_delivery_fault(const std::shared_ptr<EnhancedInputDispatchState>& dispatch,
                                 const std::shared_ptr<TargetDeliveryState>& delivery,
                                 DeliveryFault reason)
{
    if (!delivery) return; // Legacy targets retain their existing behavior.
    auto expected = DeliveryFault::None;
    if (delivery->fault.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
        dispatch->faults_pending.store(true, std::memory_order_release);
}

void write_debug_trace(
    const std::shared_ptr<EnhancedInputDispatchState>& dispatch_state,
    LuaSession* session,
    const EnhancedInputDebugInfo& debug,
    std::string_view stage,
    std::string_view phase,
    uint64_t sequence,
    std::string_view reason)
{
    if (!debug.enabled)
    {
        return;
    }

    try
    {
        std::string line = "[UE4SSLuaEventBridge][trace] label=";
        append_debug_text(line, debug.label);
        line += " stage=";
        append_debug_text(line, stage);
        line += " scope=" + std::to_string(debug.scope_id);
        line += " binding=" + std::to_string(debug.binding_id);
        line += " key=";
        append_debug_text(line, debug.key);
        line += " trigger=";
        append_debug_text(line, debug.trigger_name);
        line += " phase=";
        append_debug_text(line, phase);
        line += " event_seq=" + std::to_string(sequence);
        line += " thread_id=" + std::to_string(GetCurrentThreadId());
        line += " game_thread=";
        line += RC::Unreal::IsInGameThread() ? "true" : "false";
        line += " reason=";
        append_debug_text(line, reason);
        line.push_back('\n');

        // Make the record visible immediately to an attached debugger. The
        // queue below sends the same line through Lua's normal print route on
        // the bridge update thread, which places it in the UE4SS log.
        OutputDebugStringA(line.c_str());
        if (!dispatch_state || !session ||
            !dispatch_state->accepting.load(std::memory_order_acquire))
        {
            return;
        }

        std::scoped_lock lock(dispatch_state->mutex);
        if (dispatch_state->accepting.load(std::memory_order_acquire))
        {
            if (!dispatch_state->trace_capacity.admit(dispatch_state->traces.size())) return;
            dispatch_state->traces.push_back({session, std::move(line)});
            dispatch_state->trace_capacity.accepted(dispatch_state->traces.size());
            dispatch_state->traces_ready.publish();
        }
    }
    catch (...)
    {
        // Diagnostics must never alter input delivery. The debugger fallback
        // deliberately avoids allocation and remains useful if a log device
        // itself is the failing component.
        OutputDebugStringA("[UE4SSLuaEventBridge][trace] trace_write_failed\n");
    }
}

UObject* ABI::InputActionInstanceView::source_action() const
{
    return read_at<UObject*>(*this, instance_source_action_offset);
}
ABI::TriggerEvent ABI::InputActionInstanceView::trigger_event() const
{
    return read_at<TriggerEvent>(*this, instance_trigger_event_offset);
}
double ABI::InputActionInstanceView::x() const { return read_at<double>(*this, instance_value_x_offset); }
double ABI::InputActionInstanceView::y() const { return read_at<double>(*this, instance_value_y_offset); }
double ABI::InputActionInstanceView::z() const { return read_at<double>(*this, instance_value_z_offset); }
ABI::ValueType ABI::InputActionInstanceView::value_type() const
{
    return read_at<ValueType>(*this, instance_value_type_offset);
}
float ABI::InputActionInstanceView::elapsed_processed() const
{
    return read_at<float>(*this, instance_elapsed_processed_offset);
}
float ABI::InputActionInstanceView::elapsed_triggered() const
{
    return read_at<float>(*this, instance_elapsed_triggered_offset);
}

ABI::ActionEventBinding::ActionEventBinding(const UObject* source_action, ABI::TriggerEvent trigger, uint32_t binding_handle)
    : action(source_action), event(trigger)
{
    handle = binding_handle;
}

class EnhancedInputBackend::NativeBinding final : public ABI::ActionEventBinding
{
public:
    NativeBinding(
        std::shared_ptr<EnhancedInputDispatchState> dispatch_state,
        const UObject* action,
        ABI::TriggerEvent event,
        uint32_t handle,
        std::shared_ptr<EnhancedInputSubscription> owner)
        : ActionEventBinding(action, event, handle),
          dispatch_state_(std::move(dispatch_state)),
          owner_(std::move(owner))
    {
        dispatch_state_->live_bindings.fetch_add(1, std::memory_order_relaxed);
    }

    ~NativeBinding() override
    {
        dispatch_state_->live_bindings.fetch_sub(1, std::memory_order_release);
    }

    void Execute(const ABI::InputActionInstanceView& instance) const override
    {
        const auto sequence =
            dispatch_state_->next_event_sequence.fetch_add(1, std::memory_order_relaxed);
        write_debug_trace(
            dispatch_state_, owner_->session, owner_->debug,
            "enhanced_input_event", owner_->phase_name, sequence);
        write_debug_trace(
            dispatch_state_, owner_->session, owner_->debug,
            "native_delegate_entered", owner_->phase_name, sequence);

        if (!owner_->active.load(std::memory_order_acquire) || !owner_->delivery_valid())
        {
            write_debug_trace(
                dispatch_state_, owner_->session, owner_->debug,
                "event_rejected", owner_->phase_name, sequence, "binding_inactive");
            return;
        }
        if (!dispatch_state_->accepting.load(std::memory_order_acquire))
        {
            write_debug_trace(
                dispatch_state_, owner_->session, owner_->debug,
                "event_rejected", owner_->phase_name, sequence, "dispatch_stopped");
            return;
        }

        try
        {
            EnhancedInputEvent queued{};
            queued.subscription = owner_->id;
            queued.sequence = sequence;
            queued.owner = owner_;
            queued.elapsed_processed = instance.elapsed_processed();
            queued.elapsed_triggered = instance.elapsed_triggered();
            queued.x = instance.x();
            queued.y = instance.y();
            queued.z = instance.z();
            queued.value_type = instance.value_type();

            bool accepted{};
            std::string_view rejection_reason;
            {
                std::scoped_lock lock(dispatch_state_->mutex);
                if (!owner_->active.load(std::memory_order_acquire) || !owner_->delivery_valid())
                {
                    rejection_reason = "binding_inactive";
                }
                else if (!dispatch_state_->accepting.load(std::memory_order_acquire))
                {
                    rejection_reason = "dispatch_stopped";
                }
                else
                {
                    if (dispatch_state_->event_capacity.admit(dispatch_state_->events.size()))
                    {
                        dispatch_state_->events.push_back(std::move(queued));
                        dispatch_state_->event_capacity.accepted(dispatch_state_->events.size());
                        dispatch_state_->events_ready.publish();
                        accepted = true;
                    }
                    else
                    {
                        rejection_reason = "queue_capacity_exceeded";
                        latch_delivery_fault(dispatch_state_, owner_->delivery, DeliveryFault::QueueCapacity);
                        // One visible diagnostic per overflow episode, even with
                        // debug disabled. Never execute Lua on this producer.
                        if (dispatch_state_->event_capacity.warning_pending &&
                            dispatch_state_->trace_capacity.admit(dispatch_state_->traces.size()))
                        {
                            dispatch_state_->traces.push_back({owner_->session,
                                "[UE4SSLuaEventBridge][ERROR] Event queue capacity exceeded; newest input events rejected. Inspect GetDispatchStats()."});
                            dispatch_state_->traces_ready.publish();
                            dispatch_state_->event_capacity.warning_pending = false;
                        }
                    }
                }
            }
            if (accepted)
            {
                write_debug_trace(
                    dispatch_state_, owner_->session, owner_->debug,
                    "event_queued", owner_->phase_name, sequence);
            }
            else
            {
                write_debug_trace(
                    dispatch_state_, owner_->session, owner_->debug,
                    "event_rejected", owner_->phase_name, sequence, rejection_reason);
            }
        }
        catch (...)
        {
            // Never unwind through Unreal's input dispatcher. An allocation
            // failure disables this subscription until game-thread cleanup.
            latch_delivery_fault(dispatch_state_, owner_->delivery, DeliveryFault::QueueException);
            owner_->active.store(false, std::memory_order_release);
            write_debug_trace(
                dispatch_state_, owner_->session, owner_->debug,
                "event_rejected", owner_->phase_name, sequence, "queue_exception");
        }
    }

    const uint32_t* handle_address() const { return &handle; }

    UObject* GetUObject() const override { return nullptr; }
    bool IsBoundToObject(const void*) const override { return false; }
    void SetShouldFireWithEditorScriptGuard(bool) override {}

    ABI::UniquePtr<ABI::ActionEventBinding> Clone() const override
    {
        // A clone is owned exclusively by Unreal and cannot be found through
        // the bridge's original component/binding pair. Remember that one has
        // existed so an off-thread DLL unload can conservatively retain code.
        dispatch_state_->clone_created.store(true, std::memory_order_release);
        void* memory = FMemory::Malloc(sizeof(NativeBinding), alignof(NativeBinding));
        if (!memory)
        {
            return {};
        }
        return ABI::UniquePtr<ABI::ActionEventBinding>(
            ::new (memory) NativeBinding(dispatch_state_, action.Get(), event, handle, owner_));
    }

    static void operator delete(void* memory) noexcept { FMemory::Free(memory); }
    static void operator delete(void* memory, std::size_t) noexcept { FMemory::Free(memory); }

private:
    std::shared_ptr<EnhancedInputDispatchState> dispatch_state_;
    std::shared_ptr<EnhancedInputSubscription> owner_;
};

EnhancedInputBackend::EnhancedInputBackend()
    : dispatch_state_(std::make_shared<EnhancedInputDispatchState>())
{
}

EnhancedInputBackend::~EnhancedInputBackend() { (void)shutdown(); }

void EnhancedInputBackend::initialize()
{
    dispatch_state_->accepting.store(true, std::memory_order_release);
    initialized_.store(true, std::memory_order_release);
}

std::string EnhancedInputBackend::enable_delivery_faults(LuaSession& session, uint64_t target)
{
    if (!RC::Unreal::IsInGameThread()) return "delivery fault registration must run on the Unreal game thread";
    std::scoped_lock lock(mutex_);
    const auto found = targets_.find(target);
    if (!available() || found == targets_.end() || found->second.session != &session || inactive_sessions_.contains(&session))
        return "target is unknown or stopping";
    if (found->second.delivery) return "delivery fault policy already registered";
    if (found->second.has_bound) return "register delivery fault policy before binding actions";
    found->second.delivery = std::make_shared<TargetDeliveryState>();
    return {};
}

std::pair<bool, const char*> EnhancedInputBackend::target_delivery_valid(LuaSession& session, uint64_t target) const
{
    std::scoped_lock lock(mutex_);
    const auto found = targets_.find(target);
    if (!available() || found == targets_.end() || found->second.session != &session || inactive_sessions_.contains(&session))
        return {false, "target is unknown or stopping"};
    const auto& delivery = found->second.delivery;
    if (!delivery) return {false, "delivery fault policy is not enabled"};
    const auto reason = delivery->fault.load(std::memory_order_acquire);
    return {reason == DeliveryFault::None, delivery_fault_reason(reason)};
}

void EnhancedInputBackend::report_delivery_fault(const EnhancedInputSubscription& subscription, DeliveryFault reason)
{
    latch_delivery_fault(dispatch_state_, subscription.delivery, reason);
}

std::optional<TargetDeliveryFault> EnhancedInputBackend::take_delivery_fault()
{
    if (!dispatch_state_->faults_pending.load(std::memory_order_acquire)) return {};
    if (!dispatch_state_->faults_pending.exchange(false, std::memory_order_acq_rel)) return {};
    std::scoped_lock lock(mutex_);
    if (!available()) return {};
    for (auto& [id, target] : targets_) {
        if (!target.delivery || target.delivery->notified || inactive_sessions_.contains(target.session)) continue;
        const auto fault = target.delivery->fault.load(std::memory_order_acquire);
        if (fault == DeliveryFault::None) continue;
        target.delivery->notified = true;
        // Continue the rare scan for other faulted targets. A concurrent producer
        // can also publish here; never clear its flag after scanning.
        dispatch_state_->faults_pending.store(true, std::memory_order_release);
        return TargetDeliveryFault{target.session, id, fault};
    }
    return {};
}

void EnhancedInputBackend::set_queue_limit(std::size_t limit)
{
    std::scoped_lock lock(dispatch_state_->mutex);
    dispatch_state_->event_capacity.limit = limit;
}

EnhancedInputBackend::QueueStats EnhancedInputBackend::queue_stats() const
{
    std::scoped_lock lock(dispatch_state_->mutex);
    return {dispatch_state_->consumer_backlog.total(dispatch_state_->events.size()), dispatch_state_->event_capacity.high_water,
        dispatch_state_->event_capacity.rejected, dispatch_state_->trace_capacity.rejected};
}

bool EnhancedInputBackend::shutdown()
{
    if (!initialized_.exchange(false, std::memory_order_acq_rel))
    {
        return dispatch_state_->live_bindings.load(std::memory_order_acquire) == 0;
    }

    dispatch_state_->accepting.store(false, std::memory_order_release);

    std::scoped_lock lock(mutex_);
    const bool on_game_thread = RC::Unreal::IsInGameThread();
    const bool requires_module_pin =
        !on_game_thread &&
        (!bindings_.empty() || dispatch_state_->clone_created.load(std::memory_order_acquire));
    for (auto& [id, subscription] : subscriptions_)
    {
        subscription->active.store(false, std::memory_order_release);
    }
    if (on_game_thread)
    {
        for (auto& binding : bindings_)
        {
            detach(binding);
        }
    }
    bindings_.clear();
    subscriptions_.clear();
    targets_.clear();
    inactive_sessions_.clear();

    {
        std::scoped_lock event_lock(dispatch_state_->mutex);
        dispatch_state_->events.clear();
        dispatch_state_->traces.clear();
        dispatch_state_->events_ready.drained();
        dispatch_state_->traces_ready.drained();
    }
    return !requires_module_pin &&
           dispatch_state_->live_bindings.load(std::memory_order_acquire) == 0;
}

UObject* EnhancedInputBackend::cached_object(ObjectCache& cache, const std::wstring& path)
{
    const auto it = cache.find(path);
    if (it == cache.end())
    {
        return nullptr;
    }
    // Still the object in that GUObjectArray slot, and still at that path.
    UObject* live{};
    if (auto* item = RC::Unreal::FUObjectArray::IndexToObject(it->second.index))
    {
        std::memcpy(
            &live,
            reinterpret_cast<const std::byte*>(item) + FWeakObjectPtr::object_item_object_offset,
            sizeof(live));
    }
    if (live && live == it->second.object && live->GetPathName() == path)
    {
        return live;
    }
    cache.erase(it);
    return nullptr;
}

void EnhancedInputBackend::fill_cache(ObjectCache& cache, const wchar_t* class_name)
{
    // One object walk indexes every object of the class by path, so the
    // other paths a caller binds next are found without walking again.
    std::vector<UObject*> objects;
    RC::Unreal::UObjectGlobals::FindAllOf(class_name, objects);
    for (auto* object : objects)
    {
        if (!object)
        {
            continue;
        }
        int32_t index{};
        std::memcpy(
            &index,
            reinterpret_cast<const std::byte*>(object) + FWeakObjectPtr::uobject_internal_index_offset,
            sizeof(index));
        if (index >= 0)
        {
            cache.insert_or_assign(object->GetPathName(), CachedObject{object, index});
        }
    }
}

UObject* EnhancedInputBackend::resolve_component(const std::wstring& path, std::string& error)
{
    auto* object = cached_object(component_cache_, path);
    if (!object)
    {
        component_cache_.clear();
        fill_cache(component_cache_, L"EnhancedInputComponent");
        object = cached_object(component_cache_, path);
    }
    if (!object)
    {
        fill_cache(component_cache_, L"InputComponent");
        object = cached_object(component_cache_, path);
    }
    if (object && !validate_component(object))
    {
        auto* type = object_class(object);
        error = "EnhancedInputComponent found, but native layout validation failed: properties_size=" +
            std::to_string(type ? type->GetPropertiesSize() : -1) +
            ", expected=" + std::to_string(ABI::enhanced_input_component_size);
        return nullptr;
    }
    return object;
}

UObject* EnhancedInputBackend::resolve_action(const std::wstring& path)
{
    if (auto* action = cached_object(action_cache_, path))
    {
        return action;
    }
    action_cache_.clear();
    fill_cache(action_cache_, L"InputAction");
    return cached_object(action_cache_, path);
}

bool EnhancedInputBackend::validate_component(UObject* component) const
{
    if (!component)
    {
        return false;
    }
    auto* component_type = object_class(component);
    if (!component_type || component_type->GetPropertiesSize() != static_cast<int32_t>(ABI::enhanced_input_component_size))
    {
        return false;
    }
    const auto* bindings = reinterpret_cast<const ABI::ActionEventBindingArray*>(
        reinterpret_cast<const std::byte*>(component) + ABI::action_event_bindings_offset);
    return bindings->size >= 0 && bindings->capacity >= bindings->size && bindings->capacity < 65536 &&
           (bindings->capacity == 0 ||
            (bindings->data != nullptr &&
             reinterpret_cast<std::uintptr_t>(bindings->data) % alignof(void*) == 0));
}

std::pair<uint64_t, std::string> EnhancedInputBackend::open_target(LuaSession& session, std::string component_path)
{
    if (!initialized_.load(std::memory_order_acquire))
    {
        return {0, "Enhanced Input backend is not initialized"};
    }

    {
        std::scoped_lock lock(mutex_);
        collect_inactive_locked();
    }

    std::string resolution_error;
    auto* component = resolve_component(widen_ascii(component_path), resolution_error);
    if (!component)
    {
        return {0, resolution_error.empty() ? "EnhancedInputComponent was not found at object path: " + component_path : resolution_error};
    }

    Target target{};
    target.id = next_target_.fetch_add(1);
    target.session = &session;
    target.component.assign(component);
    if (target.component.Get() != component) return {0, "Enhanced Input component weak-reference initialization failed"};
    target.component_path_utf8 = std::move(component_path);

    const auto id = target.id;
    std::scoped_lock lock(mutex_);
    if (!initialized_.load(std::memory_order_acquire))
    {
        return {0, "Enhanced Input backend shut down during component lookup"};
    }
    const bool inserted = targets_.emplace(id, std::move(target)).second;
    if (!inserted)
    {
        return {0, "Enhanced Input target handle collision"};
    }
    return {id, {}};
}


std::pair<std::string, std::string> EnhancedInputBackend::inspect_target(LuaSession& session, uint64_t target)
{
    if (!RC::Unreal::IsInGameThread()) return {{}, "InspectInputComponent must run on the Unreal game thread"};
    std::scoped_lock lock(mutex_);
    if (!available()) return {{}, "Enhanced Input backend is not initialized"};
    const auto found = targets_.find(target);
    if (found == targets_.end() || found->second.session != &session || inactive_sessions_.contains(&session))
        return {{}, "Enhanced Input target is unknown, stopped, or belongs to another Lua session"};
    const auto read = [](const void* source, void* destination, std::size_t size) {
        std::size_t copied{};
        return source && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) && copied == size;
    };
    const auto resolve = [&](const FWeakObjectPtr& weak) -> UObject* {
        if (weak.object_serial_number == 0 || weak.object_index < 0) return nullptr;
        const auto* item = RC::Unreal::FUObjectArray::IndexToObject(weak.object_index);
        if (!item) return nullptr;
        UObject* object{};
        int32_t serial{};
        const auto address = reinterpret_cast<std::uintptr_t>(item);
        if (!read(reinterpret_cast<const void*>(address), &object, sizeof(object)) ||
            !read(reinterpret_cast<const void*>(address + FWeakObjectPtr::object_item_serial_offset), &serial, sizeof(serial))) return nullptr;
        return serial == weak.object_serial_number ? object : nullptr;
    };
    auto* component = resolve(found->second.component);
    ABI::ActionEventBindingArray array{};
    const bool array_readable = component && read(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(component) + ABI::action_event_bindings_offset), &array, sizeof(array));
    const bool invariant = array_readable && snapshot_array_valid(reinterpret_cast<std::uintptr_t>(array.data), array.size, array.capacity);
    std::vector<std::uintptr_t> entries;
    if (invariant) entries.resize(static_cast<std::size_t>(array.size));
    const bool entries_readable = invariant && (entries.empty() || read(array.data, entries.data(), entries.size() * sizeof(std::uintptr_t)));
    std::ostringstream out;
    out << "binding_snapshot schema=1 target=" << target << " game_thread=1 component_valid=" << (component != nullptr)
        << " component_index=" << found->second.component.object_index << " component_serial=" << found->second.component.object_serial_number
        << " array_readable=" << array_readable << " array_invariants=" << invariant << " array_size=" << array.size
        << " array_capacity=" << array.capacity << " entries_readable=" << entries_readable << '\n';
    std::size_t count{};
    bool truncated{};
    for (const auto& live : bindings_)
    {
        const auto it = subscriptions_.find(live.subscription);
        if (it == subscriptions_.end() || it->second->target != target || it->second->session != &session) continue;
        if (count == 256) { truncated = true; break; }
        ++count;
        const auto& sub = *it->second;
        const bool member = entries_readable && snapshot_contains(entries, reinterpret_cast<std::uintptr_t>(live.binding));
        FWeakObjectPtr action;
        ABI::TriggerEvent event{};
        uint32_t handle{};
        const bool metadata = snapshot_read_metadata(member, read, live.action_address, live.event_address, live.handle_address, action, event, handle);
        const bool action_matches = metadata && action.object_index == live.expected_action.object_index && action.object_serial_number == live.expected_action.object_serial_number;
        out << "binding subscription=" << sub.id << " scope=" << sub.debug.scope_id << " binding=" << sub.debug.binding_id
            << " active=" << sub.active.load(std::memory_order_acquire) << " member=" << member << " metadata_readable=" << metadata
            << " expected_action_index=" << live.expected_action.object_index << " expected_action_serial=" << live.expected_action.object_serial_number
            << " expected_action_valid=" << (resolve(live.expected_action) != nullptr)
            << " expected_trigger=" << static_cast<unsigned>(sub.phase) << " expected_handle=" << live.expected_handle;
        if (metadata) out << " action_index=" << action.object_index << " action_serial=" << action.object_serial_number
            << " action_valid=" << (action_matches ? (resolve(live.expected_action) != nullptr ? "1" : "0") : "unknown")
            << " action_matches=" << action_matches
            << " trigger=" << static_cast<unsigned>(event) << " trigger_matches=" << (event == sub.phase)
            << " handle=" << handle << " handle_matches=" << (handle == live.expected_handle);
        out << '\n';
    }
    out << "snapshot_end reported=" << count << " truncated=" << truncated << '\n';
    return {out.str(), {}};
}

bool EnhancedInputBackend::close_target(LuaSession& session, uint64_t target)
{
    std::scoped_lock lock(mutex_);
    if (!initialized_.load(std::memory_order_acquire))
    {
        return false;
    }
    collect_inactive_locked();
    const auto found = targets_.find(target);
    if (found == targets_.end() || found->second.session != &session)
    {
        return false;
    }

    std::vector<uint64_t> subscriptions;
    for (const auto& [id, subscription] : subscriptions_)
    {
        if (subscription->session == &session && subscription->target == target)
        {
            subscriptions.push_back(id);
        }
    }
    for (const auto id : subscriptions)
    {
        unsubscribe_locked(session, id);
    }
    targets_.erase(found);
    return true;
}

std::pair<uint64_t, std::string> EnhancedInputBackend::subscribe(
    LuaSession& session,
    uint64_t target_id,
    uint64_t callback_token,
    std::string action_path,
    std::string phase_name,
    ABI::TriggerEvent phase,
    EnhancedInputDebugInfo debug)
{
    // Resolve outside the lock; errors keep their order below.
    auto* action = resolve_action(widen_ascii(action_path));

    std::scoped_lock lock(mutex_);
    if (!initialized_.load(std::memory_order_acquire))
    {
        return {0, "Enhanced Input backend is not initialized"};
    }
    collect_inactive_locked();

    const auto target_it = targets_.find(target_id);
    if (target_it == targets_.end() || target_it->second.session != &session)
    {
        return {0, "Enhanced Input target handle is unknown or belongs to another Lua session: " +
                       std::to_string(target_id)};
    }

    if (target_it->second.delivery && !target_it->second.delivery->valid())
        return {0, "target delivery is invalid; close and recreate the target"};
    auto* component = target_it->second.component.Get();
    if (!validate_component(component))
    {
        return {0, "Enhanced Input target is no longer valid"};
    }

    if (!action)
    {
        return {0, "InputAction was not found at object path: " + action_path};
    }

    const FWeakObjectPtr action_reference(action);
    if (action_reference.Get() != action) return {0, "InputAction weak-reference initialization failed: " + action_path};

    auto subscription = std::make_shared<EnhancedInputSubscription>();
    subscription->id = next_subscription_.fetch_add(1);
    subscription->target = target_id;
    subscription->delivery = target_it->second.delivery;
    subscription->session = &session;
    subscription->callback_token = callback_token;
    subscription->action_path_utf8 = std::move(action_path);
    subscription->phase_name = std::move(phase_name);
    subscription->phase = phase;
    subscription->debug = std::move(debug);

    // Complete all potentially allocating bookkeeping before publishing the
    // polymorphic binding into Unreal's array. After reserve succeeds, the
    // final LiveBinding insertion cannot strand an untracked native object.
    if (bindings_.size() == bindings_.capacity())
    {
        const auto capacity = bindings_.capacity();
        const auto growth = std::min(std::max<std::size_t>(capacity / 2, 4), bindings_.max_size() - capacity);
        if (growth == 0) return {0, "Enhanced Input binding storage is full"};
        bindings_.reserve(capacity + growth);
    }
    const auto id = subscription->id;
    const auto [subscription_it, inserted] = subscriptions_.emplace(id, subscription);
    if (!inserted)
    {
        return {0, "Enhanced Input subscription handle collision"};
    }

    auto* binding = attach(component, action, subscription);
    if (!binding)
    {
        subscriptions_.erase(subscription_it);
        return {0, "Enhanced Input native binding creation failed"};
    }

    target_it->second.has_bound = true;
    LiveBinding live{};
    live.component.assign(component);
    live.binding = binding;
    live.subscription = id;
    live.expected_action = binding->action;
    live.action_address = &binding->action;
    live.event_address = &binding->event;
    live.handle_address = binding->handle_address();
    live.expected_handle = *binding->handle_address();
    bindings_.push_back(std::move(live));
    if (subscription->debug.primary)
    {
        write_debug_trace(
            dispatch_state_, subscription->session, subscription->debug,
            "binding_created", subscription->phase_name);
    }
    return {id, {}};
}

bool EnhancedInputBackend::unsubscribe_locked(LuaSession& session, uint64_t id)
{
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end() || it->second->session != &session)
    {
        return false;
    }

    auto subscription = it->second;
    subscription->active.store(false, std::memory_order_release);
    for (auto binding_it = bindings_.begin(); binding_it != bindings_.end();)
    {
        if (binding_it->subscription == id)
        {
            detach(*binding_it);
            binding_it = bindings_.erase(binding_it);
        }
        else
        {
            ++binding_it;
        }
    }
    subscriptions_.erase(it);
    if (subscription->debug.primary)
    {
        write_debug_trace(
            dispatch_state_, subscription->session, subscription->debug,
            "binding_removed", subscription->phase_name);
    }
    return true;
}

bool EnhancedInputBackend::unsubscribe(LuaSession& session, uint64_t id)
{
    std::scoped_lock lock(mutex_);
    collect_inactive_locked();
    return unsubscribe_locked(session, id);
}

std::size_t EnhancedInputBackend::unsubscribe_all(LuaSession& session, bool preserve_targets)
{
    std::scoped_lock lock(mutex_);
    collect_inactive_locked();
    std::vector<uint64_t> ids;
    for (const auto& [id, subscription] : subscriptions_)
    {
        if (subscription->session == &session)
        {
            ids.push_back(id);
        }
    }
    for (const auto id : ids)
    {
        unsubscribe_locked(session, id);
    }

    for (auto it = targets_.begin(); it != targets_.end();)
    {
        if (!preserve_targets && it->second.session == &session)
        {
            it = targets_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return ids.size();
}

void EnhancedInputBackend::deactivate(LuaSession& session, uint64_t id)
{
    {
        std::scoped_lock lock(mutex_);
        const auto found = subscriptions_.find(id);
        if (found == subscriptions_.end() || found->second->session != &session)
        {
            return;
        }
        found->second->active.store(false, std::memory_order_release);
    }

    std::scoped_lock event_lock(dispatch_state_->mutex);
    std::erase_if(dispatch_state_->events, [&](const EnhancedInputEvent& event) {
        return event.subscription == id;
    });
}

void EnhancedInputBackend::deactivate_all(LuaSession& session)
{
    {
        std::scoped_lock lock(mutex_);
        inactive_sessions_.insert(&session);
        for (auto& [id, subscription] : subscriptions_)
        {
            if (subscription->session == &session)
            {
                subscription->active.store(false, std::memory_order_release);
            }
        }
    }

    std::scoped_lock event_lock(dispatch_state_->mutex);
    std::erase_if(dispatch_state_->events, [&](const EnhancedInputEvent& event) {
        return event.owner && event.owner->session == &session;
    });
}

void EnhancedInputBackend::collect_inactive_locked()
{
    std::vector<uint64_t> ids;
    for (const auto& [id, subscription] : subscriptions_)
    {
        if (!subscription->active.load(std::memory_order_acquire) ||
            !subscription->session ||
            inactive_sessions_.contains(subscription->session))
        {
            ids.push_back(id);
        }
    }

    for (const auto id : ids)
    {
        const auto found = subscriptions_.find(id);
        if (found != subscriptions_.end() && found->second->session)
        {
            unsubscribe_locked(*found->second->session, id);
        }
    }

    for (auto it = targets_.begin(); it != targets_.end();)
    {
        if (!it->second.session || inactive_sessions_.contains(it->second.session))
        {
            it = targets_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::vector<EnhancedInputEvent> EnhancedInputBackend::take_events()
{
    if (!dispatch_state_->events_ready.pending()) return {};
    std::vector<EnhancedInputEvent> result;
    {
        std::scoped_lock lock(dispatch_state_->mutex);
        result = drain_queue(dispatch_state_->events, dispatch_state_->spare_events);
        dispatch_state_->consumer_backlog.remaining(result.size());
        dispatch_state_->events_ready.drained();
    }
    return result;
}

std::vector<EnhancedInputTrace> EnhancedInputBackend::take_traces()
{
    if (!dispatch_state_->traces_ready.pending()) return {};
    std::scoped_lock lock(dispatch_state_->mutex);
    auto result = drain_queue(dispatch_state_->traces, dispatch_state_->spare_traces);
    dispatch_state_->traces_ready.drained();
    return result;
}

void EnhancedInputBackend::recycle_events(std::vector<EnhancedInputEvent> batch)
{
    if (batch.capacity() == 0) return;
    batch.clear(); // Release subscription ownership outside the queue mutex.
    std::scoped_lock lock(dispatch_state_->mutex);
    retain_empty_buffer(dispatch_state_->spare_events, batch);
}

void EnhancedInputBackend::recycle_traces(std::vector<EnhancedInputTrace> batch)
{
    if (batch.capacity() == 0) return;
    batch.clear();
    std::scoped_lock lock(dispatch_state_->mutex);
    retain_empty_buffer(dispatch_state_->spare_traces, batch);
}

void EnhancedInputBackend::trace(
    LuaSession& session,
    const EnhancedInputDebugInfo& debug,
    std::string_view stage,
    std::string_view phase,
    uint64_t sequence,
    std::string_view reason)
{
    write_debug_trace(
        dispatch_state_, &session, debug, stage, phase, sequence, reason);
}

EnhancedInputBackend::NativeBinding* EnhancedInputBackend::attach(
    UObject* component,
    UObject* action,
    const std::shared_ptr<EnhancedInputSubscription>& subscription)
{
    if (!validate_component(component) || !action)
    {
        return nullptr;
    }

    auto* array = reinterpret_cast<ABI::ActionEventBindingArray*>(
        reinterpret_cast<std::byte*>(component) + ABI::action_event_bindings_offset);
    if (array->size == array->capacity)
    {
        constexpr int32_t maximum_capacity = 65535;
        if (array->capacity >= maximum_capacity)
        {
            return nullptr;
        }
        const int32_t requested_capacity =
            array->capacity == 0 ? 4 : array->capacity + std::max(4, array->capacity / 2);
        const int32_t new_capacity = std::min(requested_capacity, maximum_capacity);
        void* resized = FMemory::Realloc(array->data, static_cast<std::size_t>(new_capacity) * sizeof(void*), alignof(void*));
        if (!resized)
        {
            return nullptr;
        }
        array->data = static_cast<ABI::ActionEventBinding**>(resized);
        array->capacity = new_capacity;
    }

    void* memory = FMemory::Malloc(sizeof(NativeBinding), alignof(NativeBinding));
    if (!memory)
    {
        return nullptr;
    }
    auto* binding = ::new (memory) NativeBinding(
        dispatch_state_,
        action,
        subscription->phase,
        next_binding_handle_.fetch_add(1),
        subscription);
    if (binding->action.Get() != action)
    {
        delete binding;
        return nullptr;
    }
    array->data[array->size++] = binding;
    return binding;
}

void EnhancedInputBackend::detach(LiveBinding& live)
{
    auto* component = live.component.Get();
    if (!validate_component(component))
    {
        // If the owning component has already been destroyed, Unreal has also
        // destroyed the binding array. Do not dereference the stale binding.
        return;
    }

    auto* array = reinterpret_cast<ABI::ActionEventBindingArray*>(
        reinterpret_cast<std::byte*>(component) + ABI::action_event_bindings_offset);
    for (int32_t index = 0; index < array->size; ++index)
    {
        if (array->data[index] != live.binding)
        {
            continue;
        }
        std::memmove(
            array->data + index,
            array->data + index + 1,
            static_cast<std::size_t>(array->size - index - 1) * sizeof(void*));
        --array->size;
        delete live.binding;
        return;
    }
}
}

#endif
