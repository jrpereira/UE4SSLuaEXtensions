#include "support/FakeProperties.hpp"

#include <TestSupport.hpp>

using namespace FakeProps;

static std::vector<FakeNode> sample()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("Count", "IntProperty", 0, 4));
    nodes.push_back(boolean("Flag", 4, 0, 0x04));
    nodes.push_back(node("Mode", "EnumProperty", 8, 1));
    nodes.back().enum_names = {"A", "B"};
    nodes.push_back(node("Items", "ArrayProperty", 16, 16));
    nodes.push_back(node("Result", "BoolProperty", 32, 1, cpf_parm | cpf_out_parm | cpf_return_parm));
    return nodes;
}

static void classification()
{
    struct { const char* cls; FieldKind kind; } cases[] = {
        {"BoolProperty", FieldKind::boolean},   {"Int8Property", FieldKind::int8},     {"Int16Property", FieldKind::int16},
        {"IntProperty", FieldKind::int32},      {"Int64Property", FieldKind::int64},   {"ByteProperty", FieldKind::uint8},
        {"UInt16Property", FieldKind::uint16},  {"UInt32Property", FieldKind::uint32}, {"UInt64Property", FieldKind::uint64},
        {"FloatProperty", FieldKind::float32},  {"DoubleProperty", FieldKind::float64}, {"EnumProperty", FieldKind::enumeration},
        {"NameProperty", FieldKind::name},      {"StrProperty", FieldKind::string},    {"ObjectProperty", FieldKind::object},
        {"ClassProperty", FieldKind::object},   {"StructProperty", FieldKind::structure}, {"TextProperty", FieldKind::unsupported},
        {"ArrayProperty", FieldKind::unsupported}, {"", FieldKind::unsupported}};
    for (const auto& c : cases) CHECK(classify_property(c.cls) == c.kind);
}

static void builds_and_records()
{
    auto nodes = sample();
    const auto result = build(nodes, 40);
    CHECK(result.ok);
    const Schema& schema = result.schema;
    CHECK(schema.fields.size() == 5 && schema.parms_size == 40);
    CHECK(schema.fields[0].kind == FieldKind::int32 && schema.fields[0].size == 4 && schema.fields[0].depth == 0);
    CHECK(schema.fields[1].kind == FieldKind::boolean && schema.fields[1].bit_mask == 0x04);
    CHECK(schema.fields[2].enum_names.size() == 2);
    CHECK(schema.fields[3].kind == FieldKind::unsupported && schema.has_unsupported);
    CHECK(schema.return_field && *schema.return_field == 4);
}

static void depth_limit()
{
    // Depth 0 struct holds depth 1, 2 and 3 structs; the depth-3 struct is cut.
    std::vector<FakeNode> nodes;
    nodes.push_back(structure("S0", 0, 16, {structure("S1", 0, 16, {structure("S2", 0, 16, {structure("S3", 0, 16,
        {node("Deep", "IntProperty", 0, 4)})})})}));
    const auto result = build(nodes, 16);
    CHECK(result.ok);
    CHECK(result.schema.fields.size() == 4); // Deep is not listed by Lua nor walked
    CHECK(result.schema.fields[3].depth == 3 && result.schema.fields[3].kind == FieldKind::unsupported);
    CHECK(result.schema.fields[2].kind == FieldKind::structure && result.schema.fields[2].parent == 1);

    // A struct array is unsupported but its children are still consumed.
    std::vector<FakeNode> arr;
    arr.push_back(structure("Arr", 0, 8, {node("X", "IntProperty", 0, 4)}));
    arr.back().dim = 2;
    const auto array_result = build(arr, 16);
    CHECK(array_result.ok && array_result.schema.fields.size() == 2);
    CHECK(array_result.schema.fields[0].kind == FieldKind::unsupported);
}

static void wrong_size_is_unsupported()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("Odd", "IntProperty", 0, 8));
    nodes.push_back(node("Mode", "EnumProperty", 8, 3));
    const auto result = build(nodes, 16);
    CHECK(result.ok);
    CHECK(result.schema.fields[0].kind == FieldKind::unsupported && result.schema.fields[1].kind == FieldKind::unsupported);
}

static bool refused(const SchemaResult& result)
{
    return !result.ok && result.error.rfind("property layout mismatch", 0) == 0;
}

static void cross_check_refusals()
{
    auto nodes = sample();
    std::uintptr_t next = 1;
    assign_addresses(nodes, next);
    const auto good = lua_list(nodes);
    CHECK(build_schema(good, FakeView::make(&nodes), 40).ok);

    auto lua = good;
    lua.pop_back();
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // Lua has fewer
    lua = good;
    lua.push_back(good.back());
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // Lua has more
    lua = good;
    lua[1].name = "Other";
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // name
    lua = good;
    lua[2].offset += 4;
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // offset
    lua = good;
    lua[0].address += 8;
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // address
    lua = good;
    lua[3].parent = 0;
    CHECK(refused(build_schema(lua, FakeView::make(&nodes), 40)));      // parent
    CHECK(refused(build_schema(good, FakeView::make(&nodes), 8)));      // parameter outside the block
    CHECK(refused(build_schema(good, nullptr, 40)));                    // empty walk
}

int main()
{
    classification();
    builds_and_records();
    depth_limit();
    wrong_size_is_unsupported();
    cross_check_refusals();
    return LexTests::finish("ParamSchemaTests");
}
