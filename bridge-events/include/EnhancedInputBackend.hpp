#pragma once

#ifdef _WIN32

#include <EnhancedInputABI.hpp>
#include <SharedDispatch.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace UE4SSLuaEventBridge
{
struct LuaSession;

enum class DeliveryFault : uint8_t { None, QueueCapacity, QueueException, CallbackError };
const char* delivery_fault_reason(DeliveryFault fault);
struct TargetDeliveryState {
    std::atomic<DeliveryFault> fault{DeliveryFault::None};
    bool notified{}; // Protected by backend mutex; never accessed by producers.
    bool valid() const { return fault.load(std::memory_order_acquire) == DeliveryFault::None; }
};
struct TargetDeliveryFault {
    LuaSession* session{};
    uint64_t target{};
    DeliveryFault reason{};
};

struct EnhancedInputEvent
{
    uint64_t subscription{};
    uint64_t sequence{};
    std::shared_ptr<struct EnhancedInputSubscription> owner;
    float elapsed_processed{};
    float elapsed_triggered{};
    double x{};
    double y{};
    double z{};
    EnhancedInputABI::ValueType value_type{EnhancedInputABI::ValueType::Boolean};
};

struct EnhancedInputDebugInfo
{
    bool enabled{};
    bool primary{};
    uint64_t scope_id{};
    uint64_t binding_id{};
    std::string label;
    std::string key;
    std::string trigger_name;
};

struct EnhancedInputTrace
{
    LuaSession* session{};
    std::string line;
};

struct EnhancedInputDispatchState
{
    std::atomic_bool accepting{false};
    std::atomic_bool faults_pending{false};
    std::atomic_bool clone_created{false};
    std::atomic_uint64_t live_bindings{0};
    std::atomic_uint64_t next_event_sequence{1};
    ConsumerBacklogCount consumer_backlog;
    QueueReadiness events_ready;
    QueueReadiness traces_ready;
    std::mutex mutex;
    std::vector<EnhancedInputEvent> events;
    std::vector<EnhancedInputTrace> traces;
    std::vector<EnhancedInputEvent> spare_events;
    std::vector<EnhancedInputTrace> spare_traces;
    QueueCapacity event_capacity;
    QueueCapacity trace_capacity{1024};
};

void write_debug_trace(
    const std::shared_ptr<EnhancedInputDispatchState>& dispatch_state,
    LuaSession* session,
    const EnhancedInputDebugInfo& debug,
    std::string_view stage,
    std::string_view phase,
    uint64_t sequence = 0,
    std::string_view reason = {});

struct EnhancedInputSubscription
{
    uint64_t id{};
    uint64_t target{};
    LuaSession* session{};
    uint64_t callback_token{};
    std::string action_path_utf8;
    std::string phase_name;
    EnhancedInputABI::TriggerEvent phase{EnhancedInputABI::TriggerEvent::None};
    EnhancedInputDebugInfo debug;
    std::atomic_bool active{true};
    std::shared_ptr<TargetDeliveryState> delivery;
    bool delivery_valid() const { return !delivery || delivery->valid(); }
};

class EnhancedInputBackend
{
public:
    EnhancedInputBackend();
    ~EnhancedInputBackend();

    EnhancedInputBackend(const EnhancedInputBackend&) = delete;
    EnhancedInputBackend& operator=(const EnhancedInputBackend&) = delete;

    void initialize();
    std::string enable_delivery_faults(LuaSession& session, uint64_t target);
    std::pair<bool, const char*> target_delivery_valid(LuaSession& session, uint64_t target) const;
    void report_delivery_fault(const EnhancedInputSubscription& subscription, DeliveryFault reason);
    std::optional<TargetDeliveryFault> take_delivery_fault();
    void set_queue_limit(std::size_t limit);
    struct QueueStats { std::size_t queued, high_water; uint64_t rejected, traces_rejected; };
    QueueStats queue_stats() const;
    void consumer_events_remaining(std::size_t count) {
        dispatch_state_->consumer_backlog.remaining(count);
    }
    // Returns false when Unreal still owns one or more native binding objects.
    // In that case the containing DLL must remain loaded until process exit.
    [[nodiscard]] bool shutdown();
    [[nodiscard]] bool available() const { return initialized_.load(); }

    // Explicit-target API. The caller owns lifecycle policy: it decides when
    // an EnhancedInputComponent is meaningful and supplies its exact object path.
    std::pair<uint64_t, std::string> open_target(LuaSession& session, std::string component_path);
    bool close_target(LuaSession& session, uint64_t target);
    std::pair<std::string, std::string> inspect_target(LuaSession& session, uint64_t target);

    std::pair<uint64_t, std::string> subscribe(
        LuaSession& session,
        uint64_t target,
        uint64_t callback_token,
        std::string action_path,
        std::string phase_name,
        EnhancedInputABI::TriggerEvent phase,
        EnhancedInputDebugInfo debug = {});
    bool unsubscribe(LuaSession& session, uint64_t id);
    std::size_t unsubscribe_all(LuaSession& session, bool preserve_targets = false);
    void deactivate(LuaSession& session, uint64_t id);
    void deactivate_all(LuaSession& session);

    std::vector<EnhancedInputEvent> take_events();
    std::vector<EnhancedInputTrace> take_traces();
    void recycle_events(std::vector<EnhancedInputEvent> batch);
    void recycle_traces(std::vector<EnhancedInputTrace> batch);
    void trace(
        LuaSession& session,
        const EnhancedInputDebugInfo& debug,
        std::string_view stage,
        std::string_view phase = {},
        uint64_t sequence = 0,
        std::string_view reason = {});

private:
    class NativeBinding;
    struct Target
    {
        uint64_t id{};
        LuaSession* session{};
        RC::Unreal::FWeakObjectPtr component;
        std::string component_path_utf8;
        std::shared_ptr<TargetDeliveryState> delivery;
        bool has_bound{};
    };
    struct LiveBinding
    {
        RC::Unreal::FWeakObjectPtr component;
        NativeBinding* binding{};
        uint64_t subscription{};
        RC::Unreal::FWeakObjectPtr expected_action;
        const void* action_address{};
        const void* event_address{};
        const void* handle_address{};
        uint32_t expected_handle{};
    };

    // Objects by path, filled by one FindAllOf walk per class on a miss.
    struct CachedObject
    {
        RC::Unreal::UObject* object{};
        int32_t index{};
    };
    using ObjectCache = std::unordered_map<std::wstring, CachedObject>;
    static RC::Unreal::UObject* cached_object(ObjectCache& cache, const std::wstring& path);
    static void fill_cache(ObjectCache& cache, const wchar_t* class_name);
    RC::Unreal::UObject* resolve_component(const std::wstring& path, std::string& error);
    RC::Unreal::UObject* resolve_action(const std::wstring& path);
    bool validate_component(RC::Unreal::UObject* component) const;
    NativeBinding* attach(
        RC::Unreal::UObject* component,
        RC::Unreal::UObject* action,
        const std::shared_ptr<EnhancedInputSubscription>& subscription);
    void detach(LiveBinding& binding);
    void collect_inactive_locked();
    bool unsubscribe_locked(LuaSession& session, uint64_t id);

    // Game-thread only, like every lookup that fills them.
    ObjectCache component_cache_;
    ObjectCache action_cache_;
    std::atomic_bool initialized_{false};
    std::atomic_uint64_t next_target_{1};
    std::atomic_uint64_t next_subscription_{1};
    std::atomic_uint32_t next_binding_handle_{0x80000000u};

    std::shared_ptr<EnhancedInputDispatchState> dispatch_state_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, std::shared_ptr<EnhancedInputSubscription>> subscriptions_;
    std::unordered_set<LuaSession*> inactive_sessions_;
    std::vector<LiveBinding> bindings_;
};
}

#endif
