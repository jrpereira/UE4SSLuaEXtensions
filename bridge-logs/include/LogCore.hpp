#pragma once

// Loggers, writing into the journal and delivering to targets. Portable: the
// Windows layer supplies the Sink (UE4SS.log and files), the clocks, the
// engine tick and the worker thread.
//
// The game thread only copies lines into the journal; the worker delivers.
//   due()  the worker's pass: what is due (`frame` once a frame has passed)
//   all()  everything pending, synchronously (Flush, a full journal, stop)
// One delivery lock keeps every logger's lines in order.
//
// Without the engine tick, `frame` becomes `periodic` at 16 ms.

#include <Journal.hpp>
#include <LogBridgeNativeContract.hpp>
#include <LogFormat.hpp>
#include <LogLevels.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UE4SSLuaLogBridge::Core
{
using NativeContract::Frequency;
using NativeContract::Target;

struct Policy
{
    Target target{Target::ue4ss};
    Frequency frequency{Frequency::immediate};
    int64_t interval_ms{1000};
};

class Sink
{
public:
    virtual ~Sink() = default;
    // One UE4SS.log line, without its newline.
    virtual void ue4ss(const std::string& line) = 0;
    // Appends `text` (whole lines) to <folder>/logs/<category>.log.
    virtual void file(const std::string& folder, const std::string& category, const std::string& text) = 0;
};

// What a start makes of the previous session's journal records: the text of
// logs/previous-session.log (every record, undelivered ones marked), the lines
// each `module` file still lacks, keyed by <folder>/logs/<category>.log, and
// how many records never reached their target.
struct Recovery
{
    std::string previous_session;
    std::map<std::string, std::string> file_appends;
    int64_t undelivered{};
};

inline Recovery assemble_recovery(const std::vector<RecoveredRecord>& records)
{
    Recovery recovery;
    for (const auto& record : records)
    {
        const bool lost = record.state != RecordState::delivered;
        recovery.previous_session +=
            recovery_line(record.time, record.rank, record.module, record.category, record.message, lost);
        if (!lost) continue;
        ++recovery.undelivered;
        if (record.target == static_cast<uint8_t>(Target::module) && !record.folder.empty() &&
            valid_category_name(record.category))
        {
            recovery.file_appends[record.folder + "/logs/" + record.category + ".log"] +=
                file_line(record.time, record.rank, record.message);
        }
    }
    return recovery;
}

struct CoreStats
{
    int64_t written{};
    int64_t delivered{};
    int64_t pending{};
    int64_t stalls{};
    int64_t loggers{};
};

class LogCore
{
public:
    static constexpr int64_t frame_fallback_ms = 16;
    static constexpr std::string_view truncated_suffix = " [truncated]";

    LogCore(Journal& journal, Sink& sink, bool tick_available) : journal_(journal), sink_(sink), tick_(tick_available) {}

    // The engine tick's availability is known only at Unreal initialization;
    // until then loggers are delivered as if it exists.
    void set_tick_available(bool available) { tick_.store(available, std::memory_order_release); }

    // A logger for `module`, writing `module` target files under `folder`
    // (empty: unknown, so the target becomes ue4ss). Returns its id.
    int64_t register_logger(std::string module, std::string folder, Policy policy, int64_t now_ms)
    {
        if (policy.target == Target::module && folder.empty()) policy.target = Target::ue4ss;
        if (policy.interval_ms < NativeContract::min_interval_ms) policy.interval_ms = NativeContract::min_interval_ms;
        if (policy.interval_ms > NativeContract::max_interval_ms) policy.interval_ms = NativeContract::max_interval_ms;
        std::scoped_lock lock(mutex_);
        const auto id = next_logger_++;
        loggers_.emplace(id, Logger{std::move(module), std::move(folder), policy, now_ms});
        return id;
    }

    [[nodiscard]] Policy policy(int64_t logger) const
    {
        std::scoped_lock lock(mutex_);
        const auto found = loggers_.find(logger);
        return found == loggers_.end() ? Policy{} : effective(found->second.policy);
    }

    [[nodiscard]] std::string module(int64_t logger) const
    {
        std::scoped_lock lock(mutex_);
        const auto found = loggers_.find(logger);
        return found == loggers_.end() ? std::string() : found->second.module;
    }

    struct Written
    {
        bool ok{};
        bool wake_worker{}; // an immediate line: the worker should run now
    };

    // Copies one line into the journal. A full journal is first delivered
    // and reclaimed synchronously (a stall), so no line is ever dropped.
    Written write(int64_t logger, int rank, std::string_view category, std::string_view message, int64_t time,
        int64_t now_ms)
    {
        std::string clipped;
        if (message.size() > static_cast<std::size_t>(NativeContract::max_message_bytes))
        {
            clipped.assign(message.substr(0, static_cast<std::size_t>(NativeContract::max_message_bytes)));
            clipped.append(truncated_suffix);
            message = clipped;
        }
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            {
                std::scoped_lock lock(mutex_);
                const auto found = loggers_.find(logger);
                if (found == loggers_.end()) return {};
                const auto& owner = found->second;
                const auto policy = effective(owner.policy);
                JournalEntry entry{next_seq_, time, rank, static_cast<uint8_t>(owner.policy.target), owner.module,
                    owner.folder, category, message};
                const auto slot = journal_.append(entry);
                if (slot != 0)
                {
                    ++next_seq_;
                    ++written_;
                    pending_.push_back(Pending{slot, logger});
                    if (owner.policy.frequency == Frequency::frame) frame_lines_.fetch_add(1, std::memory_order_release);
                    return Written{true, policy.frequency == Frequency::immediate};
                }
                ++stalls_;
            }
            all(now_ms);
        }
        return {};
    }

    // Delivers what is due; `frame` says a frame has passed since the last
    // call that had it. Returns the lines delivered.
    std::size_t due(int64_t now_ms, bool frame = false)
    {
        std::scoped_lock order(delivery_);
        return deliver_locked(false, now_ms, frame);
    }

    // Delivers everything pending.
    std::size_t all(int64_t now_ms)
    {
        std::scoped_lock order(delivery_);
        return deliver_locked(true, now_ms, false);
    }

    // How long the worker may sleep: until its next periodic logger with
    // pending lines is due, at most `idle_ms`.
    [[nodiscard]] int64_t worker_wait_ms(int64_t now_ms, int64_t idle_ms) const
    {
        std::scoped_lock lock(mutex_);
        int64_t wait = idle_ms;
        for (const auto& pending : pending_)
        {
            const auto& logger = loggers_.at(pending.logger);
            const auto policy = effective(logger.policy);
            if (policy.frequency == Frequency::immediate) return 0;
            if (policy.frequency != Frequency::periodic) continue;
            const auto due = logger.last_delivery_ms + policy.interval_ms - now_ms;
            wait = std::min(wait, std::max<int64_t>(due, 0));
        }
        return wait;
    }

    // Whether a frame-frequency line waits for the next frame. Lock-free: the
    // engine tick asks every frame.
    [[nodiscard]] bool has_frame_lines() const
    {
        return tick_.load(std::memory_order_acquire) && frame_lines_.load(std::memory_order_acquire) > 0;
    }

    [[nodiscard]] CoreStats stats() const
    {
        std::scoped_lock lock(mutex_);
        return CoreStats{written_, delivered_, static_cast<int64_t>(pending_.size()), stalls_,
            static_cast<int64_t>(loggers_.size())};
    }

private:
    struct Logger
    {
        std::string module;
        std::string folder;
        Policy policy;
        int64_t last_delivery_ms{};
    };

    struct Pending
    {
        uint64_t slot;
        int64_t logger;
    };

    struct Line
    {
        uint64_t slot;
        Target target;
        RecoveredRecord record;
    };

    Journal& journal_;
    Sink& sink_;
    std::atomic<bool> tick_;
    mutable std::mutex mutex_;
    std::mutex delivery_;
    std::unordered_map<int64_t, Logger> loggers_;
    std::deque<Pending> pending_;
    int64_t next_logger_{1};
    uint64_t next_seq_{1};
    int64_t written_{};
    int64_t delivered_{};
    int64_t stalls_{};
    std::atomic<int64_t> frame_lines_{0}; // pending lines of frame-frequency loggers

    // Without the engine tick, frame becomes periodic at 16 ms.
    Policy effective(Policy policy) const
    {
        if (policy.frequency != Frequency::frame || tick_.load(std::memory_order_acquire)) return policy;
        policy.frequency = Frequency::periodic;
        policy.interval_ms = frame_fallback_ms;
        return policy;
    }

    bool is_due(const Logger& logger, int64_t now_ms, bool frame, std::unordered_map<int64_t, bool>& due,
        int64_t id) const
    {
        const auto policy = effective(logger.policy);
        if (policy.frequency == Frequency::immediate) return true;
        if (policy.frequency == Frequency::frame) return frame;
        const auto known = due.find(id);
        if (known != due.end()) return known->second;
        const bool now_due = now_ms - logger.last_delivery_ms >= policy.interval_ms;
        due.emplace(id, now_due);
        return now_due;
    }

    std::size_t deliver_locked(bool everything, int64_t now_ms, bool frame)
    {
        std::vector<Line> batch;
        {
            std::scoped_lock lock(mutex_);
            if (pending_.empty()) return 0;
            std::unordered_map<int64_t, bool> due;
            std::deque<Pending> kept;
            int64_t frame_lines = 0;
            for (const auto& pending : pending_)
            {
                auto& logger = loggers_.at(pending.logger);
                if (!everything && !is_due(logger, now_ms, frame, due, pending.logger))
                {
                    kept.push_back(pending);
                    continue;
                }
                journal_.mark(pending.slot, RecordState::delivering);
                batch.push_back(Line{pending.slot, logger.policy.target, journal_.read(pending.slot)});
                if (logger.policy.frequency == Frequency::frame) ++frame_lines;
            }
            for (const auto& [id, is_due] : due)
            {
                if (is_due) loggers_.at(id).last_delivery_ms = now_ms;
            }
            pending_.swap(kept);
            if (frame_lines != 0) frame_lines_.fetch_sub(frame_lines, std::memory_order_release);
        }
        if (batch.empty()) return 0;
        emit(batch);
        std::scoped_lock lock(mutex_);
        for (const auto& line : batch) journal_.mark(line.slot, RecordState::delivered);
        journal_.reclaim();
        delivered_ += static_cast<int64_t>(batch.size());
        return batch.size();
    }

    // One sink call per UE4SS line and one per file per batch, in order.
    void emit(const std::vector<Line>& batch)
    {
        std::vector<std::pair<std::pair<std::string, std::string>, std::string>> files;
        for (const auto& line : batch)
        {
            const auto& record = line.record;
            try
            {
                if (line.target == Target::ue4ss)
                {
                    sink_.ue4ss(ue4ss_line(record.module, record.category, record.rank, record.message));
                    continue;
                }
                const auto text = file_line(record.time, record.rank, record.message);
                auto key = std::make_pair(record.folder, record.category);
                auto found = std::find_if(files.begin(), files.end(), [&](const auto& item) { return item.first == key; });
                if (found == files.end()) files.emplace_back(std::move(key), text);
                else found->second.append(text);
            }
            catch (...)
            {
            }
        }
        for (const auto& [key, text] : files)
        {
            try
            {
                sink_.file(key.first, key.second, text);
            }
            catch (...)
            {
            }
        }
    }
};
} // namespace UE4SSLuaLogBridge::Core
