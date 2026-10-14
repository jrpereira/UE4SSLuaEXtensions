// Windows adapter for the schema builder's PropertyView (include/ParamSchema.hpp):
// the exported FProperty accessors plus FField::Next read at the pinned offset.
// Game thread only. Compiled with the DLL; the portable logic is tested on Linux.
#ifdef _WIN32

#include <HookABI.hpp>
#include <ParamSchema.hpp>

#include <cstring>

namespace UE4SSLuaEventBridge::Hooks
{
namespace
{
using RC::Unreal::FBoolProperty;
using RC::Unreal::FField;
using RC::Unreal::FFieldClass;
using RC::Unreal::FProperty;
using RC::Unreal::FStructProperty;
namespace Layout = RC::Unreal::HookLayout;

std::string narrow(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

class NativeView final : public PropertyView
{
public:
    explicit NativeView(FField* field) : field_(field) {}

    std::string name() const override { return narrow(field_->GetFName().ToString()); }

    std::string class_name() const override
    {
        FFieldClass* cls = nullptr;
        std::memcpy(&cls, reinterpret_cast<const char*>(field_) + Layout::ffield_class_private, sizeof(cls));
        return cls != nullptr ? narrow(cls->GetFName().ToString()) : std::string();
    }

    std::uintptr_t address() const override { return reinterpret_cast<std::uintptr_t>(field_); }
    int32_t offset() const override { return property()->GetOffset_Internal(); }
    int32_t element_size() const override { return property()->GetElementSize(); }
    int32_t array_dim() const override { return property()->GetArrayDim(); }
    uint64_t flags() const override { return static_cast<uint64_t>(property()->GetPropertyFlags()); }

    uint8_t bool_byte_offset() const override
    {
        return class_name() == "BoolProperty" ? static_cast<FBoolProperty*>(property())->GetByteOffset() : 0;
    }
    uint8_t bool_mask() const override
    {
        return class_name() == "BoolProperty" ? static_cast<FBoolProperty*>(property())->GetFieldMask() : 0;
    }

    std::unique_ptr<PropertyView> first_child() const override
    {
        if (class_name() != "StructProperty") return nullptr;
        RC::Unreal::UStruct* inner = reinterpret_cast<RC::Unreal::UStruct*>(
            static_cast<FStructProperty*>(property())->GetStruct().pointer);
        if (inner == nullptr) return nullptr;
        return wrap(inner->GetChildProperties());
    }

    std::unique_ptr<PropertyView> next() const override
    {
        FField* sibling = nullptr;
        std::memcpy(&sibling, reinterpret_cast<const char*>(field_) + Layout::ffield_next, sizeof(sibling));
        return wrap(sibling);
    }

    static std::unique_ptr<PropertyView> wrap(FField* field)
    {
        return field != nullptr ? std::make_unique<NativeView>(field) : nullptr;
    }

private:
    FProperty* property() const { return static_cast<FProperty*>(field_); }
    FField* field_;
};
}

std::unique_ptr<PropertyView> make_native_walker(RC::Unreal::UFunction* function)
{
    return function != nullptr ? NativeView::wrap(function->GetChildProperties()) : nullptr;
}

uint32_t native_parms_size(RC::Unreal::UFunction* function)
{
    return function != nullptr ? function->GetParmsSize() : 0;
}
}

#endif
