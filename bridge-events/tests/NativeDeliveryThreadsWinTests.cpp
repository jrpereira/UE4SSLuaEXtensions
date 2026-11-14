// Windows native integration for the single Lua consumer thread
// (LuaDelivery.hpp): the production input backend (UE imports replaced by
// controlled stubs, as in NativeBackendLifecycleWinTests.cpp) and the real
// function-hook runtime deliver into one fake Lua session from two threads,
// an injected engine tick (the game thread, which also runs the native input
// delegates and queues hook work) and on_update. The fake session counts its
// entry depth: any overlap of two calls fails. This is not Unreal runtime
// acceptance.
#define UE4SS_IMPORT
#include "../src/EnhancedInputBackend.cpp"
#include <HookRuntime.hpp>
#include <LuaDelivery.hpp>
#include <SharedDispatch.hpp>
#include <TestSupport.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace UE4SSLuaEventBridge {
struct LuaSession {
    uint64_t id{};
    std::atomic_bool active{true};
    LoopStart loop_start; // guarded by Rig::session_mutex
};
}
namespace RC::Unreal {
struct FUObjectItem { alignas(8) std::byte bytes[0x18]{}; };
static std::array<FUObjectItem,2> objects;
static UObject* component{};
static UObject* action_object{};
static thread_local bool game_thread = true;
static std::atomic<int> allocations{};
FUObjectItem* FUObjectArray::IndexToObject(int32_t index) {
    return index >= 0 && index < 2 ? &objects[static_cast<std::size_t>(index)] : nullptr;
}
bool IsInGameThread() { return game_thread; }
int32_t& UStruct::GetPropertiesSize() {
    static int32_t size = static_cast<int32_t>(UE4SSLuaEventBridge::EnhancedInputABI::enhanced_input_component_size);
    return size;
}
static UClass input_action_class;
UStruct*& UStruct::GetSuperStruct() {
    static UStruct* none{};
    none = nullptr;
    return none;
}
std::wstring UObject::GetPathName(UObject*) const {
    if (this==&input_action_class) return L"/Script/EnhancedInput.InputAction";
    if (this==component) return L"/Game/Maps/Map.Map:PersistentLevel.PlayerController_0.InputComponent0";
    return L"/Engine/Transient.IA_MCC_Test";
}
namespace UObjectGlobals {
void FindAllOf(const wchar_t* name, std::vector<UObject*>& out) {
    if (std::wstring_view(name) == L"EnhancedInputComponent") out.push_back(component);
    if (std::wstring_view(name) == L"InputAction") out.push_back(action_object);
}
}
void* FMemory::Malloc(std::size_t size, uint32_t) { auto* p=std::malloc(size); if(p) ++allocations; return p; }
void* FMemory::Realloc(void* old, std::size_t size, uint32_t) {
    auto* p=std::realloc(old,size); if(p && !old) ++allocations; return p;
}
void FMemory::Free(void* p) { if(p) { --allocations; std::free(p); } }
}

using namespace UE4SSLuaEventBridge;
namespace ABI=EnhancedInputABI;
using namespace RC::Unreal;
using namespace std::chrono_literals;

namespace {
const std::string component_path="/Game/Maps/Map.Map:PersistentLevel.PlayerController_0.InputComponent0";
const std::string action_path="/Engine/Transient.IA_MCC_Test";

struct Fixture {
    alignas(8) std::array<std::byte,ABI::enhanced_input_component_size> storage{};
    alignas(8) std::array<std::byte,0x20> action_storage{};
    UClass type;
    Fixture() {
        component=reinterpret_cast<UObject*>(storage.data());
        action_object=reinterpret_cast<UObject*>(action_storage.data());
        auto* cls=&type;
        std::memcpy(storage.data()+ABI::uobject_class_offset,&cls,sizeof(cls));
        auto* action_cls=&input_action_class;
        std::memcpy(action_storage.data()+ABI::uobject_class_offset,&action_cls,sizeof(action_cls));
        for(int32_t i=0;i<2;++i) {
            auto* obj=i==0?component:action_object;
            std::memcpy(reinterpret_cast<std::byte*>(obj)+0x0c,&i,sizeof(i));
            const int32_t serial=10+i;
            std::memcpy(objects[static_cast<std::size_t>(i)].bytes,&obj,sizeof(obj));
            std::memcpy(objects[static_cast<std::size_t>(i)].bytes+0x10,&serial,sizeof(serial));
        }
    }
    ABI::ActionEventBindingArray& bindings() {
        return *reinterpret_cast<ABI::ActionEventBindingArray*>(storage.data()+ABI::action_event_bindings_offset);
    }
    ~Fixture() {
        auto& a=bindings();
        for(int32_t i=0;i<a.size;++i) delete a.data[i];
        FMemory::Free(a.data); a={};
        objects={};
        CHECK(allocations==0);
    }
};

// The fake Lua side of BridgeMod.cpp's LuaSink: one entry at a time, from
// the expected consumer thread.
struct Sink {
    std::atomic<int> depth{}, overlaps{}, wrong_thread{}, inputs{}, hooks{}, faults{}, traces{}, reports{};
    std::atomic<std::thread::id> consumer{};
    std::mutex order_mutex;
    std::vector<uint64_t> loop_starts;
    uint64_t failing_token{};

    class Entry {
    public:
        explicit Entry(Sink& sink) : sink_(sink) {
            if (sink_.depth.fetch_add(1) != 0) ++sink_.overlaps;
            const auto expected = sink_.consumer.load();
            if (expected != std::thread::id{} && expected != std::this_thread::get_id()) ++sink_.wrong_thread;
            for (int i = 0; i < 20; ++i) std::this_thread::yield();
        }
        ~Entry() { sink_.depth.fetch_sub(1); }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
    private:
        Sink& sink_;
    };

    bool live(const LuaSession* session) const { return session && session->active.load(); }
    void input(LuaSession&, const EnhancedInputSubscription& subscription, const EnhancedInputEvent&) {
        const Entry entry(*this);
        ++inputs;
        if (subscription.callback_token == failing_token) throw std::runtime_error("callback failed");
    }
    void fault(LuaSession&, uint64_t, DeliveryFault) { const Entry entry(*this); ++faults; }
    void trace(LuaSession&, const std::string&) { const Entry entry(*this); ++traces; }
    void loop_start(LuaSession&, uint64_t token) {
        const Entry entry(*this);
        std::scoped_lock lock(order_mutex);
        loop_starts.push_back(token);
    }
    void hook(LuaSession&) { const Entry entry(*this); ++hooks; }
    void report(std::string_view, std::string_view) { ++reports; }
};

struct NoFrame {};
class NoPlatform final : public Hooks::HookPlatform<NoFrame> {
public:
    std::byte* locals(NoFrame&) override { return nullptr; }
    bool has_out_parms(NoFrame&) override { return false; }
    const void* out_param_address(NoFrame&, std::uintptr_t) override { return nullptr; }
    bool passes(const Hooks::Filter&, std::uintptr_t) override { return true; }
    Hooks::ObjectIdentity context_identity(std::uintptr_t object) override { return {object, -1, 0}; }
    bool is_game_thread() override { return true; }
    Hooks::ObjectIdentity identity(std::uintptr_t address) override { return {address, -1, 0}; }
};

// BridgeMod.cpp's glue: on_engine_tick, on_update, deliver_lua_pass, with the
// production backend and the real hook runtime.
struct Rig {
    explicit Rig(LuaDelivery mode) {
        consumer.gate().decide(mode);
        hooks.set_delivery(mode == LuaDelivery::tick ? Hooks::HookDelivery::tick : Hooks::HookDelivery::loop);
        backend.initialize();
        session.id = 1;
    }
    void mark_sessions_ready() {
        std::scoped_lock lock(session_mutex);
        if (!session.loop_start.ready) session.loop_start.mark_ready();
    }
    std::vector<std::pair<LuaSession*, uint64_t>> take_due_loop_starts() {
        std::vector<std::pair<LuaSession*, uint64_t>> due;
        std::scoped_lock lock(session_mutex);
        if (session.active.load())
            for (const auto token : session.loop_start.take_due()) due.emplace_back(&session, token);
        return due;
    }
    void deliver_lua_pass(bool input_due) {
        const auto now = [] { return DispatchBudget::Clock::now(); };
        pass.loop_start(take_due_loop_starts());
        if (!input_due) return;
        pass.faults();
        pass.input(now);
        pass.traces(now);
    }
    void notice(const Hooks::HookNotice& item) {
        if (item.session == session.id && sink.live(&session)) sink.hook(session);
    }
    bool tick() {
        return consumer.tick(
            [this] {
                hooks.tick(DispatchBudget(DispatchBudget::Clock::now(), 64, std::chrono::microseconds(0)), [] {},
                    [](Hooks::QueuedHookEvent&) { return Hooks::EventOutcome::discarded; },
                    [this](const Hooks::HookNotice& item) { notice(item); },
                    [] { return DispatchBudget::Clock::now(); });
            },
            [this](bool input_due) { deliver_lua_pass(input_due); });
    }
    bool update() {
        return consumer.update(LuaConsumer::Clock::now(), [this] { mark_sessions_ready(); },
            [this] {
                hooks.update(Hooks::HookRuntime<NoFrame>::Clock::now(),
                    DispatchBudget(DispatchBudget::Clock::now(), 64, std::chrono::microseconds(0)),
                    [](Hooks::QueuedHookEvent&) { return Hooks::EventOutcome::discarded; },
                    [this](const Hooks::HookNotice& item) { notice(item); },
                    [] { return DispatchBudget::Clock::now(); });
            },
            [this](bool input_due) { deliver_lua_pass(input_due); });
    }
    void hook_work() { hooks.notify(Hooks::HookNotice{Hooks::message_hook_wakeup, session.id, 1, {}}); }

    Fixture fixture;
    LuaConsumer consumer{1000};
    EnhancedInputBackend backend;
    Sink sink;
    LuaPass<EnhancedInputBackend, Sink> pass{backend, sink, 64, 0};
    NoPlatform platform;
    Hooks::HookRuntime<NoFrame> hooks{platform, 100000, 1000};
    std::mutex session_mutex;
    LuaSession session;
};

template <typename Step, typename Done>
bool settle(Step step, Done done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        step();
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Tick mode: the game thread runs the native delegates, queues hook work and
// runs the engine tick; on_update runs alongside and must make no Lua call.
void tick_mode() {
    Rig rig(LuaDelivery::tick);
    ABI::InputActionInstanceView event{};
    auto target=rig.backend.open_target(rig.session,component_path).first; CHECK(target);
    CHECK(rig.backend.enable_delivery_faults(rig.session,target).empty()); // before the first bind
    CHECK(rig.backend.subscribe(rig.session,target,1,action_path,"Triggered",ABI::TriggerEvent::Triggered).first);
    auto* delegate=rig.fixture.bindings().data[0];
    {
        std::scoped_lock lock(rig.session_mutex);
        rig.session.loop_start.callbacks.insert(2);
        rig.session.loop_start.callbacks.insert(1);
    }
    CHECK(rig.tick());
    CHECK(rig.sink.loop_starts.empty()); // waits for on_update's barrier

    constexpr int frames = 500;
    std::atomic_bool ticking{true};
    std::atomic<int> update_passes{};
    std::thread game([&] {
        game_thread = true;
        rig.sink.consumer = std::this_thread::get_id();
        for (int frame = 0; frame < frames; ++frame) {
            delegate->Execute(event);
            delegate->Execute(event);
            rig.hook_work();
            CHECK(rig.tick());
        }
        ticking = false;
    });
    std::thread loop([&] {
        game_thread = false;
        while (ticking.load()) {
            if (rig.update()) ++update_passes;
            std::this_thread::yield();
        }
    });
    game.join();
    loop.join();
    rig.sink.consumer = std::this_thread::get_id();
    CHECK(settle([&] { rig.tick(); }, [&] { return rig.sink.inputs.load() == 2 * frames && rig.sink.hooks.load() == frames; }));
    CHECK(update_passes.load() == 0);
    CHECK(rig.sink.overlaps.load() == 0 && rig.sink.wrong_thread.load() == 0);
    CHECK(rig.sink.inputs.load() == 2 * frames && rig.sink.hooks.load() == frames);
    CHECK((rig.sink.loop_starts == std::vector<uint64_t>{1, 2}));
    CHECK(rig.backend.queue_stats().rejected == 0);

    // A failing callback disables its binding and reports the fault in the same pass.
    rig.sink.failing_token = 1;
    delegate->Execute(event);
    CHECK(rig.tick());
    CHECK(rig.sink.inputs.load() == 2 * frames + 1 && rig.sink.faults.load() == 1 && rig.sink.reports.load() == 1);
    delegate->Execute(event);
    CHECK(rig.tick());
    CHECK(rig.sink.inputs.load() == 2 * frames + 1); // the binding is off

    rig.consumer.gate().set_inert();
    CHECK(rig.consumer.gate().wait_idle(1s));
    CHECK(!rig.tick() && !rig.update());
    rig.backend.unsubscribe_all(rig.session); CHECK(rig.backend.shutdown());
}

// Loop mode: on_update delivers input and hook work; the tick never does.
void loop_mode() {
    Rig rig(LuaDelivery::loop);
    ABI::InputActionInstanceView event{};
    auto target=rig.backend.open_target(rig.session,component_path).first; CHECK(target);
    CHECK(rig.backend.subscribe(rig.session,target,1,action_path,"Triggered",ABI::TriggerEvent::Triggered).first);
    auto* delegate=rig.fixture.bindings().data[0];
    {
        std::scoped_lock lock(rig.session_mutex);
        rig.session.loop_start.callbacks.insert(7);
    }
    constexpr int rounds = 300;
    std::atomic_bool producing{true};
    std::atomic<int> tick_passes{};
    std::thread game([&] {
        game_thread = true;
        for (int round = 0; round < rounds; ++round) {
            delegate->Execute(event);
            rig.hook_work();
            if (rig.tick()) ++tick_passes;
            std::this_thread::yield();
        }
        producing = false;
    });
    std::thread loop([&] {
        game_thread = false;
        rig.sink.consumer = std::this_thread::get_id();
        while (producing.load()) rig.update();
        CHECK(settle([&] { rig.update(); }, [&] { return rig.sink.inputs.load() == rounds && rig.sink.hooks.load() == rounds; }));
    });
    game.join();
    loop.join();
    CHECK(tick_passes.load() == 0);
    CHECK(rig.sink.overlaps.load() == 0 && rig.sink.wrong_thread.load() == 0);
    CHECK(rig.sink.inputs.load() == rounds && rig.sink.hooks.load() == rounds);
    CHECK((rig.sink.loop_starts == std::vector<uint64_t>{7}));
    CHECK(rig.backend.unsubscribe_all(rig.session)==1); CHECK(rig.backend.shutdown());
}
}

int main() {
    tick_mode();
    loop_mode();
    return LexTests::finish("NativeDeliveryThreadsTests");
}
