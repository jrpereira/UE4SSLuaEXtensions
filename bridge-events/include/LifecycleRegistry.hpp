#pragma once

#include <atomic>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace UE4SSLuaEventBridge
{
struct ObjectLifetimeIdentity
{
    std::uintptr_t address{};
    int32_t index{-1};
    int32_t serial{};

    auto operator<=>(const ObjectLifetimeIdentity&) const = default;
};

struct ObjectLifetimeIdentityHash
{
    std::size_t operator()(const ObjectLifetimeIdentity& identity) const noexcept
    {
        auto hash = std::hash<std::uintptr_t>{}(identity.address);
        hash ^= std::hash<int64_t>{}((static_cast<int64_t>(identity.index) << 32) ^
            static_cast<uint32_t>(identity.serial)) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        return hash;
    }
};

// Object-array listeners call invalidate for every UObject created or deleted,
// so it must stay cheap: an atomic fast path when nothing is observed, and
// indexed lookups otherwise.
class LifecycleRegistry
{
public:
    enum class CaptureFailure { none, unknown_session, capacity, faulted };
    struct CaptureResult { uint64_t token{}; CaptureFailure failure{CaptureFailure::none}; };
    struct LostResult { std::optional<uint64_t> token; bool overflow{}; bool unknown_session{}; };

    explicit LifecycleRegistry(std::size_t observation_limit = 4096, std::size_t loss_limit = 4096)
        : observation_limit_(observation_limit), loss_limit_(loss_limit) {}

    void start_session(uint64_t session)
    {
        std::scoped_lock lock(mutex_);
        sessions_.try_emplace(session);
    }

    void stop_session(uint64_t session)
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return;
        for (const auto& [token, record] : found->second.observations) unwatch(session, token, record.identity);
        sessions_.erase(found);
        publish_count();
    }

    // Losses are queued only for sessions that asked for them, so a session
    // that never drains its queue cannot fault its own validations.
    bool report_losses(uint64_t session)
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return false;
        found->second.report_losses = true;
        return true;
    }

    CaptureResult capture(uint64_t session, ObjectLifetimeIdentity identity)
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return {0, CaptureFailure::unknown_session};
        auto& state = found->second;
        if (state.overflow) return {0, CaptureFailure::faulted};
        const auto existing = state.tokens.find(identity);
        if (existing != state.tokens.end())
        {
            ++state.observations.at(existing->second).references;
            return {existing->second, CaptureFailure::none};
        }
        if (state.observations.size() >= observation_limit_)
            return {0, CaptureFailure::capacity};
        const auto token = next_token_++;
        state.observations.emplace(token, Observation{identity, 1});
        state.tokens.emplace(identity, token);
        by_index_[identity.index].push_back({session, token});
        by_address_[identity.address].push_back({session, token});
        publish_count();
        return {token, CaptureFailure::none};
    }

    bool valid(uint64_t session, uint64_t token, ObjectLifetimeIdentity identity) const
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end() || found->second.overflow) return false;
        const auto record = found->second.observations.find(token);
        return record != found->second.observations.end() && record->second.identity == identity;
    }

    // Drops one capture of a live observation; the last one ends it without a
    // reported loss. Returns false for unknown or already-ended tokens.
    bool release(uint64_t session, uint64_t token)
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return false;
        auto& state = found->second;
        const auto record = state.observations.find(token);
        if (record == state.observations.end()) return false;
        if (--record->second.references > 0) return true;
        unwatch(session, token, record->second.identity);
        state.tokens.erase(record->second.identity);
        state.observations.erase(record);
        publish_count();
        return true;
    }

    void invalidate(std::uintptr_t address, int32_t index)
    {
        if (observed_.load(std::memory_order_acquire) == 0) return;
        std::scoped_lock lock(mutex_);
        std::vector<Watch> matched;
        collect(by_index_, index, matched);
        collect(by_address_, address, matched);
        if (matched.empty()) return; // The common case: nothing observed this object.
        bool removed{};
        for (const auto& [session, token] : matched)
        {
            const auto found = sessions_.find(session);
            if (found == sessions_.end()) continue;
            auto& state = found->second;
            const auto record = state.observations.find(token);
            if (record == state.observations.end()) continue; // Matched by both keys.
            const auto identity = record->second.identity;
            unwatch(session, token, identity);
            state.tokens.erase(identity);
            state.observations.erase(record);
            removed = true;
            if (state.report_losses && !state.overflow)
            {
                if (state.lost.size() < loss_limit_) state.lost.push_back(token);
                else
                {
                    state.lost.clear();
                    state.overflow = true;
                }
            }
        }
        if (removed) publish_count();
    }

    LostResult take_lost(uint64_t session)
    {
        std::scoped_lock lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return {{}, false, true};
        auto& state = found->second;
        state.report_losses = true;
        if (state.overflow) return {{}, true, false};
        if (state.lost.empty()) return {};
        const auto token = state.lost.front();
        state.lost.pop_front();
        return {token, false, false};
    }

    std::size_t observed() const { return observed_.load(std::memory_order_acquire); }

    void shutdown()
    {
        std::scoped_lock lock(mutex_);
        sessions_.clear();
        by_index_.clear();
        by_address_.clear();
        publish_count();
    }

private:
    struct Watch { uint64_t session{}; uint64_t token{}; };
    struct Observation { ObjectLifetimeIdentity identity; std::size_t references{}; };
    struct SessionState
    {
        std::unordered_map<uint64_t, Observation> observations;
        std::unordered_map<ObjectLifetimeIdentity, uint64_t, ObjectLifetimeIdentityHash> tokens;
        std::deque<uint64_t> lost;
        bool report_losses{};
        bool overflow{};
    };

    template <typename Key>
    static void collect(const std::unordered_map<Key, std::vector<Watch>>& watches, Key key,
        std::vector<Watch>& matched)
    {
        const auto found = watches.find(key);
        if (found != watches.end()) matched.insert(matched.end(), found->second.begin(), found->second.end());
    }

    template <typename Key>
    static void erase_watch(std::unordered_map<Key, std::vector<Watch>>& watches, Key key,
        uint64_t session, uint64_t token)
    {
        const auto found = watches.find(key);
        if (found == watches.end()) return;
        auto& list = found->second;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if (it->session == session && it->token == token)
            {
                list.erase(it);
                break;
            }
        }
        if (list.empty()) watches.erase(found);
    }

    void unwatch(uint64_t session, uint64_t token, const ObjectLifetimeIdentity& identity)
    {
        erase_watch(by_index_, identity.index, session, token);
        erase_watch(by_address_, identity.address, session, token);
    }

    void publish_count()
    {
        std::size_t count{};
        for (const auto& [_, state] : sessions_) count += state.observations.size();
        observed_.store(count, std::memory_order_release);
    }

    const std::size_t observation_limit_;
    const std::size_t loss_limit_;
    mutable std::mutex mutex_;
    uint64_t next_token_{1};
    std::unordered_map<uint64_t, SessionState> sessions_;
    std::unordered_map<int32_t, std::vector<Watch>> by_index_;
    std::unordered_map<std::uintptr_t, std::vector<Watch>> by_address_;
    std::atomic<std::size_t> observed_{};
};
}
