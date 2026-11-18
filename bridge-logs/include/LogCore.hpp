#pragma once

// Loggers, writing into the journal and delivering to targets. Portable: the
// Windows layer supplies the Sink (UE4SS.log and files), the clocks, the
// engine tick and the worker thread, and calls deliver() from each context.
//
// Delivery passes:
//   call    inside the Lua call that wrote: game-thread immediate loggers
//   tick    the engine-tick post-callback: game-thread loggers whose time came
//   worker  the worker thread: worker loggers whose time came
//   all     everything pending, synchronously (Flush, a full journal, stop)
// A logger is delivered by one kind of pass only (and by `all`), so its lines
// keep their order. Game passes and the worker hold separate delivery locks,
// so a slow disk on the worker never blocks the game thread; `all` takes both.
//
// Without the engine tick, `frame` becomes `periodic` at 16 ms and every
// non-immediate logger is delivered by the worker.

#include <Journal.hpp>
#include <LogBridgeNativeContract.hpp>
#include <LogFormat.hpp>
#include <LogLevels.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
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
using NativeContract::Thread;

struct Policy
{
    Target target{Target::ue4ss};
    Frequency frequency{Frequency::immediate};
    int64_t interval_ms{1000};
    Thread thread{Thread::game};
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

enum class Pass
{
    call,
    tick,
    worker,
    all,
};

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
        bool deliver_now{}; // the caller runs deliver(Pass::call)
        bool wake_worker{};
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
                    const bool immediate = policy.frequency == Frequency::immediate;
                    return Written{true, immediate && policy.thread == Thread::game,
                        immediate && policy.thread == Thread::worker};
                }
                ++stalls_;
            }
            deliver(Pass::all, now_ms);
        }
        return {};
    }

    // Delivers what `pass` owns and is due. `frame` tells the worker a frame
    // has passed since its last frame delivery. Returns the lines delivered.
    std::size_t deliver(Pass pass, int64_t now_ms, bool frame = false)
    {
        if (pass == Pass::all)
        {
            std::scoped_lock order(game_delivery_, worker_delivery_);
            return deliver_locked(pass, now_ms, frame);
        }
        std::scoped_lock order(pass == Pass::worker ? worker_delivery_ : game_delivery_);
        return deliver_locked(pass, now_ms, frame);
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
            if (policy.thread != Thread::worker) continue;
            if (policy.frequency == Frequency::immediate) return 0;
            if (policy.frequency != Frequency::periodic) continue;
            const auto due = logger.last_delivery_ms + policy.interval_ms - now_ms;
            wait = std::min(wait, std::max<int64_t>(due, 0));
        }
        return wait;
    }

    [[nodiscard]] bool has_worker_frame_loggers() const
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [_, logger] : loggers_)
        {
            const auto policy = effective(logger.policy);
            if (policy.thread == Thread::worker && policy.frequency == Frequency::frame) return true;
        }
        return false;
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
    std::mutex game_delivery_;
    std::mutex worker_delivery_;
    std::unordered_map<int64_t, Logger> loggers_;
    std::deque<Pending> pending_;
    int64_t next_logger_{1};
    uint64_t next_seq_{1};
    int64_t written_{};
    int64_t delivered_{};
    int64_t stalls_{};

    // Without the engine tick, frame becomes periodic at 16 ms and every
    // non-immediate logger moves to the worker.
    Policy effective(Policy policy) const
    {
        if (tick_.load(std::memory_order_acquire) || policy.frequency == Frequency::immediate) return policy;
        if (policy.frequency == Frequency::frame) policy.interval_ms = frame_fallback_ms;
        policy.frequency = Frequency::periodic;
        policy.thread = Thread::worker;
        return policy;
    }

    bool owns(Pass pass, const Logger& logger, int64_t now_ms, bool frame, std::unordered_map<int64_t, bool>& due,
        int64_t id) const
    {
        const auto policy = effective(logger.policy);
        switch (pass)
        {
        case Pass::all: return true;
        case Pass::call: return policy.thread == Thread::game && policy.frequency == Frequency::immediate;
        case Pass::tick:
        case Pass::worker:
        {
            const auto thread = pass == Pass::tick ? Thread::game : Thread::worker;
            if (policy.thread != thread) return false;
            if (policy.frequency == Frequency::immediate) return true;
            if (policy.frequency == Frequency::frame) return pass == Pass::tick || frame;
            const auto known = due.find(id);
            if (known != due.end()) return known->second;
            const bool now_due = now_ms - logger.last_delivery_ms >= policy.interval_ms;
            due.emplace(id, now_due);
            return now_due;
        }
        }
        return false;
    }

    std::size_t deliver_locked(Pass pass, int64_t now_ms, bool frame)
    {
        std::vector<Line> batch;
        {
            std::scoped_lock lock(mutex_);
            std::unordered_map<int64_t, bool> due;
            std::deque<Pending> kept;
            for (const auto& pending : pending_)
            {
                auto& logger = loggers_.at(pending.logger);
                if (!owns(pass, logger, now_ms, frame, due, pending.logger))
                {
                    kept.push_back(pending);
                    continue;
                }
                journal_.mark(pending.slot, RecordState::delivering);
                batch.push_back(Line{pending.slot, logger.policy.target, journal_.read(pending.slot)});
            }
            for (const auto& [id, is_due] : due)
            {
                if (is_due) loggers_.at(id).last_delivery_ms = now_ms;
            }
            pending_.swap(kept);
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
