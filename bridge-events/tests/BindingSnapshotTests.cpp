#include <BindingSnapshot.hpp>
#include <array>
#include <TestSupport.hpp>
using namespace UE4SSLuaEventBridge;
int main() {
 CHECK(snapshot_array_valid(0,0,0));
 CHECK(!snapshot_array_valid(0,1,1));
 CHECK(!snapshot_array_valid(8,-1,1));
 CHECK(!snapshot_array_valid(8,2,1));
 CHECK(!snapshot_array_valid(8,0,65536));
 CHECK(!snapshot_array_valid(9,1,1));
 CHECK(snapshot_array_valid(8,65535,65535));
 std::array<std::uintptr_t,3> entries{8,16,24};
 CHECK(snapshot_contains(entries,16));
 CHECK(!snapshot_contains(entries,32));
 CHECK(!snapshot_contains(entries,0));
 CHECK(!snapshot_contains({},8));
 int calls = 0;
 int action{}, event{}, handle{};
 auto fail_read = [&](const void*, void*, std::size_t) { ++calls; return false; };
 CHECK(!snapshot_read_metadata(false, fail_read, nullptr, nullptr, nullptr, action,event,handle));
 CHECK(calls == 0); // Absent/stale binding must never be read.
 CHECK(!snapshot_read_metadata(true, fail_read, nullptr, nullptr, nullptr, action,event,handle));
 CHECK(calls == 1); // Failed read must short-circuit the remaining metadata.
 calls = 0;
 auto good_read = [&](const void*, void* out, std::size_t) { ++calls; *static_cast<int*>(out)=42; return true; };
 CHECK(snapshot_read_metadata(true, good_read, nullptr,nullptr,nullptr,action,event,handle));
 CHECK(calls == 3 && action == 42 && event == 42 && handle == 42);
    return LexTests::finish("BindingSnapshotTests");
}
