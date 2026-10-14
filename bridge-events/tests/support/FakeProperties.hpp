#pragma once

// Synthetic property trees for the schema, copier and filter suites. A
// FakeNode is one property; FakeTree turns a node list into the PropertyView
// chain the builder walks and into the Lua-side flat list (pre-order).

#include <ParamSchema.hpp>

namespace FakeProps
{
using namespace UE4SSLuaEventBridge::Hooks;

struct FakeNode
{
    std::string name;
    std::string cls;
    int32_t offset{};
    int32_t size{};
    int32_t dim{1};
    uint64_t flags{cpf_parm};
    uint8_t bool_byte{};
    uint8_t bool_mask{};
    std::vector<std::string> enum_names;
    std::vector<FakeNode> children;
    std::uintptr_t address{};
};

inline FakeNode node(std::string name, std::string cls, int32_t offset, int32_t size, uint64_t flags = cpf_parm)
{
    FakeNode n;
    n.name = std::move(name);
    n.cls = std::move(cls);
    n.offset = offset;
    n.size = size;
    n.flags = flags;
    return n;
}

inline FakeNode boolean(std::string name, int32_t offset, uint8_t byte, uint8_t mask)
{
    FakeNode n = node(std::move(name), "BoolProperty", offset, 1);
    n.bool_byte = byte;
    n.bool_mask = mask;
    return n;
}

inline FakeNode structure(std::string name, int32_t offset, int32_t size, std::vector<FakeNode> children)
{
    FakeNode n = node(std::move(name), "StructProperty", offset, size);
    n.children = std::move(children);
    return n;
}

// Assigns distinct addresses (1000 + running index) in pre-order.
inline void assign_addresses(std::vector<FakeNode>& nodes, std::uintptr_t& next)
{
    for (FakeNode& n : nodes)
    {
        n.address = next++ * 8 + 0x1000;
        assign_addresses(n.children, next);
    }
}

class FakeView final : public PropertyView
{
public:
    FakeView(const std::vector<FakeNode>* siblings, std::size_t index) : siblings_(siblings), index_(index) {}
    static std::unique_ptr<PropertyView> make(const std::vector<FakeNode>* siblings, std::size_t index = 0)
    {
        return index < siblings->size() ? std::make_unique<FakeView>(siblings, index) : nullptr;
    }
    std::string name() const override { return self().name; }
    std::string class_name() const override { return self().cls; }
    std::uintptr_t address() const override { return self().address; }
    int32_t offset() const override { return self().offset; }
    int32_t element_size() const override { return self().size; }
    int32_t array_dim() const override { return self().dim; }
    uint64_t flags() const override { return self().flags; }
    uint8_t bool_byte_offset() const override { return self().bool_byte; }
    uint8_t bool_mask() const override { return self().bool_mask; }
    std::unique_ptr<PropertyView> first_child() const override
    {
        return self().cls == "StructProperty" ? make(&self().children) : nullptr;
    }
    std::unique_ptr<PropertyView> next() const override { return make(siblings_, index_ + 1); }

private:
    const FakeNode& self() const { return (*siblings_)[index_]; }
    const std::vector<FakeNode>* siblings_;
    std::size_t index_;
};

// The list Lua would pass: pre-order, structs expanded while depth < max_struct_depth.
inline void lua_list(const std::vector<FakeNode>& nodes, uint16_t parent, unsigned depth, std::vector<LuaProperty>& out)
{
    for (const FakeNode& n : nodes)
    {
        const auto index = static_cast<uint16_t>(out.size());
        out.push_back({parent, n.address, n.offset, n.name, n.enum_names});
        if (n.cls == "StructProperty" && depth < max_struct_depth)
            lua_list(n.children, index, depth + 1, out);
    }
}

inline std::vector<LuaProperty> lua_list(const std::vector<FakeNode>& nodes)
{
    std::vector<LuaProperty> out;
    lua_list(nodes, no_parent, 0, out);
    return out;
}

// Builds a schema from nodes the way the hook would.
inline SchemaResult build(std::vector<FakeNode>& nodes, uint32_t parms_size)
{
    std::uintptr_t next = 1;
    assign_addresses(nodes, next);
    return build_schema(lua_list(nodes), FakeView::make(&nodes), parms_size);
}
}
