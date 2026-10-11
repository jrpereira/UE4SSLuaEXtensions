// Exercise the actual ABI shim with an isolated fake object array.
#ifndef _WIN32
#define _WIN32
#endif
#define UE4SS_IMPORT
#include <UE4SSABI.hpp>
#include <array>
#include <TestSupport.hpp>
namespace RC::Unreal {
struct FUObjectItem { alignas(8) std::byte bytes[0x18]{}; };
static FUObjectItem item;
FUObjectItem* FUObjectArray::IndexToObject(int32_t index) { return index == 7 ? &item : nullptr; }

}
int main() {
 using namespace RC::Unreal;
 alignas(8) std::array<std::byte, 0x20> object{};
 auto* original = reinterpret_cast<UObject*>(object.data());
 int32_t index=7;
 std::memcpy(object.data()+0x0c,&index,sizeof(index));
 std::memcpy(item.bytes,&original,sizeof(original));
 FWeakObjectPtr weak(original);
 CHECK(weak.object_serial_number == 0 && weak.Get() == nullptr); // Zero serial fails closed.
 int32_t serial=42;
 std::memcpy(item.bytes+0x10,&serial,sizeof(serial));
 weak.assign(original);
 CHECK(weak.object_index == 7 && weak.object_serial_number == 42 && weak.Get() == original);
 serial=43; std::memcpy(item.bytes+0x10,&serial,sizeof(serial));
 CHECK(weak.Get() == nullptr);
 UObject* wrong=nullptr; std::memcpy(item.bytes,&wrong,sizeof(wrong));
 weak.assign(original); CHECK(weak.object_serial_number == 0 && weak.Get() == nullptr);
 index=-1; std::memcpy(object.data()+0x0c,&index,sizeof(index));
 weak.assign(original); CHECK(weak.Get() == nullptr);
 index=8; std::memcpy(object.data()+0x0c,&index,sizeof(index));
 weak.assign(original); CHECK(weak.Get() == nullptr);
 weak.assign(nullptr); CHECK(weak.Get() == nullptr);
    return LexTests::finish("WeakObjectPtrTests");
}
