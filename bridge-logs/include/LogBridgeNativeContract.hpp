#pragma once

// UE4SSLuaLogBridge: internal contract between the native DLL and the embedded
// Lua layer (bridge-logs/lua/logs_api.lua). Mods never see it: they see only
// the `UE4SSLuaLogBridge` table the Lua layer builds. Public API:
// bridge-logs/docs/API.md.
//
// Portable (no Win32, no UE4SS includes), so the native core, its tests and
// the Lua-layer fake-native tests share one description.
//
// ---------------------------------------------------------------------------
// 1. Boundary rules (pinned UE4SS ABI 97b7e501, as the other extensions)
// ---------------------------------------------------------------------------
//   R1  Scalars only: every argument is an integer or a string.
//   R2  Checked arity and types. Each native compares get_stack_size() with
//       its shape and checks every slot before consuming anything. A mismatch
//       is `nil, "invalid", "<Name>: bad arguments"`.
//   R3  get_string pops and stops at NUL: copy the view before any other Lua
//       call. The Lua layer replaces NUL in messages before calling.
//   R4  Natives never throw into Lua and never raise Lua errors.
//   R5  Natives are registered as globals; the Lua layer captures each one
//       into an upvalue table and clears the global.
//   R6  UE4SS raises when a native is called from a coroutine the mod made;
//       the Lua layer pcalls every native and falls back to print().
//
// ---------------------------------------------------------------------------
// 2. Sessions and loggers
// ---------------------------------------------------------------------------
// on_lua_start(mod_name, lua, ...) creates a Session (process-unique id) bound
// to <Mods>/<mod_name>, registers the natives, runs `<session_global> = <id>`
// and the embedded Lua layer. Register creates a logger owned by the session:
// a module name (levels are keyed by it), the session's mod folder (where
// `module` target files go) and a delivery policy. on_lua_stop delivers the
// session's pending lines and retires its loggers.
//
// ---------------------------------------------------------------------------
// 3. Levels
// ---------------------------------------------------------------------------
// Ranks 1..7: TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL, OFF. A line of level
// L is written when L >= the effective rank of its <module>.<category>. The
// Lua layer filters with ranks it caches; the generation counter changes
// whenever any level may have changed (config.ini re-read, SetLevel), and
// the layer then asks again. config.ini is read at start and re-read only on
// ReloadSettings (ModCore Settings' Apply of the Logs page or its config
// reload); the bridge never polls it.
//
// ---------------------------------------------------------------------------
// 4. Writing and delivery
// ---------------------------------------------------------------------------
// Write copies the line into the journal (a memory-mapped file) before it
// returns; delivery to the target follows the logger's policy. See Journal.hpp
// and LogCore.hpp.
//
// ---------------------------------------------------------------------------
// 5. The registry (API 2)
// ---------------------------------------------------------------------------
// The native side is the only place every Lua mod's loggers meet, so it keeps
// the list of modules registered this session: Register records the module
// with its one default category, main, AddCategory each category the Lua
// layer registers for it. ListLoggers returns that list, plus the modules and
// categories config.ini's [Levels] names that have not registered (not
// loaded), each with its effective level and where it comes from. A new
// module or category, like any level change, changes the generation.

#include <array>
#include <cstdint>
#include <string_view>

namespace UE4SSLuaLogBridge::NativeContract
{
// --- Identity -------------------------------------------------------------

inline constexpr int64_t api_version = 2;
inline constexpr uint32_t implementation_magic = 0x4C4C4231; // 'LLB1'
inline constexpr std::string_view lua_global = "UE4SSLuaLogBridge";
inline constexpr std::string_view native_prefix = "UE4SSLuaLogBridge_";
inline constexpr std::string_view session_global = "__UE4SSLuaLogBridge_SessionId";
inline constexpr std::string_view target_ue4ss_commit = "97b7e501";
inline constexpr std::string_view embedded_lua_delimiter = ")UE4SSLLB_LUA";

// --- Limits -----------------------------------------------------------------

inline constexpr int64_t journal_bytes = 4LL * 1024 * 1024;
inline constexpr int64_t max_message_bytes = 64LL * 1024;
inline constexpr int64_t max_module_name = 64;
inline constexpr int64_t max_category_name = 32;
inline constexpr int64_t max_loggers_per_session = 256;
inline constexpr int64_t min_interval_ms = 50;
inline constexpr int64_t max_interval_ms = 3600LL * 1000;

// --- Policies -------------------------------------------------------------

enum class Target : uint8_t { ue4ss = 0, module = 1 };
enum class Frequency : uint8_t { immediate = 0, frame = 1, periodic = 2 };

// --- Error codes ----------------------------------------------------------

enum class ErrorCode : uint8_t
{
    invalid,
    io,
    busy,
};

inline constexpr std::array<std::string_view, 3> error_code_names{"invalid", "io", "busy"};

constexpr std::string_view to_string(ErrorCode code)
{
    return error_code_names[static_cast<std::size_t>(code)];
}

// --- Native function table --------------------------------------------------
// Registered as native_prefix + name. `shape`: i = integer, s = string.
// Failure is always (nil, code, message).

struct NativeFunction
{
    std::string_view name;
    std::string_view shape;
    std::string_view arguments;
    std::string_view results;
};

inline constexpr std::array<NativeFunction, 13> native_functions{{
    {"GetVersion", "", "", "version:str"},
    {"GetCapabilities", "", "", "api:int, journal_bytes:int, max_message_bytes:int, frame:int (1 = engine tick)"},
    {"GetStats", "", "",
     "written:int, delivered:int, pending:int, stalls:int, recovered:int, loggers:int, generation:int"},
    {"Register", "isiiii",
     "session, name:str (\"\" = the mod's folder name), target:int, frequency:int, interval_ms:int, "
     "default_rank:int (0 = none)",
     "logger:int, name:str"},
    {"Write", "iiiss", "session, logger, rank:int, category:str, message:str", "generation:int"},
    {"Level", "iis", "session, logger, category:str", "rank:int, generation:int"},
    {"Generation", "", "", "generation:int"},
    {"SetLevel", "ss", "key:str (\"\" = global, module or module.category), level:str (\"\" clears)", "true"},
    {"GetLevel", "s", "key:str", "level:str (effective)"},
    {"Flush", "", "", "true"},
    {"ReloadSettings", "", "", "true (the worker re-reads config.ini when it changed)"},
    // API 2: the registry of modules and categories (section 5).
    {"AddCategory", "iis", "session, logger, category:str", "true"},
    {"ListLoggers", "", "", "text:str (LevelResolver::list), generation:int"},
}};

} // namespace UE4SSLuaLogBridge::NativeContract
