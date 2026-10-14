#include "support/FakeProperties.hpp"

#include <ParamCopier.hpp>
#include <TestSupport.hpp>

#include <array>
#include <sstream>

using namespace FakeProps;

struct Collector final : TripletSink
{
    std::vector<std::string> lines;
    std::size_t announced{999};
    void begin(std::size_t count) override { announced = count; }
    void boolean(const std::string& n, bool v) override { lines.push_back(n + " b " + (v ? "true" : "false")); }
    void integer(const std::string& n, int64_t v) override { lines.push_back(n + " i " + std::to_string(v)); }
    void number(const std::string& n, double v) override { std::ostringstream s; s << n << " n " << v; lines.push_back(s.str()); }
    void string(const std::string& n, const std::string& v) override { lines.push_back(n + " s " + v); }
    void enumeration(const std::string& n, const std::string& v) override { lines.push_back(n + " e " + v); }
    void object(const std::string& n, const std::string& v) override { lines.push_back(n + " o " + v); }
    void unsupported(const std::string& n) override { lines.push_back(n + " x"); }
};

struct Objects final : ObjectReader
{
    ObjectIdentity identity(std::uintptr_t a) override { return {a, 12, 34}; }
    std::string path(std::uintptr_t) override { return "/Game/Thing"; }
};

struct Names final : NameSource
{
    std::string name(uint32_t i, uint32_t n) override { return "N" + std::to_string(i) + "_" + std::to_string(n); }
};

struct Block
{
    std::array<std::byte, 512> bytes{};
    template <typename T> void set(std::size_t at, T v) { std::memcpy(bytes.data() + at, &v, sizeof(T)); }
};

static std::vector<std::string> run(const Schema& schema, const std::vector<uint16_t>& selection, const Block& block,
                                    bool include_return = false, bool paths = false, CopyResult* result = nullptr,
                                    std::size_t* payload_size = nullptr)
{
    Objects objects;
    Names names;
    ParamCopier copier(schema, selection, include_return, paths);
    std::vector<std::byte> payload;
    const CopyResult r = copier.copy(block.bytes.data(), &objects, payload);
    if (result) *result = r;
    if (payload_size) *payload_size = payload.size();
    Collector sink;
    Flattener flattener(schema);
    CHECK(flattener.flatten(payload.data(), payload.size(), &names, sink));
    CHECK(sink.announced == sink.lines.size());
    return sink.lines;
}

static bool has(const std::vector<std::string>& lines, const std::string& line)
{
    for (const auto& l : lines) if (l == line) return true;
    return false;
}

static void every_kind_round_trips()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("I8", "Int8Property", 0, 1));
    nodes.push_back(node("I16", "Int16Property", 2, 2));
    nodes.push_back(node("I32", "IntProperty", 4, 4));
    nodes.push_back(node("I64", "Int64Property", 8, 8));
    nodes.push_back(node("U8", "ByteProperty", 16, 1));
    nodes.push_back(node("U16", "UInt16Property", 18, 2));
    nodes.push_back(node("U32", "UInt32Property", 20, 4));
    nodes.push_back(node("U64", "UInt64Property", 24, 8));
    nodes.push_back(node("F", "FloatProperty", 32, 4));
    nodes.push_back(node("D", "DoubleProperty", 40, 8));
    nodes.push_back(node("E", "EnumProperty", 48, 1));
    nodes.back().enum_names = {"Zero", "One", "Two"};
    nodes.push_back(node("E2", "EnumProperty", 52, 4));
    nodes.back().enum_names = {"Zero"};
    nodes.push_back(node("Nm", "NameProperty", 56, 8));
    nodes.push_back(node("Str", "StrProperty", 64, 16));
    nodes.push_back(node("Obj", "ObjectProperty", 80, 8));
    nodes.push_back(node("Cls", "ClassProperty", 88, 8));
    nodes.push_back(node("Null", "ObjectProperty", 96, 8));
    nodes.push_back(node("Arr", "ArrayProperty", 104, 16));
    const auto built = build(nodes, 128);
    CHECK(built.ok);

    static const char16_t text[] = u"héllo \U0001F600";
    Block block;
    block.set<int8_t>(0, -5);
    block.set<int16_t>(2, -300);
    block.set<int32_t>(4, -70000);
    block.set<int64_t>(8, -5000000000LL);
    block.set<uint8_t>(16, 250);
    block.set<uint16_t>(18, 65000);
    block.set<uint32_t>(20, 4000000000u);
    block.set<uint64_t>(24, 123456789012ull);
    block.set<float>(32, 1.5f);
    block.set<double>(40, -2.25);
    block.set<uint8_t>(48, 1);
    block.set<int32_t>(52, 7);
    block.set<uint32_t>(56, 11);
    block.set<uint32_t>(60, 3);
    block.set<std::uintptr_t>(64, reinterpret_cast<std::uintptr_t>(text));
    block.set<int32_t>(72, static_cast<int32_t>(sizeof(text) / 2)); // includes the terminator
    block.set<std::uintptr_t>(80, 0xABC0);
    block.set<std::uintptr_t>(88, 0xDEF0);

    const auto lines = run(built.schema, {}, block);
    CHECK(has(lines, "I8 i -5") && has(lines, "I16 i -300") && has(lines, "I32 i -70000"));
    CHECK(has(lines, "I64 i -5000000000") && has(lines, "U8 i 250") && has(lines, "U16 i 65000"));
    CHECK(has(lines, "U32 i 4000000000") && has(lines, "U64 i 123456789012"));
    CHECK(has(lines, "F n 1.5") && has(lines, "D n -2.25"));
    CHECK(has(lines, "E e 1|One"));
    CHECK(has(lines, "E2 e 7|"));             // value without a known name
    CHECK(has(lines, "Nm s N11_3"));
    CHECK(has(lines, "Str s h\xC3\xA9llo \xF0\x9F\x98\x80"));
    CHECK(has(lines, "Obj o 43968:12:34") && has(lines, "Cls o 57072:12:34"));
    CHECK(has(lines, "Null o 0:-1:0"));
    CHECK(has(lines, "Arr x"));
    CHECK(lines.size() == 18);

    const auto with_paths = run(built.schema, {}, block, false, true);
    CHECK(has(with_paths, "Obj o 43968:12:34:/Game/Thing") && has(with_paths, "Null o 0:-1:0"));
}

static void bitfield_bools()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(boolean("A", 0, 0, 0x01));
    nodes.push_back(boolean("B", 0, 0, 0x02));
    nodes.push_back(boolean("C", 0, 1, 0x80));
    nodes.push_back(boolean("Plain", 4, 0, 0xFF));
    nodes.push_back(boolean("Zero", 5, 0, 0));
    const auto built = build(nodes, 8);
    CHECK(built.ok);
    Block block;
    block.set<uint8_t>(0, 0x02);
    block.set<uint8_t>(1, 0x80);
    block.set<uint8_t>(4, 1);
    const auto lines = run(built.schema, {}, block);
    CHECK(has(lines, "A b false") && has(lines, "B b true") && has(lines, "C b true"));
    CHECK(has(lines, "Plain b true") && has(lines, "Zero b false"));
}

static void fixed_arrays()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("Nums", "IntProperty", 0, 4));
    nodes.back().dim = 3;
    nodes.push_back(node("Big", "ByteProperty", 16, 1));
    nodes.back().dim = 10;
    const auto built = build(nodes, 32);
    CHECK(built.ok);
    Block block;
    for (int i = 0; i < 3; ++i) block.set<int32_t>(i * 4, 100 + i);
    for (int i = 0; i < 10; ++i) block.set<uint8_t>(16 + i, static_cast<uint8_t>(i));
    CopyResult result;
    const auto lines = run(built.schema, {}, block, false, false, &result);
    CHECK(has(lines, "Nums[0] i 100") && has(lines, "Nums[1] i 101") && has(lines, "Nums[2] i 102"));
    CHECK(has(lines, "Big[7] i 7") && !has(lines, "Big[8] i 8"));
    CHECK(result.truncated && lines.size() == 3 + 8);
}

static void nested_structs()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(structure("Outer", 8, 32, {
        node("A", "IntProperty", 0, 4),
        structure("Inner", 8, 16, {
            node("B", "IntProperty", 4, 4),
            structure("Deeper", 8, 8, {structure("Cut", 0, 4, {node("Z", "IntProperty", 0, 4)})})})}));
    nodes.push_back(node("After", "IntProperty", 48, 4));
    const auto built = build(nodes, 64);
    CHECK(built.ok);
    CHECK(built.schema.fields.size() == 7); // Outer, A, Inner, B, Deeper, Cut, After (Z not listed)
    Block block;
    block.set<int32_t>(8 + 0, 1);      // Outer.A
    block.set<int32_t>(8 + 8 + 4, 2);  // Outer.Inner.B
    block.set<int32_t>(48, 3);
    const auto lines = run(built.schema, {}, block);
    CHECK(has(lines, "Outer.A i 1") && has(lines, "Outer.Inner.B i 2") && has(lines, "After i 3"));
    CHECK(has(lines, "Outer.Inner.Deeper.Cut x")); // cut at the depth limit, delivered as nil
    CHECK(lines.size() == 4);
}

static void selection()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("P", "IntProperty", 0, 4));
    nodes.push_back(structure("S", 8, 16, {node("X", "IntProperty", 0, 4), node("Y", "IntProperty", 4, 4)}));
    nodes.push_back(node("Q", "IntProperty", 24, 4));
    nodes.push_back(node("Local", "IntProperty", 28, 4, 0)); // not a parameter
    nodes.push_back(node("Ret", "IntProperty", 32, 4, cpf_parm | cpf_out_parm | cpf_return_parm));
    nodes.push_back(node("Out", "IntProperty", 36, 4, cpf_parm | cpf_out_parm));
    nodes.push_back(node("Ref", "IntProperty", 40, 4, cpf_parm | cpf_const_parm | cpf_out_parm));
    const auto built = build(nodes, 48);
    CHECK(built.ok && built.schema.return_field && *built.schema.return_field == 6);
    Block block;
    block.set<int32_t>(0, 10);
    block.set<int32_t>(8, 20);
    block.set<int32_t>(12, 21);
    block.set<int32_t>(24, 30);
    block.set<int32_t>(28, 99);
    block.set<int32_t>(32, 40);
    block.set<int32_t>(36, 50);
    block.set<int32_t>(40, 60);

    auto all = run(built.schema, {}, block);
    CHECK(all.size() == 6 && !has(all, "Local i 99") && !has(all, "ret i 40"));
    CHECK(has(all, "out:Out i 50") && has(all, "Ref i 60"));
    CHECK(has(run(built.schema, {}, block, true), "ret i 40"));

    // Indices: P=0, S=1, X=2, Y=3, Q=4, Local=5, Ret=6.
    const auto only_y = run(built.schema, {3}, block);
    CHECK(only_y.size() == 1 && only_y[0] == "S.Y i 21");
    const auto struct_and_q = run(built.schema, {4, 1, 2}, block); // order kept, X not repeated
    CHECK(struct_and_q.size() == 3 && struct_and_q[0] == "Q i 30" && struct_and_q[1] == "S.X i 20" && struct_and_q[2] == "S.Y i 21");
    CHECK(run(built.schema, {200}, block).empty()); // out of range ignored
    const auto explicit_ret = run(built.schema, {6}, block);
    CHECK(explicit_ret.size() == 1 && explicit_ret[0] == "ret i 40");
}

static void string_bound_and_payload_bound()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("Str", "StrProperty", 0, 16));
    nodes.push_back(node("Tail", "IntProperty", 16, 4));
    const auto built = build(nodes, 24);
    CHECK(built.ok);
    static std::u16string huge(5000, u'a');
    huge.push_back(u'\0');
    Block block;
    block.set<std::uintptr_t>(0, reinterpret_cast<std::uintptr_t>(huge.data()));
    block.set<int32_t>(8, static_cast<int32_t>(huge.size()));
    block.set<int32_t>(16, 5);
    CopyResult result;
    std::size_t size = 0;
    const auto lines = run(built.schema, {}, block, false, false, &result, &size);
    CHECK(lines.size() == 2 && lines[0].size() == std::string("Str s ").size() + max_string_bytes / 2);
    CHECK(!result.truncated && size == 2 * record_header_bytes + max_string_bytes + 8);

    // A payload limit cuts before the record that does not fit.
    Objects objects;
    ParamCopier tight(built.schema, {}, false, false, CopyLimits{record_header_bytes + max_string_bytes + 4, max_string_bytes});
    std::vector<std::byte> payload;
    const auto cut = tight.copy(block.bytes.data(), &objects, payload);
    CHECK(cut.truncated && payload.size() == record_header_bytes + max_string_bytes);

    // Null pointer and non-positive counts give empty strings.
    block.set<std::uintptr_t>(0, 0);
    CHECK(has(run(built.schema, {}, block), "Str s "));
    block.set<std::uintptr_t>(0, reinterpret_cast<std::uintptr_t>(huge.data()));
    block.set<int32_t>(8, 0);
    CHECK(has(run(built.schema, {}, block), "Str s "));
}

static void malformed_payloads_are_refused()
{
    std::vector<FakeNode> nodes;
    nodes.push_back(node("P", "IntProperty", 0, 4));
    const auto built = build(nodes, 8);
    Flattener flattener(built.schema);
    Collector sink;
    std::vector<std::byte> bad(4, std::byte{0});
    CHECK(!flattener.flatten(bad.data(), bad.size(), nullptr, sink));
    std::vector<std::byte> wrong_field = {std::byte{'i'}, std::byte{9}, std::byte{0}, std::byte{0}, std::byte{0}};
    CHECK(!flattener.flatten(wrong_field.data(), wrong_field.size(), nullptr, sink));
    CHECK(sink.announced == 999 && sink.lines.empty());
    CHECK(flattener.flatten(nullptr, 0, nullptr, sink) && sink.announced == 0);
}

int main()
{
    every_kind_round_trips();
    bitfield_bools();
    fixed_arrays();
    nested_structs();
    selection();
    string_bound_and_payload_bound();
    malformed_payloads_are_refused();
    return LexTests::finish("ParamCopierTests");
}
