#pragma once

// Opcode slots for the Blueprint handlers (LEX_hooks.md §4.4).
//
// GNatives (UE4SS: GNatives_Internal) is the engine's FNativeFuncPtr
// GNatives[EX_Max] table, EX_Max = 0x100, indexed by a bytecode opcode; the
// script VM's FFrame::Step runs GNatives[*Code++](Context, Stack, Result).
//
// Assumptions:
// - Every opcode the engine does not implement holds the same pointer,
//   execUndefined, and nothing else occurs as often. The most frequent non-null
//   pointer marks free slots when it occurs at least gnatives_free_minimum (8)
//   times; a tie for most frequent is ambiguous and yields no slots.
// - A null pointer never marks free slots: an unknown frame in handler A falls
//   back to the free-slot pointer, which must be callable.
// - The table lives in writable static data (the engine fills it during static
//   initialisation), so claiming a slot is a plain pointer store; it is done
//   with a compare-exchange so a concurrent claimant is detected, not
//   overwritten.
//
// Choice: A (entry handler) is the highest free index and B (exit handler) the
// next free index below it. EX_Return, EX_Nothing and EX_EndOfScript are never
// chosen even if they were to look free. Slots are never released.

#include <HookTypes.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace UE4SSLuaEventBridge::Hooks
{
template <typename Exec>
struct SlotChoice
{
    uint8_t a{};             // entry handler, written over Script[0]
    uint8_t b{};             // exit handler, written over the EX_Return byte
    Exec undefined{};        // the free-slot pointer (execUndefined)
    std::size_t free_slots{}; // how many slots held it when chosen
};

// Pure: picks the pair from a snapshot of the table. Exec is NativeFuncPtr for
// the real table; tests use their fake frame's exec type.
template <typename Exec>
std::optional<SlotChoice<Exec>> choose_slots(std::span<const Exec, gnatives_count> natives)
{
    Exec most{};
    std::size_t most_count = 0;
    bool tie = false;
    for (std::size_t index = 0; index < natives.size(); ++index)
    {
        const Exec candidate = natives[index];
        if (candidate == nullptr) continue;
        bool seen = false;
        for (std::size_t earlier = 0; earlier < index && !seen; ++earlier) seen = natives[earlier] == candidate;
        if (seen) continue;
        std::size_t count = 0;
        for (std::size_t later = index; later < natives.size(); ++later) count += natives[later] == candidate ? 1 : 0;
        if (count > most_count)
        {
            most = candidate;
            most_count = count;
            tie = false;
        }
        else if (count == most_count)
        {
            tie = true;
        }
    }
    if (most_count < gnatives_free_minimum || tie) return std::nullopt;

    std::optional<uint8_t> a;
    for (std::size_t index = natives.size(); index-- > 0;)
    {
        if (index == ex_return || index == ex_nothing || index == ex_end_of_script) continue;
        if (natives[index] != most) continue;
        if (!a)
        {
            a = static_cast<uint8_t>(index);
            continue;
        }
        return SlotChoice<Exec>{*a, static_cast<uint8_t>(index), most, most_count};
    }
    return std::nullopt;
}

enum class ClaimStatus : uint8_t { claimed, collision };

// Writes the handlers into the chosen slots, each only if it still holds the
// free-slot pointer: B first, then A; if A was taken meanwhile, B is put back.
template <typename Exec>
ClaimStatus claim_slots(std::span<Exec, gnatives_count> natives, const SlotChoice<Exec>& choice,
    Exec entry_handler, Exec exit_handler) noexcept
{
    Exec expected = choice.undefined;
    if (!std::atomic_ref<Exec>(natives[choice.b]).compare_exchange_strong(expected, exit_handler))
        return ClaimStatus::collision;
    expected = choice.undefined;
    if (!std::atomic_ref<Exec>(natives[choice.a]).compare_exchange_strong(expected, entry_handler))
    {
        Exec ours = exit_handler;
        std::atomic_ref<Exec>(natives[choice.b]).compare_exchange_strong(ours, choice.undefined);
        return ClaimStatus::collision;
    }
    return ClaimStatus::claimed;
}

template <typename Exec>
struct SlotClaim
{
    std::optional<SlotChoice<Exec>> slots;
    uint32_t collisions{};
    const char* reason{}; // set when slots is empty
};

// Choose, claim, and on a collision choose again from the current table
// (LEX_hooks.md §14), up to `attempts` times.
template <typename Exec>
SlotClaim<Exec> claim_free_pair(std::span<Exec, gnatives_count> natives, Exec entry_handler, Exec exit_handler,
    uint32_t attempts = 4)
{
    SlotClaim<Exec> claim;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt)
    {
        const auto choice = choose_slots<Exec>(std::span<const Exec, gnatives_count>(natives));
        if (!choice)
        {
            claim.reason = "no free opcode slot pair in GNatives";
            return claim;
        }
        if (claim_slots(natives, *choice, entry_handler, exit_handler) == ClaimStatus::claimed)
        {
            claim.slots = choice;
            return claim;
        }
        ++claim.collisions;
    }
    claim.reason = "opcode slots kept being taken by another writer";
    return claim;
}
}
