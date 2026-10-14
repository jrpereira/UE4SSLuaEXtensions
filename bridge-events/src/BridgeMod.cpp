#include <EnhancedInputBackend.hpp>
#include <EmbeddedLuaAPI.hpp>
#include <HookABI.hpp>
#include <HookRuntime.hpp>
#include <HookTable.hpp>
#include <ParamSchema.hpp>
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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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

extern "C" __declspec(dllimport) unsigned long UE4SSLEB_WINAPI GetModuleFileNameW(
    void* module, wchar_t* file_name, unsigned long size);

extern "C" __declspec(dllimport) unsigned long UE4SSLEB_WINAPI GetEnvironmentVariableA(
    const char* name, char* buffer, unsigned long size);

extern "C" __declspec(dllimport) void UE4SSLEB_WINAPI OutputDebugStringA(const char* message);
extern "C" __declspec(dllimport) void* UE4SSLEB_WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) int UE4SSLEB_WINAPI ReadProcessMemory(
    void*, const void*, void*, std::size_t, std::size_t*);

#undef UE4SSLEB_WINAPI

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
    // Function hooks: the transport helpers built from the dispatcher at Lua
    // start (hook_transport_source in BridgeMod.cpp).
    int32_t hook_push_ref{};
    int32_t hook_flush_ref{};
    bool hook_transport{};
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

// ------------------------------------------------------------ function hooks
// The glue between the hook core (HookTable.hpp, Core), the parameter code
// (ParamSchema.hpp, ParamCopier.hpp, HookFilters.hpp), the delivery runtime
// (HookRuntime.hpp) and the Lua natives below (LEX_hooks.md §6–§9).

namespace Hooks = UE4SSLuaEventBridge::Hooks;
namespace HookLayout = RC::Unreal::HookLayout;
using RC::Unreal::FFrame;
using RC::Unreal::UFunction;
using LiveRuntime = Hooks::HookRuntime<FFrame>;
using HookClock = std::chrono::steady_clock;

constexpr std::size_t max_hook_options_length = 65536;
constexpr std::size_t max_hook_text_length = 1024;
constexpr int64_t max_hook_properties = 512;
constexpr int64_t max_hook_enum_names = 1024;
constexpr uint32_t never_matching_name = 0xFFFFFFFFu;
// Lua guarantees LUA_MINSTACK (20) free slots, and UE4SS's call_function pushes
// one more for its error handler. A direct dispatcher call pushes the function,
// 11 header values and 3 per triplet; larger events go through the session's
// push/flush helpers in chunks of 5 triplets (function, reset flag, 15 values).
constexpr std::size_t direct_triplets = 2; // 11 header values + 3 per triplet stays under ~20 Lua stack slots
constexpr std::size_t chunk_triplets = 5;

// Set once by the first hook (or pending registration) on the game thread;
// static storage, because the handlers and the engine-tick callback outlive
// the mod object.
std::atomic_bool hooks_started{};

bool safe_read(const void* source, void* destination, std::size_t size)
{
    std::size_t copied{};
    return source != nullptr && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) != 0 &&
        copied == size;
}

// A member of a live object, at a fixed offset.
template <typename T>
T read_field(std::uintptr_t object, std::size_t offset)
{
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(object + offset), sizeof(T));
    return value;
}

// Index and serial of an object known to be live (the hooked call's object).
Hooks::ObjectIdentity live_identity(std::uintptr_t object)
{
    Hooks::ObjectIdentity identity{object, -1, 0};
    if (object == 0) return identity;
    const auto index = read_field<int32_t>(object, RC::Unreal::FWeakObjectPtr::uobject_internal_index_offset);
    if (index < 0) return identity;
    const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
    if (item == nullptr) return identity;
    const auto item_address = reinterpret_cast<std::uintptr_t>(item);
    if (read_field<std::uintptr_t>(item_address, RC::Unreal::FWeakObjectPtr::object_item_object_offset) != object)
        return identity;
    identity.index = index;
    identity.serial = read_field<int32_t>(item_address, RC::Unreal::FWeakObjectPtr::object_item_serial_offset);
    return identity;
}

// The same for a parameter value, which may hold any pointer: every read is checked.
Hooks::ObjectIdentity checked_identity(std::uintptr_t object)
{
    Hooks::ObjectIdentity identity{object, -1, 0};
    int32_t index{};
    if (object == 0 ||
        !safe_read(reinterpret_cast<const void*>(object + RC::Unreal::FWeakObjectPtr::uobject_internal_index_offset),
            &index, sizeof(index)) ||
        index < 0)
        return identity;
    const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
    if (item == nullptr) return identity;
    const auto item_address = reinterpret_cast<std::uintptr_t>(item);
    std::uintptr_t indexed{};
    int32_t serial{};
    if (!safe_read(reinterpret_cast<const void*>(item_address + RC::Unreal::FWeakObjectPtr::object_item_object_offset),
            &indexed, sizeof(indexed)) ||
        indexed != object ||
        !safe_read(reinterpret_cast<const void*>(item_address + RC::Unreal::FWeakObjectPtr::object_item_serial_offset),
            &serial, sizeof(serial)))
        return identity;
    identity.index = index;
    identity.serial = serial;
    return identity;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if constexpr (sizeof(wchar_t) == 2)
    {
        return Hooks::utf16le_to_utf8(reinterpret_cast<const std::byte*>(text.data()), text.size() * sizeof(wchar_t));
    }
    else
    {
        std::string out;
        for (const wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
        return out;
    }
}

std::string name_text(uint32_t comparison_index, uint32_t number)
{
    RC::Unreal::FName name;
    name.comparison_index = comparison_index;
    name.number = number;
    return wide_to_utf8(name.ToString());
}

// "/Game/Map.Map:PersistentLevel.Actor": outer names only (§5.1). The
// outermost object is a package, its child follows a '.', the next level a ':'.
std::string object_path(std::uintptr_t object)
{
    if (checked_identity(object).index < 0) return {};
    std::vector<std::string> names;
    for (std::uintptr_t current = object; current != 0 && names.size() < 32;)
    {
        RC::Unreal::FName name{};
        std::uintptr_t outer{};
        if (!safe_read(reinterpret_cast<const void*>(current + HookLayout::uobject_name_private), &name, sizeof(name)) ||
            !safe_read(reinterpret_cast<const void*>(current + HookLayout::uobject_outer_private), &outer, sizeof(outer)))
            return {};
        names.push_back(name_text(name.comparison_index, name.number));
        current = outer;
    }
    std::string path;
    for (std::size_t depth = 0; depth < names.size(); ++depth)
    {
        if (depth == 2) path += ':';
        else if (depth != 0) path += '.';
        path += names[names.size() - 1 - depth];
    }
    return path;
}

// Filters on the hooked call's object, at the probe-verified offsets.
class LiveObjectView final : public Hooks::ObjectView
{
public:
    explicit LiveObjectView(std::uintptr_t object) : object_(object) {}
    int32_t index() const override { return identity().index; }
    int32_t serial() const override { return identity().serial; }
    uint32_t name_index() const override { return read_field<uint32_t>(object_, HookLayout::uobject_name_private); }
    std::uintptr_t class_address() const override
    {
        return read_field<std::uintptr_t>(object_, HookLayout::uobject_class_private);
    }
    std::uintptr_t outer_class(unsigned level) const override
    {
        std::uintptr_t current = object_;
        for (unsigned step = 0; step < level && current != 0; ++step)
            current = read_field<std::uintptr_t>(current, HookLayout::uobject_outer_private);
        return current == 0 ? 0 : read_field<std::uintptr_t>(current, HookLayout::uobject_class_private);
    }
    bool is_a(std::uintptr_t cls, std::uintptr_t of_class) const override
    {
        for (unsigned depth = 0; cls != 0 && depth < 64; ++depth)
        {
            if (cls == of_class) return true;
            cls = read_field<std::uintptr_t>(cls, HookLayout::ustruct_super_struct);
        }
        return false;
    }

private:
    const Hooks::ObjectIdentity& identity() const
    {
        if (!identity_) identity_ = live_identity(object_);
        return *identity_;
    }
    std::uintptr_t object_;
    mutable std::optional<Hooks::ObjectIdentity> identity_;
};

class LivePlatform final : public Hooks::HookPlatform<FFrame>
{
public:
    std::byte* locals(FFrame& frame) override { return reinterpret_cast<std::byte*>(frame.Locals()); }
    bool has_out_parms(FFrame& frame) override { return frame.OutParms() != nullptr; }
    const void* out_param_address(FFrame& frame, std::uintptr_t property) override
    {
        return property == 0 ? nullptr
                             : RC::Unreal::FindOutParamValueAddress(frame, reinterpret_cast<RC::Unreal::FProperty*>(property));
    }
    bool passes(const Hooks::Filter& filter, std::uintptr_t object) override
    {
        if (!filter.object && filter.name_indices.empty() && filter.outer_class == 0) return true;
        return object != 0 && Hooks::passes(filter, LiveObjectView(object));
    }
    Hooks::ObjectIdentity context_identity(std::uintptr_t object) override { return live_identity(object); }
    bool is_game_thread() override { return RC::Unreal::IsInGameThread(); }
    Hooks::ObjectIdentity identity(std::uintptr_t address) override { return checked_identity(address); }
    std::string path(std::uintptr_t address) override { return object_path(address); }
};

class LiveNames final : public Hooks::NameSource
{
public:
    std::string name(uint32_t comparison_index, uint32_t number) override { return name_text(comparison_index, number); }
};

uint32_t configured_hook_check_rate()
{
    const auto rate = configured_limit("UE4SSLEB_HOOK_CHECKS_PER_SECOND", Hooks::default_hook_checks_per_second, 1000);
    return rate == 0 ? Hooks::default_hook_checks_per_second : rate;
}

// Never destroyed: hooked functions and the engine-tick callback can still
// reach it after the mod object is gone (the DLL then stays loaded).
LiveRuntime& hook_runtime()
{
    static LiveRuntime* runtime = [] {
        auto* platform = new LivePlatform();
        return new LiveRuntime(*platform,
            configured_limit("UE4SSLEB_MAX_QUEUED_HOOK_EVENTS",
                static_cast<uint32_t>(Hooks::default_max_queued_hook_events), 1000000),
            configured_hook_check_rate());
    }();
    return *runtime;
}

UE4SSLuaEventBridge::DispatchBudget hook_budget()
{
    static const uint32_t events = configured_limit(
        "UE4SSLEB_MAX_HOOK_EVENTS_PER_PASS", Hooks::default_max_hook_events_per_pass, 1000000);
    static const uint32_t micros = configured_limit(
        "UE4SSLEB_MAX_HOOK_DISPATCH_US", Hooks::default_max_hook_dispatch_us, 1000000);
    return UE4SSLuaEventBridge::DispatchBudget(
        UE4SSLuaEventBridge::DispatchBudget::Clock::now(), events, std::chrono::microseconds(micros));
}

// The tick drain if UE4SS's engine-tick detour is there, else the event loop.
bool hook_delivery_is_tick()
{
    if (hooks_started.load(std::memory_order_acquire))
        return hook_runtime().delivery() == Hooks::HookDelivery::tick;
    return RC::UE4SSRuntime::IsEngineTickAvailable();
}

const char* field_kind_name(Hooks::FieldKind kind)
{
    switch (kind)
    {
    case Hooks::FieldKind::boolean: return "bool";
    case Hooks::FieldKind::int8: return "int8";
    case Hooks::FieldKind::int16: return "int16";
    case Hooks::FieldKind::int32: return "int32";
    case Hooks::FieldKind::int64: return "int64";
    case Hooks::FieldKind::uint8: return "uint8";
    case Hooks::FieldKind::uint16: return "uint16";
    case Hooks::FieldKind::uint32: return "uint32";
    case Hooks::FieldKind::uint64: return "uint64";
    case Hooks::FieldKind::float32: return "float";
    case Hooks::FieldKind::float64: return "double";
    case Hooks::FieldKind::enumeration: return "enum";
    case Hooks::FieldKind::name: return "name";
    case Hooks::FieldKind::string: return "string";
    case Hooks::FieldKind::object: return "object";
    case Hooks::FieldKind::structure: return "struct";
    case Hooks::FieldKind::unsupported: return "unsupported";
    }
    return "unsupported";
}

// The mod folder holds mod.json; the DLL sits in <mod>/dlls/versions.
std::optional<std::filesystem::path> mod_folder()
{
    constexpr unsigned long from_address = 0x00000004UL;
    constexpr unsigned long unchanged_refcount = 0x00000002UL;
    void* module{};
    if (GetModuleHandleExW(from_address | unchanged_refcount, reinterpret_cast<const wchar_t*>(&hooks_started), &module) == 0)
        return std::nullopt;
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<unsigned long>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    buffer.resize(length);
    std::error_code error;
    auto folder = std::filesystem::path(buffer).parent_path();
    for (int level = 0; level < 4 && !folder.empty(); ++level)
    {
        if (std::filesystem::exists(folder / "mod.json", error)) return folder;
        folder = folder.parent_path();
    }
    return std::nullopt;
}

std::string lower_trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    std::string out(text);
    for (auto& c : out) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

// [Hooks] Enabled in the mod's settings.ini (§8, written by ModCore Settings).
// Only "0" turns hooks off: a missing file or an unreadable value keeps the
// default, so configuration never blocks the bridge.
bool read_hooks_setting()
{
    const auto folder = mod_folder();
    if (!folder) return true;
#if defined(_MSC_VER)
    std::ifstream file(*folder / "settings.ini"); // wide path: non-ASCII install folders work
#else
    std::ifstream file((*folder / "settings.ini").string()); // the Linux syntax check's libstdc++
#endif
    if (!file) return true;
    std::string line;
    std::string section;
    std::optional<std::string> value;
    while (std::getline(file, line))
    {
        const auto text = lower_trimmed(line);
        if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
        {
            section = lower_trimmed(std::string_view(text).substr(1, text.size() - 2));
            continue;
        }
        const auto equals = text.find('=');
        if (section != "hooks" || equals == std::string::npos) continue;
        if (lower_trimmed(std::string_view(text).substr(0, equals)) == "enabled")
            value = lower_trimmed(std::string_view(text).substr(equals + 1));
    }
    return !(value && *value == "0");
}

// Reads a native's arguments in order and never past the ones Lua passed
// (a missing argument is a failed read, not a read of an empty stack slot).
class LuaArgs
{
public:
    explicit LuaArgs(const Lua& lua) : lua_(lua), remaining_(lua.get_stack_size()) {}

    bool integer(int64_t& value)
    {
        if (remaining_ <= 0) return false;
        --remaining_;
        value = lua_.get_integer(1);
        return true;
    }

    // `length` then ceil(length / 8) words (contract decision 1).
    bool text(std::size_t maximum, bool allow_empty, std::string& value)
    {
        int64_t length{};
        if (!integer(length) || length < (allow_empty ? 0 : 1) || static_cast<uint64_t>(length) > maximum) return false;
        value.assign(static_cast<std::size_t>(length), '\0');
        const std::size_t words = (value.size() + 7) / 8;
        for (std::size_t word = 0; word < words; ++word)
        {
            int64_t packed{};
            if (!integer(packed)) return false;
            for (std::size_t byte = 0; byte < 8 && word * 8 + byte < value.size(); ++byte)
            {
                const auto ch = static_cast<char>((static_cast<uint64_t>(packed) >> (byte * 8)) & 0xFFu);
                if (ch == '\0') return false;
                value[word * 8 + byte] = ch;
            }
        }
        return true;
    }

    // A plain Lua string or packed text.
    bool name(std::size_t maximum, std::string& value)
    {
        if (remaining_ <= 0) return false;
        if (lua_.is_string(1) && !lua_.is_integer(1))
        {
            --remaining_;
            value = std::string(lua_.get_string(1));
            return !value.empty() && value.size() <= maximum;
        }
        return text(maximum, false, value);
    }

private:
    const Lua& lua_;
    int32_t remaining_;
};

// The property list after the options (contract decision 2).
bool read_property_list(LuaArgs& args, std::vector<Hooks::LuaProperty>& list, std::string& error)
{
    int64_t count{};
    if (!args.integer(count) || count < 0 || count > max_hook_properties)
    {
        error = "invalid property count";
        return false;
    }
    list.clear();
    list.reserve(static_cast<std::size_t>(count));
    for (int64_t index = 0; index < count; ++index)
    {
        Hooks::LuaProperty property;
        int64_t parent{};
        int64_t address{};
        int64_t offset{};
        int64_t enums{};
        if (!args.integer(parent) || parent < -1 || parent >= index || !args.integer(address) || address <= 0 ||
            !args.integer(offset) || offset < 0 || offset > 0x7FFFFFFF ||
            !args.text(max_hook_text_length, false, property.name) || !args.integer(enums) || enums < 0 ||
            enums > max_hook_enum_names)
        {
            error = "invalid property list entry " + std::to_string(index);
            return false;
        }
        property.enum_names.resize(static_cast<std::size_t>(enums));
        for (auto& enum_name : property.enum_names)
        {
            if (!args.text(max_hook_text_length, true, enum_name))
            {
                error = "invalid enum name in property list entry " + std::to_string(index);
                return false;
            }
        }
        property.parent = parent < 0 ? Hooks::no_parent : static_cast<uint16_t>(parent);
        property.address = static_cast<std::uintptr_t>(address);
        property.offset = static_cast<int32_t>(offset);
        list.push_back(std::move(property));
    }
    return true;
}

// One hook event as one dispatcher call (contract decision 5).
class LuaHookCall final : public Hooks::TripletSink
{
public:
    LuaHookCall(const Lua& lua, const UE4SSLuaEventBridge::LuaSession& session, const Hooks::HookEvent& event)
        : lua_(lua), session_(session), event_(event)
    {
    }

    // Two reserved triplets lead every event: "@frame" (the frame counter when the
    // call was captured) and "@age" (seconds from capture to this delivery). Lua
    // turns them into call.frame and call.clock; they are not parameters.
    static constexpr std::size_t meta_triplets = 2;

    void begin(std::size_t count) override
    {
        count_ = count + meta_triplets;
        direct_ = count_ <= direct_triplets;
        if (direct_)
        {
            lua_.registry().get_function_ref(session_.dispatcher_ref);
            header();
        }
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        integer("@frame", static_cast<int64_t>(event_.frame));
        number("@age", static_cast<double>(now_ns - event_.timestamp_ns) / 1e9);
    }
    void boolean(const std::string& name, bool value) override
    {
        open(name, "b");
        lua_.set_bool(value);
        close();
    }
    void integer(const std::string& name, int64_t value) override
    {
        open(name, "i");
        lua_.set_integer(value);
        close();
    }
    void number(const std::string& name, double value) override
    {
        open(name, "n");
        lua_.set_number(value);
        close();
    }
    void string(const std::string& name, const std::string& utf8) override
    {
        open(name, "s");
        lua_.set_string(utf8);
        close();
    }
    void enumeration(const std::string& name, const std::string& text) override
    {
        open(name, "e");
        lua_.set_string(text);
        close();
    }
    void object(const std::string& name, const std::string& text) override
    {
        open(name, "o");
        lua_.set_string(text);
        close();
    }
    void unsupported(const std::string& name) override
    {
        open(name, "x");
        lua_.set_nil();
        close();
    }

    void finish()
    {
        if (direct_)
        {
            lua_.call_function(static_cast<int32_t>(header_values + 3 * count_), 0);
            return;
        }
        flush_chunk();
        lua_.registry().get_function_ref(session_.hook_flush_ref);
        header();
        lua_.call_function(header_values, 0);
    }

private:
    static constexpr std::size_t header_values = 11;

    void header()
    {
        lua_.set_integer(Hooks::message_hook_event);
        lua_.set_integer(static_cast<int64_t>(event_.hook));
        lua_.set_integer(static_cast<int64_t>(event_.sequence));
        lua_.set_integer(event_.phase == Hooks::Phase::post ? 2 : 1);
        lua_.set_bool(event_.game_thread);
        lua_.set_integer(static_cast<int64_t>(event_.object.address));
        lua_.set_integer(event_.object.index);
        lua_.set_integer(event_.object.serial);
        if (event_.object_path.empty()) lua_.set_nil();
        else lua_.set_string(event_.object_path);
        lua_.set_bool(event_.params_available);
        lua_.set_integer(static_cast<int64_t>(count_));
    }

    void open(const std::string& name, std::string_view tag)
    {
        if (!direct_ && in_chunk_ == 0)
        {
            lua_.registry().get_function_ref(session_.hook_push_ref);
            lua_.set_bool(first_chunk_);
            first_chunk_ = false;
        }
        lua_.set_string(name);
        lua_.set_string(tag);
    }

    void close()
    {
        if (!direct_ && ++in_chunk_ == chunk_triplets) flush_chunk();
    }

    void flush_chunk()
    {
        if (in_chunk_ == 0) return;
        lua_.call_function(static_cast<int32_t>(1 + 3 * in_chunk_), 0);
        in_chunk_ = 0;
    }

    const Lua& lua_;
    const UE4SSLuaEventBridge::LuaSession& session_;
    const Hooks::HookEvent& event_;
    std::size_t count_{};
    std::size_t in_chunk_{};
    bool direct_{true};
    bool first_chunk_{true};
};

// Per session: push(reset, name, tag, value, ...) collects triplets and
// flush(header...) calls the dispatcher with them, so an event of any size
// reaches the dispatcher as flat arguments without the native side pushing
// more than LUA_MINSTACK values (UE4SS exports no lua_checkstack).
constexpr std::string_view hook_transport_source = R"(return function(dispatch)
    local values, count = {}, 0
    local function push(reset, ...)
        if reset then count = 0 end
        local n = select("#", ...)
        for i = 1, n do values[count + i] = (select(i, ...)) end
        count = count + n
    end
    local function flush(code, hook, sequence, phase, gameThread, address, index, serial, path, available, n)
        local total = count
        count = 0
        return dispatch(code, hook, sequence, phase, gameThread, address, index, serial, path, available, n,
            table.unpack(values, 1, total))
    end
    return push, flush
end)";

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
        stop_hooks_for_unload();
        (void)backend_.shutdown();
        active_mod = nullptr;
    }

    // False keeps the DLL loaded: so does any function hook ever started,
    // since opcode slots, native thunks and the engine-tick callback point
    // into this module for the rest of the process.
    [[nodiscard]] bool prepare_for_unload()
    {
        stop_lifecycle_service();
        stop_hooks_for_unload();
        const bool backend_released = backend_.shutdown();
        return backend_released && !hooks_started.load(std::memory_order_acquire);
    }

    void on_unreal_init() override {
        backend_.set_queue_limit(configured_limit("UE4SSLEB_MAX_QUEUED_EVENTS", 65536, 1000000));
        backend_.initialize();
        start_lifetime_service();
    }

    void on_update() override
    {
        if (hooks_started.load(std::memory_order_acquire))
        {
            hook_runtime().update(HookClock::now(), hook_budget(),
                [this](Hooks::QueuedHookEvent& queued) { return deliver_hook_event(queued); },
                [this](const Hooks::HookNotice& notice) { deliver_hook_notice(notice); },
                [] { return UE4SSLuaEventBridge::DispatchBudget::Clock::now(); });
        }
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
        lua.register_function("UE4SSLuaEventBridge_HooksEnabled", &hooks_enabled);
        lua.register_function("UE4SSLuaEventBridge_HookFunction", &hook_function);
        lua.register_function("UE4SSLuaEventBridge_Unhook", &hook_unhook);
        lua.register_function("UE4SSLuaEventBridge_HookStatus", &hook_status);
        lua.register_function("UE4SSLuaEventBridge_HookPendingName", &hook_pending_name);
        lua.register_function("UE4SSLuaEventBridge_HookPendingForget", &hook_pending_forget);
        lua.register_function("UE4SSLuaEventBridge_HookSchema", &hook_schema);
        lua.register_function("UE4SSLuaEventBridge_GetHookStats", &get_hook_stats);
        hooks_setting_.store(read_hooks_setting(), std::memory_order_release);

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
        build_hook_transport(lua, *session_ptr);

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
            try
            {
                lua.execute_string(
                    "if __UE4SSLuaEventBridge_StopHooks ~= nil then "
                    "__UE4SSLuaEventBridge_StopHooks() end");
            }
            catch (const std::exception& error)
            {
                report("Lua-stop hook cleanup failed: ", error.what());
            }
            catch (...)
            {
                report("Lua-stop hook cleanup failed: unknown exception");
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
        stop_session_hooks(session->id, RC::Unreal::IsInGameThread());
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
        // Pending hooks: an atomic count, then a bit set, before any lock.
        if (!hooks_started.load(std::memory_order_acquire)) return;
        auto& runtime = hook_runtime();
        if (runtime.pending_names() == 0) return;
        runtime.on_object_created(
            read_field<uint32_t>(reinterpret_cast<std::uintptr_t>(object), HookLayout::uobject_name_private));
    }

    void NotifyUObjectDeleted(const RC::Unreal::UObjectBase* object, int32_t index) override
    {
        lifetimes_.invalidate(reinterpret_cast<std::uintptr_t>(object), index);
        // A deleted hooked UFunction: the core's lock-free prefilter first.
        if (!hooks_started.load(std::memory_order_acquire)) return;
        if (const auto entry = Hooks::Core::on_function_deleted(object))
            hook_runtime().on_orphaned(Hooks::Core::table(), *entry);
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
            report("function hooks unavailable: ", probe_.hooks_reason().value_or("unknown"));
            return false;
        }
        const auto [layout_checked, layout_mismatched] = probe_hook_layout();
        probe_.record_hook_layout(layout_checked, layout_mismatched);
        if (const auto why = probe_.hooks_reason()) report("function hooks unavailable: ", *why);
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
        lua.set_integer(7);
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
        // Function hooks (api 7), positions 16-21: hooks, hooks_reason,
        // hooks_bp, hooks_native, hooks_sync, hooks_frame_delivery.
        const auto hooks_reason = active_mod
            ? active_mod->hooks_unavailable_reason()
            : std::optional<std::string>{"the bridge is not active"};
        const bool hooks = !hooks_reason;
        lua.set_bool(hooks);
        if (hooks_reason) lua.set_string(*hooks_reason);
        else lua.set_nil();
        lua.set_bool(hooks && blueprint_hooks_possible());
        lua.set_bool(hooks);
        lua.set_bool(false);
        lua.set_bool(hooks && hook_delivery_is_tick());
        return 21;
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

    // require_serial = false accepts a live object whose serial Unreal has not
    // assigned yet (serial 0): enough to prove liveness, not to hold a weak identity.
    static std::optional<UE4SSLuaEventBridge::ObjectLifetimeIdentity> read_object_identity(
        std::uintptr_t address,
        bool require_game_thread = true,
        bool require_serial = true)
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
            reinterpret_cast<std::uintptr_t>(indexed_object) != address || serial < 0 ||
            (require_serial && serial == 0)) return {};
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

    // ------------------------------------------------------ function hooks

    std::atomic_bool hooks_setting_{true};

public:
    // Why Hooks.Hook is refused, or nothing when hooks are available.
    std::optional<std::string> hooks_unavailable_reason() const
    {
        if (!hooks_setting_.load(std::memory_order_acquire))
            return std::string{"function hooks are turned off in the bridge's settings ([Hooks] Enabled=0)"};
        if (hooks_started.load(std::memory_order_acquire) && hook_runtime().inert())
            return std::string{"the bridge is unloading"};
        if (auto why = probe_.hooks_reason()) return why;
        if (!lifetime_service_ready()) return std::string{"object lifetimes are unavailable"};
        return std::nullopt;
    }

    Hooks::EventOutcome deliver_hook_event(Hooks::QueuedHookEvent& queued)
    {
        auto* session = session_for_id(queued.event.session);
        if (!session || !session->lua || !session->hook_transport || !queued.plan)
            return Hooks::EventOutcome::discarded;
        const auto& payload = queued.event.payload;
        try
        {
            LiveNames names;
            LuaHookCall call(*session->lua, *session, queued.event);
            if (!queued.plan->flattener.flatten(payload.data(), payload.size(), &names, call))
            {
                hook_runtime().notify(Hooks::HookNotice{
                    Hooks::message_hook_fault, session->id, queued.event.hook, "a hook event payload was malformed"});
                return Hooks::EventOutcome::discarded;
            }
            call.finish();
        }
        catch (const std::exception& error)
        {
            report("hook event delivery failed: ", error.what());
            hook_runtime().notify(Hooks::HookNotice{Hooks::message_hook_fault, session->id, queued.event.hook,
                std::string("hook event delivery failed: ") + error.what()});
        }
        catch (...)
        {
            report("hook event delivery failed: unknown exception");
            hook_runtime().notify(Hooks::HookNotice{
                Hooks::message_hook_fault, session->id, queued.event.hook, "hook event delivery failed"});
        }
        return Hooks::EventOutcome::delivered;
    }

    void deliver_hook_notice(const Hooks::HookNotice& notice)
    {
        auto* session = session_for_id(notice.session);
        if (!session || !session->lua) return;
        try
        {
            const auto& lua = *session->lua;
            lua.registry().get_function_ref(session->dispatcher_ref);
            lua.set_integer(notice.code);
            lua.set_integer(static_cast<int64_t>(notice.id));
            if (notice.code == Hooks::message_hook_wakeup)
            {
                lua.call_function(2, 0);
                return;
            }
            lua.set_string(notice.reason);
            lua.call_function(3, 0);
        }
        catch (const std::exception& error)
        {
            report("hook notice delivery failed: ", error.what());
        }
        catch (...)
        {
            report("hook notice delivery failed: unknown exception");
        }
    }

private:
    // The engine-tick post-callback (game thread), registered at the first
    // hook with the plain overload; it has no unregister, so the runtime's
    // inert flag (set_inert, from unload) ends it.
    static void on_engine_tick()
    {
        hook_runtime().tick(hook_budget(),
            [] {
                if (hook_runtime().take_idle_sweep()) sweep_idle_entries();
            },
            [](Hooks::QueuedHookEvent& queued) {
                auto* mod = active_mod;
                return mod ? mod->deliver_hook_event(queued) : Hooks::EventOutcome::discarded;
            },
            [](const Hooks::HookNotice& notice) {
                if (auto* mod = active_mod) mod->deliver_hook_notice(notice);
            },
            [] { return UE4SSLuaEventBridge::DispatchBudget::Clock::now(); });
    }

    // Inert until used (§3): the first hook, or the first pending hook,
    // installs the event sink and picks the drain. Game thread.
    static void ensure_hooks_started()
    {
        if (hooks_started.load(std::memory_order_acquire)) return;
        auto& runtime = hook_runtime();
        Hooks::Core::set_event_sink(&LiveRuntime::sink, &runtime);
        const bool tick = RC::UE4SSRuntime::IsEngineTickAvailable();
        runtime.set_delivery(tick ? Hooks::HookDelivery::tick : Hooks::HookDelivery::loop);
        if (tick)
        {
            RC::Unreal::Hook::RegisterEngineTickPostCallback([](RC::Unreal::UEngine*, float) { on_engine_tick(); });
        }
        hooks_started.store(true, std::memory_order_release);
        report(tick ? "function hooks started: events are delivered once per frame from the engine tick"
                    : "function hooks started: the engine tick is unavailable, so events are delivered from "
                      "the event loop (UE4SSLEB_HOOK_CHECKS_PER_SECOND)");
    }

    // Active entries no subscription uses any more (a session stopped or a
    // hook was removed off the game thread). Game thread.
    static void sweep_idle_entries()
    {
        std::vector<uint32_t> idle;
        {
            auto writer = Hooks::Core::table().write();
            idle = writer.idle_entries();
        }
        for (const auto entry : idle)
        {
            const auto outcome = Hooks::Core::unhook(entry);
            if (!outcome.ok) report("unhooking an unused hook entry failed: ", outcome.reason);
        }
    }

    static bool blueprint_hooks_possible()
    {
        if (!hooks_started.load(std::memory_order_acquire)) return true;
        const auto status = Hooks::Core::status();
        return status.blueprint_ready || status.blueprint_reason.empty();
    }

    // The session's push/flush transport (hook_transport_source). A session
    // without it refuses hooks with events of more than two values.
    static void build_hook_transport(const Lua& lua, UE4SSLuaEventBridge::LuaSession& session)
    {
        try
        {
            lua.execute_string(hook_transport_source);
            lua.registry().get_function_ref(session.dispatcher_ref);
            lua.call_function(1, 2);
            session.hook_flush_ref = lua.registry().make_ref();
            session.hook_push_ref = lua.registry().make_ref();
            session.hook_transport = true;
        }
        catch (const std::exception& error)
        {
            report("function hook transport failed to load; hooks are unavailable in this Lua session: ", error.what());
        }
        catch (...)
        {
            report("function hook transport failed to load; hooks are unavailable in this Lua session");
        }
    }

    // Everything a stopped session owned: subscriptions, records, pending
    // names. Entries left unused are unhooked now on the game thread, or by
    // the next game-thread pass otherwise. Then waits (bounded) for a drain
    // pass on another thread, which may be inside this session's Lua state.
    static void stop_session_hooks(uint64_t session, bool game_thread)
    {
        if (!hooks_started.load(std::memory_order_acquire)) return;
        auto& runtime = hook_runtime();
        runtime.forget_session(session);
        std::vector<uint32_t> idle;
        {
            auto writer = Hooks::Core::table().write();
            idle = writer.unsubscribe_session(session);
        }
        for (const auto hook : runtime.session_hooks(session)) runtime.erase_record(hook);
        if (game_thread)
        {
            for (const auto entry : idle)
            {
                const auto outcome = Hooks::Core::unhook(entry);
                if (!outcome.ok) report("Lua-stop unhook failed: ", outcome.reason);
            }
        }
        else if (!idle.empty())
        {
            runtime.request_idle_sweep();
        }
        if (!runtime.wait_for_drain(std::chrono::milliseconds(250)))
            report("Lua stop: a hook delivery pass was still running after 250 ms");
    }

    static void stop_hooks_for_unload()
    {
        if (!hooks_started.load(std::memory_order_acquire)) return;
        auto& runtime = hook_runtime();
        runtime.set_inert();
        if (!runtime.wait_for_drain(std::chrono::milliseconds(1000)))
            report("unload: a hook delivery pass was still running after 1 s");
    }

    // HookLayout's UObject offsets against UE4SS's accessors (§6.3), on the
    // objects the lifetime probe can verify.
    static std::pair<std::size_t, std::size_t> probe_hook_layout()
    {
        constexpr int32_t probe_index_limit = 4096;
        std::size_t checked{};
        std::size_t mismatched{};
        try
        {
            for (int32_t index = 0;
                 index < probe_index_limit && checked < UE4SSLuaEventBridge::LifetimeProbe::hook_layout_sample; ++index)
            {
                const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
                RC::Unreal::UObject* object{};
                if (!item || !safe_read(item, &object, sizeof(object)) || !object) continue;
                const auto address = reinterpret_cast<std::uintptr_t>(object);
                if (!read_object_identity(address, false)) continue;
                std::uintptr_t fixed_class{};
                std::uintptr_t fixed_outer{};
                RC::Unreal::FName fixed_name{};
                if (!safe_read(reinterpret_cast<const void*>(address + HookLayout::uobject_class_private), &fixed_class,
                        sizeof(fixed_class)) ||
                    !safe_read(reinterpret_cast<const void*>(address + HookLayout::uobject_name_private), &fixed_name,
                        sizeof(fixed_name)) ||
                    !safe_read(reinterpret_cast<const void*>(address + HookLayout::uobject_outer_private), &fixed_outer,
                        sizeof(fixed_outer)))
                    continue;
                ++checked;
                auto* base = static_cast<RC::Unreal::UObjectBase*>(object);
                const auto& name = base->GetNamePrivate();
                const bool same = reinterpret_cast<std::uintptr_t>(base->GetClassPrivate()) == fixed_class &&
                    name.comparison_index == fixed_name.comparison_index && name.number == fixed_name.number &&
                    reinterpret_cast<std::uintptr_t>(base->GetOuterPrivate()) == fixed_outer;
                if (!same) ++mismatched;
            }
        }
        catch (...)
        {
        }
        return {checked, mismatched};
    }

    // A live UObject that is a UFunction. Game thread. UFunctions are rarely
    // weakly referenced, so most have no serial yet; liveness is enough here.
    static UFunction* live_function(int64_t address)
    {
        if (address <= 0 || !read_object_identity(static_cast<std::uintptr_t>(address), true, false)) return nullptr;
        auto* object = reinterpret_cast<RC::Unreal::UObjectBase*>(static_cast<std::uintptr_t>(address));
        if (!object->IsA(UFunction::StaticClass())) return nullptr;
        return reinterpret_cast<UFunction*>(static_cast<std::uintptr_t>(address));
    }

    static int hooks_enabled(const Lua& lua)
    {
        const auto reason = active_mod
            ? active_mod->hooks_unavailable_reason()
            : std::optional<std::string>{"the bridge is not active"};
        lua.set_bool(!reason);
        if (!reason) return 1;
        lua.set_string(*reason);
        return 2;
    }

    // HookFunction(session, functionAddress, classAddress, phaseMask, options, properties)
    //   -> hookId | nil, reason
    static int hook_function(const Lua& lua)
    {
        const auto fail = [&](std::string_view why) {
            lua.set_nil();
            lua.set_string(why);
            return 2;
        };
        LuaArgs args(lua);
        int64_t session_id{};
        int64_t function_address{};
        int64_t class_address{};
        int64_t mask{};
        std::string options_text;
        std::vector<Hooks::LuaProperty> properties;
        std::string error;
        if (!args.integer(session_id) || !args.integer(function_address) || !args.integer(class_address) ||
            !args.integer(mask))
            return fail("HookFunction: missing arguments");
        if (!args.text(max_hook_options_length, true, options_text)) return fail("HookFunction: invalid options text");
        if (!read_property_list(args, properties, error)) return fail("HookFunction: " + error);
        if (!active_mod) return fail("the bridge is not active");
        if (!RC::Unreal::IsInGameThread()) return fail("Hooks.Hook must run on the Unreal game thread");
        if (const auto why = active_mod->hooks_unavailable_reason()) return fail(*why);
        auto* session = active_mod->session_for_id(static_cast<uint64_t>(session_id));
        if (!session) return fail("Lua session is not registered or is stopping");
        if (!session->hook_transport) return fail("the hook transport failed to load in this Lua session");
        if (mask < 1 || mask > 3) return fail("invalid phase mask");
        auto* function = live_function(function_address);
        if (function == nullptr) return fail("target is not a live UFunction");

        Hooks::HookOptions options;
        if (const auto why = Hooks::parse_hook_options(options_text, options)) return fail(*why);
        if (options.sync) return fail("sync delivery is not available in this version (hooks_sync = false)");

        auto built = Hooks::build_schema(properties, Hooks::make_native_walker(function), Hooks::native_parms_size(function));
        if (!built.ok) return fail(built.error);
        auto schema = std::make_shared<const Hooks::Schema>(std::move(built.schema));
        std::vector<uint16_t> selection;
        if (const auto why = Hooks::select_fields(*schema, options.params, selection)) return fail(*why);

        Hooks::Filter filter;
        std::string note;
        if (options.has_object)
        {
            const auto identity = read_object_identity(options.object_address);
            if (!identity || !active_mod->lifetimes_.valid(session->id, options.object_token, *identity))
                return fail("object weak handle is no longer valid");
            filter.object = Hooks::ObjectIdentity{identity->address, identity->index, identity->serial};
        }
        if (!options.names.empty())
        {
            std::string missing;
            for (const auto& name : options.names)
            {
                const auto wide = Lex::utf8_to_wide(name);
                const RC::Unreal::FName found(wide.c_str(), RC::Unreal::FNAME_Find);
                if (found.comparison_index == 0 && name != "None")
                {
                    missing += (missing.empty() ? "" : ", ") + name;
                    continue;
                }
                filter.name_indices.push_back(found.comparison_index);
            }
            // Names that do not exist yet can never match; an empty set would match everything.
            if (filter.name_indices.empty()) filter.name_indices.push_back(never_matching_name);
            if (!missing.empty()) note = "name not found: " + missing;
        }
        if (options.outer != 0)
        {
            if (!read_object_identity(options.outer, true, false)) return fail("outer_class is not a live object");
            auto* outer_class = reinterpret_cast<RC::Unreal::UStruct*>(options.outer);
            if (reinterpret_cast<std::uintptr_t>(outer_class->GetSuperStruct()) !=
                read_field<std::uintptr_t>(options.outer, HookLayout::ustruct_super_struct))
                return fail("outer_class filters are unavailable: the SuperStruct offset does not match UE4SS");
            filter.outer_class = options.outer;
            filter.outer_exact = options.exact;
        }
        std::vector<std::uintptr_t> addresses;
        addresses.reserve(properties.size());
        for (const auto& property : properties) addresses.push_back(property.address);
        auto plan = std::make_shared<Hooks::SubscriptionPlan>(schema, std::move(addresses), selection, options.paths);
        plan->note = note;

        ensure_hooks_started();
        auto& runtime = hook_runtime();
        if (runtime.take_idle_sweep()) sweep_idle_entries();
        const bool native = (function->GetFunctionFlags() & Hooks::func_native) != 0;
        const auto outcome = native ? Hooks::Core::hook_native(function) : Hooks::Core::hook_blueprint(function);
        if (!outcome.ok) return fail(outcome.reason);

        const auto hook = runtime.next_hook_id();
        {
            auto writer = Hooks::Core::table().write();
            auto* entry = writer.entry(outcome.entry);
            auto* subscription = entry ? writer.subscribe(outcome.entry, session->id, hook) : nullptr;
            if (subscription == nullptr)
            {
                runtime.request_idle_sweep(); // the entry may now be active without a subscription
                return fail("hook table entry vanished");
            }
            if (entry->owner_class == 0)
            {
                entry->owner_class = static_cast<std::uintptr_t>(class_address);
                entry->owner_name = name_text(
                    read_field<uint32_t>(reinterpret_cast<std::uintptr_t>(function), HookLayout::uobject_name_private), 0);
            }
            if (!entry->schema) entry->schema = schema;
            subscription->phases = static_cast<uint8_t>(mask);
            subscription->filter = std::move(filter);
            subscription->fields = selection;
            subscription->object_paths = options.paths;
            subscription->max_queued = options.max_queued;
            runtime.set_plan(subscription, std::move(plan));
        }
        runtime.add_record(hook, Hooks::HookRecord{session->id, outcome.entry, false});
        lua.set_integer(static_cast<int64_t>(hook));
        return 1;
    }

    // Unhook(session, hookId) -> bool, reason. An orphaned or already inert
    // hook is a success (contract decision 7); a restore note comes back as
    // the second value of a success.
    static int hook_unhook(const Lua& lua)
    {
        const auto fail = [&](std::string_view why) {
            lua.set_bool(false);
            lua.set_string(why);
            return 2;
        };
        LuaArgs args(lua);
        int64_t session_id{};
        int64_t hook{};
        if (!args.integer(session_id) || !args.integer(hook)) return fail("Unhook: missing arguments");
        if (!active_mod) return fail("the bridge is not active");
        auto* session = active_mod->session_for_id(static_cast<uint64_t>(session_id));
        if (!session) return fail("Lua session is not registered or is stopping");
        if (!hooks_started.load(std::memory_order_acquire)) return fail("unknown hook");
        auto& runtime = hook_runtime();
        const auto record = runtime.record(static_cast<uint64_t>(hook));
        if (!record || record->session != session->id) return fail("unknown hook " + std::to_string(hook));
        runtime.erase_record(static_cast<uint64_t>(hook));
        if (record->orphaned)
        {
            lua.set_bool(true);
            return 1;
        }
        bool idle{};
        {
            auto writer = Hooks::Core::table().write();
            writer.unsubscribe(record->entry, session->id, static_cast<uint64_t>(hook));
            idle = writer.active_subscriptions(record->entry) == 0;
        }
        std::string note;
        if (idle)
        {
            if (RC::Unreal::IsInGameThread())
            {
                const auto outcome = Hooks::Core::unhook(record->entry);
                note = outcome.reason;
                if (!outcome.ok) report("unhook failed: ", outcome.reason);
            }
            else
            {
                runtime.request_idle_sweep();
            }
        }
        lua.set_bool(true);
        if (note.empty()) return 1;
        lua.set_string(note);
        return 2;
    }

    // HookStatus(session, hookId) -> state, reason|nil, calls, matched, delivered, dropped, reentries
    static int hook_status(const Lua& lua)
    {
        const auto fail = [&](std::string_view why) {
            lua.set_nil();
            lua.set_string(why);
            return 2;
        };
        LuaArgs args(lua);
        int64_t session_id{};
        int64_t hook{};
        if (!args.integer(session_id) || !args.integer(hook)) return fail("HookStatus: missing arguments");
        if (!active_mod || !hooks_started.load(std::memory_order_acquire)) return fail("unknown hook");
        auto* session = active_mod->session_for_id(static_cast<uint64_t>(session_id));
        if (!session) return fail("Lua session is not registered or is stopping");
        auto& runtime = hook_runtime();
        const auto record = runtime.record(static_cast<uint64_t>(hook));
        if (!record || record->session != session->id) return fail("unknown hook " + std::to_string(hook));

        std::string_view state = "unhooked";
        std::string reason;
        std::array<uint64_t, 5> counts{};
        {
            auto reader = Hooks::Core::table().read();
            const auto* entry = reader.entry(record->entry);
            const Hooks::Subscription* found = nullptr;
            if (entry != nullptr)
            {
                for (const auto& subscription : entry->subscriptions)
                {
                    if (subscription.session != session->id || subscription.hook != static_cast<uint64_t>(hook)) continue;
                    found = &subscription;
                    if (subscription.active.load(std::memory_order_acquire)) break;
                }
            }
            if (record->orphaned || (entry != nullptr && entry->state == Hooks::EntryState::orphaned)) state = "orphaned";
            else if (found != nullptr && found->active.load(std::memory_order_acquire)) state = "active";
            if (found != nullptr)
            {
                const auto& counters = found->counters;
                counts = {counters.calls.load(), counters.matched.load(), counters.delivered.load(),
                    counters.dropped.load(), counters.reentries.load()};
                if (const auto* plan = runtime.plan_of(found); plan != nullptr && *plan) reason = (*plan)->note;
            }
            if (reason.empty() && entry != nullptr) reason = entry->reason;
        }
        lua.set_string(state);
        if (reason.empty()) lua.set_nil();
        else lua.set_string(reason);
        for (const auto count : counts) lua.set_integer(static_cast<int64_t>(count));
        return 7;
    }

    // HookPendingName(session, token, shortClassName) -> bool
    static int hook_pending_name(const Lua& lua)
    {
        LuaArgs args(lua);
        int64_t session_id{};
        int64_t token{};
        std::string name;
        if (!args.integer(session_id) || !args.integer(token) || !args.name(max_hook_text_length, name) || token <= 0 ||
            !active_mod || active_mod->hooks_unavailable_reason())
        {
            lua.set_bool(false);
            return 1;
        }
        auto* session = active_mod->session_for_id(static_cast<uint64_t>(session_id));
        if (!session)
        {
            lua.set_bool(false);
            return 1;
        }
        if (RC::Unreal::IsInGameThread()) ensure_hooks_started();
        // FNAME_Add: the class may not exist yet, and its name must be comparable when it does.
        const auto wide = Lex::utf8_to_wide(name);
        const RC::Unreal::FName registered(wide.c_str(), RC::Unreal::FNAME_Add);
        hook_runtime().add_pending_name(session->id, static_cast<uint64_t>(token), registered.comparison_index);
        lua.set_bool(true);
        return 1;
    }

    // HookPendingForget(session, token) -> bool
    static int hook_pending_forget(const Lua& lua)
    {
        LuaArgs args(lua);
        int64_t session_id{};
        int64_t token{};
        const bool read = args.integer(session_id) && args.integer(token);
        lua.set_bool(read && hooks_started.load(std::memory_order_acquire) &&
            hook_runtime().forget_pending_name(static_cast<uint64_t>(session_id), static_cast<uint64_t>(token)));
        return 1;
    }

    // HookSchema(functionAddress, properties) -> (path, kind, "offset:size:dim:flags")... | nil, reason.
    // Every result value replaces at least two consumed arguments, so the
    // results fit in the stack space the call already has.
    static int hook_schema(const Lua& lua)
    {
        const auto fail = [&](std::string_view why) {
            lua.set_nil();
            lua.set_string(why);
            return 2;
        };
        LuaArgs args(lua);
        int64_t function_address{};
        std::vector<Hooks::LuaProperty> properties;
        std::string error;
        if (!args.integer(function_address)) return fail("HookSchema: missing arguments");
        if (!read_property_list(args, properties, error)) return fail("HookSchema: " + error);
        if (!active_mod) return fail("the bridge is not active");
        if (!RC::Unreal::IsInGameThread()) return fail("Hooks.Schema must run on the Unreal game thread");
        auto* function = live_function(function_address);
        if (function == nullptr) return fail("target is not a live UFunction");
        const auto built =
            Hooks::build_schema(properties, Hooks::make_native_walker(function), Hooks::native_parms_size(function));
        if (!built.ok) return fail(built.error);
        const auto& fields = built.schema.fields;
        for (std::size_t index = 0; index < fields.size(); ++index)
        {
            const auto& field = fields[index];
            lua.set_string(Hooks::field_path(built.schema, static_cast<uint16_t>(index)));
            lua.set_string(field_kind_name(field.kind));
            lua.set_string(std::to_string(field.offset) + ":" + std::to_string(field.size) + ":" +
                std::to_string(field.dim) + ":" + std::to_string(field.flags));
        }
        return static_cast<int>(fields.size() * 3);
    }

    // GetHookStats() -> queued, high_water, rejected, deliveryIsTick, ticks, update_calls, entries, slotA, slotB
    static int get_hook_stats(const Lua& lua)
    {
        if (!hooks_started.load(std::memory_order_acquire))
        {
            lua.set_integer(0);
            lua.set_integer(0);
            lua.set_integer(0);
            lua.set_bool(hook_delivery_is_tick());
            lua.set_integer(0);
            lua.set_integer(0);
            lua.set_integer(0);
            lua.set_integer(-1);
            lua.set_integer(-1);
            return 9;
        }
        auto& runtime = hook_runtime();
        const auto queue = runtime.stats();
        const auto status = Hooks::Core::status();
        lua.set_integer(static_cast<int64_t>(queue.queued));
        lua.set_integer(static_cast<int64_t>(queue.high_water));
        lua.set_integer(static_cast<int64_t>(queue.rejected));
        lua.set_bool(runtime.delivery() == Hooks::HookDelivery::tick);
        lua.set_integer(static_cast<int64_t>(runtime.ticks()));
        lua.set_integer(static_cast<int64_t>(runtime.update_calls()));
        lua.set_integer(static_cast<int64_t>(status.entries));
        lua.set_integer(status.slot_a);
        lua.set_integer(status.slot_b);
        return 9;
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
