// Function-hook delivery (HookRuntime.hpp) driven through the real hook core
// handlers on the fake frame (support/FakeFrame.hpp): the sink with filters
// and limits, both drains (an injected engine tick and the on_update
// fallback), a fake Lua session, notices, pending names and orphaning, unload.
// Built by the Windows CI job with /W4 /WX; the code is portable, so it also
// compiles with g++ for a local run.

#include <HookRuntime.hpp>
#include <FakeFrame.hpp>
#include <TestSupport.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace UE4SSLuaEventBridge;
using namespace UE4SSLuaEventBridge::Hooks;
using namespace UE4SSLuaEventBridge::Hooks::Testing;

namespace
{
using Runtime = HookRuntime<FakeFrame>;
using Clock = Runtime::Clock;

// Fake objects: the address is the key; index, serial and name are looked up.
struct FakeObject
{
    int32_t index{};
    int32_t serial{};
    uint32_t name{};
};

class FakePlatform final : public HookPlatform<FakeFrame>
{
public:
    std::map<std::uintptr_t, FakeObject> objects;
    const void* out_parms{};               // non-null: the frame has OutParms
    std::map<std::uintptr_t, const void*> out_addresses; // property → value address
    bool game_thread{true};

    std::byte* locals(FakeFrame& frame) override { return static_cast<std::byte*>(frame.locals); }
    bool has_out_parms(FakeFrame&) override { return out_parms != nullptr; }
    const void* out_param_address(FakeFrame&, std::uintptr_t property) override
    {
        const auto found = out_addresses.find(property);
        return found == out_addresses.end() ? nullptr : found->second;
    }
    bool passes(const Filter& filter, std::uintptr_t object) override
    {
        if (!filter.object && filter.name_indices.empty() && filter.outer_class == 0) return true;
        const auto found = objects.find(object);
        if (found == objects.end()) return false;
        View view(found->second);
        return Hooks::passes(filter, view);
    }
    ObjectIdentity context_identity(std::uintptr_t object) override { return identity(object); }
    bool is_game_thread() override { return game_thread; }
    ObjectIdentity identity(std::uintptr_t address) override
    {
        const auto found = objects.find(address);
        if (found == objects.end()) return {address, -1, 0};
        return {address, found->second.index, found->second.serial};
    }
    std::string path(std::uintptr_t address) override { return "/Fake/Object_" + std::to_string(address % 1000); }

private:
    class View final : public ObjectView
    {
    public:
        explicit View(const FakeObject& object) : object_(object) {}
        int32_t index() const override { return object_.index; }
        int32_t serial() const override { return object_.serial; }
        uint32_t name_index() const override { return object_.name; }
        std::uintptr_t class_address() const override { return 0; }
        std::uintptr_t outer_class(unsigned) const override { return 0; }
        bool is_a(std::uintptr_t, std::uintptr_t) const override { return false; }

    private:
        const FakeObject& object_;
    };
};

// One delivered dispatcher call, flattened as BridgeMod.cpp's LuaHookCall
// would push it.
struct Delivered
{
    uint64_t session{};
    uint64_t hook{};
    Phase phase{};
    uint64_t sequence{};
    bool params_available{};
    ObjectIdentity object{};
    std::string object_path;
    std::map<std::string, std::string> values;
};

class Triplets final : public TripletSink
{
public:
    explicit Triplets(Delivered& out) : out_(out) {}
    void begin(std::size_t) override {}
    void boolean(const std::string& name, bool value) override { out_.values[name] = value ? "true" : "false"; }
    void integer(const std::string& name, int64_t value) override { out_.values[name] = std::to_string(value); }
    void number(const std::string& name, double value) override { out_.values[name] = std::to_string(value); }
    void string(const std::string& name, const std::string& utf8) override { out_.values[name] = utf8; }
    void enumeration(const std::string& name, const std::string& text) override { out_.values[name] = text; }
    void object(const std::string& name, const std::string& text) override { out_.values[name] = text; }
    void unsupported(const std::string& name) override { out_.values[name] = "nil"; }

private:
    Delivered& out_;
};

// The fake Lua side: sessions that can stop, and what reached them.
struct FakeLua
{
    std::map<uint64_t, bool> active;
    std::vector<Delivered> events;
    std::vector<HookNotice> notices;

    EventOutcome deliver(QueuedHookEvent& queued)
    {
        const auto found = active.find(queued.event.session);
        if (found == active.end() || !found->second) return EventOutcome::discarded;
        Delivered out;
        out.session = queued.event.session;
        out.hook = queued.event.hook;
        out.phase = queued.event.phase;
        out.sequence = queued.event.sequence;
        out.params_available = queued.event.params_available;
        out.object = queued.event.object;
        out.object_path = queued.event.object_path;
        Triplets sink(out);
        CHECK(queued.plan->flattener.flatten(queued.event.payload.data(), queued.event.payload.size(), nullptr, sink));
        events.push_back(std::move(out));
        return EventOutcome::delivered;
    }

    void notice(const HookNotice& item) { notices.push_back(item); }
};

// Parameter block of the fake functions: Value int32 @0, Flag bool @4,
// Out int32 @8 (out), ReturnValue int32 @12. ParmsSize 16.
std::shared_ptr<const Schema> make_schema()
{
    auto schema = std::make_shared<Schema>();
    const auto field = [](const char* name, FieldKind kind, uint32_t offset, uint32_t size, uint64_t flags) {
        Field out;
        out.name = name;
        out.class_name = kind == FieldKind::boolean ? "BoolProperty" : "IntProperty";
        out.kind = kind;
        out.offset = offset;
        out.size = size;
        out.flags = flags;
        return out;
    };
    schema->fields.push_back(field("Value", FieldKind::int32, 0, 4, cpf_parm));
    schema->fields.push_back(field("Flag", FieldKind::boolean, 4, 1, cpf_parm));
    schema->fields.push_back(field("Out", FieldKind::int32, 8, 4, cpf_parm | cpf_out_parm));
    schema->fields.push_back(field("ReturnValue", FieldKind::int32, 12, 4, cpf_parm | cpf_out_parm | cpf_return_parm));
    schema->parms_size = 16;
    schema->return_field = static_cast<uint16_t>(3);
    return schema;
}

constexpr std::uintptr_t property_base = 0x5000;

std::vector<std::uintptr_t> property_addresses()
{
    return {property_base, property_base + 1, property_base + 2, property_base + 3};
}

struct Parms
{
    int32_t value{};
    uint8_t flag{};
    uint8_t pad[3]{};
    int32_t out{};
    int32_t ret{};
};
static_assert(sizeof(Parms) == 16);

// Subscribes as HookFunction does: under the writer, with a plan.
Subscription* subscribe(Runtime& runtime, HookTable& table, uint32_t entry, uint64_t session, uint64_t hook,
    uint8_t phases, Filter filter = {}, uint32_t max_queued = default_max_queued_per_hook,
    std::vector<uint16_t> selection = {}, bool object_paths = false)
{
    auto plan = std::make_shared<SubscriptionPlan>(make_schema(), property_addresses(), selection, object_paths);
    auto writer = table.write();
    auto* subscription = writer.subscribe(entry, session, hook);
    subscription->phases = phases;
    subscription->filter = std::move(filter);
    subscription->fields = selection;
    subscription->object_paths = object_paths;
    subscription->max_queued = max_queued;
    runtime.set_plan(subscription, std::move(plan));
    runtime.add_record(hook, HookRecord{session, entry, false});
    return subscription;
}

DispatchBudget budget(std::size_t count = default_max_hook_events_per_pass)
{
    return DispatchBudget(DispatchBudget::Clock::now(), count, std::chrono::microseconds(0));
}

template <typename Rig>
void run_blueprint(FakeFunction& function, Parms& parms, RC::Unreal::UObject* object)
{
    FakeFrame frame;
    frame.locals = &parms;
    run_script(Rig::natives.table.data(), function, frame, object);
}

struct TickRig {};
struct LoopRig {};
struct NativeRig {};
struct StressRig {};
struct OrphanRig {};
}

int main()
{
    // ---- options and selection (portable decoding of HookFunction's text)
    {
        HookOptions options;
        CHECK(!parse_hook_options("names=Reticle|TopArrow; outer=4096; exact=1; obj=8192:7; paths=1; maxq=12; "
                                  "params=Value|Struct.Field; unknown=x",
            options));
        CHECK(options.names.size() == 2 && options.names[1] == "TopArrow");
        CHECK(options.outer == 4096 && options.exact && options.has_object && options.object_address == 8192 &&
            options.object_token == 7);
        CHECK(options.paths && options.max_queued == 12 && options.params.size() == 2 && !options.sync);
        CHECK(!parse_hook_options("", options) && options.names.empty() &&
            options.max_queued == default_max_queued_per_hook);
        CHECK(parse_hook_options("maxq=0", options).has_value());
        CHECK(parse_hook_options("obj=12", options).has_value());
        CHECK(parse_hook_options("names=a||b", options).has_value());
        CHECK(parse_hook_options("exact=yes", options).has_value());
        CHECK(!parse_hook_options("sync=1", options) && options.sync);

        const auto schema = make_schema();
        std::vector<uint16_t> selection;
        CHECK(!select_fields(*schema, {"Flag", "ReturnValue"}, selection) && selection.size() == 2);
        CHECK(select_fields(*schema, {"Missing"}, selection)->find("unknown parameter: Missing") != std::string::npos);
        CHECK(pre_selection(*schema, {}).size() == 2);   // Value, Flag: no pure out, no return
        CHECK(post_selection(*schema, {}).size() == 4);
        CHECK(post_selection(*schema, {1}).size() == 2); // Flag and the return value
        CHECK(pre_selection(*schema, {3}).empty());
    }

    // ---- tick drain: Blueprint pre and post, filters, budget, ticks
    {
        using Rig = FakeHookRig<TickRig>;
        FakePlatform platform;
        Runtime runtime(platform, 1000);
        Rig::context.set_sink(&Runtime::sink, &runtime);
        CHECK(Rig::claim().slots.has_value());
        FakeFunction function{fake_op_count, ex_return, ex_nothing, ex_end_of_script};
        DeleteNotice notice{Rig::table, function};
        // The slots the claim wrote.
        uint8_t slot_a = 0;
        uint8_t slot_b = 0;
        for (std::size_t index = gnatives_count; index-- > 0;)
        {
            if (Rig::natives.table[index] == &Rig::entry_handler) slot_a = static_cast<uint8_t>(index);
            if (Rig::natives.table[index] == &Rig::exit_handler) slot_b = static_cast<uint8_t>(index);
        }
        const auto attached = Rig::hook(function, slot_a, slot_b);
        CHECK(attached.ok());

        FakeObject reticle{3, 30, 77};
        FakeObject other{4, 40, 78};
        alignas(8) std::array<std::byte, 8> reticle_storage{};
        alignas(8) std::array<std::byte, 8> other_storage{};
        auto* reticle_object = reinterpret_cast<RC::Unreal::UObject*>(reticle_storage.data());
        auto* other_object = reinterpret_cast<RC::Unreal::UObject*>(other_storage.data());
        platform.objects[reinterpret_cast<std::uintptr_t>(reticle_object)] = reticle;
        platform.objects[reinterpret_cast<std::uintptr_t>(other_object)] = other;

        Filter by_name;
        by_name.name_indices = {77};
        auto* all = subscribe(runtime, Rig::table, attached.entry, 1, 101, 3);
        auto* named = subscribe(runtime, Rig::table, attached.entry, 1, 102, 1, by_name, 256, {}, true);
        runtime.set_delivery(HookDelivery::tick);

        Parms parms{};
        parms.value = 42;
        parms.flag = 1;
        parms.out = 5;
        parms.ret = 9;
        run_blueprint<Rig>(function, parms, reticle_object);
        run_blueprint<Rig>(function, parms, other_object);
        CHECK(runtime.stats().queued == 5); // all: 2 calls x pre and post; named: one pre

        FakeLua lua;
        lua.active[1] = true;
        const auto deliver = [&](QueuedHookEvent& queued) { return lua.deliver(queued); };
        const auto notices = [&](const HookNotice& item) { lua.notice(item); };
        int before_calls = 0;
        const auto before = [&] { ++before_calls; };
        const auto now = [] { return DispatchBudget::Clock::now(); };

        runtime.tick(budget(2), before, deliver, notices, now);
        CHECK(lua.events.size() == 2 && runtime.ticks() == 1 && before_calls == 1);
        runtime.tick(budget(), before, deliver, notices, now);
        CHECK(lua.events.size() == 5 && runtime.ticks() == 2);
        runtime.tick(budget(), before, deliver, notices, now); // nothing queued: still a frame
        CHECK(lua.events.size() == 5 && runtime.ticks() == 3);
        CHECK(!runtime.update(Clock::now(), budget(), deliver, notices, now)); // tick mode: no loop pass
        CHECK(runtime.update_calls() == 0);

        const auto& first = lua.events[0];
        CHECK(first.hook == 101 && first.phase == Phase::pre && first.params_available);
        CHECK(first.values.at("Value") == "42" && first.values.at("Flag") == "true");
        CHECK(first.values.count("out:Out") == 0 && first.values.count("ret") == 0); // pre: inputs only
        CHECK(first.object.index == 3 && first.object.serial == 30);
        uint64_t last_sequence = 0;
        bool ordered = true;
        for (const auto& event : lua.events)
        {
            ordered = ordered && event.sequence > last_sequence;
            last_sequence = event.sequence;
        }
        CHECK(ordered);
        std::size_t named_events = 0;
        for (const auto& event : lua.events)
        {
            if (event.hook == 102)
            {
                ++named_events;
                CHECK(event.object.index == 3 && !event.object_path.empty());
            }
            if (event.hook == 101 && event.phase == Phase::post)
                CHECK(event.values.at("out:Out") == "5" && event.values.at("ret") == "9");
        }
        CHECK(named_events == 1);
        CHECK(all->counters.calls.load() == 2 && all->counters.matched.load() == 2 &&
            all->counters.delivered.load() == 4 && all->queued.load() == 0);
        CHECK(named->counters.calls.load() == 2 && named->counters.matched.load() == 1 &&
            named->counters.delivered.load() == 1);

        // Post with OutParms: out and return values come from FindOutParamValueAddress.
        int32_t redirected_out = 70;
        int32_t redirected_ret = 71;
        platform.out_parms = &redirected_out;
        platform.out_addresses[property_base + 2] = &redirected_out;
        platform.out_addresses[property_base + 3] = &redirected_ret;
        lua.events.clear();
        run_blueprint<Rig>(function, parms, other_object);
        runtime.tick(budget(), before, deliver, notices, now);
        bool redirected = false;
        for (const auto& event : lua.events)
            if (event.hook == 101 && event.phase == Phase::post)
                redirected = event.values.at("out:Out") == "70" && event.values.at("ret") == "71" &&
                    event.values.at("Value") == "42";
        CHECK(redirected);
        platform.out_parms = nullptr;

        // A stopped session: its events are discarded, not delivered, and the
        // per-hook count still drops back to zero.
        lua.active[1] = false;
        lua.events.clear();
        run_blueprint<Rig>(function, parms, reticle_object);
        runtime.tick(budget(), before, deliver, notices, now);
        CHECK(lua.events.empty() && all->queued.load() == 0 && named->queued.load() == 0);
        CHECK(all->counters.delivered.load() == 6);
        lua.active[1] = true;

        // Per-hook limit: max_queued 2 drops the rest of a burst.
        {
            auto writer = Rig::table.write();
            writer.unsubscribe(attached.entry, 1, 102);
            all->max_queued = 2;
        }
        for (int call = 0; call < 3; ++call) run_blueprint<Rig>(function, parms, reticle_object);
        CHECK(all->queued.load() == 2 && all->counters.dropped.load() == 4); // 3 calls x 2 phases, 2 kept
        lua.events.clear();
        runtime.tick(budget(), before, deliver, notices, now);
        CHECK(lua.events.size() == 2 && all->queued.load() == 0);

        // Unload: nothing is recorded or delivered after set_inert.
        runtime.set_inert();
        CHECK(runtime.wait_for_drain(std::chrono::milliseconds(10)));
        run_blueprint<Rig>(function, parms, reticle_object);
        CHECK(runtime.stats().queued == 0);
        const auto ticks = runtime.ticks();
        runtime.tick(budget(), before, deliver, notices, now);
        CHECK(runtime.ticks() == ticks);
        Rig::unhook(function, attached.entry);
        Rig::context.set_sink(nullptr, nullptr);
    }

    // ---- loop fallback: on_update passes on the schedule; a queue limit
    {
        using Rig = FakeHookRig<LoopRig>;
        FakePlatform platform;
        Runtime runtime(platform, 3, 10); // queue of 3, 10 checks per second
        Rig::context.set_sink(&Runtime::sink, &runtime);
        CHECK(Rig::claim().slots.has_value());
        uint8_t slot_a = 0;
        uint8_t slot_b = 0;
        for (std::size_t index = gnatives_count; index-- > 0;)
        {
            if (Rig::natives.table[index] == &Rig::entry_handler) slot_a = static_cast<uint8_t>(index);
            if (Rig::natives.table[index] == &Rig::exit_handler) slot_b = static_cast<uint8_t>(index);
        }
        FakeFunction function{fake_op_count, ex_return, ex_nothing, ex_end_of_script};
        DeleteNotice notice{Rig::table, function};
        const auto attached = Rig::hook(function, slot_a, slot_b);
        CHECK(attached.ok());
        auto* subscription = subscribe(runtime, Rig::table, attached.entry, 2, 201, 2);
        runtime.set_delivery(HookDelivery::loop);

        Parms parms{};
        parms.value = 7;
        for (int call = 0; call < 5; ++call) run_blueprint<Rig>(function, parms, nullptr);
        CHECK(runtime.stats().queued == 3 && runtime.stats().rejected == 2 && runtime.stats().high_water == 3);
        CHECK(subscription->counters.dropped.load() == 2);

        FakeLua lua;
        lua.active[2] = true;
        const auto deliver = [&](QueuedHookEvent& queued) { return lua.deliver(queued); };
        const auto notices = [&](const HookNotice& item) { lua.notice(item); };
        const auto now = [] { return DispatchBudget::Clock::now(); };
        const auto start = Clock::now();
        CHECK(runtime.update(start, budget(), deliver, notices, now));
        CHECK(lua.events.size() == 3 && runtime.update_calls() == 1);
        CHECK(!runtime.update(start + std::chrono::milliseconds(50), budget(), deliver, notices, now));
        CHECK(runtime.update(start + std::chrono::milliseconds(120), budget(), deliver, notices, now));
        CHECK(runtime.update_calls() == 2);
        // The tick body counts frames in loop mode but never drains.
        run_blueprint<Rig>(function, parms, nullptr);
        runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(runtime.ticks() == 1 && runtime.stats().queued == 1 && lua.events.size() == 3);
        CHECK(lua.events[0].phase == Phase::post && lua.events[0].values.at("Value") == "7" &&
            lua.events[0].params_available);
        Rig::unhook(function, attached.entry);
        Rig::context.set_sink(nullptr, nullptr);
    }

    // ---- native thunk: ProcessEvent path and the Blueprint → native path
    {
        using Rig = FakeHookRig<NativeRig>;
        FakePlatform platform;
        Runtime runtime(platform);
        Rig::context.set_sink(&Runtime::sink, &runtime);
        FakeFunction function;
        function.flags = func_native;
        function.set_func(as_native(+[](RC::Unreal::UObject*, FakeFrame&, void* result) {
            if (result != nullptr) *static_cast<int32_t*>(result) = 33;
        }));
        DeleteNotice notice{Rig::table, function};
        const auto attached = Rig::hook_native(function);
        CHECK(attached.ok());
        subscribe(runtime, Rig::table, attached.entry, 3, 301, 3);
        runtime.set_delivery(HookDelivery::tick);
        FakeLua lua;
        lua.active[3] = true;
        const auto deliver = [&](QueuedHookEvent& queued) { return lua.deliver(queued); };
        const auto notices = [&](const HookNotice& item) { lua.notice(item); };
        const auto now = [] { return DispatchBudget::Clock::now(); };

        Parms parms{};
        parms.value = 4;
        FakeFrame through_process_event;
        through_process_event.node = &function;
        through_process_event.locals = &parms;
        invoke(function, nullptr, through_process_event, &parms.ret);

        FakeFrame from_blueprint;
        int caller = 0;
        from_blueprint.node = &caller; // a Blueprint caller's frame
        from_blueprint.locals = &caller;
        int32_t result = 0;
        invoke(function, nullptr, from_blueprint, &result);
        runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(lua.events.size() == 4);
        if (lua.events.size() == 4)
        {
            CHECK(lua.events[0].params_available && lua.events[0].values.at("Value") == "4");
            CHECK(lua.events[1].phase == Phase::post && lua.events[1].values.at("ret") == "33");
            CHECK(!lua.events[2].params_available && lua.events[2].values.empty());
            CHECK(!lua.events[3].params_available && lua.events[3].values.size() == 1 &&
                lua.events[3].values.at("ret") == "33");
        }
        Rig::unhook_native(function, attached.entry);
        Rig::context.set_sink(nullptr, nullptr);
    }

    // ---- notices: pending names wake sessions up; a deleted function orphans its hooks
    {
        using Rig = FakeHookRig<OrphanRig>;
        FakePlatform platform;
        Runtime runtime(platform);
        Rig::context.set_sink(&Runtime::sink, &runtime);
        runtime.set_delivery(HookDelivery::tick);
        FakeLua lua;
        lua.active[4] = true;
        lua.active[5] = true;
        const auto deliver = [&](QueuedHookEvent& queued) { return lua.deliver(queued); };
        const auto notices = [&](const HookNotice& item) { lua.notice(item); };
        const auto now = [] { return DispatchBudget::Clock::now(); };

        CHECK(runtime.pending_names() == 0);
        runtime.on_object_created(500); // nothing pending: no lock, no notice
        runtime.add_pending_name(4, 1, 500);
        runtime.add_pending_name(5, 9, 500);
        runtime.add_pending_name(4, 2, 600);
        CHECK(runtime.pending_names() == 3);
        runtime.on_object_created(501);
        runtime.on_object_created(500);
        runtime.on_object_created(500); // one wake-up per token until delivered
        runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(lua.notices.size() == 2);
        for (const auto& item : lua.notices)
            CHECK(item.code == message_hook_wakeup &&
                ((item.session == 4 && item.id == 1) || (item.session == 5 && item.id == 9)));
        CHECK(runtime.forget_pending_name(4, 1) && !runtime.forget_pending_name(4, 1));
        runtime.forget_session(4);
        CHECK(runtime.pending_names() == 1);
        lua.notices.clear();
        runtime.on_object_created(600);
        runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(lua.notices.empty());

        FakeFunction function;
        function.flags = func_native;
        function.set_func(as_native(+[](RC::Unreal::UObject*, FakeFrame&, void*) {}));
        const auto attached = Rig::hook_native(function);
        CHECK(attached.ok());
        subscribe(runtime, Rig::table, attached.entry, 4, 401, 1);
        subscribe(runtime, Rig::table, attached.entry, 5, 501, 1);
        Parms parms{};
        FakeFrame frame;
        frame.node = &function;
        frame.locals = &parms;
        invoke(function, nullptr, frame, nullptr); // queued before the delete
        const auto orphaned = Rig::table.on_deleted(&function);
        CHECK(orphaned.has_value());
        CHECK(runtime.on_orphaned(Rig::table, *orphaned) == 2);
        CHECK(runtime.record(401)->orphaned && runtime.record(501)->orphaned);
        lua.notices.clear();
        lua.events.clear();
        runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(lua.notices.size() == 2 && lua.notices[0].code == message_hook_pending &&
            lua.notices[0].reason == "function was deleted");
        CHECK(lua.events.empty()); // the subscriptions went inactive with the function
        CHECK(runtime.erase_record(401) && !runtime.record(401));
        CHECK(runtime.session_hooks(5).size() == 1);
        Rig::context.set_sink(nullptr, nullptr);
    }

    // ---- producers on worker threads while the tick drains on this one
    {
        using Rig = FakeHookRig<StressRig>;
        FakePlatform platform;
        platform.game_thread = false;
        Runtime runtime(platform, 1u << 20);
        Rig::context.set_sink(&Runtime::sink, &runtime);
        FakeFunction function;
        function.flags = func_native;
        function.set_func(as_native(+[](RC::Unreal::UObject*, FakeFrame&, void*) {}));
        DeleteNotice notice{Rig::table, function};
        const auto attached = Rig::hook_native(function);
        CHECK(attached.ok());
        auto* subscription = subscribe(runtime, Rig::table, attached.entry, 6, 601, 3, {}, 1000000);
        runtime.set_delivery(HookDelivery::tick);
        FakeLua lua;
        lua.active[6] = true;
        std::atomic<std::size_t> delivered{0};
        const auto deliver = [&](QueuedHookEvent& queued) {
            delivered.fetch_add(1);
            CHECK(!queued.event.game_thread && queued.event.payload.size() > 0);
            return EventOutcome::delivered;
        };
        const auto notices = [&](const HookNotice&) {};
        const auto now = [] { return DispatchBudget::Clock::now(); };

        constexpr int threads = 4;
        constexpr int calls = 2000;
        std::atomic<int> done{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < threads; ++worker)
        {
            workers.emplace_back([&] {
                Parms parms{};
                FakeFrame frame;
                frame.node = &function;
                frame.locals = &parms;
                for (int call = 0; call < calls; ++call)
                {
                    parms.value = call;
                    invoke(function, nullptr, frame, &parms.ret);
                }
                done.fetch_add(1);
            });
        }
        while (done.load() < threads) runtime.tick(budget(), [] {}, deliver, notices, now);
        for (auto& worker : workers) worker.join();
        while (runtime.stats().queued != 0) runtime.tick(budget(), [] {}, deliver, notices, now);
        CHECK(delivered.load() == static_cast<std::size_t>(threads * calls * 2));
        CHECK(subscription->counters.calls.load() == static_cast<uint64_t>(threads * calls));
        CHECK(subscription->counters.delivered.load() == static_cast<uint64_t>(threads * calls * 2));
        CHECK(subscription->queued.load() == 0 && subscription->counters.dropped.load() == 0);
        Rig::unhook_native(function, attached.entry);
        Rig::context.set_sink(nullptr, nullptr);
    }

    return LexTests::finish("NativeHooksLifecycleTests");
}
