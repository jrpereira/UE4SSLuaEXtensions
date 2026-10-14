#pragma once

// The event bridge's hook part of the pinned UE4SS 97b7e501 ABI: UFunction and
// bytecode access, FFrame accessors, the property reflection that builds
// parameter schemas, and the engine-tick callback. Primitives that live inside
// classes UE4SSABI.hpp already defines (UObjectBase, UStruct, FName, TArray) are
// declared there. Imports resolve through shared/ue4ss-abi/UE4SS-97b7e501.def.
// Portable types (schemas, events, limits) are in HookTypes.hpp.
#include <HookTypes.hpp>
#include <UE4SSABI.hpp>

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <functional>

namespace RC
{
namespace Unreal
{
class UEngine;
class UEnum;
class UScriptStruct;
class FFieldClass;
class FField;
class FProperty;
class FNumericProperty;
struct FOutParmRec;

// Member offsets UE4SS reported for this target at startup (UE4SS.log,
// 2026-10-14). They are read directly on hot paths; the startup probe checks
// each one against the exported accessor before hooks become available
// (LEX_hooks.md §6.3). InternalIndex (0x0C) and the FUObjectItem layout are
// in UE4SSABI.hpp's FWeakObjectPtr.
namespace HookLayout
{
inline constexpr std::size_t uobject_class_private = 0x10;
inline constexpr std::size_t uobject_name_private = 0x18;
inline constexpr std::size_t uobject_outer_private = 0x20;
inline constexpr std::size_t ustruct_super_struct = 0x40;
inline constexpr std::size_t ustruct_child_properties = 0x50;
inline constexpr std::size_t ustruct_script = 0x60;
inline constexpr std::size_t ufunction_function_flags = 0xB0;
inline constexpr std::size_t ufunction_num_parms = 0xB4;
inline constexpr std::size_t ufunction_parms_size = 0xB6;
inline constexpr std::size_t ufunction_return_value_offset = 0xB8;
inline constexpr std::size_t ufunction_func = 0xD8;
inline constexpr std::size_t ffield_class_private = 0x08;
inline constexpr std::size_t ffield_next = 0x18;
inline constexpr std::size_t ffield_name_private = 0x20;
}

enum class EPropertyFlags : uint64_t
{
};

class UFunction : public UStruct
{
public:
    UE4SS_IMPORT uint32_t& GetFunctionFlags();
    UE4SS_IMPORT uint16_t& GetReturnValueOffset(); // 0xFFFF when the function returns nothing
    UE4SS_IMPORT uint8_t& GetNumParms();
    UE4SS_IMPORT uint16_t& GetParmsSize();
    UE4SS_IMPORT UE4SSLuaEventBridge::Hooks::NativeFuncPtr GetFuncPtr();
    UE4SS_IMPORT void SetFuncPtr(UE4SSLuaEventBridge::Hooks::NativeFuncPtr func);
    UE4SS_IMPORT static UClass* StaticClass();
};

// FField::GetClass() returns an FFieldClassVariant by value, whose layout this
// header does not pin. Read ClassPrivate at HookLayout::ffield_class_private
// instead and ask the FFieldClass for its name.
class FFieldClass
{
public:
    UE4SS_IMPORT FName& GetFName();
};

// FField::GetNext() is private in the pinned build. Walk the chain by reading
// HookLayout::ffield_next; the schema builder cross-checks the walk against the
// names Lua enumerated with ForEachProperty (LEX_hooks.md §5.1).
class FField
{
public:
    UE4SS_IMPORT FName GetFName();
};

class FProperty : public FField
{
public:
    UE4SS_IMPORT int32_t& GetOffset_Internal();
    UE4SS_IMPORT int32_t& GetElementSize();
    UE4SS_IMPORT int32_t& GetArrayDim();
    UE4SS_IMPORT EPropertyFlags& GetPropertyFlags();
};

class FNumericProperty : public FProperty
{
};

class FBoolProperty : public FProperty
{
public:
    UE4SS_IMPORT uint8_t& GetByteOffset();
    UE4SS_IMPORT uint8_t& GetFieldMask(); // 0xFF for a plain bool
};

class FStructProperty : public FProperty
{
public:
    UE4SS_IMPORT TObjectPtr<UScriptStruct>& GetStruct();
};

class FEnumProperty : public FProperty
{
public:
    UE4SS_IMPORT TObjectPtr<UEnum>& GetEnum();
    UE4SS_IMPORT FNumericProperty* GetUnderlyingProperty();
};

class FObjectPropertyBase : public FProperty
{
public:
    UE4SS_IMPORT TObjectPtr<UClass>& GetPropertyClass();
};

// Opaque: only the exported accessors touch it. Code() points past the opcode
// byte when an exec handler runs; Locals() is the parameter block when
// Node() is the hooked function.
struct FFrame
{
    UE4SS_IMPORT uint8_t*& Code();
    UE4SS_IMPORT UFunction*& Node();
    UE4SS_IMPORT UObject*& Object();
    UE4SS_IMPORT uint8_t*& Locals();
    UE4SS_IMPORT FOutParmRec*& OutParms();
    UE4SS_IMPORT UFunction*& CurrentNativeFunction();
    UE4SS_IMPORT void Step(UObject* context, void* result);
};

UE4SS_IMPORT void* FindOutParamValueAddress(FFrame& stack, FProperty* property);

// UE4SS's pointer to the engine's FNativeFuncPtr GNatives[EX_Max].
UE4SS_IMPORT extern UE4SSLuaEventBridge::Hooks::NativeFuncPtr* GNatives_Internal;

namespace Hook
{
// Runs on the game thread after UEngine::Tick. Requires the engine-tick detour
// (UE4SSRuntime::IsEngineTickAvailable()); there is no matching unregister for
// this overload, so the callback checks an inert flag first.
UE4SS_IMPORT void RegisterEngineTickPostCallback(std::function<void(UEngine*, float)> callback);
}
}

struct UE4SSRuntime
{
    UE4SS_IMPORT static bool IsEngineTickAvailable();
};
}

#endif
