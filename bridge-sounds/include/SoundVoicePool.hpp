#pragma once

// The voice pool the audio thread runs. It decides which voice a play gets and
// keeps sample memory alive while a voice may still read it; the backend does
// the audio work. Only the audio thread touches a pool.
//
// Backend requirements (Voice is a cheap handle, compared with ==):
//   Voice create(const Format&)    a stopped voice, or a null Voice on failure
//   void destroy(Voice)            synchronous: the voice reads nothing afterwards
//   bool start(Voice, const SoundData&, const PlayParams&)
//   void halt(Voice)               stop and flush; may finish on the next pass
//   bool finished(Voice)           nothing left queued
//
// A play gets, in order: an idle voice of its format; a new voice while fewer
// than `capacity` exist; an idle voice of another format, recreated; the
// oldest voice playing its format (stolen); the oldest voice of any format,
// recreated (stolen). Samples of a halted voice stay referenced for
// `retire_after`, well past any audio pass that could still read them.

#include <SoundData.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace UE4SSLuaSoundBridge::Core
{
struct PlayRequest
{
    int64_t owner{}; // session id: only its own plays can be stopped by id
    int64_t play{};
    int64_t sound{};
    std::shared_ptr<const SoundData> data;
    PlayParams params;
};

enum class PlayResult
{
    started,
    stolen,
    failed,
};

template<class Backend>
class VoicePool
{
public:
    using Voice = typename Backend::Voice;
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds retire_after{250};

    explicit VoicePool(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    PlayResult play(Backend& backend, const PlayRequest& request, Clock::time_point now)
    {
        reap(backend, now);
        const auto format = request.data->format();
        if (auto* slot = find_idle(&format)) return begin(backend, *slot, request, PlayResult::started, now);
        if (slots_.size() < capacity_)
        {
            Voice voice = backend.create(format);
            if (!voice) return PlayResult::failed;
            Slot slot;
            slot.voice = voice;
            slot.format = format;
            slots_.push_back(std::move(slot));
            return begin(backend, slots_.back(), request, PlayResult::started, now);
        }
        if (auto* slot = find_idle(nullptr)) return recreate(backend, *slot, request, PlayResult::started, now);
        if (auto* slot = oldest(&format))
        {
            backend.halt(slot->voice);
            retire(*slot, now);
            return begin(backend, *slot, request, PlayResult::stolen, now);
        }
        if (auto* slot = oldest(nullptr))
        {
            backend.halt(slot->voice);
            retire(*slot, now);
            return recreate(backend, *slot, request, PlayResult::stolen, now);
        }
        return PlayResult::failed;
    }

    // Creates an idle voice for `format` when none exists and there is room, so
    // the first play of a newly loaded sound doesn't wait for voice creation.
    void prepare(Backend& backend, const Format& format)
    {
        for (const auto& slot : slots_)
        {
            if (slot.format == format) return;
        }
        if (slots_.size() >= capacity_) return;
        Voice voice = backend.create(format);
        if (!voice) return;
        Slot slot;
        slot.voice = voice;
        slot.format = format;
        slots_.push_back(std::move(slot));
    }

    void stop_play(Backend& backend, int64_t owner, int64_t play, Clock::time_point now)
    {
        for (auto& slot : slots_)
        {
            if (slot.busy && slot.owner == owner && slot.play == play)
            {
                backend.halt(slot.voice);
                retire(slot, now);
            }
        }
    }

    void stop_sound(Backend& backend, int64_t sound, Clock::time_point now)
    {
        for (auto& slot : slots_)
        {
            if (slot.busy && slot.sound == sound)
            {
                backend.halt(slot.voice);
                retire(slot, now);
            }
        }
    }

    // Frees voices that finished and samples retired long enough ago.
    void reap(Backend& backend, Clock::time_point now)
    {
        for (auto& slot : slots_)
        {
            if (slot.busy && backend.finished(slot.voice))
            {
                slot.busy = false;
                slot.play = 0;
                slot.sound = 0;
                slot.data.reset();
            }
        }
        std::erase_if(retired_, [&](const Retired& item) { return now - item.at >= retire_after; });
    }

    // Destroys every voice (engine shutdown or device change).
    void clear(Backend& backend)
    {
        for (auto& slot : slots_)
        {
            if (slot.voice) backend.destroy(slot.voice);
        }
        slots_.clear();
        retired_.clear();
    }

    [[nodiscard]] std::size_t voices() const { return slots_.size(); }
    [[nodiscard]] std::size_t playing() const
    {
        std::size_t count = 0;
        for (const auto& slot : slots_) count += slot.busy ? 1 : 0;
        return count;
    }
    [[nodiscard]] std::size_t retired() const { return retired_.size(); }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

private:
    struct Slot
    {
        Voice voice{};
        Format format;
        bool busy{false};
        int64_t owner{};
        int64_t play{};
        int64_t sound{};
        uint64_t sequence{};
        std::shared_ptr<const SoundData> data;
    };

    struct Retired
    {
        std::shared_ptr<const SoundData> data;
        Clock::time_point at;
    };

    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::vector<Retired> retired_;
    uint64_t sequence_{0};

    Slot* find_idle(const Format* format)
    {
        for (auto& slot : slots_)
        {
            if (!slot.busy && (!format || slot.format == *format)) return &slot;
        }
        return nullptr;
    }

    Slot* oldest(const Format* format)
    {
        Slot* found = nullptr;
        for (auto& slot : slots_)
        {
            if (!slot.busy || (format && !(slot.format == *format))) continue;
            if (!found || slot.sequence < found->sequence) found = &slot;
        }
        return found;
    }

    void retire(Slot& slot, Clock::time_point now)
    {
        if (slot.data) retired_.push_back(Retired{std::move(slot.data), now});
        slot.data.reset();
        slot.busy = false;
        slot.play = 0;
        slot.sound = 0;
    }

    PlayResult recreate(Backend& backend, Slot& slot, const PlayRequest& request, PlayResult result,
        Clock::time_point now)
    {
        const auto format = request.data->format();
        backend.destroy(slot.voice);
        slot.voice = backend.create(format);
        slot.format = format;
        if (!slot.voice)
        {
            // Keep the slot list free of dead voices.
            std::erase_if(slots_, [](const Slot& s) { return !s.voice; });
            return PlayResult::failed;
        }
        return begin(backend, slot, request, result, now);
    }

    PlayResult begin(Backend& backend, Slot& slot, const PlayRequest& request, PlayResult result,
        Clock::time_point now)
    {
        if (!backend.start(slot.voice, *request.data, request.params))
        {
            // A buffer may have been queued before the failure: keep its samples.
            backend.halt(slot.voice);
            retired_.push_back(Retired{request.data, now});
            return PlayResult::failed;
        }
        slot.busy = true;
        slot.owner = request.owner;
        slot.play = request.play;
        slot.sound = request.sound;
        slot.sequence = ++sequence_;
        slot.data = request.data;
        return result;
    }
};
} // namespace UE4SSLuaSoundBridge::Core
