// Windows native integration: compile the production backend, replacing only
// UE imports with controlled stubs. This is not Unreal runtime/GC acceptance.
#define UE4SS_IMPORT
#include "../src/EnhancedInputBackend.cpp"
#include <array>
#include <barrier>
#include <SharedDispatch.hpp>
#include <TestSupport.hpp>
#include <cstdlib>
#include <thread>

// Inject the first vector allocation failure without changing production code.
static thread_local bool fail_next_cpp_allocation{};
void* operator new(std::size_t size) {
    if (std::exchange(fail_next_cpp_allocation, false)) throw std::bad_alloc{};
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace UE4SSLuaEventBridge { struct LuaSession {}; }
namespace RC::Unreal {
struct FUObjectItem { alignas(8) std::byte bytes[0x18]{}; };
static std::array<FUObjectItem,3> objects;
static UObject* component{};
static UObject* action_object{};
static UObject* asset_action{};
static thread_local bool game_thread = true;
static std::atomic<int> allocations{};
static bool fail_allocation{};
FUObjectItem* FUObjectArray::IndexToObject(int32_t index) {
    return index >= 0 && index < 3 ? &objects[static_cast<std::size_t>(index)] : nullptr;
}
bool IsInGameThread() { return game_thread; }
int32_t& UStruct::GetPropertiesSize() {
    static int32_t size = static_cast<int32_t>(UE4SSLuaEventBridge::EnhancedInputABI::enhanced_input_component_size);
    return size;
}
// The class of both actions; its own class chain ends here.
static UClass input_action_class;
UStruct*& UStruct::GetSuperStruct() {
    static UStruct* none{};
    none = nullptr;
    return none;
}
std::wstring UObject::GetPathName(UObject*) const {
    if (this==&input_action_class) return L"/Script/EnhancedInput.InputAction";
    if (this==component) return L"/Game/Maps/Map.Map:PersistentLevel.PlayerController_0.InputComponent0";
    if (this==asset_action) return L"/Game/Input/Actions/IA_ToggleQuickslots.IA_ToggleQuickslots";
    return L"/Engine/Transient.IA_MCC_Test";
}
static std::atomic<int> scans{};
namespace UObjectGlobals {
// A full object walk, counted: the cost the backend must not repeat per call.
void FindAllOf(const wchar_t* name, std::vector<UObject*>& out) {
    ++scans;
    if (std::wstring_view(name) == L"EnhancedInputComponent") out.push_back(component);
    if (std::wstring_view(name) == L"InputAction") { out.push_back(action_object); out.push_back(asset_action); }
}
}
void* FMemory::Malloc(std::size_t size, uint32_t) {
    if (fail_allocation) return nullptr;
    auto* p=std::malloc(size); if(p) ++allocations; return p;
}
void* FMemory::Realloc(void* old, std::size_t size, uint32_t) {
    if (fail_allocation) return nullptr;
    auto* p=std::realloc(old,size); if(p && !old) ++allocations; return p;
}
void FMemory::Free(void* p) { if(p) { --allocations; std::free(p); } }
}
using namespace UE4SSLuaEventBridge;
namespace ABI=EnhancedInputABI;
using namespace RC::Unreal;
static const std::string component_path="/Game/Maps/Map.Map:PersistentLevel.PlayerController_0.InputComponent0";
static const std::string action_path="/Engine/Transient.IA_MCC_Test";
static const std::string asset_action_path="/Game/Input/Actions/IA_ToggleQuickslots.IA_ToggleQuickslots";
struct Fixture {
    alignas(8) std::array<std::byte,ABI::enhanced_input_component_size> storage{};
    alignas(8) std::array<std::byte,0x20> action_storage{};
    alignas(8) std::array<std::byte,0x20> asset_action_storage{};
    UClass type;
    Fixture() {
        component=reinterpret_cast<UObject*>(storage.data());
        action_object=reinterpret_cast<UObject*>(action_storage.data());
        asset_action=reinterpret_cast<UObject*>(asset_action_storage.data());
        auto* cls=&type;
        std::memcpy(storage.data()+ABI::uobject_class_offset,&cls,sizeof(cls));
        auto* action_cls=&input_action_class;
        std::memcpy(action_storage.data()+ABI::uobject_class_offset,&action_cls,sizeof(action_cls));
        std::memcpy(asset_action_storage.data()+ABI::uobject_class_offset,&action_cls,sizeof(action_cls));
        for(int32_t i=0;i<3;++i) {
            auto* obj=i==0?component:i==1?action_object:asset_action;
            std::memcpy(reinterpret_cast<std::byte*>(obj)+0x0c,&i,sizeof(i));
            const int32_t serial=10+i;
            std::memcpy(objects[i].bytes,&obj,sizeof(obj));
            std::memcpy(objects[i].bytes+0x10,&serial,sizeof(serial));
        }
    }
    ABI::ActionEventBindingArray& bindings() {
        return *reinterpret_cast<ABI::ActionEventBindingArray*>(storage.data()+ABI::action_event_bindings_offset);
    }
    void destroy_component() {
        auto& a=bindings();
        for(int32_t i=0;i<a.size;++i) delete a.data[i];
        FMemory::Free(a.data); a={};
        objects[0]={}; // Invalidate object-array identity before backend cleanup.
    }
    ~Fixture() { destroy_component(); CHECK(allocations==0); }
};
uint64_t bind(EnhancedInputBackend& backend, LuaSession& session) {
    auto target=backend.open_target(session,component_path); CHECK(target.first && target.second.empty());
    auto subscription=backend.subscribe(session,target.first,1,action_path,"Triggered",ABI::TriggerEvent::Triggered);
    CHECK(subscription.first && subscription.second.empty()); return subscription.first;
}
int main() {
    ABI::InputActionInstanceView event{};
    // A lost terminal phase invalidates the entire opted-in target, including
    // an already-drained Started and every later delegate call. Notification
    // requires neither queue capacity nor further input.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s, stranger;
        auto target=backend.open_target(s,component_path).first; CHECK(target);
        CHECK(!backend.target_delivery_valid(s,target).first);
        CHECK(!backend.enable_delivery_faults(stranger,target).empty());
        game_thread=false; CHECK(!backend.enable_delivery_faults(s,target).empty()); game_thread=true;
        CHECK(backend.enable_delivery_faults(s,target).empty());
        CHECK(!backend.enable_delivery_faults(s,target).empty());
        CHECK(backend.target_delivery_valid(s,target).first);
        CHECK(!backend.target_delivery_valid(stranger,target).first);
        for (auto phase : {ABI::TriggerEvent::Started,ABI::TriggerEvent::Completed,ABI::TriggerEvent::Canceled}) {
            auto result=backend.subscribe(s,target,1,action_path,"phase",phase); CHECK(result.first);
        }
        auto* started=f.bindings().data[0]; auto* release=f.bindings().data[1];
        backend.set_queue_limit(1);
        started->Execute(event); auto stale=backend.take_events(); CHECK(stale.size()==1);
        started->Execute(event); release->Execute(event);
        CHECK(backend.queue_stats().rejected==1);
        CHECK(!backend.target_delivery_valid(s,target).first && !stale[0].owner->delivery_valid());
        CHECK(std::string_view(backend.target_delivery_valid(s,target).second)=="queue_capacity_exceeded");
        auto fault=backend.take_delivery_fault();
        CHECK(fault && fault->target==target && fault->session==&s && fault->reason==DeliveryFault::QueueCapacity);
        CHECK(!backend.take_delivery_fault());
        release->Execute(event); CHECK(backend.queue_stats().rejected==1);
        auto denied=backend.subscribe(s,target,1,action_path,"Started",ABI::TriggerEvent::Started); CHECK(!denied.first);
        backend.consumer_events_remaining(0); auto queued=backend.take_events(); CHECK(queued.size()==1);
        CHECK(!queued[0].owner->delivery_valid()); backend.consumer_events_remaining(0);
        DispatchBacklog<EnhancedInputEvent> backlog; backlog.load(std::move(stale));
        const auto now=DispatchBudget::Clock::now();
        DispatchBudget budget(now,1,std::chrono::microseconds(0));
        int delivered=0;
        CHECK(dispatch_budgeted(backlog,budget,[&](auto item) {
            if (!item.owner->active.load() || !item.owner->delivery_valid()) return false;
            ++delivered; return true;
        },[&] { return now; })==0);
        CHECK(backlog.empty() && delivered==0);
        CHECK(backend.close_target(s,target)); CHECK(!backend.target_delivery_valid(s,target).first);
        auto fresh=backend.open_target(s,component_path).first; CHECK(fresh && fresh!=target);
        CHECK(backend.enable_delivery_faults(s,fresh).empty() && backend.target_delivery_valid(s,fresh).first);
        CHECK(backend.subscribe(s,fresh,1,action_path,"Started",ABI::TriggerEvent::Started).first);
        f.bindings().data[0]->Execute(event); auto live=backend.take_events(); CHECK(live.size()==1 && live[0].owner->delivery_valid());
        backend.consumer_events_remaining(0); CHECK(backend.shutdown());
    }
    // Legacy targets keep overflow behavior and cannot opt in retroactively.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        auto target=backend.open_target(s,component_path).first;
        auto id=backend.subscribe(s,target,1,action_path,"Triggered",ABI::TriggerEvent::Triggered).first; CHECK(id);
        CHECK(!backend.enable_delivery_faults(s,target).empty());
        backend.set_queue_limit(1); f.bindings().data[0]->Execute(event); f.bindings().data[0]->Execute(event);
        CHECK(backend.queue_stats().rejected==1 && !backend.take_delivery_fault());
        auto accepted=backend.take_events(); CHECK(accepted.size()==1 && accepted[0].owner->delivery_valid());
        backend.consumer_events_remaining(0); CHECK(backend.unsubscribe(s,id));
        CHECK(!backend.enable_delivery_faults(s,target).empty()); CHECK(backend.shutdown());
    }
    // Actual queue allocation exception latches independently of the trace queue.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        auto target=backend.open_target(s,component_path).first;
        CHECK(backend.enable_delivery_faults(s,target).empty());
        CHECK(backend.subscribe(s,target,1,action_path,"Started",ABI::TriggerEvent::Started).first);
        fail_next_cpp_allocation=true; f.bindings().data[0]->Execute(event);
        CHECK(!fail_next_cpp_allocation && !backend.target_delivery_valid(s,target).first);
        auto fault=backend.take_delivery_fault(); CHECK(fault && fault->reason==DeliveryFault::QueueException);
        CHECK(backend.take_events().empty()); CHECK(backend.shutdown());
    }
    // Callback faults coalesce; unrelated targets remain valid. Closing/stopping
    // before notification prevents a callback into a released consumer.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        auto target=backend.open_target(s,component_path).first;
        auto other=backend.open_target(s,component_path).first;
        CHECK(backend.enable_delivery_faults(s,target).empty()); CHECK(backend.enable_delivery_faults(s,other).empty());
        CHECK(backend.subscribe(s,target,1,action_path,"Started",ABI::TriggerEvent::Started).first);
        f.bindings().data[0]->Execute(event); auto batch=backend.take_events(); CHECK(batch.size()==1);
        backend.consumer_events_remaining(0);
        backend.report_delivery_fault(*batch[0].owner,DeliveryFault::CallbackError);
        backend.report_delivery_fault(*batch[0].owner,DeliveryFault::QueueCapacity);
        CHECK(!batch[0].owner->delivery_valid() && backend.target_delivery_valid(s,other).first);
        CHECK(backend.close_target(s,target)); CHECK(!backend.take_delivery_fault());
        CHECK(backend.subscribe(s,other,1,action_path,"Started",ABI::TriggerEvent::Started).first);
        f.bindings().data[0]->Execute(event); batch=backend.take_events(); backend.consumer_events_remaining(0);
        backend.report_delivery_fault(*batch[0].owner,DeliveryFault::CallbackError);
        backend.deactivate_all(s); CHECK(!backend.take_delivery_fault()); CHECK(backend.shutdown());
    }
    // Actual attachment, ownership, queue production, removal and reload cycles.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize();
        std::array<LuaSession,100> sessions;
        uint64_t previous{};
        for(auto& session:sessions) {
            const auto id=bind(backend,session); CHECK(id>previous); previous=id;
            f.bindings().data[0]->Execute(event);
            auto batch=backend.take_events(); CHECK(batch.size()==1 && batch[0].subscription==id);
            CHECK(backend.queue_stats().queued==1);
            CHECK(backend.unsubscribe_all(session)==1 && f.bindings().size==0);
            CHECK(!batch[0].owner->active.load()); // Captured native ownership is inert.
            backend.consumer_events_remaining(0); batch.clear(); backend.recycle_events(std::move(batch));
            CHECK(backend.queue_stats().queued==0);
        }
        CHECK(backend.shutdown());
    }
    // Unreal-owned clone survives original removal and backend destruction.
    {
        Fixture f; auto backend=std::make_unique<EnhancedInputBackend>(); backend->initialize(); LuaSession s;
        bind(*backend,s);
        auto clone=f.bindings().data[0]->Clone(); auto* late=clone.release(); CHECK(late);
        CHECK(backend->unsubscribe_all(s)==1);
        late->Execute(event); CHECK(backend->take_events().empty());
        CHECK(!backend->shutdown()); backend.reset();
        late->Execute(event); delete late; // ASan covers shared-state lifetime.
    }
    // Stop off-thread while another thread executes the real native delegate.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        bind(backend,s); auto* delegate=f.bindings().data[0];
        std::barrier start(2);
        std::thread producer([&] { game_thread=false; start.arrive_and_wait();
            for(int i=0;i<10000;++i) delegate->Execute(event); });
        std::thread stop([&] { game_thread=false; start.arrive_and_wait(); CHECK(!backend.shutdown()); });
        producer.join(); stop.join();
        CHECK(f.bindings().size==1); // No off-thread mutation of engine ownership.
        delegate->Execute(event); CHECK(backend.take_events().empty());
        f.destroy_component(); CHECK(backend.shutdown());
    }
    // Native dead-component path must not dereference an already-freed binding.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        const auto id=bind(backend,s); f.destroy_component();
        CHECK(backend.unsubscribe(s,id)); CHECK(backend.shutdown());
    }
    // A failed native allocation is recoverable and publishes no binding.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        auto target=backend.open_target(s,component_path); CHECK(target.first);
        fail_allocation=true;
        auto failed=backend.subscribe(s,target.first,1,action_path,"Triggered",ABI::TriggerEvent::Triggered);
        fail_allocation=false; CHECK(!failed.first && !failed.second.empty() && f.bindings().size==0);
        bind(backend,s); CHECK(backend.unsubscribe_all(s)==1); CHECK(backend.shutdown());
    }
    // Lookups walk the object array once per class and then come from a
    // path cache: a transient-package action and a game asset action share
    // one walk, repeat binds and reopened targets walk nothing, and a miss
    // walks again and keeps its error text.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        scans=0;
        auto target=backend.open_target(s,component_path); CHECK(target.first && target.second.empty());
        CHECK(scans==1);
        auto transient=backend.subscribe(s,target.first,1,action_path,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(transient.first && transient.second.empty()); CHECK(scans==2);
        auto asset=backend.subscribe(s,target.first,2,asset_action_path,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(asset.first && asset.second.empty()); CHECK(scans==2);
        CHECK(f.bindings().size==2);
        CHECK(f.bindings().data[0]->action.Get()==action_object && f.bindings().data[1]->action.Get()==asset_action);
        auto again=backend.subscribe(s,target.first,3,action_path,"Started",ABI::TriggerEvent::Started);
        CHECK(again.first && scans==2);
        auto reopened=backend.open_target(s,component_path); CHECK(reopened.first && scans==2);
        const std::string missing_action="/Game/Input/Actions/IA_Missing.IA_Missing";
        auto miss=backend.subscribe(s,target.first,4,missing_action,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(!miss.first && miss.second=="InputAction was not found at object path: "+missing_action);
        CHECK(scans==3);
        // The refilled cache still serves the known paths.
        CHECK(backend.subscribe(s,target.first,5,asset_action_path,"Started",ABI::TriggerEvent::Started).first);
        CHECK(scans==3);
        // A component path is not an InputAction.
        auto wrong=backend.subscribe(s,target.first,6,component_path,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(!wrong.first && wrong.second=="InputAction was not found at object path: "+component_path);
        // A missing component walks both component classes, as before.
        const std::string missing_component="/Script/Nowhere.Nothing:Missing";
        const int before_component=scans;
        auto none=backend.open_target(s,missing_component);
        CHECK(!none.first && none.second=="EnhancedInputComponent was not found at object path: "+missing_component);
        CHECK(scans==before_component+2);
        // The target error still wins over a missing action, and costs no walk.
        const int before_unknown=scans;
        auto unknown=backend.subscribe(s,999,7,missing_action,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(!unknown.first && unknown.second.find("target handle is unknown")!=std::string::npos);
        CHECK(scans==before_unknown);
        // An object that left its slot is not served from the cache.
        const auto saved=objects[2]; objects[2]={};
        const int before_stale=scans;
        auto stale=backend.subscribe(s,target.first,8,asset_action_path,"Triggered",ABI::TriggerEvent::Triggered);
        CHECK(!stale.first && scans==before_stale+1);
        objects[2]=saved;
        CHECK(backend.subscribe(s,target.first,9,asset_action_path,"Triggered",ABI::TriggerEvent::Triggered).first);
        CHECK(f.bindings().size==5);
        CHECK(backend.unsubscribe_all(s)==5); CHECK(backend.shutdown());
    }
    // An action handed over by address binds with no walk at all; an object
    // that is not an InputAction is refused, and so is an unknown target.
    {
        Fixture f; EnhancedInputBackend backend; backend.initialize(); LuaSession s;
        auto target=backend.open_target(s,component_path); CHECK(target.first);
        scans=0;
        auto direct=backend.subscribe(s,target.first,1,action_path,"Triggered",ABI::TriggerEvent::Triggered,{},action_object);
        CHECK(direct.first && direct.second.empty() && scans==0);
        CHECK(f.bindings().size==1 && f.bindings().data[0]->action.Get()==action_object);
        auto wrong=backend.subscribe(s,target.first,2,component_path,"Triggered",ABI::TriggerEvent::Triggered,{},component);
        CHECK(!wrong.first && wrong.second=="object is not an InputAction: "+component_path && scans==0);
        auto unknown=backend.subscribe(s,999,3,action_path,"Triggered",ABI::TriggerEvent::Triggered,{},action_object);
        CHECK(!unknown.first && unknown.second.find("target handle is unknown")!=std::string::npos && scans==0);
        CHECK(backend.unsubscribe_all(s)==1); CHECK(backend.shutdown());
    }
    return LexTests::finish("NativeBackendLifecycleTests");
}
