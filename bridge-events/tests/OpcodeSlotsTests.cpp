#include <OpcodeSlots.hpp>

#include <TestSupport.hpp>
#include <array>
#include <span>

using namespace UE4SSLuaEventBridge::Hooks;

namespace
{
using RC::Unreal::FFrame;
using RC::Unreal::UObject;

void undefined_exec(UObject*, FFrame&, void*) {}
void other_exec(UObject*, FFrame&, void*) {}
void third_exec(UObject*, FFrame&, void*) {}
void entry_exec(UObject*, FFrame&, void*) {}
void exit_exec(UObject*, FFrame&, void*) {}

using Table = std::array<NativeFuncPtr, gnatives_count>;

std::span<const NativeFuncPtr, gnatives_count> view(const Table& table)
{
    return std::span<const NativeFuncPtr, gnatives_count>(table);
}

std::span<NativeFuncPtr, gnatives_count> edit(Table& table)
{
    return std::span<NativeFuncPtr, gnatives_count>(table);
}

// Opcodes 0x00-0x6F implemented (two handlers, neither as frequent as the
// free pointer), everything above free.
Table engine_like()
{
    Table table{};
    table.fill(&undefined_exec);
    for (std::size_t index = 0; index < 0x70; ++index) table[index] = index % 3 == 0 ? &other_exec : &third_exec;
    return table;
}

void most_frequent_pointer_marks_free_slots()
{
    const Table table = engine_like();
    const auto choice = choose_slots<NativeFuncPtr>(view(table));
    CHECK(choice.has_value());
    if (!choice) return;
    CHECK(choice->undefined == &undefined_exec);
    CHECK(choice->a == 0xFF && choice->b == 0xFE); // A the highest, B the next one down
    CHECK(choice->free_slots == 0x100 - 0x70);
}

void two_highest_free_indices_anywhere()
{
    // Free slots in the middle of the table; the top is implemented.
    Table table{};
    for (std::size_t index = 0x40; index < 0x50; ++index) table[index] = &undefined_exec;
    table[0x48] = &other_exec;
    table[0x4F] = &other_exec;
    const auto choice = choose_slots<NativeFuncPtr>(view(table));
    CHECK(choice.has_value() && choice->a == 0x4E && choice->b == 0x4D);
}

void tie_is_ambiguous()
{
    Table table{};
    for (std::size_t index = 0; index < table.size(); ++index) table[index] = index % 2 ? &other_exec : &third_exec;
    CHECK(!choose_slots<NativeFuncPtr>(view(table)).has_value());
}

void minimum_count_and_null()
{
    // Null never marks free slots: 249 nulls, 7 undefined is below the minimum.
    Table table{};
    for (std::size_t index = 0xF9; index < 0x100; ++index) table[index] = &undefined_exec;
    CHECK(!choose_slots<NativeFuncPtr>(view(table)).has_value());
    // The eighth occurrence makes it eligible.
    table[0xF8] = &undefined_exec;
    const auto choice = choose_slots<NativeFuncPtr>(view(table));
    CHECK(choice.has_value() && choice->a == 0xFF && choice->b == 0xFE && choice->free_slots == gnatives_free_minimum);
}

void reserved_opcodes_never_chosen()
{
    // EX_Return, EX_Nothing and EX_EndOfScript look free here; 0x53 is the
    // highest "free" index but is skipped.
    Table table{};
    for (std::size_t slot : {std::size_t{ex_return}, std::size_t{ex_nothing}, std::size_t{ex_end_of_script}})
        table[slot] = &undefined_exec;
    for (std::size_t index = 0x10; index < 0x15; ++index) table[index] = &undefined_exec;
    const auto choice = choose_slots<NativeFuncPtr>(view(table));
    CHECK(choice.has_value() && choice->a == 0x14 && choice->b == 0x13);
}

void claim_writes_handlers_and_detects_collisions()
{
    Table table = engine_like();
    const auto choice = choose_slots<NativeFuncPtr>(view(table));
    CHECK(choice.has_value());
    if (!choice) return;

    // Someone takes A between choice and claim: B is put back.
    Table raced = table;
    raced[choice->a] = &third_exec;
    CHECK(claim_slots<NativeFuncPtr>(edit(raced), *choice, &entry_exec, &exit_exec) == ClaimStatus::collision);
    CHECK(raced[choice->a] == &third_exec && raced[choice->b] == &undefined_exec);

    // Someone takes B: A is never written.
    Table raced_b = table;
    raced_b[choice->b] = &third_exec;
    CHECK(claim_slots<NativeFuncPtr>(edit(raced_b), *choice, &entry_exec, &exit_exec) == ClaimStatus::collision);
    CHECK(raced_b[choice->a] == &undefined_exec && raced_b[choice->b] == &third_exec);

    const Table before = table;
    CHECK(claim_slots<NativeFuncPtr>(edit(table), *choice, &entry_exec, &exit_exec) == ClaimStatus::claimed);
    CHECK(table[0xFF] == &entry_exec && table[0xFE] == &exit_exec);
    for (std::size_t index = 0; index < 0xFE; ++index) CHECK(table[index] == before[index]);
    // The slots are ours now: claiming the same choice again collides.
    CHECK(claim_slots<NativeFuncPtr>(edit(table), *choice, &entry_exec, &exit_exec) == ClaimStatus::collision);
}

void claim_free_pair_repicks()
{
    // Another mod already owns 0xFF: the pair moves down.
    Table table = engine_like();
    table[0xFF] = &third_exec;
    const auto claim = claim_free_pair<NativeFuncPtr>(edit(table), &entry_exec, &exit_exec);
    CHECK(claim.slots.has_value() && claim.reason == nullptr && claim.collisions == 0);
    if (claim.slots) CHECK(claim.slots->a == 0xFE && claim.slots->b == 0xFD);
    CHECK(table[0xFF] == &third_exec && table[0xFE] == &entry_exec && table[0xFD] == &exit_exec);

    // No pair: a reason and nothing written.
    Table full{};
    for (std::size_t index = 0; index < full.size(); ++index) full[index] = index % 2 ? &other_exec : &third_exec;
    const Table before = full;
    const auto none = claim_free_pair<NativeFuncPtr>(edit(full), &entry_exec, &exit_exec);
    CHECK(!none.slots.has_value() && none.reason != nullptr);
    CHECK(full == before);
}
}

int main()
{
    most_frequent_pointer_marks_free_slots();
    two_highest_free_indices_anywhere();
    tie_is_ambiguous();
    minimum_count_and_null();
    reserved_opcodes_never_chosen();
    claim_writes_handlers_and_detects_collisions();
    claim_free_pair_repicks();
    return LexTests::finish("OpcodeSlotsTests");
}
