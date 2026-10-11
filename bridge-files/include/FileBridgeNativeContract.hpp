#pragma once

// UE4SSLuaFileBridge: internal contract between the native DLL and the
// embedded Lua layer (bridge-files/lua/files_api.lua).
//
// Status: G-spec review applied (L's SPEC-REVIEW.md M1-M15 and K's decisions
// K1-K4, F9, F12, 2026-10-10); ready to freeze. It implements the public API in
// bridge-files/docs/API.md. Nothing here is visible to mods: mods see only the
// `UE4SSLuaFileBridge` table that the Lua layer builds. Changes after G-spec go
// through K and reach F, L and D together.
//
// This header is portable (no Win32, no UE4SS includes) so the portable core,
// its tests and the Lua-layer fake-native tests can share one description.
//
// ---------------------------------------------------------------------------
// 0. Build
// ---------------------------------------------------------------------------
// * bridge-files/CMakeLists.txt calls
//       add_bridge_bootstrap(PRODUCT UE4SSLuaFileBridge MAGIC 0x4C464231 ...)
//   ('LFB1'), and the implementation target defines
//   UE4SSLEB_IMPLEMENTATION_MAGIC to the same value (shared/contract/ImplementationABI.h
//   defaults to the event bridge's 'LEB1' otherwise). `implementation_magic`
//   below must equal both.
// * Tail delivery builds against E's shared dispatch code: INTERFACE target
//   UE4SSXBDispatch, namespace UE4SSXB, includes <dispatch/...>; portable tests
//   add -I<repo>/shared.
// * Import library: bridge-files/abi/UE4SS-97b7e501.def is the event bridge's
//   list plus three LuaMadeSimple exports verified in UE4SS 97b7e501 (K1):
//       ?get_stack_size@Lua@LuaMadeSimple@RC@@QEBAHXZ
//       ?is_string@Lua@LuaMadeSimple@RC@@QEBA_NH@Z
//       ?is_integer@Lua@LuaMadeSimple@RC@@QEBA_NH@Z
//   (all three confirmed in the installed UE4SS.dll's export table with
//   `objdump -p`). bridge-events' .def is unchanged.
// * The Lua layer is embedded like bridge-events': CMake splits files_api.lua
//   into 7000-byte raw-string chunks in a generated EmbeddedLuaAPI.hpp, with
//   delimiter `)UE4SSLFB_LUA`.
//
// ---------------------------------------------------------------------------
// 1. Boundary rules (pinned UE4SS ABI 97b7e501)
// ---------------------------------------------------------------------------
// Natives use only: register_function, execute_string,
// registry().make_ref/get_function_ref, get_stack_size, is_string,
// is_integer, is_function, get_string, get_integer,
// set_nil/bool/integer/number/string, call_function.
//
//   R1  Scalars only. Every argument is an integer or a string; every result
//       is nil, boolean, integer, number or string. Lists and records cross as
//       one string in the record format (section 6). (UE4SS also exports
//       enough to build tables; records were kept as simpler to fake in Lua
//       tests.)
//   R2  Checked arity and types (K1). Each native first compares
//       get_stack_size() with its declared argument count and checks each
//       slot with is_integer/is_string, then consumes arguments left to right
//       (get_integer/get_string remove argument 1). A mismatch returns
//       `nil, "invalid", "<Name>: bad arguments"` and touches nothing. The
//       Lua layer still passes exactly the listed arguments ("" for an absent
//       string, 0 or -1 for an absent integer as listed); the native check is
//       defence in depth.
//   R3  Inbound strings stop at the first NUL (get_string is
//       lua_tostring + strlen, and it pops the value), so the native copies
//       the view into a std::string before any other Lua API call. Paths and
//       names never contain NUL (Lua returns `invalid` first). File content
//       crosses with the data escape (section 5). Outbound strings are
//       binary-safe (set_string is lua_pushlstring).
//   R4  Natives never throw into Lua and never raise Lua errors. An escaping
//       C++ exception becomes `nil, "io", "<Name>: internal error: <what>"`.
//   R5  Natives are registered as globals (register_function has no other
//       target). The Lua layer captures every name in `native_functions` into
//       an upvalue table at load, asserts each is present, and sets each
//       global to nil.
//   R6  Coroutines (M9). UE4SS raises a Lua error when a registered C function
//       is called from a lua_State not created through Lua::new_thread, i.e.
//       from any coroutine the mod creates with coroutine.create/wrap. That
//       error is UE4SS's, before the native runs. The Lua layer pcalls every
//       native call and maps a raised error to `nil, "invalid", message`.
//   R7  No native holds a session, policy, stream or subscription lock while
//       calling a Lua API that can allocate or run Lua (set_string,
//       get_string on a number, call_function). Results are computed under
//       the lock, copied, the lock is released, then pushed.
//
// ---------------------------------------------------------------------------
// 2. Start-up per Lua environment (on_lua_start)
// ---------------------------------------------------------------------------
// UE4SS calls on_lua_start(mod_name, lua, main_lua, async_lua, hook_lua) once
// for every Lua mod; mod_name is the mod's folder name under Mods/
// (LuaMod::get_name()). For each call the native side:
//
//   1. Creates a Session: id (uint64, process-unique, never reused), the
//      mod name, the per-session location paths, the safe-set policy, and
//      empty stream, subscription and policy tables. `mod` is bound here:
//      <Mods>/<mod_name>, where <Mods> is three folders above the bridge's
//      own versioned DLL (Mods/0_ModCore_UE4SSLuaFileBridge/dlls/versions/
//      *.dll), checked to lie inside the game root, with
//      <game>/Dawnwalker/Binaries/Win64/ue4ss/Mods as fallback. `moddata` and
//      `temp` are bound from the same name. A mod name that isn't one valid
//      path segment leaves `mod`, `moddata` and `temp` unbound: Locations
//      reports them with an empty path field (Lua: path = nil,
//      exists = false), and any path through them is `invalid` (M6).
//   2. Registers every `native_functions` entry on `lua` as
//      native_prefix + name.
//   3. Executes `<session_global> = <id>`, then the embedded files_api.lua.
//      The chunk reads and clears the session global (keeping the id as an
//      upvalue), captures and clears the natives (R5), defines the global
//      `UE4SSLuaFileBridge`, and RETURNS ONE FUNCTION: the dispatcher
//      (section 7). No other global is defined.
//   4. Stores registry().make_ref() of that dispatcher in the Session.
//   5. Only after all of that succeeds, publishes the session. On any failure
//      nothing is published and the error is logged; that mod simply has no
//      UE4SSLuaFileBridge.
//
// Every native that acts for a mod takes the session id as its first
// argument. The id alone identifies the session (M8): it is not compared with
// the calling lua_State, because hook_lua is created lazily after
// on_lua_start (as in bridge-events' session_for_id). An unknown or stopped
// id is `nil, "invalid", "Lua session is not registered or is stopping"`.
//
// ---------------------------------------------------------------------------
// 3. Access environments as native policies
// ---------------------------------------------------------------------------
// A Lua environment carries one policy id (int64 > 0). Policies are native,
// immutable and owned by the session that created them.
//
//   * SafePolicy(session) returns the session's safe-set policy, the same id
//     every call: game ro, user ro, mod rw, temp rw + delete.
//   * AddPath(session, parent, path, mode, extensions, flags) validates the
//     arguments, resolves `path`, and returns a NEW policy id whose grant list
//     is the parent's plus this grant. A grant whose resolved path equals
//     (case-insensitively) an existing grant's path replaces it in the new
//     list. The parent never changes. Lua raises on failure
//     ("<code>: <message>", level 2), as API.md requires.
//   * ReleasePolicy(session, policy) frees one id. On the safe-set id it is a
//     no-op returning true (M10). Releasing doesn't affect policies derived
//     from it (each holds its own flattened grant list, storage shared
//     internally), nor streams or subscriptions opened through it.
//   * Garbage collection (M11): no native is called from __gc. The Lua
//     layer's __gc only queues policy and stream ids; the queue is drained
//     (ReleasePolicy / StreamClose) at the start of the next helper call in
//     that Lua environment. Whatever is left is freed natively at session stop.
//   * Limits: 256 grants per policy, 4096 live policies per session. Beyond
//     either: `io` (F9). On the policy limit Lua runs one collectgarbage(),
//     drains its queue and retries once.
//
// Grant fields (native representation, `Grant` below):
//   path        resolved real path (UTF-16 final-path form), plus the text the
//               mod gave, for messages
//   mode        stat | ro | wo | rw | ao | inherit
//   extensions  inherit | list (possibly empty = covers no files); lowercase,
//               no dot; compared case-insensitively. An entry may not contain
//               `.`, `/`, `\`, `;`, `< > : " | ? *` or a control character (M5)
//   delete      bool, never inherited (default false)
//   recursive   bool (default true); false covers the path and its direct
//               children only
//
// The check (one per path an operation touches; `check_steps` below):
//   1. Normalize (API.md "Paths"). Syntax faults: `invalid`; `..` above the
//      location or root, device/UNC/reserved names: `outside_root`.
//   2. Resolve the real path: open the deepest existing ancestor (following
//      links), GetFinalPathNameByHandleW, append the non-existing rest.
//      Remove, RemoveTree, Move (both ends) and the bridge's own .bak and
//      temporary siblings act on a link itself: for them the parent is
//      resolved and the leaf appended unfollowed (M1). Everything else,
//      including Stat, follows the leaf.
//   3. Containment: the real path must equal or lie under the final path of
//      `game` or `user`; otherwise `outside_root`.
//   4. Coverage: grant paths are resolved the same way (memoized per call). A
//      grant covers the target when the target equals the grant path or lies
//      under it (direct children only when recursive=false) AND, for a file
//      (existing or being created), its extension is in the grant's effective
//      extensions. Folders are always covered. Non-covering grants are skipped.
//   5. Winner: the covering grant with the most path segments. Grant paths are
//      unique within a policy, so there are no ties.
//   6. Effective mode and extensions of a grant with `inherit` come from the
//      most specific OTHER grant covering the grant's own path (as a folder),
//      recursively; the safe set's root grants have explicit values, so this
//      terminates. They depend on the policy, not on the target, so they are
//      worked out once per policy (F5).
//   7. required_permissions(access) must be a subset of
//      permissions_for(effective mode, delete); otherwise `denied`. `delete`
//      with an effective mode that can't write is inert (F6).
//   8. Fixed denials, whatever the grants (`denied`):
//      - delete floor, for Remove, RemoveTree and the Move source: any
//        location's own path, any drive root, the user profile root, and any
//        folder that CONTAINS a location, unless the winning grant for that
//        folder was added on exactly that path with delete = true (K4/F12);
//      - moddata/.owner for anything but stat and read.
//   9. Fixed savegames rules (`invalid`): no non-atomic WriteText, no
//      truncating Open.
// The check isn't atomic with the operation (a junction could be swapped in
// between). The sandbox prevents mistakes; it isn't a security boundary.
//
// ---------------------------------------------------------------------------
// 4. Errors
// ---------------------------------------------------------------------------
// Every native returns its success values first; none is nil. On failure it
// returns exactly three values: nil, code, message.
//   code     one of `error_code_names` (stable, for program logic)
//   message  English, "<Operation> <path>: <reason>", plus "(win32 <n>)" for
//            an operating-system failure. Wording isn't part of the contract.
// The Lua layer tests success with `~= nil` and passes failures through
// unchanged (AddPath: raises). Win32 mapping: `win32_error_mapping`. Every
// capacity limit (grants, policies, streams, subscriptions) is `io` (F9).
//
// ---------------------------------------------------------------------------
// 5. Data escape for inbound content (R3)
// ---------------------------------------------------------------------------
// Content written by WriteText, Append and StreamWrite crosses as
// (byte_length:int, escaped:string). The Lua layer escapes bytes 0 and 1 only:
//     \0 -> \1\2      \1 -> \1\1
//     escaped = text:find("[%z\1]") and text:gsub("[%z\1]", ESC) or text
// The native decodes with `decode_escaped`; a malformed escape or a decoded
// length other than byte_length is `invalid`. Content returned to Lua
// (ReadText, ReadBytes, Tail) is raw.
//
// ---------------------------------------------------------------------------
// 6. Record format for structured results (R1)
// ---------------------------------------------------------------------------
// One string: records separated by '\n', fields by '\t'; empty fields are
// kept. Booleans are "1"/"0"; integers are decimal; times are decimal Unix
// seconds with up to 7 fraction digits. Field order is fixed by the `*_fields`
// arrays. An empty result is "". List skips any entry whose name isn't valid
// UTF-16 or contains '\t' or '\n' (possible through the NT API), and logs it
// once per call (M12; API.md 27).
//
// ---------------------------------------------------------------------------
// 7. Threads, delivery and lifetime
// ---------------------------------------------------------------------------
// Synchronous natives run on whatever registered Lua thread calls them (game
// thread, LoopAsync, hooks; not mod-made coroutines, R6). Session state is
// guarded by one mutex per session, a stream by its own mutex; file I/O runs
// outside the session mutex; R7 applies throughout. Natives never require the
// game thread and never call into Unreal.
//
// Tail delivery uses shared/dispatch (UE4SSXB::QueueDispatchSchedule,
// DispatchBudget, DispatchBacklog). In on_update, when the schedule is due,
// the bridge polls each active subscription's file (size via the held handle;
// identity by re-opening the path at most every `tail_identity_check_ms`),
// reads at most what the budget allows (tuning: `tail_read_bytes_per_pass`
// across all subscriptions, at most `tail_read_bytes_per_subscription` from
// one file, round-robin by subscription id from where the last pass stopped),
// splits lines, and calls the session's
// dispatcher on the session's root `lua` state:
//     dispatcher(kind, token, text, offset, flags)
// This is the same thread, root state and lock discipline that bridge-events
// uses for its dispatcher today (M15): on_update, no bridge lock held during
// call_function.
//   kind = data    text is a line (no terminator) or a chunk; offset is its
//                  byte offset; flags has DeliveryFlag bits (reset, partial;
//                  Lua maps them to info.reset and info.partial, M2).
//   kind = closed  the native side closed the subscription (file error,
//                  limit, a dispatcher error for that token). text is
//                  "<code>\t<message>". The bridge has already logged it. Lua
//                  sets sub.closed = true and drops the callback; the mod's
//                  callback isn't called (K2).
//   kind = stop    the session is stopping (on_lua_stop or reload); token,
//                  text, offset and flags are 0/"". Lua clears all callback
//                  and queue state (M7). Delivered only on the game thread,
//                  where on_lua_stop runs it, as bridge-events does.
// The dispatcher returns nothing. If it raises for a data delivery (the mod's
// callback failed), the bridge closes that subscription and logs the path and
// error; no `closed` kind follows, because the Lua layer already dropped it.
// The file is the buffer: only the held partial line (<= 1 MiB) is buffered
// natively, so nothing is lost while the budget defers delivery. Budget
// defaults: 256 deliveries and 2000 us per pass, overridable with
// UE4SSLFB_MAX_EVENTS_PER_PASS / UE4SSLFB_MAX_DISPATCH_US.
//
// on_lua_stop (and reload), in order: deliver `stop` if on the game thread;
// mark the session inactive (natives then fail with `invalid`); close every
// subscription (no further dispatch); flush and close every stream; free every
// policy. The Session record stays until bridge destruction (ids are never
// reused), as in bridge-events.
//
// Bridge start (before the first on_lua_start): resolve the game and user
// roots and the shared locations, then empty `temp` in every
// <user>/Saved/ModData/* folder that has an `.owner` marker.

// ---------------------------------------------------------------------------
// 8. Known limitations (1.0.1)
// ---------------------------------------------------------------------------
// * Results are pushed to Lua after the native's error handling: if Lua runs
//   out of memory while receiving one (a read near max_read_bytes), the error
//   is Lua's own and is not turned into `nil, code, message`.
// * Temporary siblings left by a crash mid-write ("<name>.xbtmp-<pid>-<n>")
//   are removed at bridge start only under ModData folders with an .owner
//   marker; elsewhere (mod folders, savegames) they stay and List shows them.
// * A Move of a link across volumes is refused (`invalid`): a copy would
//   follow the link and leave a plain file.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace UE4SSLuaFileBridge::NativeContract
{
// --- Identity -------------------------------------------------------------

inline constexpr int64_t api_version = 1;
inline constexpr uint32_t implementation_magic = 0x4C464231; // 'LFB1'
inline constexpr std::string_view lua_global = "UE4SSLuaFileBridge";
inline constexpr std::string_view native_prefix = "UE4SSLuaFileBridge_";
inline constexpr std::string_view session_global = "__UE4SSLuaFileBridge_SessionId";
inline constexpr std::string_view target_ue4ss_commit = "97b7e501";
inline constexpr std::string_view embedded_lua_delimiter = ")UE4SSLFB_LUA";

// --- Limits (all capacity limits fail with `io`) ---------------------------

inline constexpr int64_t max_read_bytes = 64LL * 1024 * 1024;
inline constexpr int64_t max_grants_per_policy = 256;
inline constexpr int64_t max_policies_per_session = 4096;
inline constexpr int64_t max_streams_per_session = 64;
inline constexpr int64_t max_subscriptions_per_session = 64;
inline constexpr int64_t stream_buffer_bytes = 64 * 1024;
inline constexpr int64_t tail_max_line_bytes = 1024 * 1024;
inline constexpr int64_t tail_identity_check_ms = 250;
// Tuning (not capacity limits): Tail file reads per dispatch pass (20 passes
// per second by default), so polling can't stall UE4SS's update thread.
inline constexpr int64_t tail_read_bytes_per_pass = 256 * 1024;
inline constexpr int64_t tail_read_bytes_per_subscription = 64 * 1024;
inline constexpr uint32_t default_max_deliveries_per_pass = 256;
inline constexpr uint32_t default_max_dispatch_us = 2000;

// --- Error codes ----------------------------------------------------------

enum class ErrorCode : uint8_t
{
    not_found,
    denied,
    exists,
    outside_root,
    read_only,
    busy,
    io,
    invalid,
};

inline constexpr std::array<std::string_view, 8> error_code_names{
    "not_found", "denied", "exists", "outside_root", "read_only", "busy", "io", "invalid"};

constexpr std::string_view to_string(ErrorCode code)
{
    return error_code_names[static_cast<std::size_t>(code)];
}

// Win32 error -> code. Unlisted errors are `io`. ERROR_ACCESS_DENIED on a file
// whose read-only attribute is set is reported as read_only (the backend
// checks the attribute before choosing).
struct Win32ErrorMapping
{
    uint32_t win32;
    ErrorCode code;
};

inline constexpr std::array<Win32ErrorMapping, 11> win32_error_mapping{{
    {2, ErrorCode::not_found},    // ERROR_FILE_NOT_FOUND
    {3, ErrorCode::not_found},    // ERROR_PATH_NOT_FOUND
    {5, ErrorCode::denied},       // ERROR_ACCESS_DENIED
    {19, ErrorCode::read_only},   // ERROR_WRITE_PROTECT
    {32, ErrorCode::busy},        // ERROR_SHARING_VIOLATION
    {33, ErrorCode::busy},        // ERROR_LOCK_VIOLATION
    {80, ErrorCode::exists},      // ERROR_FILE_EXISTS
    {145, ErrorCode::invalid},    // ERROR_DIR_NOT_EMPTY
    {183, ErrorCode::exists},     // ERROR_ALREADY_EXISTS
    {267, ErrorCode::invalid},    // ERROR_DIRECTORY (a file where a folder was expected)
    {1224, ErrorCode::busy},      // ERROR_USER_MAPPED_FILE
}};

// --- Modes and permissions ------------------------------------------------

enum class Mode : uint8_t
{
    inherit,
    stat,
    ro,
    wo,
    rw,
    ao,
};

// Wire form of AddPath's `mode` argument: "" means inherit.
constexpr std::optional<Mode> parse_mode(std::string_view text)
{
    if (text.empty()) return Mode::inherit;
    if (text == "stat") return Mode::stat;
    if (text == "ro") return Mode::ro;
    if (text == "wo") return Mode::wo;
    if (text == "rw") return Mode::rw;
    if (text == "ao") return Mode::ao;
    return std::nullopt;
}

namespace Permission
{
inline constexpr uint32_t stat = 1u << 0;     // Exists, Stat, List
inline constexpr uint32_t read = 1u << 1;     // ReadText, ReadBytes, Tail, Copy source
inline constexpr uint32_t make_dir = 1u << 2; // MakeDir (each folder it creates)
inline constexpr uint32_t append = 1u << 3;   // Append, Open append=true
inline constexpr uint32_t write = 1u << 4;    // WriteText, Open append=false, Copy/Move destination, Move source
inline constexpr uint32_t remove = 1u << 5;   // Remove, RemoveTree (every entry), Move source; needs `delete`
}

// API.md "What each mode allows": every mode includes stat.
constexpr uint32_t permissions_for(Mode mode, bool delete_allowed)
{
    using namespace Permission;
    uint32_t bits = 0;
    switch (mode)
    {
    case Mode::stat: bits = stat; break;
    case Mode::ro: bits = stat | read; break;
    case Mode::wo: bits = stat | make_dir | append | write; break;
    case Mode::rw: bits = stat | read | make_dir | append | write; break;
    case Mode::ao: bits = stat | make_dir | append; break;
    case Mode::inherit: bits = 0; break; // never effective; resolved first
    }
    // `delete` only counts together with write: AddPath rejects it with an
    // explicit ro/stat/ao, and it is inert with an inherited mode that can't
    // write (F6).
    if (delete_allowed && (bits & write) != 0) bits |= remove;
    return bits;
}

// What a checked path is used for.
enum class Access : uint8_t
{
    stat,          // Exists, Stat, List
    read,          // ReadText, ReadBytes, Tail, Copy source
    make_dir,      // each folder MakeDir would create
    append,        // Append, Open append=true
    write,         // WriteText, Open append=false, Copy/Move destination
    delete_entry,  // Remove, each RemoveTree entry
    move_source,   // Move source
};

constexpr uint32_t required_permissions(Access access)
{
    using namespace Permission;
    switch (access)
    {
    case Access::stat: return stat;
    case Access::read: return read;
    case Access::make_dir: return make_dir;
    case Access::append: return append;
    case Access::write: return write;
    case Access::delete_entry: return write | remove;
    case Access::move_source: return write | remove;
    }
    return ~0u;
}

// --- Grants (native representation) ----------------------------------------

struct Grant
{
    std::u16string real_path;        // resolved final path; re-resolved at check time
    std::string given_path;          // what the mod passed, for messages
    Mode mode{Mode::inherit};
    bool extensions_inherited{true}; // false: `extensions` is authoritative (may be empty)
    std::string extensions;          // ';'-joined lowercase, no dots; "" with !inherited covers no files
    bool delete_allowed{false};
    bool recursive{true};
};

// AddPath `flags` argument.
namespace AddPathFlag
{
inline constexpr int64_t delete_allowed = 1 << 0;
inline constexpr int64_t non_recursive = 1 << 1;  // recursive=false
inline constexpr int64_t extensions_set = 1 << 2; // `extensions` given (else inherit)
inline constexpr int64_t known = delete_allowed | non_recursive | extensions_set;
}

inline constexpr char extension_separator = ';';
// Bytes an extension entry may not contain, besides control characters (M5).
inline constexpr std::string_view extension_forbidden = "./\\;<>:\"|?*";

// --- Other flag arguments (unknown bits are `invalid`) ----------------------

namespace WriteFlag
{
inline constexpr int64_t non_atomic = 1 << 0; // WriteText atomic=false
inline constexpr int64_t known = non_atomic;
}

namespace CopyMoveFlag
{
inline constexpr int64_t overwrite = 1 << 0;
inline constexpr int64_t known = overwrite;
}

namespace OpenFlag
{
inline constexpr int64_t truncate = 1 << 0;     // append=false
inline constexpr int64_t no_create = 1 << 1;    // create=false
inline constexpr int64_t manual_flush = 1 << 2; // flush="manual"
inline constexpr int64_t known = truncate | no_create | manual_flush;
}

namespace TailFlag
{
inline constexpr int64_t chunks = 1 << 0;     // chunks=true
inline constexpr int64_t from_start = 1 << 1; // from="start"
inline constexpr int64_t known = chunks | from_start;
}

// --- Tail dispatch (section 7) ----------------------------------------------

enum class DispatchKind : int64_t
{
    data = 1,   // text = line or chunk; offset = its byte offset; flags = DeliveryFlag
    closed = 2, // closed natively; text = "<code>\t<message>"; Lua sets sub.closed
    stop = 3,   // session stopping; Lua clears all state
};

namespace DeliveryFlag
{
inline constexpr int64_t reset = 1 << 0;   // info.reset: first delivery after truncation/rotation
inline constexpr int64_t partial = 1 << 1; // info.partial: a piece of a line over tail_max_line_bytes
}

// --- Record formats (section 6) ---------------------------------------------

inline constexpr char record_separator = '\n';
inline constexpr char field_separator = '\t';

// Locations(session), public env.Locations() (M14): one record per enabled
// location, in this table's order. An unbound location has an empty path.
inline constexpr std::array<std::string_view, 4> location_fields{"name", "path", "root", "exists"};

struct LocationEntry
{
    std::string_view name;
    bool root;
    bool per_mod;  // bound per Lua environment from on_lua_start
    bool enabled;  // a disabled entry is unknown to paths and absent from Locations
    std::string_view resolves_to;
};

inline constexpr std::array<LocationEntry, 7> locations{{
    {"game", true, false, true, "Steam install folder (holds Dawnwalker/ and Engine/)"},
    {"user", true, false, true, "%LOCALAPPDATA%/Dawnwalker"},
    {"mod", false, true, true, "<Mods>/<mod name>"},
    {"moddata", false, true, true, "<user>/Saved/ModData/<mod name>"},
    {"temp", false, true, true, "<moddata>/temp"},
    {"savegames", false, false, true, "<user>/Saved/SaveGames"},
    // PENDING (Jorge, API.md 1 / F11): the UE4SS Mods folder, read-only through
    // `game`. Hook only; enable by flipping `enabled` once decided. It joins
    // the delete floor like every location.
    {"mods", false, false, false, "<Mods>"},
}};

// List(session, policy, path): one record per entry, sorted by name (bytes).
// type is "file", "directory" or "other". modified and created are Unix
// seconds (UTC, possibly fractional) as in Stat; created was appended last so
// earlier fields keep their positions.
inline constexpr std::array<std::string_view, 6> list_fields{"name", "type", "size", "modified", "link", "created"};

// --- Data escape (section 5) ------------------------------------------------

inline constexpr char escape_byte = '\1';

// Decodes `escaped` into `out`. Returns false on a malformed escape or a
// length mismatch (the caller reports `invalid`).
inline bool decode_escaped(std::string_view escaped, int64_t byte_length, std::string& out)
{
    if (byte_length < 0) return false;
    out.clear();
    out.reserve(static_cast<std::size_t>(byte_length));
    for (std::size_t i = 0; i < escaped.size(); ++i)
    {
        const char c = escaped[i];
        if (c != escape_byte)
        {
            out.push_back(c);
            continue;
        }
        if (++i == escaped.size()) return false;
        if (escaped[i] == '\1') out.push_back('\1');
        else if (escaped[i] == '\2') out.push_back('\0');
        else return false;
    }
    return static_cast<int64_t>(out.size()) == byte_length;
}

// --- Native function table --------------------------------------------------
//
// Each entry is registered as native_prefix + name. `arguments` lists the
// exact stack shape the native checks (R2): s = session id (int),
// p = policy id (int), path = string (UTF-8), int, str,
// data = byte_length:int, escaped:str (two slots). `arity` is the number of
// stack slots. `results` is the success shape; failure is always
// (nil, code, message).

struct NativeFunction
{
    std::string_view name;
    int32_t arity;
    std::string_view arguments;
    std::string_view results;
    std::string_view backs; // the public API.md function(s) it serves
};

inline constexpr std::array<NativeFunction, 26> native_functions{{
    // Version, capabilities, statistics, locations
    {"GetVersion", 0, "", "version:str", "GetVersion"},
    {"GetCapabilities", 0, "",
     "api:int, environments:bool, append_only:bool, streams:bool, tail:bool, "
     "max_read_bytes:int, target_ue4ss_commit:str",
     "GetCapabilities (Lua adds `locations` and the deferred keys)"},
    {"GetDispatchStats", 0, "",
     "subscriptions:int, delivered:int, budget_exhausted_passes:int",
     "GetDispatchStats (public, K3)"},
    {"Locations", 1, "s", "records:str (location_fields)", "Locations"},

    // Paths (no grant; reads only the cached locations)
    {"Normalize", 2, "s, path", "absolute:str", "Normalize, Path (Lua joins, then Normalize)"},

    // Policies
    {"SafePolicy", 1, "s", "policy:int", "UE4SSLuaFileBridge()"},
    {"AddPath", 6, "s, p, path, mode:str, extensions:str, flags:int", "policy:int", "env.AddPath"},
    {"ReleasePolicy", 2, "s, p", "true (no-op for the safe-set id)", "queued environment __gc"},

    // Queries
    {"Exists", 3, "s, p, path", "exists:bool", "env.Exists"},
    {"Stat", 3, "s, p, path",
     "type:str, size:int, modified:num, created:num, readonly:bool, link:bool", "env.Stat"},
    {"List", 3, "s, p, path", "records:str (list_fields)", "env.List"},

    // Reading
    {"ReadText", 3, "s, p, path", "content:str (UTF-8 BOM removed)", "env.ReadText"},
    {"ReadBytes", 5, "s, p, path, offset:int, length:int (-1 = to end)", "content:str", "env.ReadBytes"},

    // Writing
    {"MakeDir", 3, "s, p, path", "true", "env.MakeDir"},
    {"WriteText", 6, "s, p, path, data, flags:int (WriteFlag)", "true", "env.WriteText"},
    {"Append", 5, "s, p, path, data", "true", "env.Append"},

    // Removing, copying, moving
    {"Remove", 3, "s, p, path", "true", "env.Remove"},
    {"RemoveTree", 3, "s, p, path", "true", "env.RemoveTree"},
    {"Copy", 5, "s, p, from:path, to:path, flags:int (CopyMoveFlag)", "true", "env.Copy"},
    {"Move", 5, "s, p, from:path, to:path, flags:int (CopyMoveFlag)", "true", "env.Move"},

    // Streams (checked once, at open)
    {"StreamOpen", 4, "s, p, path, flags:int (OpenFlag)", "stream:int, normalized:str", "env.Open"},
    {"StreamWrite", 4, "s, stream:int, data", "true", "stream:Write (Lua concatenates the arguments)"},
    {"StreamFlush", 2, "s, stream:int", "true", "stream:Flush"},
    {"StreamClose", 2, "s, stream:int", "true (also for an id this session already closed)",
     "stream:Close, queued stream __gc"},

    // Tail (checked once, at subscribe)
    {"TailOpen", 5, "s, p, path, token:int (>0, chosen by Lua), flags:int (TailFlag)",
     "subscription:int, normalized:str", "env.Tail"},
    {"TailClose", 2, "s, subscription:int", "true (idempotent)", "sub:Close"},
}};

// --- Check pipeline (section 3), for tests and reviewers ----------------------

inline constexpr std::array<std::string_view, 9> check_steps{
    "normalize (invalid / outside_root)",
    "resolve real path (leaf unfollowed for Remove, RemoveTree, Move)",
    "containment in game or user (outside_root)",
    "coverage: path prefix, recursive, effective extensions (files only)",
    "winner: most path segments",
    "inherit mode/extensions from the most specific grant covering the grant's path",
    "required_permissions subset of permissions_for (denied)",
    "fixed denials: delete floor incl. folders containing a location, moddata/.owner (denied)",
    "fixed savegames rules: no non-atomic write, no truncating Open (invalid)",
};

} // namespace UE4SSLuaFileBridge::NativeContract

// ---------------------------------------------------------------------------
// Decisions recorded at G-spec (2026-10-10)
// ---------------------------------------------------------------------------
// L's M1-M15 accepted. K1: the file bridge's .def adds get_stack_size,
// is_string, is_integer; natives validate arity and types. F9: every capacity
// limit is `io`. K2: a natively closed subscription is logged and surfaces as
// sub.closed, with no callback. K3: GetDispatchStats is public. K4/F12: the
// delete floor covers folders containing a location unless granted with
// delete on exactly that path. F11 (`mods`): pending Jorge, hook disabled
// in `locations`.
//
// Remaining open points
// ---------------------------------------------------------------------------
// O1  `mods` location (F11 / API.md 1): pending Jorge.
// O2  `stop` is delivered only when on_lua_stop runs on the game thread (as
//     bridge-events does its helper cleanup). Off the game thread the session
//     is still closed natively; Lua state then simply goes away with the
//     environment. E/K to confirm on_lua_stop's thread in the game.
// O3  API.md needs D's edits for M1, M2, M4, M5, M6, M9, M11, M14, K2, K3
//     and the extended delete floor; this header already follows them.
