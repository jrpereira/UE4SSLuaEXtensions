// UE4SSLuaLogBridge: the UE4SS C++ mod. Owns the journal, the level resolver,
// the delivery core, one Session per Lua mod, the natives of
// LogBridgeNativeContract.hpp, the worker thread and the engine-tick wake-up.

#include <UE4SSLuaABI.hpp>

#include <EmbeddedLuaAPI.hpp>
#include <Journal.hpp>
#include <LexImplementation.hpp>
#include <LexNatives.hpp>
#include <LexReport.hpp>
#include <LogBridgeNativeContract.hpp>
#include <LogCore.hpp>
#include <LogFormat.hpp>
#include <LogLevels.hpp>
#include <Version.hpp>
#include <Win32Logs.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using RC::LuaMadeSimple::Lua;
using UE4SSLuaLogBridge::NativeContract::ErrorCode;
namespace Contract = UE4SSLuaLogBridge::NativeContract;
namespace Core = UE4SSLuaLogBridge::Core;
namespace Win32 = UE4SSLuaLogBridge::Win32;
using Lex::Natives::Call;
using Lex::Natives::failure;
using Lex::Natives::Reply;
using Lex::Natives::session_gone;
using Lex::Natives::success;
using Lex::Natives::utf16_to_utf8;

constexpr std::string_view own_tag = "UE4SSLuaLogBridge";

void report(std::string_view message)
{
    Lex::report(own_tag, message);
}

int64_t now_time()
{
    return Core::pack_time(Win32::local_time());
}

// --- Sessions --------------------------------------------------------------

struct Session
{
    int64_t id{};
    lua_State* state{};
    std::string mod_name;
    std::string mod_folder; // empty: unknown
    std::atomic_bool active{true};
    std::mutex mutex;
    std::unordered_set<int64_t> loggers;
};

class LogBridgeMod;
// The unload handshake with the engine tick (which can't be unregistered) is
// sequentially consistent: the destructor stores active_bridge and then loads
// tick_users, the tick increments tick_users and then loads active_bridge, so
// either the tick sees nullptr or the destructor sees the tick. Release and
// acquire alone would let each side miss the other's write.
std::atomic<LogBridgeMod*> active_bridge{};
std::atomic<int> tick_users{};
std::atomic<bool> tick_registered{};

bool single_folder_name(std::string_view name)
{
    if (name.empty() || name == "." || name == "..") return false;
    for (char c : name)
    {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

class LogBridgeMod final : public RC::CppUserModBase
{
public:
    LogBridgeMod()
    {
        ModName = L"UE4SSLuaLogBridge";
        ModVersion = UE4SSLLB_WIDEN(UE4SSLLB_VERSION);
        ModDescription = L"Leveled logging for UE4SS Lua mods: per-module levels, files and delivery, crash-safe";
        ModAuthors = L"UE4SS Lua Extension Bridges contributors";
        ModIntendedSDKVersion = L"3.0.1-97b7e501";
        folders_ = Win32::folders(&active_bridge);
        open_journal();
        core_ = std::make_unique<Core::LogCore>(*journal_, sink_, true);
        read_settings(true);
        worker_ = std::thread([this] { worker_loop(); });
        active_bridge.store(this, std::memory_order_release);
    }

    ~LogBridgeMod() override
    {
        active_bridge.store(nullptr, std::memory_order_seq_cst);
        for (int spin = 0; spin < 1000 && tick_users.load(std::memory_order_seq_cst) != 0; ++spin)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::scoped_lock lock(wake_mutex_);
            stop_ = true;
        }
        wake_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        core_->all(Win32::monotonic_ms());
    }

    void on_unreal_init() override
    {
        const bool tick = RC::UE4SSRuntime::IsEngineTickAvailable();
        tick_available_.store(tick, std::memory_order_release);
        tick_decided_.store(true, std::memory_order_release);
        core_->set_tick_available(tick);
        if (tick && !tick_registered.exchange(true, std::memory_order_acq_rel))
        {
            RC::Unreal::Hook::RegisterEngineTickPostCallback([](RC::Unreal::UEngine*, float) { on_engine_tick(); });
        }
        if (!tick) report("the engine tick is unavailable: frame loggers are delivered every 16 ms instead");
        wake();
    }

    void on_lua_start(RC::StringViewType mod_name, Lua& lua, Lua& main_lua, Lua& async_lua, Lua* hook_lua) override
    {
        try
        {
            start_session(mod_name, lua);
        }
        catch (const std::exception& error)
        {
            report(std::string("starting the log bridge for a mod failed: ") + error.what());
        }
        catch (...)
        {
            report("starting the log bridge for a mod failed: unknown exception");
        }
        (void)main_lua;
        (void)async_lua;
        (void)hook_lua;
    }

    void on_lua_stop(RC::StringViewType, Lua& lua, Lua&, Lua&, Lua*) override
    {
        std::shared_ptr<Session> session;
        {
            std::scoped_lock lock(sessions_mutex_);
            const auto found = by_state_.find(lua.get_lua_state());
            if (found == by_state_.end()) return;
            const auto record = sessions_.find(found->second);
            if (record != sessions_.end()) session = record->second;
            by_state_.erase(found);
        }
        if (session) session->active.store(false);
        core_->all(Win32::monotonic_ms());
    }

    std::shared_ptr<Session> find_session(int64_t id)
    {
        std::scoped_lock lock(sessions_mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end() || !found->second->active.load()) return nullptr;
        return found->second;
    }

    Core::LogCore& core() { return *core_; }
    Core::LevelResolver& levels() { return levels_; }
    int64_t recovered() const { return recovered_; }
    bool frame_available() const
    {
        return tick_decided_.load(std::memory_order_acquire) && tick_available_.load(std::memory_order_acquire);
    }

    // Any thread: asks the worker to re-read config.ini (ReloadSettings).
    void request_reload()
    {
        {
            std::scoped_lock lock(wake_mutex_);
            reload_ = true;
            woken_ = true;
        }
        wake_cv_.notify_one();
    }

    void wake()
    {
        {
            std::scoped_lock lock(wake_mutex_);
            woken_ = true;
        }
        wake_cv_.notify_one();
    }

    static void on_engine_tick()
    {
        tick_users.fetch_add(1, std::memory_order_seq_cst);
        auto* bridge = active_bridge.load(std::memory_order_seq_cst);
        if (bridge)
        {
            try
            {
                if (bridge->core_->has_frame_lines())
                {
                    bridge->frame_.store(true, std::memory_order_release);
                    bridge->wake();
                }
            }
            catch (...)
            {
            }
        }
        tick_users.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    Win32::Folders folders_;
    Win32::MappedFile mapped_;
    std::vector<uint8_t> memory_journal_;
    std::unique_ptr<Core::Journal> journal_;
    Win32::FileSink sink_;
    std::unique_ptr<Core::LogCore> core_;
    Core::LevelResolver levels_;
    int64_t recovered_{};
    int64_t settings_stamp_{-1};
    std::atomic<bool> tick_available_{false};
    std::atomic<bool> tick_decided_{false};
    std::atomic<bool> frame_{false};

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool woken_{};
    bool reload_{};
    bool stop_{};
    std::thread worker_;

    std::mutex sessions_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<lua_State*, int64_t> by_state_;
    std::atomic<int64_t> next_session_id_{1};

    std::string logs_folder() const { return folders_.own + "/logs"; }

    // Maps logs/journal.bin; without it (no folder, a second game process)
    // the journal lives in memory and survives no crash.
    void open_journal()
    {
        const auto bytes = static_cast<std::size_t>(Core::Journal::header_bytes + Contract::journal_bytes);
        std::string why = "the bridge's own folder can't be found";
        bool mapped = false;
        if (!folders_.own.empty() && Win32::make_folder(logs_folder()))
        {
            mapped = mapped_.open(logs_folder() + "/journal.bin", bytes, why);
        }
        if (mapped)
        {
            journal_ = std::make_unique<Core::Journal>(mapped_.data(), bytes);
        }
        else
        {
            report("the journal is in memory only, so lines pending at a crash are lost: " + why);
            memory_journal_.assign(bytes, 0);
            journal_ = std::make_unique<Core::Journal>(memory_journal_.data(), bytes);
        }
        const auto previous = journal_->open();
        if (mapped) recover(previous);
    }

    // The previous session's retained lines go to logs/previous-session.log;
    // its undelivered `module` lines are also appended to their own files
    // (before this session rotates them).
    void recover(uint64_t previous)
    {
        const auto records = journal_->recover(previous);
        if (records.empty()) return;
        const auto recovery = Core::assemble_recovery(records);
        const auto undelivered = recovery.undelivered;
        Win32::write_file(logs_folder() + "/previous-session.log", recovery.previous_session);
        for (const auto& [path, lines] : recovery.file_appends)
        {
            const auto slash = path.rfind('/');
            Win32::make_folder(path.substr(0, slash));
            Win32::append_file(path, lines);
        }
        recovered_ = undelivered;
        if (undelivered > 0)
        {
            report(std::to_string(undelivered) + " line(s) of the previous session never reached their target (a "
                "crash?); they are marked * in " + logs_folder() + "/previous-session.log");
        }
    }

    // Levels from config.ini, re-read when the file changes.
    void read_settings(bool first)
    {
        if (folders_.own.empty()) return;
        const auto path = folders_.own + "/config.ini";
        const auto stamp = Win32::file_stamp(path);
        if (stamp == settings_stamp_) return;
        settings_stamp_ = stamp;
        const auto text = Win32::read_file(path, 1024 * 1024);
        std::vector<std::string> ignored;
        std::vector<std::string> names;
        auto table = Core::parse_level_settings(text ? *text : std::string(), &ignored, &names);
        const bool changed = levels_.set_settings(std::move(table), std::move(names));
        for (const auto& line : ignored) report("config.ini: ignored \"" + line + "\"");
        if (changed && !first) report("levels re-read from config.ini");
    }

    // Sleeps until a line, a periodic logger's deadline, a frame, a reload
    // request or stop. No timer of its own: with nothing pending it waits
    // without a timeout (a periodic deadline is at most max_interval_ms away).
    void worker_loop()
    {
        constexpr int64_t idle = Contract::max_interval_ms + 1;
        std::unique_lock lock(wake_mutex_);
        while (!stop_)
        {
            const auto now = Win32::monotonic_ms();
            const auto wait = core_->worker_wait_ms(now, idle);
            const auto woken = [this] { return stop_ || woken_; };
            if (wait >= idle) wake_cv_.wait(lock, woken);
            else wake_cv_.wait_for(lock, std::chrono::milliseconds(wait), woken);
            if (stop_) break;
            woken_ = false;
            const bool reload = std::exchange(reload_, false);
            lock.unlock();
            try
            {
                core_->due(Win32::monotonic_ms(), frame_.exchange(false, std::memory_order_acq_rel));
                if (reload) read_settings(false);
            }
            catch (...)
            {
            }
            lock.lock();
        }
    }

    void register_natives(Lua& lua);

    void start_session(RC::StringViewType mod_name, Lua& lua)
    {
        auto session = std::make_shared<Session>();
        session->id = next_session_id_.fetch_add(1);
        session->state = lua.get_lua_state();
        std::string name;
        if (!utf16_to_utf8(std::wstring_view(mod_name.data(), mod_name.size()), name)) name.clear();
        session->mod_name = name;
        if (!folders_.mods.empty() && single_folder_name(name)) session->mod_folder = folders_.mods + "/" + name;

        register_natives(lua);
        lua.execute_string(std::string(Contract::session_global) + " = " + std::to_string(session->id));
        std::string api;
        for (const auto chunk : UE4SSLuaLogBridge::embedded_lua_api_chunks) api.append(chunk);
        lua.execute_string(api);

        std::scoped_lock lock(sessions_mutex_);
        sessions_.emplace(session->id, session);
        by_state_[session->state] = session->id;
    }
};

// --- Natives -----------------------------------------------------------------

using NativeEntry = Lex::Natives::NativeEntry<LogBridgeMod>;

bool owns_logger(Session& session, int64_t logger)
{
    std::scoped_lock lock(session.mutex);
    return session.loggers.contains(logger);
}

Reply get_version(LogBridgeMod&, Call&)
{
    return success(std::string(UE4SSLLB_VERSION));
}

Reply get_capabilities(LogBridgeMod& bridge, Call&)
{
    return success(Contract::api_version, Contract::journal_bytes, Contract::max_message_bytes,
        static_cast<int64_t>(bridge.frame_available() ? 1 : 0));
}

Reply get_stats(LogBridgeMod& bridge, Call&)
{
    const auto stats = bridge.core().stats();
    return success(stats.written, stats.delivered, stats.pending, stats.stalls, bridge.recovered(), stats.loggers,
        bridge.levels().generation());
}

Reply register_logger(LogBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    auto name = call.string();
    const auto target = call.integer();
    const auto frequency = call.integer();
    const auto interval = call.integer();
    const auto rank = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (target < 0 || target > 1 || frequency < 0 || frequency > 2)
    {
        return failure(ErrorCode::invalid, "Register: unknown target or frequency");
    }
    if (rank < 0 || rank > Core::rank_off) return failure(ErrorCode::invalid, "Register: unknown level");
    if (name.empty()) name = Core::module_name_from_folder(session->mod_name);
    if (!Core::valid_module_name(name))
    {
        return failure(ErrorCode::invalid,
            "Register: a module name is 1 to 64 letters, digits, _, - or inner spaces, and not \"global\"");
    }
    {
        std::scoped_lock lock(session->mutex);
        if (static_cast<int64_t>(session->loggers.size()) >= Contract::max_loggers_per_session)
        {
            return failure(ErrorCode::busy, "Register: a Lua environment holds at most 256 loggers");
        }
    }
    Core::Policy policy;
    policy.target = static_cast<Contract::Target>(target);
    policy.frequency = static_cast<Contract::Frequency>(frequency);
    policy.interval_ms = interval;
    const auto id = bridge.core().register_logger(name, session->mod_folder, policy, Win32::monotonic_ms());
    bridge.levels().set_default(name, static_cast<int>(rank));
    bridge.levels().note(name);
    {
        std::scoped_lock lock(session->mutex);
        session->loggers.insert(id);
    }
    return success(id, name);
}

Reply write_line(LogBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto logger = call.integer();
    const auto rank = call.integer();
    const auto category = call.string();
    const auto message = call.string();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (!owns_logger(*session, logger)) return failure(ErrorCode::invalid, "Write: unknown logger");
    if (rank < 1 || rank > Core::rank_critical) return failure(ErrorCode::invalid, "Write: unknown level");
    if (!Core::valid_category_name(category)) return failure(ErrorCode::invalid, "Write: bad category name");
    const auto now = Win32::monotonic_ms();
    const auto written = bridge.core().write(logger, static_cast<int>(rank), category, message, now_time(), now);
    if (!written.ok) return failure(ErrorCode::io, "Write: the journal refused the line");
    if (written.wake_worker) bridge.wake();
    return success(bridge.levels().generation());
}

Reply level(LogBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto logger = call.integer();
    const auto category = call.string();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (!owns_logger(*session, logger)) return failure(ErrorCode::invalid, "Level: unknown logger");
    if (!Core::valid_category_name(category)) return failure(ErrorCode::invalid, "Level: bad category name");
    const auto generation = bridge.levels().generation();
    const auto rank = bridge.levels().resolve(bridge.core().module(logger), category);
    return success(static_cast<int64_t>(rank), generation);
}

Reply generation(LogBridgeMod& bridge, Call&)
{
    return success(bridge.levels().generation());
}

Reply set_level(LogBridgeMod& bridge, Call& call)
{
    const auto key = call.string();
    const auto name = call.string();
    const auto folded = Core::level_key(key);
    if (!folded) return failure(ErrorCode::invalid, "SetLevel: a key is \"\", a module or module.category");
    std::optional<int> rank;
    if (!name.empty())
    {
        rank = Core::parse_level(name);
        if (!rank) return failure(ErrorCode::invalid, "SetLevel: unknown level " + name);
    }
    bridge.levels().set_override(*folded, rank);
    return success(true);
}

Reply get_level(LogBridgeMod& bridge, Call& call)
{
    const auto key = call.string();
    const auto folded = Core::level_key(key);
    if (!folded) return failure(ErrorCode::invalid, "GetLevel: a key is \"\", a module or module.category");
    return success(std::string(Core::level_name(bridge.levels().resolve_key(*folded))));
}

Reply flush(LogBridgeMod& bridge, Call&)
{
    bridge.core().all(Win32::monotonic_ms());
    return success(true);
}

Reply reload_settings(LogBridgeMod& bridge, Call&)
{
    bridge.request_reload();
    return success(true);
}

Reply add_category(LogBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto logger = call.integer();
    const auto category = call.string();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (!owns_logger(*session, logger)) return failure(ErrorCode::invalid, "AddCategory: unknown logger");
    if (!Core::valid_category_name(category)) return failure(ErrorCode::invalid, "AddCategory: bad category name");
    bridge.levels().note(bridge.core().module(logger), category);
    return success(true);
}

Reply list_loggers(LogBridgeMod& bridge, Call&)
{
    const auto generation = bridge.levels().generation();
    return success(bridge.levels().list(), generation);
}

// Same order as Contract::native_functions; checked at compile time below.
constexpr std::array<NativeEntry, 13> natives{{
    {"GetVersion", "", &get_version},
    {"GetCapabilities", "", &get_capabilities},
    {"GetStats", "", &get_stats},
    {"Register", "isiiii", &register_logger},
    {"Write", "iiiss", &write_line},
    {"Level", "iis", &level},
    {"Generation", "", &generation},
    {"SetLevel", "ss", &set_level},
    {"GetLevel", "s", &get_level},
    {"Flush", "", &flush},
    {"ReloadSettings", "", &reload_settings},
    {"AddCategory", "iis", &add_category},
    {"ListLoggers", "", &list_loggers},
}};

static_assert(Lex::Natives::natives_match_contract<natives, Contract::native_functions>(),
    "native table differs from LogBridgeNativeContract.hpp");

LogBridgeMod* ready(const NativeEntry& entry, Reply& refusal)
{
    auto* bridge = active_bridge.load(std::memory_order_acquire);
    if (!bridge) refusal = Lex::Natives::inactive(entry);
    return bridge;
}

void LogBridgeMod::register_natives(Lua& lua)
{
    Lex::Natives::register_natives<natives, &ready>(lua, Contract::native_prefix);
}
} // namespace

namespace
{
UE4SSLEB_ModHandle start_implementation()
{
    return new LogBridgeMod();
}

UE4SSLEB_UninstallResult uninstall_implementation(UE4SSLEB_ModHandle mod)
{
    delete static_cast<LogBridgeMod*>(mod);
    // The engine-tick callback can't be unregistered and lives in this DLL.
    return tick_registered.load(std::memory_order_acquire) ? UE4SSLEB_UNINSTALL_RETAIN_MODULE
                                                           : UE4SSLEB_UNINSTALL_CAN_UNLOAD;
}

static_assert(UE4SSLEB_IMPLEMENTATION_MAGIC == Contract::implementation_magic,
    "UE4SSLEB_IMPLEMENTATION_MAGIC must be the log bridge's LLB1");
} // namespace

LEX_EXPORT_IMPLEMENTATION(UE4SSLLB_VERSION_MAJOR, UE4SSLLB_VERSION_MINOR, UE4SSLLB_VERSION_PATCH,
    &start_implementation, &uninstall_implementation)
