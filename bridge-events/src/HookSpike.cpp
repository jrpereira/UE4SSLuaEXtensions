// G0 spike (COORDINATOR/LEX/LEX_hooks.md §10, §12): a diagnostic around the
// hook core, built only on feature/hooks-spike and never merged. It reports
// through UE4SS.log; every Lua action also returns its lines as one string.
//
//   at on_unreal_init   GNatives histogram and the dry slot choice (no claim),
//                       Core::status(), HookLayout UObject offsets against the
//                       exported accessors for 16 live objects
//   UE4SSLuaEventBridge_HookSpike(functionAddress, action), action:
//     "init"      repeats the init report
//     "status"    Core::status() and the dry slot choice
//     "inspect"   FunctionFlags, Script length and bytes, HookLayout UFunction
//                 offsets against the accessors, the native property walk
//     "hook"      Core::hook_native or hook_blueprint (by FUNC_Native) with a
//                 sink that only counts pre and post per entry
//     "counts"    the entry's counts and Core::status()
//     "unhook"    Core::unhook, outcome and reason
//     "tick"      engine-tick post-callback: frames/s next to on_update calls/s
//                 every 5 s, off-game-thread ticks counted
//     "tick_off"  sets the tick callback's inert flag
// functionAddress is ignored (pass 0) by init, status, tick and tick_off.
#ifdef _WIN32

#include <HookABI.hpp>
#include <HookTable.hpp>
#include <LexReport.hpp>
#include <OpcodeSlots.hpp>
#include <ParamSchema.hpp>
#include <UE4SSABI.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define UE4SSLEB_SPIKE_WINAPI __stdcall
#else
#define UE4SSLEB_SPIKE_WINAPI
#endif
extern "C" __declspec(dllimport) void* UE4SSLEB_SPIKE_WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) int UE4SSLEB_SPIKE_WINAPI ReadProcessMemory(
    void*, const void*, void*, std::size_t, std::size_t*);
#undef UE4SSLEB_SPIKE_WINAPI

namespace UE4SSLuaEventBridge::HookSpike
{
namespace
{
using namespace UE4SSLuaEventBridge::Hooks;
using RC::LuaMadeSimple::Lua;
using RC::Unreal::FFrame;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
using RC::Unreal::UObjectBase;
namespace Layout = RC::Unreal::HookLayout;

constexpr std::size_t layout_sample_objects = 16;
constexpr int32_t layout_index_limit = 4096;
constexpr std::size_t max_logged_properties = 256;

// Collects one action's lines: each goes to UE4SS.log at once (a crash later
// in the action still leaves them there) and back to Lua at the end.
class Lines
{
public:
    void add(std::string line)
    {
        Lex::report("UE4SSLuaEventBridge spike", line);
        text_.append(line).push_back('\n');
    }
    const std::string& text() const { return text_; }

private:
    std::string text_;
};

bool read_memory(const void* source, void* destination, std::size_t size)
{
    std::size_t copied{};
    return source != nullptr && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) != 0 &&
        copied == size;
}

template <typename T>
bool read_at(const void* base, std::size_t offset, T& value)
{
    return read_memory(static_cast<const std::byte*>(base) + offset, &value, sizeof(T));
}

std::string hex(uint64_t value)
{
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string hex_pointer(const void* value) { return hex(reinterpret_cast<std::uintptr_t>(value)); }

std::string byte_text(uint8_t value)
{
    char buffer[4];
    std::snprintf(buffer, sizeof(buffer), "%02X", static_cast<unsigned>(value));
    return buffer;
}

std::string narrow(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

std::string pass(bool ok) { return ok ? "pass" : "FAIL"; }

// ---------------------------------------------------------------- slots

void report_slots(Lines& lines)
{
    NativeFuncPtr* natives = RC::Unreal::GNatives_Internal;
    if (natives == nullptr)
    {
        lines.add("GNatives: GNatives_Internal is null (UE4SS did not find GNatives)");
        return;
    }
    std::array<NativeFuncPtr, gnatives_count> snapshot{};
    if (!read_memory(natives, snapshot.data(), sizeof(NativeFuncPtr) * gnatives_count))
    {
        lines.add("GNatives: table at " + hex_pointer(reinterpret_cast<const void*>(natives)) + " is not readable");
        return;
    }
    std::vector<std::pair<NativeFuncPtr, std::size_t>> counts;
    std::size_t nulls = 0;
    for (const NativeFuncPtr value : snapshot)
    {
        if (value == nullptr)
        {
            ++nulls;
            continue;
        }
        auto found = std::find_if(counts.begin(), counts.end(), [&](const auto& item) { return item.first == value; });
        if (found == counts.end()) counts.emplace_back(value, 1);
        else ++found->second;
    }
    std::stable_sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    lines.add("GNatives: table " + hex_pointer(reinterpret_cast<const void*>(natives)) + ", " +
        std::to_string(counts.size()) + " distinct pointers, " + std::to_string(nulls) + " null");
    for (std::size_t rank = 0; rank < counts.size() && rank < 3; ++rank)
    {
        lines.add("GNatives: top " + std::to_string(rank + 1) + ": " +
            hex(reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(counts[rank].first))) + " x" +
            std::to_string(counts[rank].second));
    }
    const auto choice = choose_slots<NativeFuncPtr>(std::span<const NativeFuncPtr, gnatives_count>(snapshot));
    if (!choice)
    {
        lines.add("GNatives: dry slot choice found no free pair (Blueprint hooks would be unavailable)");
        return;
    }
    lines.add("GNatives: dry slot choice A=" + hex(choice->a) + " B=" + hex(choice->b) + " free-slot pointer " +
        hex(reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(choice->undefined))) + " held by " +
        std::to_string(choice->free_slots) + " slots (not claimed)");
}

void report_status(Lines& lines)
{
    const auto status = Core::status();
    lines.add("Core::status: blueprint_ready=" + std::string(status.blueprint_ready ? "true" : "false") +
        " reason=\"" + status.blueprint_reason + "\" slot_a=" + std::to_string(status.slot_a) +
        " slot_b=" + std::to_string(status.slot_b) + " collisions=" + std::to_string(status.slot_collisions) +
        " entries=" + std::to_string(status.entries) + " unknown_nodes=" + std::to_string(status.unknown_nodes) +
        " guard_overflows=" + std::to_string(status.guard_overflows) + " sink_faults=" +
        std::to_string(status.sink_faults) + " thunk_misses=" + std::to_string(status.thunk_misses));
}

// ---------------------------------------------------------- object layout

void report_object_layout(Lines& lines)
{
    std::size_t sampled = 0;
    std::size_t class_ok = 0;
    std::size_t name_ok = 0;
    std::size_t outer_ok = 0;
    lines.add("layout: checking HookLayout UObject offsets (Class " + hex(Layout::uobject_class_private) + ", Name " +
        hex(Layout::uobject_name_private) + ", Outer " + hex(Layout::uobject_outer_private) +
        ") against GetClassPrivate/GetNamePrivate/GetOuterPrivate");
    for (int32_t index = 0; index < layout_index_limit && sampled < layout_sample_objects; ++index)
    {
        const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
        UObject* object{};
        if (item == nullptr || !read_memory(item, &object, sizeof(object)) || object == nullptr) continue;
        void* fixed_class{};
        RC::Unreal::FName fixed_name{};
        void* fixed_outer{};
        if (!read_at(object, Layout::uobject_class_private, fixed_class) ||
            !read_at(object, Layout::uobject_name_private, fixed_name) ||
            !read_at(object, Layout::uobject_outer_private, fixed_outer))
            continue;
        ++sampled;
        auto* base = static_cast<UObjectBase*>(object);
        const void* accessor_class = base->GetClassPrivate();
        const RC::Unreal::FName& accessor_name = base->GetNamePrivate();
        const void* accessor_outer = base->GetOuterPrivate();
        const bool class_pass = accessor_class == fixed_class;
        const bool name_pass = accessor_name.comparison_index == fixed_name.comparison_index &&
            accessor_name.number == fixed_name.number;
        const bool outer_pass = accessor_outer == fixed_outer;
        class_ok += class_pass ? 1 : 0;
        name_ok += name_pass ? 1 : 0;
        outer_ok += outer_pass ? 1 : 0;
        std::string name_text = name_pass ? narrow(accessor_name.ToString()) : std::string("?");
        lines.add("layout: object " + std::to_string(index) + " " + hex_pointer(object) + " \"" + name_text +
            "\": class " + pass(class_pass) + " (" + hex_pointer(fixed_class) + " vs " + hex_pointer(accessor_class) +
            "), name " + pass(name_pass) + " (" + std::to_string(fixed_name.comparison_index) + "/" +
            std::to_string(fixed_name.number) + " vs " + std::to_string(accessor_name.comparison_index) + "/" +
            std::to_string(accessor_name.number) + "), outer " + pass(outer_pass) + " (" + hex_pointer(fixed_outer) +
            " vs " + hex_pointer(accessor_outer) + ")");
    }
    lines.add("layout: " + std::to_string(sampled) + " objects; class " + std::to_string(class_ok) + "/" +
        std::to_string(sampled) + ", name " + std::to_string(name_ok) + "/" + std::to_string(sampled) + ", outer " +
        std::to_string(outer_ok) + "/" + std::to_string(sampled));
}

// -------------------------------------------------------------- functions

bool is_function(UFunction* function)
{
    std::array<std::byte, 0xE0> probe{};
    if (!read_memory(function, probe.data(), probe.size())) return false;
    return static_cast<UObjectBase*>(static_cast<UObject*>(function))->IsA(UFunction::StaticClass());
}

std::span<uint8_t> script_of(UFunction* function)
{
    auto& script = function->GetScript();
    if (script.data == nullptr || script.num <= 0) return {};
    return std::span<uint8_t>(script.data, static_cast<std::size_t>(script.num));
}

void report_script(Lines& lines, UFunction* function, std::string_view label)
{
    const auto script = script_of(function);
    if (script.empty())
    {
        lines.add(std::string(label) + ": Script empty");
        return;
    }
    std::string tail;
    for (std::size_t index = script.size() >= 3 ? script.size() - 3 : 0; index < script.size(); ++index)
    {
        if (!tail.empty()) tail += ' ';
        tail += byte_text(script[index]);
    }
    lines.add(std::string(label) + ": Script length " + std::to_string(script.size()) + ", first byte " +
        byte_text(script[0]) + ", last three " + tail);
}

void report_function_layout(Lines& lines, UFunction* function)
{
    uint32_t flags{};
    uint8_t num_parms{};
    uint16_t parms_size{};
    uint16_t return_offset{};
    void* func{};
    void* script_data{};
    int32_t script_num{};
    void* children{};
    void* super{};
    read_at(function, Layout::ufunction_function_flags, flags);
    read_at(function, Layout::ufunction_num_parms, num_parms);
    read_at(function, Layout::ufunction_parms_size, parms_size);
    read_at(function, Layout::ufunction_return_value_offset, return_offset);
    read_at(function, Layout::ufunction_func, func);
    read_at(function, Layout::ustruct_script, script_data);
    read_at(function, Layout::ustruct_script + sizeof(void*), script_num);
    read_at(function, Layout::ustruct_child_properties, children);
    read_at(function, Layout::ustruct_super_struct, super);

    const uint32_t accessor_flags = function->GetFunctionFlags();
    const auto& script = function->GetScript();
    const void* accessor_func = reinterpret_cast<void*>(function->GetFuncPtr());
    lines.add("inspect: FunctionFlags " + hex(accessor_flags) + ((accessor_flags & func_native) != 0 ? " (FUNC_Native)" : "") +
        "; fixed reads: flags " + pass(flags == accessor_flags) + ", NumParms " +
        pass(num_parms == function->GetNumParms()) + " (" + std::to_string(num_parms) + "), ParmsSize " +
        pass(parms_size == function->GetParmsSize()) + " (" + std::to_string(parms_size) + "), ReturnValueOffset " +
        pass(return_offset == function->GetReturnValueOffset()) + " (" + hex(return_offset) + "), Func " +
        pass(func == accessor_func) + " (" + hex_pointer(accessor_func) + "), Script " +
        pass(script_data == static_cast<void*>(script.data) && script_num == script.num) + ", ChildProperties " +
        pass(children == static_cast<void*>(function->GetChildProperties())) + ", SuperStruct " +
        pass(super == static_cast<void*>(function->GetSuperStruct())));
}

void walk_properties(Lines& lines, std::unique_ptr<PropertyView> view, unsigned depth, std::size_t& count)
{
    for (; view && count < max_logged_properties; view = view->next())
    {
        ++count;
        std::string line = "inspect: property " + std::string(depth * 2, ' ') + view->name() + " class " +
            view->class_name() + " offset " + std::to_string(view->offset()) + " size " +
            std::to_string(view->element_size()) + " dim " + std::to_string(view->array_dim()) + " flags " +
            hex(view->flags()) + " address " + hex(view->address());
        if (view->class_name() == "BoolProperty")
            line += " byte " + std::to_string(view->bool_byte_offset()) + " mask " + hex(view->bool_mask());
        lines.add(line);
        if (depth < max_struct_depth) walk_properties(lines, view->first_child(), depth + 1, count);
    }
}

void inspect(Lines& lines, UFunction* function)
{
    report_function_layout(lines, function);
    report_script(lines, function, "inspect");
    std::size_t count = 0;
    walk_properties(lines, make_native_walker(function), 0, count);
    lines.add("inspect: " + std::to_string(count) + " properties walked natively; ParmsSize " +
        std::to_string(native_parms_size(function)));
}

// -------------------------------------------------------------------- hook

constexpr std::size_t phase_slots = 2;
std::array<std::atomic<uint64_t>, max_entries * phase_slots> phase_counts{};
std::once_flag sink_installed;

void counting_sink(const HookCall<FFrame>& call, void*)
{
    const std::size_t slot = static_cast<std::size_t>(call.entry.id) * phase_slots +
        (call.phase == Phase::post ? 1 : 0);
    if (slot < phase_counts.size()) phase_counts[slot].fetch_add(1, std::memory_order_relaxed);
}

std::optional<std::pair<uint32_t, EntryState>> entry_of(UFunction* function)
{
    auto reader = Core::table().read();
    const auto* entry = reader.find(function);
    if (entry == nullptr) return std::nullopt;
    return std::make_pair(entry->id, entry->state);
}

const char* state_name(EntryState state)
{
    switch (state)
    {
    case EntryState::active: return "active";
    case EntryState::inert: return "inert";
    case EntryState::orphaned: return "orphaned";
    }
    return "unknown";
}

void hook(Lines& lines, UFunction* function)
{
    std::call_once(sink_installed, [] { Core::set_event_sink(&counting_sink, nullptr); });
    const bool native = (function->GetFunctionFlags() & func_native) != 0;
    report_script(lines, function, "hook (before)");
    const auto outcome = native ? Core::hook_native(function) : Core::hook_blueprint(function);
    lines.add(std::string("hook: ") + (native ? "hook_native" : "hook_blueprint") + " ok=" +
        (outcome.ok ? "true" : "false") + " entry=" + std::to_string(outcome.entry) + " reason=\"" + outcome.reason +
        "\"");
    if (outcome.ok && outcome.entry < max_entries)
    {
        phase_counts[outcome.entry * phase_slots].store(0, std::memory_order_relaxed);
        phase_counts[outcome.entry * phase_slots + 1].store(0, std::memory_order_relaxed);
    }
    if (!native) report_script(lines, function, "hook (after)");
    report_status(lines);
}

void counts(Lines& lines, UFunction* function)
{
    const auto entry = entry_of(function);
    if (!entry)
    {
        lines.add("counts: the function has no hook entry");
    }
    else
    {
        const std::size_t base = static_cast<std::size_t>(entry->first) * phase_slots;
        lines.add("counts: entry " + std::to_string(entry->first) + " state " + state_name(entry->second) + " pre " +
            std::to_string(base < phase_counts.size() ? phase_counts[base].load() : 0) + " post " +
            std::to_string(base + 1 < phase_counts.size() ? phase_counts[base + 1].load() : 0));
    }
    report_status(lines);
}

void unhook(Lines& lines, UFunction* function)
{
    const auto entry = entry_of(function);
    if (!entry)
    {
        lines.add("unhook: the function has no hook entry");
        return;
    }
    const auto outcome = Core::unhook(entry->first);
    const auto after = entry_of(function);
    lines.add("unhook: entry " + std::to_string(entry->first) + " ok=" + (outcome.ok ? "true" : "false") +
        " reason=\"" + outcome.reason + "\" state now " + (after ? state_name(after->second) : "none"));
    if ((function->GetFunctionFlags() & func_native) == 0) report_script(lines, function, "unhook (after)");
    report_status(lines);
}

// -------------------------------------------------------------------- tick

std::atomic_bool tick_registered{};
std::atomic_bool tick_inert{true};
std::atomic<uint64_t> frames{};
std::atomic<uint64_t> off_thread_ticks{};
std::atomic<uint64_t> update_calls{};

struct TickWindow
{
    std::chrono::steady_clock::time_point start{};
    uint64_t frames{};
    uint64_t updates{};
};
TickWindow window{}; // game thread only (the tick callback)

void on_engine_tick()
{
    if (tick_inert.load(std::memory_order_acquire)) return;
    if (!RC::Unreal::IsInGameThread()) off_thread_ticks.fetch_add(1, std::memory_order_relaxed);
    // Frame counter per rule 9: incremented after the (here empty) drain.
    const uint64_t frame = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t updates = update_calls.load(std::memory_order_relaxed);
    if (window.start == std::chrono::steady_clock::time_point{})
    {
        window = TickWindow{now, frame, updates};
        return;
    }
    const auto elapsed = std::chrono::duration<double>(now - window.start).count();
    if (elapsed < 5.0) return;
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer),
        "tick: %.1f frames/s, on_update %.1f calls/s over %.1f s; frames %llu, off-game-thread ticks %llu",
        static_cast<double>(frame - window.frames) / elapsed, static_cast<double>(updates - window.updates) / elapsed,
        elapsed, static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(off_thread_ticks.load(std::memory_order_relaxed)));
    Lex::report("UE4SSLuaEventBridge spike", buffer);
    window = TickWindow{now, frame, updates};
}

void tick_on(Lines& lines)
{
    if (!RC::UE4SSRuntime::IsEngineTickAvailable())
    {
        lines.add("tick: IsEngineTickAvailable() is false; no callback registered");
        return;
    }
    window = TickWindow{};
    tick_inert.store(false, std::memory_order_release);
    if (!tick_registered.exchange(true, std::memory_order_acq_rel))
    {
        RC::Unreal::Hook::RegisterEngineTickPostCallback([](RC::Unreal::UEngine*, float) { on_engine_tick(); });
        lines.add("tick: engine-tick post-callback registered; reporting every 5 s");
    }
    else
    {
        lines.add("tick: callback re-armed (already registered)");
    }
}

void tick_off(Lines& lines)
{
    tick_inert.store(true, std::memory_order_release);
    lines.add("tick: inert; frames " + std::to_string(frames.load()) + ", off-game-thread ticks " +
        std::to_string(off_thread_ticks.load()) + ", on_update calls " + std::to_string(update_calls.load()));
}

void init_report(Lines& lines)
{
    report_slots(lines);
    report_status(lines);
    report_object_layout(lines);
}

bool needs_function(std::string_view action)
{
    return action == "inspect" || action == "hook" || action == "counts" || action == "unhook";
}

bool needs_game_thread(std::string_view action)
{
    return action == "inspect" || action == "hook" || action == "unhook" || action == "init";
}
}

void on_unreal_init()
{
    Lines lines;
    try
    {
        init_report(lines);
    }
    catch (const std::exception& error)
    {
        lines.add(std::string("init report failed: ") + error.what());
    }
    catch (...)
    {
        lines.add("init report failed: unknown exception");
    }
}

void on_update()
{
    update_calls.fetch_add(1, std::memory_order_relaxed);
}

// Called by prepare_for_unload: the tick callback has no unregister.
void shut_down()
{
    tick_inert.store(true, std::memory_order_release);
}

// True once anything the spike did leaves pointers into this DLL behind
// (opcode slots, a thunk, the tick callback): the module must stay loaded.
bool retains_module()
{
    return Core::table().size() > 0 || Core::status().blueprint_ready || tick_registered.load();
}

int lua_entry(const Lua& lua)
{
    Lines lines;
    try
    {
        const auto address = lua.get_integer(1);
        const std::string action(lua.get_string(1));
        lines.add("action \"" + action + "\" function " + hex(static_cast<uint64_t>(address)));
        if (needs_game_thread(action) && !RC::Unreal::IsInGameThread())
        {
            lines.add("refused: \"" + action + "\" must run on the game thread");
        }
        else if (needs_function(action))
        {
            auto* function = reinterpret_cast<UFunction*>(static_cast<std::uintptr_t>(address));
            if (address <= 0 || !is_function(function))
                lines.add("refused: " + hex(static_cast<uint64_t>(address)) + " is not a live UFunction");
            else if (action == "inspect") inspect(lines, function);
            else if (action == "hook") hook(lines, function);
            else if (action == "counts") counts(lines, function);
            else unhook(lines, function);
        }
        else if (action == "init") init_report(lines);
        else if (action == "status")
        {
            report_slots(lines);
            report_status(lines);
        }
        else if (action == "tick") tick_on(lines);
        else if (action == "tick_off") tick_off(lines);
        else lines.add("unknown action; use init, status, inspect, hook, counts, unhook, tick or tick_off");
    }
    catch (const std::exception& error)
    {
        lines.add(std::string("failed: ") + error.what());
    }
    catch (...)
    {
        lines.add("failed: unknown exception");
    }
    lua.set_string(lines.text());
    return 1;
}
}

#endif
