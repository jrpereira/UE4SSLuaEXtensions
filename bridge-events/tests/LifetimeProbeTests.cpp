#include <LifetimeProbe.hpp>

#include <TestSupport.hpp>

using UE4SSLuaEventBridge::LifetimeProbe;

int main()
{
    {
        LifetimeProbe probe;
        CHECK(!probe.ready() && probe.reason()->find("has not run") != std::string::npos);
        probe.record(40, 1);
        CHECK(!probe.ready() && probe.reason()->find("verified 1 of 40 objects (2 required)") != std::string::npos);
    }
    {
        LifetimeProbe probe;
        probe.record(0, 0);
        CHECK(!probe.ready() && probe.reason()->find("found no live objects") != std::string::npos);
    }
    {
        LifetimeProbe probe;
        probe.record(300, 2);
        CHECK(probe.ready() && !probe.reason());
        probe.shut_down();
        CHECK(!probe.ready() && probe.reason()->find("shut down") != std::string::npos);
    }
    {
        LifetimeProbe probe;
        probe.record(300, 2);
        CHECK(!probe.hooks_ready() && probe.hooks_reason()->find("hook layout probe has not run") != std::string::npos);
        probe.record_hook_layout(16, 0);
        CHECK(probe.hooks_ready() && !probe.hooks_reason());
        probe.record_hook_layout(16, 1);
        CHECK(!probe.hooks_ready() && probe.hooks_reason()->find("found 1 of 16 objects") != std::string::npos);
        probe.record_hook_layout(1, 0);
        CHECK(!probe.hooks_ready() && probe.hooks_reason()->find("checked 1 objects (2 required)") != std::string::npos);
        probe.record_hook_layout(16, 0);
        probe.shut_down();
        CHECK(!probe.hooks_ready() && probe.hooks_reason()->find("object lifetimes are unavailable") != std::string::npos);
    }
    {
        LifetimeProbe probe;
        probe.record(40, 1);
        probe.record_hook_layout(16, 0);
        CHECK(!probe.hooks_ready() && probe.hooks_reason()->find("verified 1 of 40") != std::string::npos);
    }
    return LexTests::finish("LifetimeProbeTests");
}
