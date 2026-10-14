#include <EnhancedInputBackend.hpp>
#include <EmbeddedLuaAPI.hpp>
#include <SessionAliasIndex.hpp>
#include <SharedDispatch.hpp>
#include <UE4SSABI.hpp>
#include <Version.hpp>
#include <LifecycleRegistry.hpp>
#include <LifetimeProbe.hpp>
#include <LexImplementation.hpp>
#include <LexReport.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define UE4SSLEB_WINAPI __stdcall
#else
#define UE4SSLEB_WINAPI
#endif

extern "C" __declspec(dllimport) int UE4SSLEB_WINAPI GetModuleHandleExW(
    unsigned long flags,
    const wchar_t* module_address,
    void** module);

extern "C" __declspec(dllimport) unsigned long UE4SSLEB_WINAPI GetEnvironmentVariableA(
    const char* name, char* buffer, unsigned long size);

extern "C" __declspec(dllimport) void UE4SSLEB_WINAPI OutputDebugStringA(const char* message);
extern "C" __declspec(dllimport) void* UE4SSLEB_WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) int UE4SSLEB_WINAPI ReadProcessMemory(
    void*, const void*, void*, std::size_t, std::size_t*);

#undef UE4SSLEB_WINAPI

// G0 spike (src/HookSpike.cpp, feature/hooks-spike only).
namespace UE4SSLuaEventBridge::HookSpike
{
void on_unreal_init();
void on_update();
void shut_down();
bool retains_module();
int lua_entry(const RC::LuaMadeSimple::Lua& lua);
}

namespace UE4SSLuaEventBridge
{
struct LuaSession
{
    uint64_t id{};
    RC::LuaMadeSimple::Lua* lua{};
    int32_t dispatcher_ref{};
    std::atomic_bool active{true};
    bool loop_start_delivered{};
    std::unordered_set<uint64_t> loop_start_callbacks;
};
}

namespace
{
using RC::LuaMadeSimple::Lua;
using UE4SSLuaEventBridge::EnhancedInputABI::TriggerEvent;

constexpr int32_t packed_path_word_count = 64;
constexpr int32_t packed_path_max_length = packed_path_word_count * 8;
constexpr int32_t packed_debug_word_count = 8;
constexpr int32_t packed_debug_max_length = packed_debug_word_count * 8;

class UE4SSLuaEventBridgeMod;
UE4SSLuaEventBridgeMod* active_mod{};

// Reports one diagnostic line to UE4SS.log and to an attached debugger.
void report(std::string_view message, std::string_view detail = {})
{
    std::string text(message);
    text.append(detail);
    Lex::report("UE4SSLuaEventBridge", text);
}

uint32_t configured_queue_check_rate()
{
    std::array<char, 32> value{};
    const auto size = GetEnvironmentVariableA(
        "UE4SSLEB_QUEUE_CHECKS_PER_SECOND", value.data(), static_cast<unsigned long>(value.size()));
    if (size == 0 || size >= value.size()) return UE4SSLuaEventBridge::default_queue_checks_per_second;
    return UE4SSLuaEventBridge::parse_queue_check_rate(std::string_view(value.data(), size));
}

uint32_t configured_limit(const char* name, uint32_t fallback, uint32_t maximum)
{
    std::array<char, 32> value{};
    const auto size = GetEnvironmentVariableA(name, value.data(), static_cast<unsigned long>(value.size()));
    if (size == 0 || size >= value.size()) return fallback;
    uint32_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + size, result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + size && result <= maximum ? result : fallback;
}

bool pin_current_module()
{
    constexpr unsigned long from_address = 0x00000004UL;
    static const bool pinned = [] {
        void* module{};
        return GetModuleHandleExW(
                   from_address,
                   reinterpret_cast<const wchar_t*>(&active_mod),
                   &module) != 0;
    }();
    return pinned;
}


bool decode_packed_text(
    const Lua& lua,
    int32_t word_count,
    int32_t maximum_length,
    bool allow_empty,
    std::string_view description,
    std::string& value,
    std::string& error)
{
    const auto length = lua.get_integer(1);
    if (length < (allow_empty ? 0 : 1) || length > maximum_length)
    {
        error = "invalid encoded " + std::string(description) + " length";
        return false;
    }

    std::array<uint64_t, packed_path_word_count> words{};
    for (int32_t index = 0; index < word_count; ++index)
    {
        words[static_cast<std::size_t>(index)] = static_cast<uint64_t>(lua.get_integer(1));
    }

    value.assign(static_cast<std::size_t>(length), '\0');
    for (int64_t index = 0; index < length; ++index)
    {
        const auto word = words[static_cast<std::size_t>(index / 8)];
        const auto shift = static_cast<uint32_t>((index % 8) * 8);
        const auto byte = static_cast<uint8_t>((word >> shift) & 0xFFu);
        if (byte == 0)
        {
            error = "invalid encoded " + std::string(description) + " byte";
            return false;
        }
        value[static_cast<std::size_t>(index)] = static_cast<char>(byte);
    }
    return true;
}

bool decode_packed_path(const Lua& lua, std::string& path, std::string& error)
{
    return decode_packed_text(
        lua,
        packed_path_word_count,
        packed_path_max_length,
        false,
        "object path",
        path,
        error);
}

class UE4SSLuaEventBridgeMod final :
    public RC::CppUserModBase,
    public RC::Unreal::FUObjectCreateListener,
    public RC::Unreal::FUObjectDeleteListener
{
public:
    UE4SSLuaEventBridgeMod()
    {
        ModName = L"UE4SSLuaEventBridge";
        ModVersion = UE4SSLEB_WIDEN(UE4SSLEB_VERSION);
        ModDescription = L"Game-agnostic native Enhanced Input callbacks for UE4SS Lua mods";
        ModAuthors = L"UE4SS Lua Event Bridge contributors";
        ModIntendedSDKVersion = L"3.0.1-97b7e501";
        active_mod = this;
    }

    ~UE4SSLuaEventBridgeMod() override
    {
        stop_lifecycle_service();
        (void)backend_.shutdown();
        active_mod = nullptr;
    }

    [[nodiscard]] bool prepare_for_unload()
    {
        stop_lifecycle_service();
        UE4SSLuaEventBridge::HookSpike::shut_down();
        const bool backend_released = backend_.shutdown();
        return backend_released && !UE4SSLuaEventBridge::HookSpike::retains_module();
    }

    void on_unreal_init() override {
        backend_.set_queue_limit(configured_limit("UE4SSLEB_MAX_QUEUED_EVENTS", 65536, 1000000));
        backend_.initialize();
        start_lifetime_service();
        UE4SSLuaEventBridge::HookSpike::on_unreal_init();
    }

    void on_update() override
    {
        UE4SSLuaEventBridge::HookSpike::on_update();
        flush_loop_start_callbacks();
        if (!queue_schedule_.due(UE4SSLuaEventBridge::QueueDispatchSchedule::Clock::now())) return;
        flush_delivery_faults();
        using Budget = UE4SSLuaEventBridge::DispatchBudget;
        const Budget budget(Budget::Clock::now(), max_events_per_pass_, std::chrono::microseconds(max_dispatch_us_));
        if (pending_events_.empty())
        {
            backend_.recycle_events(pending_events_.release_buffer());
            pending_events_.load(backend_.take_events());
        }
        UE4SSLuaEventBridge::dispatch_budgeted(pending_events_, budget, [&](auto event) {
            backend_.consumer_events_remaining(pending_events_.size());
            flush_delivery_faults();
            const auto& subscription = event.owner;
            auto* session = subscription ? subscription->session : nullptr;
            if (!subscription)
            {
                return false;
            }
            if (session)
            {
                backend_.trace(*session, subscription->debug, "event_dequeued",
                               subscription->phase_name, event.sequence);
            }
            if (!subscription->active.load() || !subscription->delivery_valid())
            {
                if (session)
                {
                    backend_.trace(
                        *session,
                        subscription->debug,
                        "callback_skipped",
                        subscription->phase_name,
                        event.sequence,
                        "binding_inactive");
                }
                return false;
            }
            if (!session || !session->active.load() || !session->lua)
            {
                if (session)
                {
                    backend_.trace(
                        *session,
                        subscription->debug,
                        "callback_skipped",
                        subscription->phase_name,
                        event.sequence,
                        "lua_session_inactive");
                }
                return false;
            }

            try
            {
                backend_.trace(
                    *session,
                    subscription->debug,
                    "lua_callback_started",
                    subscription->phase_name,
                    event.sequence);
                const auto& lua = *session->lua;
                lua.registry().get_function_ref(session->dispatcher_ref);
                lua.set_integer(static_cast<int64_t>(subscription->callback_token));
                lua.set_integer(static_cast<int64_t>(event.subscription));
                lua.set_string(subscription->action_path_utf8);
                lua.set_string(subscription->phase_name);
                lua.set_float(event.elapsed_processed);
                lua.set_float(event.elapsed_triggered);
                lua.set_number(event.x);
                lua.set_number(event.y);
                lua.set_number(event.z);
                lua.set_integer(static_cast<int64_t>(event.value_type));
                lua.set_integer(static_cast<int64_t>(event.sequence));
                lua.call_function(11, 0);
                backend_.trace(
                    *session,
                    subscription->debug,
                    "lua_callback_completed",
                    subscription->phase_name,
                    event.sequence);
            }
            catch (const std::exception& callback_error)
            {
                backend_.trace(
                    *session,
                    subscription->debug,
                    "lua_callback_failed",
                    subscription->phase_name,
                    event.sequence,
                    callback_error.what());
                // The binding is disabled below; say so even without tracing.
                report("input callback for " + subscription->action_path_utf8 + " (" +
                           subscription->phase_name + ") failed; binding disabled: ",
                       callback_error.what());
                // Lua callbacks run on UE4SS's event-loop thread. Deactivate
                // immediately, then let the next game-thread bridge operation
                // detach the native binding.
                backend_.report_delivery_fault(*subscription, UE4SSLuaEventBridge::DeliveryFault::CallbackError);
                backend_.deactivate(*session, subscription->id);
                flush_delivery_faults();
            }
            catch (...)
            {
                backend_.trace(
                    *session,
                    subscription->debug,
                    "lua_callback_failed",
                    subscription->phase_name,
                    event.sequence,
                    "unknown_lua_exception");
                report("input callback for " + subscription->action_path_utf8 + " (" +
                       subscription->phase_name + ") failed; binding disabled: unknown exception");
                backend_.report_delivery_fault(*subscription, UE4SSLuaEventBridge::DeliveryFault::CallbackError);
                backend_.deactivate(*session, subscription->id);
                flush_delivery_faults();
            }
            return true; // Failed callback attempts also consume the count allowance.
        }, [] { return Budget::Clock::now(); });
        flush_debug_traces();
    }

    void on_lua_start(RC::StringViewType, Lua& lua, Lua& main_lua, Lua& async_lua, Lua* hook_lua) override
    {
        auto session = std::make_unique<UE4SSLuaEventBridge::LuaSession>();
        session->id = next_session_id_.fetch_add(1);
        session->lua = &lua;
        auto* session_ptr = session.get();

        lua.register_function("UE4SSLuaEventBridge_EnableTargetDeliveryFaults", &enable_target_delivery_faults);
        lua.register_function("UE4SSLuaEventBridge_IsTargetDeliveryValid", &is_target_delivery_valid);
        lua.register_function("UE4SSLuaEventBridge_GetVersion", &get_version);
        lua.register_function("UE4SSLuaEventBridge_GetDispatchStats", &get_dispatch_stats);
        lua.register_function("UE4SSLuaEventBridge_GetCapabilities", &get_capabilities);
        lua.register_function("UE4SSLuaEventBridge_IsInGameThread", &is_in_game_thread);
        lua.register_function("UE4SSLuaEventBridge_OpenInputComponent", &open_input_component);
        lua.register_function("UE4SSLuaEventBridge_CloseInputComponent", &close_input_component);
        lua.register_function("UE4SSLuaEventBridge_InspectInputComponent", &inspect_input_component);
        lua.register_function("UE4SSLuaEventBridge_BindAction", &bind_action);
        lua.register_function("UE4SSLuaEventBridge_Unbind", &unbind);
        lua.register_function("UE4SSLuaEventBridge_UnbindAll", &unbind_all);
        lua.register_function("UE4SSLuaEventBridge_UnbindAllPreserveTargets", &unbind_all_preserve_targets);
        lua.register_function("UE4SSLuaEventBridge_TraceScope", &trace_scope);
        lua.register_function("UE4SSLuaEventBridge_RegisterLoopStart", &register_loop_start);
        lua.register_function("UE4SSLuaEventBridge_CancelLoopStart", &cancel_loop_start);
        lua.register_function("UE4SSLuaEventBridge_LifetimeCapture", &lifetime_capture);
        lua.register_function("UE4SSLuaEventBridge_LifetimeValid", &lifetime_valid);
        lua.register_function("UE4SSLuaEventBridge_LifetimeTakeLost", &lifetime_take_lost);
        lua.register_function("UE4SSLuaEventBridge_LifetimeReportLosses", &lifetime_report_losses);
        lua.register_function("UE4SSLuaEventBridge_LifetimeRelease", &lifetime_release);
        lua.register_function("UE4SSLuaEventBridge_HookSpike", &UE4SSLuaEventBridge::HookSpike::lua_entry);

        const auto session_script =
            std::string{"__UE4SSLuaEventBridge_SessionId = "} + std::to_string(session_ptr->id);
        lua.execute_string(session_script);

        std::string lua_api;
        for (const auto chunk : UE4SSLuaEventBridge::embedded_lua_api_chunks)
        {
            lua_api.append(chunk);
        }
        lua.execute_string(lua_api);

        // The setup chunk returns one dispatcher closure. Keeping one registry
        // reference per Lua session avoids retaining one native registry entry
        // for every bind/unbind cycle. The Lua-side tables own individual
        // callbacks and release them immediately on unbind or bind failure.
        session_ptr->dispatcher_ref = lua.registry().make_ref();

        // Publish the session only after all Lua setup has succeeded. Session
        // storage owns the pointer before aliases become visible, and alias
        // publication rolls back completely if an allocation fails.
        const auto register_state = [&](Lua* state) {
            if (!state) return;
            auto* raw_state = state->get_lua_state();
            if (!raw_state) return;
            session_index_.bind(raw_state, session_ptr);
        };

        {
            std::scoped_lock lock(session_mutex_);
            const bool inserted = sessions_.emplace(session_ptr->id, std::move(session)).second;
            if (!inserted)
            {
                throw std::runtime_error("Lua session ID collision");
            }
        }

        try
        {
            register_state(&lua);
            register_state(&main_lua);
            register_state(&async_lua);
            register_state(hook_lua);
            lifetimes_.start_session(session_ptr->id);
        }
        catch (...)
        {
            session_index_.unbind(session_ptr);
            std::scoped_lock lock(session_mutex_);
            sessions_.erase(session_ptr->id);
            throw;
        }
    }

    void on_lua_stop(RC::StringViewType, Lua& lua, Lua&, Lua&, Lua*) override
    {
        auto* session = session_for(lua);
        if (!session) return;

        lifetimes_.stop_session(session->id);
        {
            std::scoped_lock lock(session_mutex_);
            session->loop_start_callbacks.clear();
            session->loop_start_delivered = true;
        }

        if (RC::Unreal::IsInGameThread())
        {
            try
            {
                lua.execute_string(
                    "if __UE4SSLuaEventBridge_StopHelperScopes ~= nil then "
                    "__UE4SSLuaEventBridge_StopHelperScopes() end");
            }
            catch (const std::exception& error)
            {
                report("Lua-stop helper cleanup failed: ", error.what());
            }
            catch (...)
            {
                report("Lua-stop helper cleanup failed: unknown exception");
            }
            // Native detachment is still required after a helper failure.
            // It does not remove mapping contexts owned by the Lua helper.
            backend_.unsubscribe_all(*session);
            session->active.store(false, std::memory_order_release);
        }
        else
        {
            session->active.store(false, std::memory_order_release);
            backend_.deactivate_all(*session);
        }
        session_index_.unbind(session);
        // Keep the inactive record until bridge destruction. Queued events
        // and in-flight child-state calls can still hold its raw address, and
        // session IDs are never reused.
    }

    UE4SSLuaEventBridge::LuaSession* session_for(const Lua& lua)
    {
        return session_index_.find(lua.get_lua_state());
    }

    UE4SSLuaEventBridge::LuaSession* session_for_id(uint64_t id)
    {
        std::scoped_lock lock(session_mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end() || !found->second || !found->second->active.load())
        {
            return nullptr;
        }
        return found->second.get();
    }

    void NotifyUObjectCreated(const RC::Unreal::UObjectBase* object, int32_t index) override
    {
        lifetimes_.invalidate(reinterpret_cast<std::uintptr_t>(object), index);
    }

    void NotifyUObjectDeleted(const RC::Unreal::UObjectBase* object, int32_t index) override
    {
        lifetimes_.invalidate(reinterpret_cast<std::uintptr_t>(object), index);
    }

    void OnUObjectArrayShutdown() override
    {
        probe_.shut_down();
        lifetime_abi_ready_.store(false, std::memory_order_release);
        if (listeners_registered_.exchange(false, std::memory_order_acq_rel))
        {
            RC::Unreal::FUObjectArray::RemoveUObjectCreateListener(this);
            RC::Unreal::FUObjectArray::RemoveUObjectDeleteListener(this);
        }
        lifetimes_.shutdown();
    }

private:
    UE4SSLuaEventBridge::QueueDispatchSchedule queue_schedule_{configured_queue_check_rate()};
    const uint32_t max_events_per_pass_{configured_limit("UE4SSLEB_MAX_EVENTS_PER_PASS", 256, 1000000)};
    const uint32_t max_dispatch_us_{configured_limit("UE4SSLEB_MAX_DISPATCH_US", 2000, 1000000)};
    UE4SSLuaEventBridge::DispatchBacklog<UE4SSLuaEventBridge::EnhancedInputEvent> pending_events_;
    UE4SSLuaEventBridge::DispatchBacklog<UE4SSLuaEventBridge::EnhancedInputTrace> pending_traces_;
    UE4SSLuaEventBridge::LifecycleRegistry lifetimes_;
    std::atomic_bool listeners_registered_{};
    std::atomic_bool lifetime_abi_ready_{};
    UE4SSLuaEventBridge::LifetimeProbe probe_;

    // Runs the layout probe; on success starts observing UObject lifetimes.
    bool start_lifetime_service()
    {
        const auto [checked, verified] = probe_lifetime_abi();
        probe_.record(checked, verified);
        if (!probe_.ready())
        {
            report("object lifetimes unavailable: ", probe_.reason().value_or("unknown"));
            return false;
        }
        RC::Unreal::FUObjectArray::AddUObjectCreateListener(this);
        RC::Unreal::FUObjectArray::AddUObjectDeleteListener(this);
        listeners_registered_.store(true, std::memory_order_release);
        lifetime_abi_ready_.store(true, std::memory_order_release);
        report("object lifetimes available: the UObject layout probe verified ",
            std::to_string(verified) + " of " + std::to_string(checked) + " objects");
        return true;
    }

    bool lifetime_service_ready() const
    {
        return lifetime_abi_ready_.load(std::memory_order_acquire) &&
            listeners_registered_.load(std::memory_order_acquire);
    }

    void stop_lifecycle_service()
    {
        probe_.shut_down();
        lifetime_abi_ready_.store(false, std::memory_order_release);
        if (listeners_registered_.exchange(false, std::memory_order_acq_rel))
        {
            RC::Unreal::FUObjectArray::RemoveUObjectCreateListener(this);
            RC::Unreal::FUObjectArray::RemoveUObjectDeleteListener(this);
        }
        lifetimes_.shutdown();
    }

    void flush_loop_start_callbacks()
    {
        std::vector<std::pair<UE4SSLuaEventBridge::LuaSession*, uint64_t>> pending;
        {
            std::scoped_lock lock(session_mutex_);
            for (auto& [_, owned] : sessions_)
            {
                auto* session = owned.get();
                if (!session || !session->active.load(std::memory_order_acquire) ||
                    session->loop_start_delivered) continue;
                session->loop_start_delivered = true;
                for (const auto token : session->loop_start_callbacks)
                    pending.emplace_back(session, token);
                session->loop_start_callbacks.clear();
            }
        }
        for (const auto& [session, token] : pending)
        {
            if (!session->active.load(std::memory_order_acquire) || !session->lua) continue;
            try
            {
                const auto& lua = *session->lua;
                lua.registry().get_function_ref(session->dispatcher_ref);
                lua.set_integer(-2);
                lua.set_integer(static_cast<int64_t>(token));
                lua.call_function(2, 0);
            }
            catch (const std::exception& error)
            {
                report("loop-start callback failed: ", error.what());
            }
            catch (...)
            {
                report("loop-start callback failed: unknown exception");
            }
        }
    }

    void flush_delivery_faults()
    {
        while (auto fault = backend_.take_delivery_fault()) {
            auto* session = fault->session;
            if (!session || !session->active.load(std::memory_order_acquire) || !session->lua) continue;
            try {
                const auto& lua = *session->lua;
                lua.registry().get_function_ref(session->dispatcher_ref);
                lua.set_integer(-1); // Reserved out-of-band delivery-fault message.
                lua.set_integer(static_cast<int64_t>(fault->target));
                lua.set_string(UE4SSLuaEventBridge::delivery_fault_reason(fault->reason));
                lua.call_function(3, 0);
            } catch (...) {
                report("delivery fault handler failed; target remains disabled");
            }
        }
    }

    void flush_debug_traces()
    {
        using Budget = UE4SSLuaEventBridge::DispatchBudget;
        const Budget budget(Budget::Clock::now(), 32, std::chrono::microseconds(250));
        if (pending_traces_.empty())
        {
            backend_.recycle_traces(pending_traces_.release_buffer());
            pending_traces_.load(backend_.take_traces());
        }
        std::size_t processed{};
        while (!pending_traces_.empty() && budget.permits(processed, Budget::Clock::now()))
        {
            auto trace = pending_traces_.pop();
            ++processed;
            auto* session = trace.session;
            if (!session || !session->active.load(std::memory_order_acquire) || !session->lua)
            {
                continue;
            }
            try
            {
                const auto& lua = *session->lua;
                lua.registry().get_function_ref(session->dispatcher_ref);
                lua.set_integer(0);
                lua.set_string(trace.line);
                lua.call_function(2, 0);
            }
            catch (...)
            {
                // The native trace writer already emitted the same line to
                // OutputDebugString. Logging must not affect input dispatch.
            }
        }
    }

    static int get_version(const Lua& lua)
    {
        lua.set_string(UE4SSLEB_VERSION);
        return 1;
    }

    static int get_dispatch_stats(const Lua& lua)
    {
        if (!active_mod) { lua.set_nil(); return 1; }
        const auto stats = active_mod->backend_.queue_stats();
        lua.set_integer(static_cast<int64_t>(stats.queued));
        lua.set_integer(static_cast<int64_t>(stats.high_water));
        lua.set_integer(static_cast<int64_t>(stats.rejected));
        lua.set_integer(static_cast<int64_t>(stats.traces_rejected));
        return 4;
    }

    static int get_capabilities(const Lua& lua)
    {
        lua.set_integer(6);
        lua.set_bool(active_mod && active_mod->backend_.available());
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_bool(true);
        lua.set_string("97b7e501");
        lua.set_bool(true); // Additive binding_snapshot capability.
        lua.set_bool(true); // Additive target_delivery_faults capability.
        lua.set_bool(true); // Additive loop_start capability.
        lua.set_bool(active_mod && active_mod->lifetime_service_ready());
        // Additive object_lifetimes capability, enabled only after the runtime ABI probe.
        const auto reason = active_mod
            ? active_mod->probe_.reason()
            : std::optional<std::string>{"the bridge is not active"};
        if (reason) lua.set_string(*reason);
        else lua.set_nil();
        // Additive object_lifetimes_reason: why lifetimes are unavailable, nil when available.
        return 15;
    }

    static int is_in_game_thread(const Lua& lua)
    {
        lua.set_bool(RC::Unreal::IsInGameThread());
        return 1;
    }

    static std::pair<TriggerEvent, std::string_view> parse_phase(int64_t phase)
    {
        if (phase == 1) return {TriggerEvent::Triggered, "Triggered"};
        if (phase == 2) return {TriggerEvent::Started, "Started"};
        if (phase == 3) return {TriggerEvent::Ongoing, "Ongoing"};
        if (phase == 4) return {TriggerEvent::Canceled, "Canceled"};
        if (phase == 5) return {TriggerEvent::Completed, "Completed"};
        return {TriggerEvent::None, {}};
    }

    static UE4SSLuaEventBridge::LuaSession* consume_session(const Lua& lua)
    {
        if (!active_mod)
        {
            return nullptr;
        }
        const auto id = static_cast<uint64_t>(lua.get_integer(1));
        return active_mod->session_for_id(id);
    }

    static int register_loop_start(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto token = lua.get_integer(1);
        if (!session || token <= 0)
        {
            lua.set_bool(false);
            lua.set_string("invalid or stopping Lua session");
            return 2;
        }
        std::scoped_lock lock(active_mod->session_mutex_);
        if (session->loop_start_delivered)
        {
            lua.set_bool(false);
            lua.set_string("loop-start notification has already been delivered");
            return 2;
        }
        if (session->loop_start_callbacks.size() >= 4096)
        {
            lua.set_bool(false);
            lua.set_string("loop-start callback capacity exceeded");
            return 2;
        }
        session->loop_start_callbacks.insert(static_cast<uint64_t>(token));
        lua.set_bool(true);
        return 1;
    }

    static int cancel_loop_start(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto token = lua.get_integer(1);
        if (!session || token <= 0)
        {
            lua.set_bool(false);
            return 1;
        }
        std::scoped_lock lock(active_mod->session_mutex_);
        lua.set_bool(session->loop_start_callbacks.erase(static_cast<uint64_t>(token)) != 0);
        return 1;
    }

    static std::optional<UE4SSLuaEventBridge::ObjectLifetimeIdentity> read_object_identity(
        std::uintptr_t address,
        bool require_game_thread = true)
    {
        if (!address || (require_game_thread && !RC::Unreal::IsInGameThread())) return {};
        const auto read = [](const void* source, void* destination, std::size_t size) {
            std::size_t copied{};
            return source && ReadProcessMemory(
                GetCurrentProcess(), source, destination, size, &copied) && copied == size;
        };
        int32_t index{};
        if (!read(
                reinterpret_cast<const void*>(
                    address + RC::Unreal::FWeakObjectPtr::uobject_internal_index_offset),
                &index,
                sizeof(index)) || index < 0) return {};
        const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
        if (!item) return {};
        RC::Unreal::UObject* indexed_object{};
        int32_t serial{};
        const auto item_address = reinterpret_cast<std::uintptr_t>(item);
        if (!read(reinterpret_cast<const void*>(item_address), &indexed_object, sizeof(indexed_object)) ||
            !read(
                reinterpret_cast<const void*>(
                    item_address + RC::Unreal::FWeakObjectPtr::object_item_serial_offset),
                &serial,
                sizeof(serial)) ||
            reinterpret_cast<std::uintptr_t>(indexed_object) != address || serial <= 0) return {};
        return UE4SSLuaEventBridge::ObjectLifetimeIdentity{address, index, serial};
    }

    // Returns how many live objects were checked and how many verified the
    // layout. Walks the object array by index rather than by class name: name
    // lookups can come back empty at initialization. The bound stays inside the
    // array's first 64K-slot chunk, so every index is backed by memory.
    static std::pair<std::size_t, std::size_t> probe_lifetime_abi()
    {
        constexpr int32_t probe_index_limit = 4096;
        const auto read = [](const void* source, void* destination, std::size_t size) {
            std::size_t copied{};
            return source && ReadProcessMemory(
                GetCurrentProcess(), source, destination, size, &copied) && copied == size;
        };
        std::size_t checked{};
        std::size_t verified{};
        try
        {
            for (int32_t index = 0; index < probe_index_limit; ++index)
            {
                const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
                RC::Unreal::UObject* object{};
                if (!item || !read(item, &object, sizeof(object)) || !object) continue;
                ++checked;
                const auto address = reinterpret_cast<std::uintptr_t>(object);
                const auto first = read_object_identity(address, false);
                const auto second = read_object_identity(address, false);
                if (!first || !second || *first != *second || first->index != index) continue;
                if (++verified >= UE4SSLuaEventBridge::LifetimeProbe::required_objects) break;
            }
        }
        catch (...)
        {
        }
        return {checked, verified};
    }

    static std::optional<uint64_t> parse_token(std::string_view value)
    {
        uint64_t token{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), token);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || token == 0)
            return {};
        return token;
    }

    static int lifetime_capture(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto address_value = lua.get_integer(1);
        if (!session || !active_mod->lifetime_service_ready())
        {
            lua.set_nil();
            lua.set_string("object lifetime service is unavailable");
            return 2;
        }
        const auto identity = address_value > 0
            ? read_object_identity(static_cast<std::uintptr_t>(address_value))
            : std::nullopt;
        if (!identity)
        {
            lua.set_nil();
            lua.set_string(RC::Unreal::IsInGameThread()
                ? "address is not a live UObject"
                : "lifetime capture must run on the Unreal game thread");
            return 2;
        }
        const auto result = active_mod->lifetimes_.capture(session->id, *identity);
        if (result.token == 0)
        {
            lua.set_nil();
            lua.set_string(result.failure == UE4SSLuaEventBridge::LifecycleRegistry::CaptureFailure::capacity
                ? "lifetime observation capacity exceeded"
                : result.failure == UE4SSLuaEventBridge::LifecycleRegistry::CaptureFailure::faulted
                    ? "lifetime notification overflow; reload the Lua session"
                    : "Lua session is not registered or is stopping");
            return 2;
        }
        const auto token = std::to_string(result.token);
        lua.set_string(token);
        return 1;
    }

    static int lifetime_valid(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto address_value = lua.get_integer(1);
        const auto token = parse_token(lua.get_string(1));
        if (!session || address_value <= 0 || !token ||
            !active_mod->lifetime_abi_ready_.load(std::memory_order_acquire) ||
            !active_mod->listeners_registered_.load(std::memory_order_acquire))
        {
            lua.set_bool(false);
            return 1;
        }
        const auto identity = read_object_identity(static_cast<std::uintptr_t>(address_value));
        lua.set_bool(identity && active_mod->lifetimes_.valid(session->id, *token, *identity));
        return 1;
    }

    static int lifetime_release(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto token = session ? parse_token(lua.get_string(1)) : std::nullopt;
        lua.set_bool(token && active_mod->lifetimes_.release(session->id, *token));
        return 1;
    }

    static int lifetime_report_losses(const Lua& lua)
    {
        auto* session = consume_session(lua);
        lua.set_bool(session && active_mod->lifetimes_.report_losses(session->id));
        return 1;
    }

    static int lifetime_take_lost(const Lua& lua)
    {
        auto* session = consume_session(lua);
        if (!session || !active_mod->lifetime_abi_ready_.load(std::memory_order_acquire) ||
            !RC::Unreal::IsInGameThread())
        {
            lua.set_nil();
            lua.set_string(!session
                ? "Lua session is not registered or is stopping"
                : !active_mod->lifetime_abi_ready_.load(std::memory_order_acquire)
                    ? "object lifetime service is unavailable"
                    : "lifetime loss draining must run on the Unreal game thread");
            return 2;
        }
        const auto result = active_mod->lifetimes_.take_lost(session->id);
        if (result.overflow)
        {
            lua.set_nil();
            lua.set_string("lifetime notification overflow; discard cached objects and reload the Lua session");
            return 2;
        }
        if (result.unknown_session)
        {
            lua.set_nil();
            lua.set_string("Lua session is not registered or is stopping");
            return 2;
        }
        if (!result.token)
        {
            lua.set_nil();
            return 1;
        }
        const auto token = std::to_string(*result.token);
        lua.set_string(token);
        return 1;
    }

    static int target_delivery_operation(const Lua& lua, bool enable)
    {
        auto* session = consume_session(lua);
        if (!session) {
            lua.set_bool(false); lua.set_string("Lua session is not registered or is stopping"); return 2;
        }
        const auto target = lua.get_integer(1);
        if (target <= 0) {
            lua.set_bool(false); lua.set_string("invalid target handle"); return 2;
        }
        if (enable) {
            const auto error = active_mod->backend_.enable_delivery_faults(*session, static_cast<uint64_t>(target));
            lua.set_bool(error.empty());
            if (!error.empty()) { lua.set_string(error); return 2; }
        } else {
            const auto [valid, reason] = active_mod->backend_.target_delivery_valid(*session, static_cast<uint64_t>(target));
            lua.set_bool(valid);
            if (!valid) { lua.set_string(reason); return 2; }
        }
        return 1;
    }
    static int enable_target_delivery_faults(const Lua& lua) { return target_delivery_operation(lua, true); }
    static int is_target_delivery_valid(const Lua& lua) { return target_delivery_operation(lua, false); }

    static int open_input_component(const Lua& lua)
    {
        if (!active_mod || !active_mod->backend_.available())
        {
            lua.set_nil();
            lua.set_string("Enhanced Input backend is not initialized");
            return 2;
        }
        if (!RC::Unreal::IsInGameThread())
        {
            lua.set_nil();
            lua.set_string("OpenInputComponent must run on the Unreal game thread");
            return 2;
        }

        auto* session = consume_session(lua);
        if (!session)
        {
            lua.set_nil();
            lua.set_string("Lua session is not registered or is stopping");
            return 2;
        }

        std::string component_path;
        std::string error;
        if (!decode_packed_path(lua, component_path, error))
        {
            lua.set_nil();
            lua.set_string(error);
            return 2;
        }

        auto [handle, backend_error] =
            active_mod->backend_.open_target(*session, std::move(component_path));
        if (handle == 0)
        {
            lua.set_nil();
            lua.set_string(backend_error);
            return 2;
        }

        lua.set_integer(static_cast<int64_t>(handle));
        return 1;
    }

    static int inspect_input_component(const Lua& lua)
    {
        const auto fail = [&](std::string_view error) { lua.set_nil(); lua.set_string(error); return 2; };
        if (!active_mod || !active_mod->backend_.available()) return fail("Enhanced Input backend is not initialized");
        if (!RC::Unreal::IsInGameThread()) return fail("InspectInputComponent must run on the Unreal game thread");
        auto* session = consume_session(lua);
        if (!session) return fail("Lua session is not registered or is stopping");
        const auto target = lua.get_integer(1);
        if (target <= 0) return fail("invalid Enhanced Input target handle");
        try {
            auto [snapshot, error] = active_mod->backend_.inspect_target(*session, static_cast<uint64_t>(target));
            if (!error.empty()) return fail(error);
            lua.set_string(snapshot);
            return 1;
        } catch (const std::exception& error) { return fail(error.what()); }
        catch (...) { return fail("binding snapshot failed"); }
    }

    static int close_input_component(const Lua& lua)
    {
        if (!active_mod || !active_mod->backend_.available())
        {
            lua.set_bool(false);
            lua.set_string("Enhanced Input backend is not initialized");
            return 2;
        }
        if (!RC::Unreal::IsInGameThread())
        {
            lua.set_bool(false);
            lua.set_string("CloseInputComponent must run on the Unreal game thread");
            return 2;
        }

        auto* session = consume_session(lua);
        if (!session)
        {
            lua.set_bool(false);
            lua.set_string("Lua session is not registered or is stopping");
            return 2;
        }

        const auto handle_value = lua.get_integer(1);
        if (handle_value <= 0)
        {
            lua.set_bool(false);
            lua.set_string("invalid Enhanced Input target handle");
            return 2;
        }

        if (!active_mod->backend_.close_target(*session, static_cast<uint64_t>(handle_value)))
        {
            lua.set_bool(false);
            const auto message = active_mod->backend_.available()
                ? "Enhanced Input target handle is unknown or belongs to another Lua session: " +
                      std::to_string(handle_value)
                : "Enhanced Input backend shut down while closing target handle: " +
                      std::to_string(handle_value);
            lua.set_string(message);
            return 2;
        }

        lua.set_bool(true);
        return 1;
    }

    static int bind_action(const Lua& lua)
    {
        if (!active_mod || !active_mod->backend_.available())
        {
            lua.set_nil();
            lua.set_string("Enhanced Input backend is not initialized");
            return 2;
        }
        if (!RC::Unreal::IsInGameThread())
        {
            lua.set_nil();
            lua.set_string("BindAction must run on the Unreal game thread");
            return 2;
        }

        auto* session = consume_session(lua);
        if (!session)
        {
            lua.set_nil();
            lua.set_string("Lua session is not registered or is stopping");
            return 2;
        }

        const auto target_value = lua.get_integer(1);
        if (target_value <= 0)
        {
            lua.set_nil();
            lua.set_string("invalid Enhanced Input target handle");
            return 2;
        }
        const auto target = static_cast<uint64_t>(target_value);
        std::string action_path;
        std::string error;
        if (!decode_packed_path(lua, action_path, error))
        {
            lua.set_nil();
            lua.set_string(error);
            return 2;
        }

        const auto [phase, phase_view] = parse_phase(lua.get_integer(1));
        if (phase == TriggerEvent::None)
        {
            lua.set_nil();
            lua.set_string("invalid trigger event");
            return 2;
        }
        const std::string phase_name(phase_view);

        const auto callback_token_value = lua.get_integer(1);
        if (callback_token_value <= 0)
        {
            lua.set_nil();
            lua.set_string("invalid callback token");
            return 2;
        }
        const auto callback_token = static_cast<uint64_t>(callback_token_value);

        UE4SSLuaEventBridge::EnhancedInputDebugInfo debug;
        const auto debug_enabled = lua.get_integer(1);
        if (debug_enabled != 0 && debug_enabled != 1)
        {
            lua.set_nil();
            lua.set_string("invalid debug tracing flag");
            return 2;
        }
        if (debug_enabled == 1)
        {
            const auto scope_id = lua.get_integer(1);
            const auto binding_id = lua.get_integer(1);
            const auto primary = lua.get_integer(1);
            const auto trigger = lua.get_integer(1);
            if (scope_id <= 0 || binding_id <= 0 || (primary != 0 && primary != 1) ||
                (trigger != 1 && trigger != 2))
            {
                lua.set_nil();
                lua.set_string("invalid debug tracing metadata");
                return 2;
            }

            std::string key;
            std::string label;
            if (!decode_packed_text(
                    lua,
                    packed_debug_word_count,
                    packed_debug_max_length,
                    false,
                    "debug key",
                    key,
                    error) ||
                !decode_packed_text(
                    lua,
                    packed_debug_word_count,
                    packed_debug_max_length,
                    false,
                    "debug label",
                    label,
                    error))
            {
                lua.set_nil();
                lua.set_string(error);
                return 2;
            }

            debug.enabled = true;
            debug.primary = primary == 1;
            debug.scope_id = static_cast<uint64_t>(scope_id);
            debug.binding_id = static_cast<uint64_t>(binding_id);
            debug.key = std::move(key);
            debug.label = std::move(label);
            debug.trigger_name = trigger == 1 ? "Tap" : "Hold";
        }

        auto [handle, backend_error] = active_mod->backend_.subscribe(
            *session,
            target,
            callback_token,
            std::move(action_path),
            phase_name,
            phase,
            std::move(debug));
        if (handle == 0)
        {
            lua.set_nil();
            lua.set_string(backend_error);
            return 2;
        }

        lua.set_integer(static_cast<int64_t>(handle));
        return 1;
    }

    static int unbind(const Lua& lua)
    {
        if (!active_mod || !active_mod->backend_.available())
        {
            lua.set_bool(false);
            lua.set_string("Enhanced Input backend is not initialized");
            return 2;
        }
        if (!RC::Unreal::IsInGameThread())
        {
            lua.set_bool(false);
            lua.set_string("Unbind must run on the Unreal game thread");
            return 2;
        }

        auto* session = consume_session(lua);
        if (!session)
        {
            lua.set_bool(false);
            lua.set_string("Lua session is not registered or is stopping");
            return 2;
        }

        const auto handle_value = lua.get_integer(1);
        if (handle_value <= 0)
        {
            lua.set_bool(false);
            lua.set_string("invalid Enhanced Input subscription handle");
            return 2;
        }

        if (!active_mod->backend_.unsubscribe(*session, static_cast<uint64_t>(handle_value)))
        {
            lua.set_bool(false);
            const auto message = active_mod->backend_.available()
                ? "Enhanced Input subscription handle is unknown or belongs to another Lua session: " +
                      std::to_string(handle_value)
                : "Enhanced Input backend shut down while unbinding subscription handle: " +
                      std::to_string(handle_value);
            lua.set_string(message);
            return 2;
        }

        lua.set_bool(true);
        return 1;
    }

    static int unbind_all(const Lua& lua) { return unbind_all_impl(lua, false); }
    static int unbind_all_preserve_targets(const Lua& lua) { return unbind_all_impl(lua, true); }

    static int unbind_all_impl(const Lua& lua, bool preserve_targets)
    {
        if (!active_mod || !active_mod->backend_.available())
        {
            lua.set_integer(0);
            lua.set_bool(false);
            lua.set_string("Enhanced Input backend is not initialized");
            return 3;
        }
        if (!RC::Unreal::IsInGameThread())
        {
            lua.set_integer(0);
            lua.set_bool(false);
            lua.set_string("UnbindAll must run on the Unreal game thread");
            return 3;
        }

        auto* session = consume_session(lua);
        if (!session)
        {
            lua.set_integer(0);
            lua.set_bool(false);
            lua.set_string("Lua session is not registered or is stopping");
            return 3;
        }

        lua.set_integer(static_cast<int64_t>(active_mod->backend_.unsubscribe_all(*session, preserve_targets)));
        lua.set_bool(true);
        return 2;
    }

    static int trace_scope(const Lua& lua)
    {
        auto* session = consume_session(lua);
        const auto scope_id = lua.get_integer(1);
        const auto stage = lua.get_integer(1);
        std::string label;
        std::string error;
        const bool decoded = decode_packed_text(
            lua,
            packed_debug_word_count,
            packed_debug_max_length,
            false,
            "debug label",
            label,
            error);
        if (!session || scope_id <= 0 || (stage != 1 && stage != 2) || !decoded)
        {
            return 0;
        }

        UE4SSLuaEventBridge::EnhancedInputDebugInfo debug;
        debug.enabled = true;
        debug.scope_id = static_cast<uint64_t>(scope_id);
        debug.label = std::move(label);
        active_mod->backend_.trace(
            *session, debug, stage == 1 ? "scope_opened" : "scope_closed");
        return 0;
    }

    UE4SSLuaEventBridge::EnhancedInputBackend backend_;
    std::atomic_uint64_t next_session_id_{1};
    std::mutex session_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<UE4SSLuaEventBridge::LuaSession>> sessions_;
    UE4SSLuaEventBridge::SessionAliasIndex<lua_State, UE4SSLuaEventBridge::LuaSession> session_index_;
};
}

namespace
{
UE4SSLEB_ModHandle start_implementation()
{
    return new UE4SSLuaEventBridgeMod();
}

UE4SSLEB_UninstallResult uninstall_implementation(UE4SSLEB_ModHandle mod)
{
    auto* bridge = static_cast<UE4SSLuaEventBridgeMod*>(mod);
    const bool retain_module = bridge && !bridge->prepare_for_unload();
    if (retain_module)
    {
        // UE4SS unloads C++ mods from its event-loop thread. If Unreal still
        // owns a native binding (including an engine-created clone), retain
        // this DLL so its vtable and destructor code cannot become dangling.
        (void)pin_current_module();
    }
    delete bridge;
    return retain_module ? UE4SSLEB_UNINSTALL_RETAIN_MODULE : UE4SSLEB_UNINSTALL_CAN_UNLOAD;
}
}

LEX_EXPORT_IMPLEMENTATION(UE4SSLEB_VERSION_MAJOR, UE4SSLEB_VERSION_MINOR, UE4SSLEB_VERSION_PATCH,
    &start_implementation, &uninstall_implementation)
