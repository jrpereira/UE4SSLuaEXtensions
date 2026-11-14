#pragma once

// Checked reads of object memory: every read goes through ReadProcessMemory,
// so an address that is not an object yields "no result" instead of a fault.
// The one implementation of the GUObjectArray identity read (index, slot,
// serial); callers apply their own serial rule.

#ifdef _WIN32

#include <UE4SSABI.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

#if defined(_MSC_VER)
#define UE4SSLEB_READS_WINAPI __stdcall
#else
#define UE4SSLEB_READS_WINAPI
#endif
extern "C" __declspec(dllimport) void* UE4SSLEB_READS_WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) int UE4SSLEB_READS_WINAPI ReadProcessMemory(
    void*, const void*, void*, std::size_t, std::size_t*);
#undef UE4SSLEB_READS_WINAPI

namespace UE4SSLuaEventBridge
{
inline bool safe_read(const void* source, void* destination, std::size_t size)
{
    std::size_t copied{};
    return source != nullptr && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) != 0 &&
        copied == size;
}

// What a GUObjectArray slot holds: the object and its serial (0 when Unreal
// has not assigned one yet).
struct ObjectSlot
{
    std::uintptr_t object{};
    int32_t serial{};
};

inline std::optional<ObjectSlot> read_slot(int32_t index)
{
    if (index < 0) return {};
    const auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
    if (item == nullptr) return {};
    const auto address = reinterpret_cast<std::uintptr_t>(item);
    ObjectSlot slot{};
    if (!safe_read(reinterpret_cast<const void*>(address + RC::Unreal::FWeakObjectPtr::object_item_object_offset),
            &slot.object, sizeof(slot.object)) ||
        !safe_read(reinterpret_cast<const void*>(address + RC::Unreal::FWeakObjectPtr::object_item_serial_offset),
            &slot.serial, sizeof(slot.serial)))
        return {};
    return slot;
}

// Index and serial of the object at address, when its slot points back at it.
struct SlotIdentity
{
    int32_t index{};
    int32_t serial{};
};

inline std::optional<SlotIdentity> read_identity(std::uintptr_t address)
{
    int32_t index{};
    if (address == 0 ||
        !safe_read(reinterpret_cast<const void*>(address + RC::Unreal::FWeakObjectPtr::uobject_internal_index_offset),
            &index, sizeof(index)))
        return {};
    const auto slot = read_slot(index);
    if (!slot || slot->object != address) return {};
    return SlotIdentity{index, slot->serial};
}
}

#endif
