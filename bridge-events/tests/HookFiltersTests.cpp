#include <HookFilters.hpp>

#include <TestSupport.hpp>

using namespace UE4SSLuaEventBridge::Hooks;

struct FakeObject final : ObjectView
{
    int32_t idx{5};
    int32_t ser{9};
    uint32_t name{77};
    std::uintptr_t cls{0x100};
    std::vector<std::uintptr_t> outers;     // class of each outer, nearest first
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> derived; // (class, base)
    mutable unsigned outer_calls{};
    mutable unsigned is_a_calls{};

    int32_t index() const override { return idx; }
    int32_t serial() const override { return ser; }
    uint32_t name_index() const override { return name; }
    std::uintptr_t class_address() const override { return cls; }
    std::uintptr_t outer_class(unsigned level) const override
    {
        ++outer_calls;
        return level <= outers.size() ? outers[level - 1] : 0;
    }
    bool is_a(std::uintptr_t c, std::uintptr_t of) const override
    {
        ++is_a_calls;
        for (const auto& d : derived) if (d.first == c && d.second == of) return true;
        return false;
    }
};

static void empty_filter_passes()
{
    FakeObject object;
    CHECK(passes(Filter{}, object));
}

static void object_and_names()
{
    FakeObject object;
    Filter filter;
    filter.object = ObjectIdentity{0, 5, 9};
    CHECK(passes(filter, object));
    filter.object = ObjectIdentity{0, 5, 10};
    CHECK(!passes(filter, object));
    filter.object = ObjectIdentity{0, 6, 9};
    CHECK(!passes(filter, object));

    Filter names;
    names.name_indices = {1, 77, 3};
    CHECK(passes(names, object));
    names.name_indices = {1, 3};
    CHECK(!passes(names, object));
}

static void outer_class()
{
    FakeObject object;
    object.outers = {0x200, 0x300, 0x400};
    object.derived = {{0x300, 0x900}};

    Filter filter;
    filter.outer_class = 0x400;
    filter.outer_exact = true;
    CHECK(passes(filter, object));
    filter.outer_class = 0x900;
    CHECK(!passes(filter, object));            // exact: no IsA
    filter.outer_exact = false;
    CHECK(passes(filter, object));             // 0x300 derives from 0x900
    filter.outer_class = 0x999;
    CHECK(!passes(filter, object));

    // Depth limit: the match is at level 3.
    filter.outer_class = 0x400;
    filter.outer_exact = true;
    filter.outer_depth = 2;
    CHECK(!passes(filter, object));
    filter.outer_depth = 3;
    CHECK(passes(filter, object));

    // The walk never goes past outer_walk_depth, whatever the filter says.
    FakeObject deep;
    deep.outers.assign(20, 0x200);
    deep.outers[10] = 0x400;
    Filter capped;
    capped.outer_class = 0x400;
    capped.outer_exact = true;
    capped.outer_depth = 20;
    CHECK(!passes(capped, deep) && deep.outer_calls == outer_walk_depth);
}

static void order_and_short_circuit()
{
    FakeObject object;
    Filter filter;
    filter.object = ObjectIdentity{0, 1, 1}; // fails
    filter.name_indices = {77};
    filter.outer_class = 0x200;
    object.outers = {0x200};
    CHECK(!passes(filter, object));
    CHECK(object.outer_calls == 0);          // outer walk never started

    filter.object = ObjectIdentity{0, 5, 9};
    filter.name_indices = {1};               // name fails before the outer walk
    CHECK(!passes(filter, object) && object.outer_calls == 0);

    filter.name_indices = {77};
    CHECK(passes(filter, object) && object.outer_calls == 1 && object.is_a_calls == 0);
}

int main()
{
    empty_filter_passes();
    object_and_names();
    outer_class();
    order_and_short_circuit();
    return LexTests::finish("HookFiltersTests");
}
