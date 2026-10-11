#include <dispatch/QueueDispatchSchedule.hpp>
#include <TestSupport.hpp>
#include <chrono>
using namespace UE4SSXB;
using namespace std::chrono_literals;
int main()
{
    CHECK(parse_queue_check_rate("") == 20);
    CHECK(parse_queue_check_rate("30") == 30);
    CHECK(parse_queue_check_rate("60") == 60);
    CHECK(parse_queue_check_rate("1") == 1);
    CHECK(parse_queue_check_rate("1000") == 1000);
    for (auto invalid : {"0", "-1", "1001", "30.5", "60x", " 60", "4294967296"})
        CHECK(parse_queue_check_rate(invalid) == 20);
    const QueueDispatchSchedule::Clock::time_point zero{};
    QueueDispatchSchedule schedule;
    CHECK(schedule.due(zero));
    CHECK(!schedule.due(zero + 49ms));
    CHECK(schedule.due(zero + 50ms));
    CHECK(!schedule.due(zero + 50ms));
    CHECK(schedule.due(zero + 10s));
    CHECK(!schedule.due(zero + 10s + 1ms));
    QueueDispatchSchedule host;
    int count = 0;
    for (int ms = 0; ms < 1000; ms += 5)
        if (host.due(zero + std::chrono::milliseconds(ms))) ++count;
    CHECK(count == 20);
    QueueDispatchSchedule sixty(60);
    CHECK(sixty.due(zero));
    CHECK(!sixty.due(zero + 16ms));
    CHECK(sixty.due(zero + 17ms));
    QueueDispatchSchedule one(1);
    CHECK(one.due(zero));
    CHECK(!one.due(zero + 999ms));
    CHECK(one.due(zero + 1s));
    QueueDispatchSchedule invalid(0);
    CHECK(invalid.due(zero));
    CHECK(!invalid.due(zero + 49ms));
    CHECK(invalid.due(zero + 50ms));
    return LexTests::finish("QueueDispatchScheduleTests");
}
