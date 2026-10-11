#pragma once

// The event bridge's part of the pinned UE4SS 97b7e501 ABI: the Unreal object
// model, object listeners, FindAllOf and the ProcessEvent hook. The part every
// extension uses (Lua, output, CppUserModBase) is shared/ue4ss-abi/UE4SSLuaABI.hpp.
#include <UE4SSLuaABI.hpp>

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RC
{
namespace Unreal
{
class UObject;
class UFunction;
class UClass;
struct FUObjectItem;
class FUObjectCreateListener;
class FUObjectDeleteListener;

class FUObjectArray
{
public:
    UE4SS_IMPORT static FUObjectItem* IndexToObject(int32_t index);
    UE4SS_IMPORT static void AddUObjectCreateListener(FUObjectCreateListener* listener);
    UE4SS_IMPORT static void AddUObjectDeleteListener(FUObjectDeleteListener* listener);
    UE4SS_IMPORT static void RemoveUObjectCreateListener(FUObjectCreateListener* listener);
    UE4SS_IMPORT static void RemoveUObjectDeleteListener(FUObjectDeleteListener* listener);
};

class UObjectBase
{
};

class UObject : public UObjectBase
{
public:
    UE4SS_IMPORT std::wstring GetPathName(UObject* stop_outer = nullptr) const;
    UE4SS_IMPORT void* GetValuePtrByPropertyNameInChain(const wchar_t* property_name);
};

class UStruct : public UObject
{
public:
    UE4SS_IMPORT int32_t& GetPropertiesSize();
};

class UClass : public UStruct
{
};

// FWeakObjectPtr in the pinned UE4SS build is two int32 values: UObject's
// GUObjectArray index and the corresponding FUObjectItem serial number.  Do
// not call UE4SS's exported FWeakObjectPtr(UObject*) constructor here.  On
// Dawnwalker/UE5.5 that path reaches UE4SS's dynamic member-offset accessor
// and crashes while binding transient InputActions.  The runtime offsets
// below are independently reported by UE4SS at startup for this target:
//   UObjectBase::InternalIndex_Private = 0x0C
//   FUObjectItem::Object               = 0x00
//   FUObjectItem::SerialNumber         = 0x10
// Reconstructing the same weak reference locally preserves native lifetime
// semantics while avoiding the failing accessor path.
struct FWeakObjectPtr
{
    static constexpr std::size_t uobject_internal_index_offset = 0x0C;
    static constexpr std::size_t object_item_object_offset = 0x00;
    static constexpr std::size_t object_item_serial_offset = 0x10;

    int32_t object_index{0};
    int32_t object_serial_number{0};

    FWeakObjectPtr() = default;

    explicit FWeakObjectPtr(const UObject* object)
    {
        assign(object);
    }

    void assign(const UObject* object)
    {
        object_index = 0;
        object_serial_number = 0;
        if (!object)
        {
            return;
        }

        int32_t index{};
        std::memcpy(
            &index,
            reinterpret_cast<const std::byte*>(object) + uobject_internal_index_offset,
            sizeof(index));
        if (index < 0)
        {
            return;
        }

        auto* item = FUObjectArray::IndexToObject(index);
        if (!item)
        {
            return;
        }

        UObject* indexed_object{};
        int32_t serial{};
        const auto* item_bytes = reinterpret_cast<const std::byte*>(item);
        std::memcpy(&indexed_object, item_bytes + object_item_object_offset, sizeof(indexed_object));
        std::memcpy(&serial, item_bytes + object_item_serial_offset, sizeof(serial));

        if (indexed_object != object) return;
        // Serial assignment must be performed by Unreal through the helper's
        // reflected Lua call. The pinned C++ allocator's soft return copy crashes.
        if (serial <= 0) return;

        object_index = index;
        object_serial_number = serial;
    }

    [[nodiscard]] UObject* Get() const
    {
        if (object_serial_number == 0 || object_index < 0)
        {
            return nullptr;
        }

        auto* item = FUObjectArray::IndexToObject(object_index);
        if (!item)
        {
            return nullptr;
        }

        UObject* object{};
        int32_t serial{};
        const auto* item_bytes = reinterpret_cast<const std::byte*>(item);
        std::memcpy(&object, item_bytes + object_item_object_offset, sizeof(object));
        std::memcpy(&serial, item_bytes + object_item_serial_offset, sizeof(serial));

        return serial == object_serial_number ? object : nullptr;
    }
};

class FMemory
{
public:
    UE4SS_IMPORT static void* Malloc(std::size_t size, uint32_t alignment = 0);
    UE4SS_IMPORT static void* Realloc(void* allocation, std::size_t size, uint32_t alignment = 0);
    UE4SS_IMPORT static void Free(void* allocation);
};

class FUObjectCreateListener
{
public:
    UE4SS_IMPORT FUObjectCreateListener();
    UE4SS_IMPORT virtual ~FUObjectCreateListener();
    virtual void NotifyUObjectCreated(const UObjectBase* object, int32_t index) = 0;
    virtual void OnUObjectArrayShutdown() = 0;
};

class FUObjectDeleteListener
{
public:
    UE4SS_IMPORT FUObjectDeleteListener();
    UE4SS_IMPORT virtual ~FUObjectDeleteListener();
    virtual void NotifyUObjectDeleted(const UObjectBase* object, int32_t index) = 0;
    virtual void OnUObjectArrayShutdown() = 0;
};

namespace UObjectGlobals
{
UE4SS_IMPORT void FindAllOf(const wchar_t* class_name, std::vector<UObject*>& objects);
}


namespace Hook
{
using ProcessEventCallback = std::function<void(UObject*, UFunction*, void*)>;
UE4SS_IMPORT void RegisterProcessEventPostCallback(ProcessEventCallback callback);
}
}
}

#endif
