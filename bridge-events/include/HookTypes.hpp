#pragma once

// Types the hook tracks share: core patching, parameter schemas and copying,
// filters, the queue and the Lua wiring. Portable: no UE4SS header, so every
// test suite compiles against it on Linux. This file is the day-0 contract of
// COORDINATOR/LEX/LEX_hooks.md (§9); changes go through the coordinator.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RC::Unreal
{
class UObject;
class UFunction;
class UClass;
struct FFrame;
}

namespace UE4SSLuaEventBridge::Hooks
{
// A UFunction's Func and every GNatives entry: void(UObject* context, FFrame& stack, void* result).
using NativeFuncPtr = void (*)(RC::Unreal::UObject*, RC::Unreal::FFrame&, void*);

// Kismet bytecode constants the patch relies on. A Blueprint function body is
// `... EX_Return EX_Nothing EX_EndOfScript`; the hook refuses anything else.
inline constexpr uint8_t ex_return = 0x04;
inline constexpr uint8_t ex_nothing = 0x0B;
inline constexpr uint8_t ex_end_of_script = 0x53;
inline constexpr std::size_t gnatives_count = 0x100;
inline constexpr std::size_t gnatives_free_minimum = 8; // occurrences of execUndefined that mark slots as free

// EFunctionFlags and EPropertyFlags bits the hooks read.
inline constexpr uint32_t func_native = 0x00000400;
inline constexpr uint64_t cpf_const_parm = 0x0000000000000002;
inline constexpr uint64_t cpf_parm = 0x0000000000000080;
inline constexpr uint64_t cpf_out_parm = 0x0000000000000100;
inline constexpr uint64_t cpf_return_parm = 0x0000000000000400;
inline constexpr uint64_t cpf_reference_parm = 0x0000000008000000;

// Limits. Fixed here so every track and the documentation agree.
inline constexpr std::size_t max_entries = 1024;          // distinct hooked functions per process
inline constexpr std::size_t max_guard_records = 64;      // re-entry guard records per thread
inline constexpr std::size_t max_struct_depth = 3;        // nested struct parameters
inline constexpr std::size_t max_string_bytes = 4096;     // one FString value in a payload
inline constexpr std::size_t max_payload_bytes = 16384;   // one event's payload
inline constexpr std::size_t outer_walk_depth = 8;        // outer_class filter
inline constexpr uint32_t default_max_queued_per_hook = 256;

enum class EntryKind : uint8_t { blueprint, native };

// Bits of a subscription's phase mask and the phase of one event.
enum class Phase : uint8_t { pre = 1, post = 2 };

// A table entry's life: active while patched, inert after unhook (saved bytes
// or Func kept so a racing frame still finds them), orphaned when the UFunction
// object was deleted (nothing is ever written back).
enum class EntryState : uint8_t { active, inert, orphaned };

// What Status() reports for one Lua hook.
enum class HookState : uint8_t { active, pending, orphaned, failed, unhooked };

// GUObjectArray index and FUObjectItem serial, the bridge's weak identity.
struct ObjectIdentity
{
    std::uintptr_t address{};
    int32_t index{-1};
    int32_t serial{};
};

// Parameter schema: built once when a hook attaches, read-only afterwards.
enum class FieldKind : uint8_t
{
    boolean, int8, int16, int32, int64, uint8, uint16, uint32, uint64,
    float32, float64, enumeration, name, string, object, structure, unsupported
};

inline constexpr uint16_t no_parent = 0xFFFF;

struct Field
{
    std::string name;            // property name; nested fields keep their own name, the parent gives the path
    std::string class_name;      // FField class name, for diagnostics ("IntProperty", "StructProperty", ...)
    FieldKind kind{FieldKind::unsupported};
    uint32_t offset{};           // from the start of the containing block (Locals, or the parent struct)
    uint32_t size{};             // element size
    uint32_t dim{1};             // fixed array dimension
    uint8_t byte_offset{};       // FBoolProperty: byte within the field
    uint8_t bit_mask{};          // FBoolProperty: mask within that byte; 0 for a plain bool
    uint64_t flags{};            // EPropertyFlags
    uint16_t parent{no_parent};  // index of the containing struct field
    uint8_t depth{};             // 0 for a parameter, up to max_struct_depth
    std::vector<std::string> enum_names; // index = enum value, when known
};

struct Schema
{
    std::vector<Field> fields;
    uint32_t parms_size{};
    std::optional<uint16_t> return_field;
    bool has_unsupported{};
};

// Payload blob records: tag (1 byte) | field index (2 bytes, little endian) |
// length (2 bytes) | value bytes. Tags double as the Lua dispatcher's tag letters.
enum class Tag : uint8_t
{
    boolean = 'b', integer = 'i', number = 'n', string = 's',
    enumeration = 'e', object = 'o', unsupported = 'x'
};

// Native filters, evaluated in this order before any copying.
struct Filter
{
    std::optional<ObjectIdentity> object;   // one instance
    std::vector<uint32_t> name_indices;     // FName comparison indices of the frame object
    std::uintptr_t outer_class{};           // UClass address; 0 = no outer filter
    bool outer_exact{};                     // exact class, else IsA
    uint8_t outer_depth{outer_walk_depth};
};

struct Counters
{
    std::atomic<uint64_t> calls{};      // handler entries, before filters
    std::atomic<uint64_t> matched{};    // passed the filters
    std::atomic<uint64_t> delivered{};  // reached a Lua callback
    std::atomic<uint64_t> dropped{};    // per-hook or queue limit
    std::atomic<uint64_t> reentries{};  // re-entry guard hits
};

// One Lua hook on one entry. Owned by the table; sessions reference it by id.
struct Subscription
{
    uint64_t session{};
    uint64_t hook{};
    uint8_t phases{};                  // Phase bits
    Filter filter;
    std::vector<uint16_t> fields;      // schema indices to copy; empty = every supported parameter
    bool object_paths{};
    uint32_t max_queued{default_max_queued_per_hook};
    std::atomic<uint32_t> queued{};
    std::atomic_bool active{true};
    Counters counters;
};

struct HookEvent
{
    uint32_t entry{};
    uint64_t session{};
    uint64_t hook{};
    Phase phase{Phase::pre};
    uint64_t sequence{};
    int64_t timestamp_ns{};
    bool game_thread{};
    bool params_available{};
    ObjectIdentity object;
    std::string object_path;           // only with object_paths
    std::vector<std::byte> payload;
};

// First argument of the session's dispatcher closure. Input uses -2, -1, 0 and
// positive callback tokens; hooks use these.
inline constexpr int64_t message_hook_event = -10;
inline constexpr int64_t message_hook_wakeup = -3;   // a pending class name was created; retry pending hooks
inline constexpr int64_t message_hook_pending = -4;  // a hooked function was deleted; the hook is pending again
inline constexpr int64_t message_hook_fault = -5;    // a hook was disabled (callback faults, queue loss)
}
