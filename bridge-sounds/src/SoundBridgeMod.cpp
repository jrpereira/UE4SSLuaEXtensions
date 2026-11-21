// UE4SSLuaSoundBridge: the UE4SS C++ mod. Owns one Session per Lua mod, the
// natives of SoundBridgeNativeContract.hpp and the audio engine.

#include <UE4SSLuaABI.hpp>

#include <EmbeddedLuaAPI.hpp>
#include <LexImplementation.hpp>
#include <LexNatives.hpp>
#include <LexReport.hpp>
#include <SoundBridgeNativeContract.hpp>
#include <SoundEngine.hpp>
#include <SoundMix.hpp>
#include <SoundPaths.hpp>
#include <SoundWav.hpp>
#include <Version.hpp>
#include <Win32Sounds.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using RC::LuaMadeSimple::Lua;
using UE4SSLuaSoundBridge::Engine;
using UE4SSLuaSoundBridge::Core::ErrorCode;
using UE4SSLuaSoundBridge::Core::Failure;
using UE4SSLuaSoundBridge::Core::SoundData;
namespace Contract = UE4SSLuaSoundBridge::NativeContract;
namespace Core = UE4SSLuaSoundBridge::Core;
namespace Win32 = UE4SSLuaSoundBridge::Win32;
using Lex::Natives::Call;
using Lex::Natives::failure;
using Lex::Natives::Reply;
using Lex::Natives::session_gone;
using Lex::Natives::success;
using Lex::Natives::utf16_to_utf8;

void report(std::string_view message)
{
    Lex::report("UE4SSLuaSoundBridge", message);
}

// --- Results ---------------------------------------------------------------

Reply failure(const Failure& failed)
{
    return failure(failed.code, failed.message);
}

// --- Sessions --------------------------------------------------------------

struct Session
{
    int64_t id{};
    lua_State* state{};
    std::string mod_name;
    std::string mod_folder; // empty: the mod's folder is unknown
    std::atomic_bool active{true};

    std::mutex mutex;
    std::unordered_map<int64_t, std::shared_ptr<const SoundData>> sounds;
};

class SoundBridgeMod;
SoundBridgeMod* active_bridge{};

class SoundBridgeMod final : public RC::CppUserModBase
{
public:
    SoundBridgeMod() : engine_([](std::string_view message) { report(message); })
    {
        ModName = L"UE4SSLuaSoundBridge";
        ModVersion = UE4SSLSB_WIDEN(UE4SSLSB_VERSION);
        ModDescription = L"Sound playback for UE4SS Lua mods: WAV files, with pan, volume, pitch and low-pass";
        ModAuthors = L"UE4SS Lua Extension Bridges contributors";
        ModIntendedSDKVersion = L"3.0.1-97b7e501";
        active_bridge = this;
        mods_ = Win32::mods_folder(&active_bridge);
        if (mods_.empty()) report("the Mods folder can't be found from the bridge DLL; Load is unavailable");
    }

    ~SoundBridgeMod() override
    {
        std::vector<std::shared_ptr<Session>> all;
        {
            std::scoped_lock lock(sessions_mutex_);
            for (auto& [_, session] : sessions_) all.push_back(session);
        }
        for (auto& session : all) close_session(*session);
        active_bridge = nullptr;
    }

    void on_lua_start(RC::StringViewType mod_name, Lua& lua, Lua& main_lua, Lua& async_lua, Lua* hook_lua) override
    {
        try
        {
            start_session(mod_name, lua);
        }
        catch (const std::exception& error)
        {
            report(std::string("starting the sound bridge for a mod failed: ") + error.what());
        }
        catch (...)
        {
            report("starting the sound bridge for a mod failed: unknown exception");
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
        if (session) close_session(*session);
    }

    std::shared_ptr<Session> find_session(int64_t id)
    {
        std::scoped_lock lock(sessions_mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end() || !found->second->active.load()) return nullptr;
        return found->second;
    }

    Engine& engine() { return engine_; }
    int64_t next_sound_id() { return next_sound_id_.fetch_add(1); }
    int64_t next_play_id() { return next_play_id_.fetch_add(1); }

    // Loaded sounds and their sample bytes across every live session.
    std::pair<int64_t, int64_t> totals()
    {
        int64_t count = 0, bytes = 0;
        std::scoped_lock lock(sessions_mutex_);
        for (auto& [_, session] : sessions_)
        {
            std::scoped_lock session_lock(session->mutex);
            for (auto& [__, sound] : session->sounds)
            {
                ++count;
                bytes += static_cast<int64_t>(sound->bytes());
            }
        }
        return {count, bytes};
    }

private:
    Engine engine_;
    std::string mods_;
    std::mutex sessions_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<lua_State*, int64_t> by_state_;
    std::atomic<int64_t> next_session_id_{1};
    std::atomic<int64_t> next_sound_id_{1};
    std::atomic<int64_t> next_play_id_{1};

    void register_natives(Lua& lua);

    void start_session(RC::StringViewType mod_name, Lua& lua)
    {
        auto session = std::make_shared<Session>();
        session->id = next_session_id_.fetch_add(1);
        session->state = lua.get_lua_state();
        std::string name;
        if (!utf16_to_utf8(std::wstring_view(mod_name.data(), mod_name.size()), name)) name.clear();
        session->mod_name = name;
        // The mod name must be exactly one folder name.
        if (!mods_.empty() && Core::single_folder_name(name))
        {
            session->mod_folder = mods_ + "/" + name;
        }
        else if (!mods_.empty())
        {
            report("mod \"" + name + "\": its name isn't one folder name, so it can load sounds only by absolute path");
        }

        register_natives(lua);
        lua.execute_string(std::string(Contract::session_global) + " = " + std::to_string(session->id));
        std::string api;
        for (const auto chunk : UE4SSLuaSoundBridge::embedded_lua_api_chunks) api.append(chunk);
        lua.execute_string(api);

        std::scoped_lock lock(sessions_mutex_);
        sessions_.emplace(session->id, session);
        by_state_[session->state] = session->id;
    }

    void close_session(Session& session)
    {
        session.active.store(false);
        std::unordered_map<int64_t, std::shared_ptr<const SoundData>> sounds;
        {
            std::scoped_lock lock(session.mutex);
            sounds.swap(session.sounds);
        }
        for (auto& [id, _] : sounds) engine_.stop_sound(id);
        // The Session record stays until bridge destruction; ids are never reused.
    }
};

// --- Natives -----------------------------------------------------------------

using NativeEntry = Lex::Natives::NativeEntry<SoundBridgeMod>;

std::shared_ptr<const SoundData> find_sound(Session& session, int64_t id)
{
    std::scoped_lock lock(session.mutex);
    const auto found = session.sounds.find(id);
    return found == session.sounds.end() ? nullptr : found->second;
}

Reply get_version(SoundBridgeMod&, Call&)
{
    return success(std::string(UE4SSLSB_VERSION));
}

Reply get_capabilities(SoundBridgeMod&, Call&)
{
    return success(Contract::api_version, Contract::max_voices, Contract::max_file_bytes,
        std::string(Contract::backend_name), std::string(Contract::target_ue4ss_commit));
}

Reply get_stats(SoundBridgeMod& bridge, Call&)
{
    const auto [sounds, bytes] = bridge.totals();
    const auto stats = bridge.engine().stats();
    return success(sounds, bytes, stats.playing, stats.started, stats.dropped, stats.stolen, stats.device,
        stats.device_changes);
}

Reply load(SoundBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto path = call.string();
    const auto max_ms = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    auto normalized = Core::normalize_sound_path(path);
    if (!normalized) return failure(normalized.failure().code, "Load " + normalized.failure().message);
    auto absolute = Core::resolve_sound_path(normalized.value(), session->mod_folder);
    if (!absolute) return failure(absolute.failure().code, "Load " + absolute.failure().message);
    {
        std::scoped_lock lock(session->mutex);
        if (static_cast<int64_t>(session->sounds.size()) >= Contract::max_sounds_per_session)
        {
            return failure(ErrorCode::io, "Load " + path + ": a Lua environment holds at most 4096 sounds");
        }
    }
    auto bytes = Win32::read_sound_file(absolute.value(), normalized.value(), Contract::max_file_bytes);
    if (!bytes) return failure(bytes.failure().code, "Load " + bytes.failure().message);
    auto decoded = Core::decode_wav(bytes.value(), max_ms < 0 ? -1 : max_ms);
    if (!decoded) return failure(decoded.failure().code, "Load " + normalized.value() + ": " + decoded.failure().message);
    auto sound = std::make_shared<const SoundData>(std::move(decoded.value()));
    const auto id = bridge.next_sound_id();
    {
        std::scoped_lock lock(session->mutex);
        session->sounds.emplace(id, sound);
    }
    // The audio thread opens the device and a voice now, so the first play waits for neither.
    bridge.engine().start();
    bridge.engine().prepare(sound->format());
    return success(id, static_cast<int64_t>(sound->frames()), static_cast<int64_t>(sound->channels),
        static_cast<int64_t>(sound->rate), normalized.value());
}

Reply release(SoundBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    bool found = false;
    {
        std::scoped_lock lock(session->mutex);
        found = session->sounds.erase(id) != 0;
    }
    if (found) bridge.engine().stop_sound(id);
    return success(true);
}

Reply play(SoundBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto sound_id = call.integer();
    const auto pan = call.integer();
    const auto volume = call.integer();
    const auto pitch = call.integer();
    const auto lowpass = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    auto sound = find_sound(*session, sound_id);
    if (!sound) return failure(ErrorCode::invalid, "Play: the sound was released");
    Core::PlayRequest request;
    request.owner = session->id;
    request.play = bridge.next_play_id();
    request.sound = sound_id;
    request.data = std::move(sound);
    request.params = Core::play_params(pan, volume, pitch, lowpass);
    const auto id = request.play;
    bridge.engine().play(std::move(request));
    return success(id);
}

Reply stop(SoundBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    bridge.engine().stop_play(session->id, id);
    return success(true);
}

Reply stop_sound(SoundBridgeMod& bridge, Call& call)
{
    auto session = bridge.find_session(call.integer());
    const auto id = call.integer();
    if (!session) return failure(ErrorCode::invalid, session_gone());
    if (find_sound(*session, id)) bridge.engine().stop_sound(id);
    return success(true);
}

// Same order as Contract::native_functions; checked at compile time below.
constexpr std::array<NativeEntry, 8> natives{{
    {"GetVersion", "", &get_version},
    {"GetCapabilities", "", &get_capabilities},
    {"GetStats", "", &get_stats},
    {"Load", "isi", &load},
    {"Release", "ii", &release},
    {"Play", "iiiiii", &play},
    {"Stop", "ii", &stop},
    {"StopSound", "ii", &stop_sound},
}};

static_assert(Lex::Natives::natives_match_contract<natives, Contract::native_functions>(),
    "native table differs from SoundBridgeNativeContract.hpp");

SoundBridgeMod* ready(const NativeEntry& entry, Reply& refusal)
{
    if (!active_bridge) refusal = Lex::Natives::inactive(entry);
    return active_bridge;
}

void SoundBridgeMod::register_natives(Lua& lua)
{
    Lex::Natives::register_natives<natives, &ready>(lua, Contract::native_prefix);
}
} // namespace

namespace
{
UE4SSLEB_ModHandle start_implementation()
{
    return new SoundBridgeMod();
}

UE4SSLEB_UninstallResult uninstall_implementation(UE4SSLEB_ModHandle mod)
{
    delete static_cast<SoundBridgeMod*>(mod);
    return UE4SSLEB_UNINSTALL_CAN_UNLOAD;
}

static_assert(UE4SSLEB_IMPLEMENTATION_MAGIC == Contract::implementation_magic,
    "UE4SSLEB_IMPLEMENTATION_MAGIC must be the sound bridge's LSB1");
} // namespace

LEX_EXPORT_IMPLEMENTATION(UE4SSLSB_VERSION_MAJOR, UE4SSLSB_VERSION_MINOR, UE4SSLSB_VERSION_PATCH,
    &start_implementation, &uninstall_implementation)
