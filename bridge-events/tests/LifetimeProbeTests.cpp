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
    return LexTests::finish("LifetimeProbeTests");
}
