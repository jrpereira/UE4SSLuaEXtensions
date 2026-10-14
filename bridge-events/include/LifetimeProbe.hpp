#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace UE4SSLuaEventBridge
{
// Status of the UObject layout probe that gates object lifetimes. The probe
// runs once at Unreal initialization: it checks offsets against objects that
// already sit in the object array then, so a failure is a layout mismatch that
// a later attempt would not change, and registering object-array listeners
// mid-game could race the engine's loading threads.
//
// Function hooks add a second check on the same sample (LEX_hooks.md §6.3):
// HookLayout's ClassPrivate, NamePrivate and OuterPrivate offsets read
// directly against the exported accessors. Hooks need both checks; one
// mismatch fails the hook layout, since filters and wake-ups read those
// offsets on every call.
class LifetimeProbe
{
public:
    static constexpr std::size_t required_objects = 2;
    static constexpr std::size_t hook_layout_sample = 16;

    void record(std::size_t checked, std::size_t verified)
    {
        std::scoped_lock lock(mutex_);
        ran_ = true;
        checked_ = checked;
        verified_ = verified;
        ready_ = verified >= required_objects;
    }

    // checked: objects compared; mismatched: objects where any of the three
    // fixed-offset reads differed from its accessor.
    void record_hook_layout(std::size_t checked, std::size_t mismatched)
    {
        std::scoped_lock lock(mutex_);
        hook_layout_ran_ = true;
        hook_layout_checked_ = checked;
        hook_layout_mismatched_ = mismatched;
    }

    bool hooks_ready() const
    {
        std::scoped_lock lock(mutex_);
        return ready_ && hook_layout_ok();
    }

    // Why the layout keeps function hooks off, or nothing when it does not.
    std::optional<std::string> hooks_reason() const
    {
        if (auto lifetimes = reason()) return "object lifetimes are unavailable: " + *lifetimes;
        std::scoped_lock lock(mutex_);
        if (hook_layout_ok()) return std::nullopt;
        if (!hook_layout_ran_) return std::string{"the hook layout probe has not run"};
        if (hook_layout_mismatched_ != 0)
            return "the hook layout probe found " + std::to_string(hook_layout_mismatched_) + " of " +
                std::to_string(hook_layout_checked_) + " objects whose class, name or outer differ from UE4SS's accessors";
        return "the hook layout probe checked " + std::to_string(hook_layout_checked_) + " objects (" +
            std::to_string(required_objects) + " required)";
    }

    void shut_down()
    {
        std::scoped_lock lock(mutex_);
        shut_down_ = true;
        ready_ = false;
    }

    bool ready() const
    {
        std::scoped_lock lock(mutex_);
        return ready_;
    }

    // Why lifetimes are unavailable, or nothing when they are available.
    std::optional<std::string> reason() const
    {
        std::scoped_lock lock(mutex_);
        if (shut_down_) return std::string{"the UObject array has shut down"};
        if (ready_) return std::nullopt;
        if (!ran_) return std::string{"the UObject layout probe has not run"};
        if (checked_ == 0) return std::string{"the UObject layout probe found no live objects to check"};
        return "the UObject layout probe verified " + std::to_string(verified_) + " of " +
            std::to_string(checked_) + " objects (" + std::to_string(required_objects) + " required)";
    }

private:
    bool hook_layout_ok() const
    {
        return hook_layout_ran_ && hook_layout_mismatched_ == 0 && hook_layout_checked_ >= required_objects;
    }

    mutable std::mutex mutex_;
    std::size_t hook_layout_checked_{};
    std::size_t hook_layout_mismatched_{};
    bool hook_layout_ran_{};
    std::size_t checked_{};
    std::size_t verified_{};
    bool ran_{};
    bool ready_{};
    bool shut_down_{};
};
}
