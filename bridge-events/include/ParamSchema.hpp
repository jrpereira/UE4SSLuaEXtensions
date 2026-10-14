#pragma once

// Parameter schema builder (LEX_hooks.md §5.1). Portable; only the native
// walker adapter at the bottom (src/ParamSchema.cpp) needs UE4SS.
//
// Walker interface: PropertyView is one FProperty. `next()` is the sibling
// (FField::Next at HookLayout::ffield_next), `first_child()` the first
// property of a StructProperty's struct (nullptr for any other class).
// Each call returns a fresh owning view or nullptr at the end of a chain.
//
// Lua list (LuaProperty): the flat pre-order list Lua enumerates with
// ForEachProperty: a struct's children directly follow it, recursing while the
// struct's depth is below max_struct_depth. `parent` is the list index of the
// containing struct (no_parent for parameters). The same index is the Field's
// index in the Schema, so TLV records and the `params` selection use it.
//
// Depth: parameters are depth 0; a struct at depth < max_struct_depth is
// expanded, so fields reach depth max_struct_depth. A struct at that depth is
// cut: kind unsupported, no children. Fixed arrays of structs are also
// unsupported (the payload carries one element index per record).

#include <HookTypes.hpp>

#ifdef _WIN32
#include <HookABI.hpp>
#endif

#include <memory>
#include <string>
#include <vector>

namespace UE4SSLuaEventBridge::Hooks
{
class PropertyView
{
public:
    virtual ~PropertyView() = default;
    virtual std::string name() const = 0;
    virtual std::string class_name() const = 0;       // "IntProperty", ...
    virtual std::uintptr_t address() const = 0;       // the FProperty's address
    virtual int32_t offset() const = 0;
    virtual int32_t element_size() const = 0;
    virtual int32_t array_dim() const = 0;
    virtual uint64_t flags() const = 0;               // EPropertyFlags
    virtual uint8_t bool_byte_offset() const = 0;     // BoolProperty only
    virtual uint8_t bool_mask() const = 0;            // BoolProperty only; 0 or 0xFF = plain bool
    virtual std::unique_ptr<PropertyView> first_child() const = 0;
    virtual std::unique_ptr<PropertyView> next() const = 0;
};

struct LuaProperty
{
    uint16_t parent{no_parent};
    std::uintptr_t address{};
    int32_t offset{};
    std::string name;
    std::vector<std::string> enum_names;
};

struct SchemaResult
{
    bool ok{};
    std::string error; // "property layout mismatch (<what>)" or another reason
    Schema schema;
};

inline FieldKind classify_property(const std::string& class_name)
{
    struct Entry { const char* name; FieldKind kind; };
    static constexpr Entry table[] = {
        {"BoolProperty", FieldKind::boolean},   {"Int8Property", FieldKind::int8},
        {"Int16Property", FieldKind::int16},    {"IntProperty", FieldKind::int32},
        {"Int64Property", FieldKind::int64},    {"ByteProperty", FieldKind::uint8},
        {"UInt16Property", FieldKind::uint16},  {"UInt32Property", FieldKind::uint32},
        {"UInt64Property", FieldKind::uint64},  {"FloatProperty", FieldKind::float32},
        {"DoubleProperty", FieldKind::float64}, {"EnumProperty", FieldKind::enumeration},
        {"NameProperty", FieldKind::name},      {"StrProperty", FieldKind::string},
        {"ObjectProperty", FieldKind::object},  {"ClassProperty", FieldKind::object},
        {"StructProperty", FieldKind::structure},
    };
    for (const Entry& entry : table)
        if (class_name == entry.name) return entry.kind;
    return FieldKind::unsupported;
}

// Byte size a kind must have; 0 = not fixed (bool, enum, struct, unsupported).
inline uint32_t expected_size(FieldKind kind)
{
    switch (kind)
    {
    case FieldKind::int8: case FieldKind::uint8: return 1;
    case FieldKind::int16: case FieldKind::uint16: return 2;
    case FieldKind::int32: case FieldKind::uint32: case FieldKind::float32: return 4;
    case FieldKind::int64: case FieldKind::uint64: case FieldKind::float64:
    case FieldKind::name: case FieldKind::object: return 8;
    case FieldKind::string: return 16; // TArray<TCHAR>
    default: return 0;
    }
}

namespace detail
{
inline SchemaResult mismatch(const char* what)
{
    SchemaResult result;
    result.error = std::string("property layout mismatch (") + what + ")";
    return result;
}

// Walks one chain depth-first against the Lua list. Returns the failure
// reason or nullptr.
inline const char* walk_chain(const std::vector<LuaProperty>& lua, std::unique_ptr<PropertyView> view,
                              uint16_t parent, uint8_t depth, Schema& schema)
{
    for (; view; view = view->next())
    {
        const std::size_t index = schema.fields.size();
        if (index >= lua.size()) return "count";
        const LuaProperty& expected = lua[index];
        if (expected.name != view->name()) return "name";
        if (expected.offset != view->offset()) return "offset";
        if (expected.address != view->address()) return "address";
        if (expected.parent != parent) return "parent";
        if (index >= no_parent) return "count";

        Field field;
        field.name = expected.name;
        field.class_name = view->class_name();
        field.kind = classify_property(field.class_name);
        field.offset = static_cast<uint32_t>(view->offset());
        field.size = static_cast<uint32_t>(view->element_size() > 0 ? view->element_size() : 0);
        field.dim = static_cast<uint32_t>(view->array_dim() > 0 ? view->array_dim() : 1);
        field.flags = view->flags();
        field.parent = parent;
        field.depth = depth;
        field.enum_names = expected.enum_names;
        if (field.kind == FieldKind::boolean)
        {
            field.byte_offset = view->bool_byte_offset();
            field.bit_mask = view->bool_mask();
        }
        const uint32_t fixed = expected_size(field.kind);
        if (fixed != 0 && field.size != fixed) field.kind = FieldKind::unsupported;
        if (field.kind == FieldKind::enumeration && field.size != 1 && field.size != 2 && field.size != 4 && field.size != 8)
            field.kind = FieldKind::unsupported;
        if (field.kind == FieldKind::boolean && field.size == 0) field.kind = FieldKind::unsupported;
        if (field.kind == FieldKind::structure && (field.dim > 1 || depth >= max_struct_depth))
            field.kind = FieldKind::unsupported;

        // Children are walked even for an unsupported struct array, so the walk
        // consumes the same entries Lua listed; only a struct cut at the depth
        // limit has none.
        const bool expand = field.class_name == "StructProperty" && depth < max_struct_depth;
        std::unique_ptr<PropertyView> child = expand ? view->first_child() : nullptr;
        schema.fields.push_back(std::move(field));
        if (expand)
        {
            if (const char* why = walk_chain(lua, std::move(child), static_cast<uint16_t>(index),
                                             static_cast<uint8_t>(depth + 1), schema))
                return why;
        }
    }
    return nullptr;
}
}

// Cross-checks Lua's list against the native walk and completes the schema.
// `first` is the function's first property (UStruct::GetChildProperties()).
inline SchemaResult build_schema(const std::vector<LuaProperty>& lua, std::unique_ptr<PropertyView> first,
                                 uint32_t parms_size)
{
    SchemaResult result;
    if (const char* why = detail::walk_chain(lua, std::move(first), no_parent, 0, result.schema))
        return detail::mismatch(why);
    if (result.schema.fields.size() != lua.size()) return detail::mismatch("count");

    Schema& schema = result.schema;
    schema.parms_size = parms_size;
    for (std::size_t i = 0; i < schema.fields.size(); ++i)
    {
        Field& field = schema.fields[i];
        if (field.kind == FieldKind::unsupported) schema.has_unsupported = true;
        if (field.depth != 0) continue;
        if ((field.flags & cpf_return_parm) != 0 && !schema.return_field) schema.return_field = static_cast<uint16_t>(i);
        if ((field.flags & cpf_parm) != 0)
        {
            const uint64_t end = static_cast<uint64_t>(field.offset) + static_cast<uint64_t>(field.size) * field.dim;
            if (end > parms_size)
            {
                result.schema = Schema{};
                return detail::mismatch("parameter outside the parameter block");
            }
        }
    }
    result.ok = true;
    return result;
}

#ifdef _WIN32
// Windows adapter (src/ParamSchema.cpp): a view over the function's first
// property (UStruct::GetChildProperties), using the exported FProperty
// accessors and HookLayout::ffield_next. Game thread only.
std::unique_ptr<PropertyView> make_native_walker(RC::Unreal::UFunction* function);
// Reads UFunction::GetParmsSize().
uint32_t native_parms_size(RC::Unreal::UFunction* function);
#endif
}
