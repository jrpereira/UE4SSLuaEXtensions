// Loggers and delivery: which pass delivers which policy, order, periodic
// timing, a full journal and the no-tick fallback.
#include <LogCore.hpp>
#include <TestSupport.hpp>

#include <string>
#include <thread>
#include <vector>

using namespace UE4SSLuaLogBridge::Core;
namespace NativeContract = UE4SSLuaLogBridge::NativeContract;

namespace
{
struct FakeSink final : Sink
{
    std::vector<std::string> ue4ss_lines;
    std::vector<std::string> files; // "<folder>|<category>|<text>"
    void ue4ss(const std::string& line) override { ue4ss_lines.push_back(line); }
    void file(const std::string& folder, const std::string& category, const std::string& text) override
    {
        files.push_back(folder + "|" + category + "|" + text);
    }
};

constexpr int64_t time = 20261118140322123;

struct Fixture
{
    explicit Fixture(bool tick = true, std::size_t bytes = 64 * 1024)
        : region(bytes), journal(region.data(), region.size()), core((journal.open(), journal), sink, tick)
    {
    }
    std::vector<uint8_t> region;
    Journal journal;
    FakeSink sink;
    LogCore core;
};

Policy policy(Target target, Frequency frequency, Thread thread, int64_t interval = 1000)
{
    return Policy{target, frequency, interval, thread};
}

void game_immediate()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "C:/Mods/Mod", policy(Target::ue4ss, Frequency::immediate, Thread::game), 0);
    const auto written = f.core.write(logger, 2, "main", "hello", time, 0);
    CHECK(written.ok && written.deliver_now && !written.wake_worker);
    CHECK(f.core.deliver(Pass::worker, 0) == 0 && f.core.deliver(Pass::call, 0) == 1);
    CHECK(f.sink.ue4ss_lines.size() == 1 && f.sink.ue4ss_lines[0] == "[Mod] DEBUG hello");
    CHECK(f.core.stats().delivered == 1 && f.core.stats().pending == 0 && f.journal.empty());
    CHECK(!f.core.write(99, 2, "main", "unknown logger", time, 0).ok);
}

void frame_and_files()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "C:/Mods/Mod", policy(Target::module, Frequency::frame, Thread::game), 0);
    for (int i = 0; i < 3; ++i)
    {
        const auto written = f.core.write(logger, 3, i == 1 ? "combat" : "main", "line " + std::to_string(i), time, 0);
        CHECK(written.ok && !written.deliver_now && !written.wake_worker);
    }
    CHECK(f.core.deliver(Pass::call, 0) == 0 && f.core.deliver(Pass::worker, 0, true) == 0);
    CHECK(f.core.deliver(Pass::tick, 0) == 3);
    // One sink call per file, lines in order.
    CHECK(f.sink.files.size() == 2);
    CHECK(f.sink.files[0] ==
        "C:/Mods/Mod|main|2026-11-18 14:03:22.123 INFO line 0\n2026-11-18 14:03:22.123 INFO line 2\n");
    CHECK(f.sink.files[1] == "C:/Mods/Mod|combat|2026-11-18 14:03:22.123 INFO line 1\n");
}

void worker_policies()
{
    Fixture f;
    const auto now = f.core.register_logger("A", "F", policy(Target::ue4ss, Frequency::immediate, Thread::worker), 0);
    const auto frame = f.core.register_logger("B", "F", policy(Target::ue4ss, Frequency::frame, Thread::worker), 0);
    const auto slow = f.core.register_logger("C", "F", policy(Target::ue4ss, Frequency::periodic, Thread::worker, 2000), 0);
    CHECK(f.core.has_worker_frame_loggers());
    CHECK(f.core.write(now, 4, "main", "a", time, 0).wake_worker);
    CHECK(!f.core.write(frame, 4, "main", "b", time, 0).wake_worker);
    CHECK(!f.core.write(slow, 4, "main", "c", time, 0).wake_worker);
    CHECK(f.core.worker_wait_ms(0, 500) == 0);
    CHECK(f.core.deliver(Pass::tick, 0) == 0);
    CHECK(f.core.deliver(Pass::worker, 100) == 1);              // immediate only
    CHECK(f.core.deliver(Pass::worker, 100, true) == 1);        // the frame logger once a frame passed
    CHECK(f.core.worker_wait_ms(100, 500) == 500 && f.core.worker_wait_ms(100, 5000) == 1900);
    CHECK(f.core.deliver(Pass::worker, 1999) == 0);
    CHECK(f.core.deliver(Pass::worker, 2000) == 1);             // periodic, due
    CHECK(f.sink.ue4ss_lines.size() == 3 && f.sink.ue4ss_lines[2] == "[C] WARN c");
    CHECK(f.core.write(slow, 4, "main", "d", time, 2100).ok);
    CHECK(f.core.worker_wait_ms(2100, 5000) == 1900);
    CHECK(f.core.deliver(Pass::worker, 3000) == 0 && f.core.deliver(Pass::all, 3000) == 1);
}

void game_periodic()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "", policy(Target::module, Frequency::periodic, Thread::game, 10), 0);
    CHECK(f.core.policy(logger).target == Target::ue4ss); // no folder: UE4SS.log
    CHECK(f.core.policy(logger).interval_ms == NativeContract::min_interval_ms);
    f.core.write(logger, 5, "ui", "x", time, 0);
    CHECK(f.core.deliver(Pass::tick, 10) == 0 && f.core.deliver(Pass::tick, 50) == 1);
    CHECK(f.sink.ue4ss_lines[0] == "[Mod:ui] ERROR x");
}

void no_tick_fallback()
{
    Fixture f(false);
    const auto frame = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::frame, Thread::game), 0);
    const auto now = f.core.register_logger("Now", "F", policy(Target::ue4ss, Frequency::immediate, Thread::game), 0);
    const auto p = f.core.policy(frame);
    CHECK(p.frequency == Frequency::periodic && p.thread == Thread::worker && p.interval_ms == LogCore::frame_fallback_ms);
    CHECK(f.core.policy(now).thread == Thread::game);
    f.core.write(frame, 4, "main", "late", time, 0);
    CHECK(f.core.deliver(Pass::tick, 100) == 0 && f.core.deliver(Pass::worker, 100) == 1);

    // Decided late: a logger registered while the tick was assumed moves when it turns out missing.
    Fixture g;
    const auto late = g.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::frame, Thread::game), 0);
    CHECK(g.core.policy(late).thread == Thread::game);
    g.core.write(late, 4, "main", "x", time, 0);
    g.core.set_tick_available(false);
    CHECK(g.core.policy(late).thread == Thread::worker && g.core.deliver(Pass::worker, 100) == 1);
}

void full_journal_stalls()
{
    Fixture f(true, Journal::header_bytes + 1024);
    const auto logger = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::periodic, Thread::worker, 60000), 0);
    for (int i = 0; i < 40; ++i) CHECK(f.core.write(logger, 4, "main", "line " + std::to_string(i), time, 0).ok);
    const auto stats = f.core.stats();
    CHECK(stats.written == 40 && stats.stalls > 0);
    f.core.deliver(Pass::all, 0);
    CHECK(f.sink.ue4ss_lines.size() == 40);
    bool ordered = true;
    for (int i = 0; i < 40; ++i) ordered = ordered && f.sink.ue4ss_lines[static_cast<std::size_t>(i)] == "[Mod] WARN line " + std::to_string(i);
    CHECK(ordered);
}

void truncation()
{
    Fixture f(true, 256 * 1024);
    const auto logger = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::immediate, Thread::game), 0);
    f.core.write(logger, 4, "main", std::string(100000, 'z'), time, 0);
    f.core.deliver(Pass::call, 0);
    const auto& line = f.sink.ue4ss_lines.at(0);
    CHECK(line.size() == std::string("[Mod] WARN ").size() + NativeContract::max_message_bytes + LogCore::truncated_suffix.size());
    CHECK(line.ends_with(" [truncated]"));
}

void threads()
{
    Fixture f(true, Journal::header_bytes + 8192);
    const auto game = f.core.register_logger("G", "F", policy(Target::ue4ss, Frequency::immediate, Thread::game), 0);
    const auto work = f.core.register_logger("W", "F", policy(Target::module, Frequency::immediate, Thread::worker), 0);
    std::thread worker([&] {
        for (int i = 0; i < 2000; ++i) f.core.deliver(Pass::worker, 0);
    });
    for (int i = 0; i < 1000; ++i)
    {
        if (f.core.write(game, 4, "main", "g", time, 0).deliver_now) f.core.deliver(Pass::call, 0);
        f.core.write(work, 4, "main", "w", time, 0);
    }
    worker.join();
    f.core.deliver(Pass::all, 0);
    std::size_t lines = 0;
    for (const auto& text : f.sink.files)
    {
        for (char c : text) lines += c == '\n';
    }
    CHECK(f.sink.ue4ss_lines.size() == 1000 && lines == 1000);
    CHECK(f.core.stats().delivered == 2000 && f.journal.empty());
}
} // namespace

int main()
{
    game_immediate();
    frame_and_files();
    worker_policies();
    game_periodic();
    no_tick_fallback();
    full_journal_stalls();
    truncation();
    threads();
    return LexTests::finish("CoreTests");
}
