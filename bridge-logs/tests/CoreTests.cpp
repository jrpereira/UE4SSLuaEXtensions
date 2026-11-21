// Loggers and delivery: what is due for each policy, order, periodic timing,
// a full journal, the no-tick fallback and the assembly of crash recovery.
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

Policy policy(Target target, Frequency frequency, int64_t interval = 1000)
{
    return Policy{target, frequency, interval};
}

void immediate()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "C:/Mods/Mod", policy(Target::ue4ss, Frequency::immediate), 0);
    const auto written = f.core.write(logger, 2, "main", "hello", time, 0);
    CHECK(written.ok && written.wake_worker);
    CHECK(f.core.worker_wait_ms(0, 500) == 0);
    CHECK(f.core.due(0) == 1);
    CHECK(f.sink.ue4ss_lines.size() == 1 && f.sink.ue4ss_lines[0] == "[Mod] DEBUG hello");
    CHECK(f.core.stats().delivered == 1 && f.core.stats().pending == 0 && f.journal.empty());
    CHECK(!f.core.write(99, 2, "main", "unknown logger", time, 0).ok);
}

void frame_and_files()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "C:/Mods/Mod", policy(Target::module, Frequency::frame), 0);
    CHECK(!f.core.has_frame_lines()); // a frame logger with nothing pending: the tick stays quiet
    for (int i = 0; i < 3; ++i)
    {
        const auto written = f.core.write(logger, 3, i == 1 ? "combat" : "main", "line " + std::to_string(i), time, 0);
        CHECK(written.ok && !written.wake_worker);
    }
    CHECK(f.core.has_frame_lines());
    CHECK(f.core.due(0) == 0 && f.core.has_frame_lines());
    CHECK(f.core.due(0, true) == 3);
    CHECK(!f.core.has_frame_lines());
    // One sink call per file, lines in order.
    CHECK(f.sink.files.size() == 2);
    CHECK(f.sink.files[0] ==
        "C:/Mods/Mod|main|2026-11-18 14:03:22.123 INFO line 0\n2026-11-18 14:03:22.123 INFO line 2\n");
    CHECK(f.sink.files[1] == "C:/Mods/Mod|combat|2026-11-18 14:03:22.123 INFO line 1\n");
    CHECK(f.core.due(0, true) == 0); // nothing pending: an early return
    f.core.write(logger, 3, "main", "flushed", time, 0);
    CHECK(f.core.has_frame_lines() && f.core.all(0) == 1 && !f.core.has_frame_lines());
}

void policies()
{
    Fixture f;
    const auto now = f.core.register_logger("A", "F", policy(Target::ue4ss, Frequency::immediate), 0);
    const auto frame = f.core.register_logger("B", "F", policy(Target::ue4ss, Frequency::frame), 0);
    const auto slow = f.core.register_logger("C", "F", policy(Target::ue4ss, Frequency::periodic, 2000), 0);
    CHECK(f.core.write(now, 4, "main", "a", time, 0).wake_worker);
    CHECK(!f.core.write(frame, 4, "main", "b", time, 0).wake_worker);
    CHECK(!f.core.write(slow, 4, "main", "c", time, 0).wake_worker);
    CHECK(f.core.worker_wait_ms(0, 500) == 0);
    CHECK(f.core.due(100) == 1);              // immediate only
    CHECK(f.core.due(100, true) == 1);        // the frame logger once a frame passed
    CHECK(f.core.worker_wait_ms(100, 500) == 500 && f.core.worker_wait_ms(100, 5000) == 1900);
    CHECK(f.core.due(1999) == 0);
    CHECK(f.core.due(2000) == 1);             // periodic, due
    CHECK(f.sink.ue4ss_lines.size() == 3 && f.sink.ue4ss_lines[2] == "[C] WARN c");
    CHECK(f.core.write(slow, 4, "main", "d", time, 2100).ok);
    CHECK(f.core.worker_wait_ms(2100, 5000) == 1900);
    CHECK(f.core.due(3000) == 0 && f.core.all(3000) == 1);
}

void periodic_limits()
{
    Fixture f;
    const auto logger = f.core.register_logger("Mod", "", policy(Target::module, Frequency::periodic, 10), 0);
    CHECK(f.core.policy(logger).target == Target::ue4ss); // no folder: UE4SS.log
    CHECK(f.core.policy(logger).interval_ms == NativeContract::min_interval_ms);
    f.core.write(logger, 5, "ui", "x", time, 0);
    CHECK(f.core.due(10) == 0 && f.core.due(50) == 1);
    CHECK(f.sink.ue4ss_lines[0] == "[Mod:ui] ERROR x");
}

void no_tick_fallback()
{
    Fixture f(false);
    const auto frame = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::frame), 0);
    const auto now = f.core.register_logger("Now", "F", policy(Target::ue4ss, Frequency::immediate), 0);
    const auto p = f.core.policy(frame);
    CHECK(p.frequency == Frequency::periodic && p.interval_ms == LogCore::frame_fallback_ms);
    CHECK(f.core.policy(now).frequency == Frequency::immediate);
    f.core.write(frame, 4, "main", "late", time, 0);
    CHECK(!f.core.has_frame_lines()); // no tick to ask
    CHECK(f.core.due(10) == 0 && f.core.due(16) == 1);

    // Decided late: a logger registered while the tick was assumed falls back when it turns out missing.
    Fixture g;
    const auto late = g.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::frame), 0);
    CHECK(g.core.policy(late).frequency == Frequency::frame);
    g.core.write(late, 4, "main", "x", time, 0);
    g.core.set_tick_available(false);
    CHECK(g.core.policy(late).frequency == Frequency::periodic && g.core.due(100) == 1);
}

void full_journal_stalls()
{
    Fixture f(true, Journal::header_bytes + 1024);
    const auto logger = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::periodic, 60000), 0);
    for (int i = 0; i < 40; ++i) CHECK(f.core.write(logger, 4, "main", "line " + std::to_string(i), time, 0).ok);
    const auto stats = f.core.stats();
    CHECK(stats.written == 40 && stats.stalls > 0);
    f.core.all(0);
    CHECK(f.sink.ue4ss_lines.size() == 40);
    bool ordered = true;
    for (int i = 0; i < 40; ++i) ordered = ordered && f.sink.ue4ss_lines[static_cast<std::size_t>(i)] == "[Mod] WARN line " + std::to_string(i);
    CHECK(ordered);
}

void truncation()
{
    Fixture f(true, 256 * 1024);
    const auto logger = f.core.register_logger("Mod", "F", policy(Target::ue4ss, Frequency::immediate), 0);
    f.core.write(logger, 4, "main", std::string(100000, 'z'), time, 0);
    f.core.due(0);
    const auto& line = f.sink.ue4ss_lines.at(0);
    CHECK(line.size() == std::string("[Mod] WARN ").size() + NativeContract::max_message_bytes + LogCore::truncated_suffix.size());
    CHECK(line.ends_with(" [truncated]"));
}

// The game thread writes while the worker delivers; a small journal makes the writer stall too.
void threads()
{
    Fixture f(true, Journal::header_bytes + 8192);
    const auto game = f.core.register_logger("G", "F", policy(Target::ue4ss, Frequency::immediate), 0);
    const auto file = f.core.register_logger("W", "F", policy(Target::module, Frequency::frame), 0);
    std::thread worker([&] {
        for (int i = 0; i < 2000; ++i) f.core.due(0, i % 2 == 0);
    });
    for (int i = 0; i < 1000; ++i)
    {
        f.core.write(game, 4, "main", "g", time, 0);
        f.core.write(file, 4, "main", "w", time, 0);
    }
    worker.join();
    f.core.all(0);
    std::size_t lines = 0;
    for (const auto& text : f.sink.files)
    {
        for (char c : text) lines += c == '\n';
    }
    CHECK(f.sink.ue4ss_lines.size() == 1000 && lines == 1000);
    CHECK(f.core.stats().delivered == 2000 && f.journal.empty() && !f.core.has_frame_lines());
}
// A previous session's journal: what previous-session.log holds and which
// module files get the lines they never received.
void recovery_assembly()
{
    std::vector<uint8_t> region(Journal::header_bytes + 8192);
    {
        Journal journal(region.data(), region.size());
        journal.open();
        const auto module = static_cast<uint8_t>(Target::module);
        const auto ue4ss = static_cast<uint8_t>(Target::ue4ss);
        const auto add = [&](uint64_t seq, uint8_t target, std::string_view folder, std::string_view category,
                             std::string_view message, RecordState state) {
            const auto id = journal.append(JournalEntry{seq, time, 3, target, "Mod", folder, category, message});
            CHECK(id != 0);
            if (state != RecordState::written) journal.mark(id, state);
        };
        add(1, module, "C:/Mods/Mod", "main", "delivered", RecordState::delivered);
        add(2, module, "C:/Mods/Mod", "main", "lost main", RecordState::written);
        add(3, module, "C:/Mods/Mod", "combat", "lost combat", RecordState::delivering);
        add(4, ue4ss, "C:/Mods/Mod", "main", "lost to UE4SS.log", RecordState::written);
        add(5, module, "", "main", "lost without a folder", RecordState::written);
        add(6, module, "C:/Mods/Mod", "main", "lost main again", RecordState::written);
        // The process "crashes" here.
    }
    Journal next(region.data(), region.size());
    const auto records = next.recover(next.open());
    CHECK(records.size() == 6);
    const auto recovery = assemble_recovery(records);
    CHECK(recovery.undelivered == 5);

    // Every record, in order; only the delivered one is unmarked.
    std::string expected;
    for (const auto& record : records)
    {
        expected += recovery_line(record.time, record.rank, record.module, record.category, record.message,
            record.seq != 1);
    }
    CHECK(recovery.previous_session == expected);
    CHECK(recovery.previous_session.find(" * ") != std::string::npos);
    CHECK(recovery.previous_session.starts_with("2026-11-18 14:03:22.123 INFO"));

    // Only undelivered `module` lines with a folder, grouped per file in order.
    CHECK(recovery.file_appends.size() == 2);
    CHECK(recovery.file_appends.at("C:/Mods/Mod/logs/main.log") ==
        "2026-11-18 14:03:22.123 INFO lost main\n2026-11-18 14:03:22.123 INFO lost main again\n");
    CHECK(recovery.file_appends.at("C:/Mods/Mod/logs/combat.log") == "2026-11-18 14:03:22.123 INFO lost combat\n");

    CHECK(assemble_recovery({}).previous_session.empty() && assemble_recovery({}).undelivered == 0);
}
} // namespace

int main()
{
    immediate();
    frame_and_files();
    policies();
    periodic_limits();
    no_tick_fallback();
    full_journal_stalls();
    truncation();
    threads();
    recovery_assembly();
    return LexTests::finish("CoreTests");
}
