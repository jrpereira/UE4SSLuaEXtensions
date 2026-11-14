#pragma once

// UE4SSLuaSoundBridge: internal contract between the native DLL and the
// embedded Lua layer (bridge-sounds/lua/sounds_api.lua). Mods never see it:
// they see only the `UE4SSLuaSoundBridge` table the Lua layer builds. Public
// API: bridge-sounds/docs/API.md.
//
// Portable (no Win32, no UE4SS includes), so the native core, its tests and
// the Lua-layer fake-native tests share one description.
//
// ---------------------------------------------------------------------------
// 1. Boundary rules (pinned UE4SS ABI 97b7e501, as File Bridge)
// ---------------------------------------------------------------------------
//   R1  Scalars only: every argument is an integer or a string. The pinned ABI
//       has no get_number, so fractional values cross as integers in
//       millionths (`micro`); results may be numbers.
//   R2  Checked arity and types. Each native compares get_stack_size() with
//       its shape and checks every slot with is_integer/is_string before
//       consuming anything. A mismatch is `nil, "invalid", "<Name>: bad
//       arguments"`.
//   R3  get_string pops and stops at NUL: copy the view before any other Lua
//       call. Paths never contain NUL (Lua refuses them first).
//   R4  Natives never throw into Lua and never raise Lua errors.
//   R5  Natives are registered as globals; the Lua layer captures each one
//       into an upvalue table and clears the global.
//   R6  UE4SS raises when a native is called from a coroutine the mod made;
//       the Lua layer pcalls every native and maps that to `invalid`.
//
// ---------------------------------------------------------------------------
// 2. Sessions
// ---------------------------------------------------------------------------
// on_lua_start(mod_name, lua, ...) creates a Session (process-unique id, never
// reused) bound to <Mods>/<mod_name>, registers the natives, runs
// `<session_global> = <id>` and the embedded Lua layer. Every native that acts
// for a mod takes the session id first. on_lua_stop (and a Lua reload) marks
// the session inactive, stops every play of its sounds and releases them; the
// sample memory goes once no voice still holds it.
//
// ---------------------------------------------------------------------------
// 3. Files
// ---------------------------------------------------------------------------
// Load reads one file from the calling mod's own folder, read-only. The path
// is relative to that folder (`sounds/cue.wav` by convention); `/` and `\`
// both separate segments. Refused (`invalid`): empty, absolute or
// drive-relative paths, `:` anywhere (drives, streams), wildcards and control
// characters, segments ending in a dot or space. Refused (`outside_root`): a
// `..` that climbs out of the folder, reserved device names, and a file whose
// final path (links followed) lies outside the mod folder's final path. Files
// larger than max_file_bytes are `invalid` before they are read.
//
// ---------------------------------------------------------------------------
// 4. Playback
// ---------------------------------------------------------------------------
// Load decodes once to 32-bit float at the file's own rate. Play never reads
// files, decodes or waits on the audio engine: it takes a play id and queues
// a command for the audio thread, which owns the engine and every voice. A
// queued play is dropped silently (and counted) when no output device exists.
// Values outside their range are clamped (Lua clamps first; natives again).

#include <array>
#include <cstdint>
#include <string_view>

namespace UE4SSLuaSoundBridge::NativeContract
{
// --- Identity -------------------------------------------------------------

inline constexpr int64_t api_version = 1;
inline constexpr uint32_t implementation_magic = 0x4C534231; // 'LSB1'
inline constexpr std::string_view lua_global = "UE4SSLuaSoundBridge";
inline constexpr std::string_view native_prefix = "UE4SSLuaSoundBridge_";
inline constexpr std::string_view session_global = "__UE4SSLuaSoundBridge_SessionId";
inline constexpr std::string_view target_ue4ss_commit = "97b7e501";
inline constexpr std::string_view embedded_lua_delimiter = ")UE4SSLSB_LUA";
inline constexpr std::string_view backend_name = "xaudio2";

// --- Limits -----------------------------------------------------------------

inline constexpr int64_t max_file_bytes = 64LL * 1024 * 1024;
inline constexpr int64_t max_voices = 32;
inline constexpr int64_t max_sounds_per_session = 4096;
inline constexpr int64_t max_channels = 2;
inline constexpr int64_t min_sample_rate = 1000;   // XAUDIO2_MIN_SAMPLE_RATE
inline constexpr int64_t max_sample_rate = 200000; // XAUDIO2_MAX_SAMPLE_RATE

// Per-play values, in millionths (R1). Out-of-range values are clamped.
inline constexpr int64_t micro = 1000000;
inline constexpr int64_t min_pan_micro = -micro;
inline constexpr int64_t max_pan_micro = micro;
inline constexpr int64_t min_volume_micro = 0;
inline constexpr int64_t max_volume_micro = 4 * micro;
inline constexpr int64_t min_pitch_micro = micro / 2;
inline constexpr int64_t max_pitch_micro = 2 * micro;
// Low-pass cutoff in hertz; 0 = no filter. Cutoffs at or above a sixth of the
// output rate (8 kHz at 48 kHz) are the filter's top and play unfiltered.
inline constexpr int64_t min_lowpass_hz = 20;

// --- Error codes ----------------------------------------------------------

enum class ErrorCode : uint8_t
{
    not_found,
    denied,
    outside_root,
    busy,
    io,
    invalid,
    unsupported,
    too_long,
};

inline constexpr std::array<std::string_view, 8> error_code_names{
    "not_found", "denied", "outside_root", "busy", "io", "invalid", "unsupported", "too_long"};

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

inline constexpr std::array<NativeFunction, 8> native_functions{{
    {"GetVersion", "", "", "version:str"},
    {"GetCapabilities", "", "",
     "api:int, max_voices:int, max_file_bytes:int, backend:str, target_ue4ss_commit:str"},
    {"GetStats", "", "",
     "sounds:int, sound_bytes:int, playing:int, started:int, dropped:int, stolen:int, device:str, "
     "device_changes:int"},
    {"Load", "isi", "session, path:str, max_ms:int (-1 = no limit)",
     "sound:int, frames:int, channels:int, rate:int, path:str (normalized, relative)"},
    {"Release", "ii", "session, sound", "true (also for a sound already released)"},
    {"Play", "iiiiii", "session, sound, pan:micro, volume:micro, pitch:micro, lowpass:hz (0 = none)", "play:int"},
    {"Stop", "ii", "session, play", "true (also for a play that has ended)"},
    {"StopSound", "ii", "session, sound", "true"},
}};

} // namespace UE4SSLuaSoundBridge::NativeContract
