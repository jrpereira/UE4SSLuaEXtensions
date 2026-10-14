#pragma once

// Native hook filters (LEX_hooks.md §5.2). Evaluated in the handler before any
// copying, in this order, short-circuiting at the first failure:
//   1. object        one instance: index and serial equal
//   2. name_indices  the frame object's FName comparison index is in the set
//   3. outer_class   an outer (1..outer_depth levels up) has this class, or IsA it
// An unset filter (no object, empty name set, outer_class == 0) passes. A name
// filter whose names do not exist yet is the caller's concern: it must not
// build a Filter with an empty set in that case (it can never match).
// Portable: ObjectView is implemented by the Windows side with fixed-offset
// reads (HookLayout) and by the tests with a fake.

#include <HookTypes.hpp>

namespace UE4SSLuaEventBridge::Hooks
{
class ObjectView
{
public:
    virtual ~ObjectView() = default;
    virtual int32_t index() const = 0;
    virtual int32_t serial() const = 0;
    virtual uint32_t name_index() const = 0;             // FName comparison index
    virtual std::uintptr_t class_address() const = 0;    // the object's own class
    // Class of the outer `level` steps up (1 = direct outer); 0 when the chain
    // ends first. Called with level 1, 2, ... in order, never beyond outer_depth.
    virtual std::uintptr_t outer_class(unsigned level) const = 0;
    virtual bool is_a(std::uintptr_t cls, std::uintptr_t of_class) const = 0;
};

inline bool passes(const Filter& filter, const ObjectView& object)
{
    if (filter.object)
    {
        if (object.index() != filter.object->index || object.serial() != filter.object->serial) return false;
    }
    if (!filter.name_indices.empty())
    {
        const uint32_t name = object.name_index();
        bool found = false;
        for (const uint32_t wanted : filter.name_indices)
        {
            if (wanted == name) { found = true; break; }
        }
        if (!found) return false;
    }
    if (filter.outer_class != 0)
    {
        const unsigned depth = filter.outer_depth < outer_walk_depth ? filter.outer_depth : static_cast<unsigned>(outer_walk_depth);
        for (unsigned level = 1; level <= depth; ++level)
        {
            const std::uintptr_t cls = object.outer_class(level);
            if (cls == 0) return false;
            if (cls == filter.outer_class) return true;
            if (!filter.outer_exact && object.is_a(cls, filter.outer_class)) return true;
        }
        return false;
    }
    return true;
}
}
