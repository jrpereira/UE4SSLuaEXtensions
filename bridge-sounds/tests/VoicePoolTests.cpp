// The voice pool against a fake backend: reuse, growth, stealing, stopping and
// how long sample memory stays referenced.
#include <SoundVoicePool.hpp>
#include <TestSupport.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <vector>

using namespace UE4SSLuaSoundBridge::Core;
using namespace std::chrono_literals;

namespace
{
struct FakeVoice
{
    Format format;
    bool playing{false};
    bool destroyed{false};
    int starts{0};
    int halts{0};
};

struct FakeBackend
{
    using Voice = FakeVoice*;
    std::vector<std::unique_ptr<FakeVoice>> made;
    int created{0};
    int destroyed{0};
    bool fail_create{false};
    bool fail_start{false};

    Voice create(const Format& format)
    {
        if (fail_create) return nullptr;
        made.push_back(std::make_unique<FakeVoice>());
        made.back()->format = format;
        ++created;
        return made.back().get();
    }
    void destroy(Voice voice)
    {
        voice->destroyed = true;
        voice->playing = false;
        ++destroyed;
    }
    bool start(Voice voice, const SoundData& data, const PlayParams&)
    {
        CHECK(!voice->destroyed);
        CHECK(voice->format == data.format());
        if (fail_start) return false;
        voice->playing = true;
        ++voice->starts;
        return true;
    }
    void halt(Voice voice)
    {
        voice->playing = false;
        ++voice->halts;
    }
    bool finished(Voice voice) { return !voice->playing; }

    // Ends every playing voice, as if their sounds ran out.
    void finish_all()
    {
        for (auto& voice : made) voice->playing = false;
    }
    int playing() const
    {
        int count = 0;
        for (auto& voice : made) count += voice->playing && !voice->destroyed ? 1 : 0;
        return count;
    }
};

std::shared_ptr<const SoundData> sound(uint32_t channels, uint32_t rate)
{
    auto data = std::make_shared<SoundData>();
    data->channels = channels;
    data->rate = rate;
    data->samples.assign(channels * 16, 0.0f);
    return data;
}

PlayRequest request(int64_t play, int64_t sound_id, std::shared_ptr<const SoundData> data, int64_t owner = 1)
{
    return PlayRequest{owner, play, sound_id, std::move(data), PlayParams{}};
}

using Pool = VoicePool<FakeBackend>;
const Pool::Clock::time_point t0{};

void reuse_and_growth()
{
    FakeBackend backend;
    Pool pool(4);
    auto cue = sound(1, 48000);
    CHECK(pool.play(backend, request(1, 10, cue), t0) == PlayResult::started);
    CHECK(pool.play(backend, request(2, 10, cue), t0) == PlayResult::started);
    CHECK(backend.created == 2 && pool.playing() == 2);
    backend.finish_all();
    CHECK(pool.play(backend, request(3, 10, cue), t0) == PlayResult::started);
    CHECK(backend.created == 2); // an idle voice of the same format is reused
    CHECK(pool.playing() == 1);
    CHECK(cue.use_count() == 2); // the caller and the one playing voice
}

void overlap_and_stealing()
{
    FakeBackend backend;
    Pool pool(8);
    auto cue = sound(1, 48000);
    for (int64_t play = 1; play <= 8; ++play)
    {
        CHECK(pool.play(backend, request(play, 10, cue), t0) == PlayResult::started);
    }
    CHECK(backend.playing() == 8 && pool.voices() == 8);
    // The ninth steals the oldest voice of its format; nothing fails.
    CHECK(pool.play(backend, request(9, 10, cue), t0) == PlayResult::stolen);
    CHECK(backend.created == 8 && backend.made[0]->halts == 1 && backend.made[0]->starts == 2);
    // Stopping play 1 now does nothing: its voice plays 9.
    pool.stop_play(backend, 1, 1, t0);
    CHECK(backend.made[0]->playing);
    pool.stop_play(backend, 1, 9, t0);
    CHECK(!backend.made[0]->playing && pool.playing() == 7);

    // Twenty plays a second for a second over a pool of eight: all start.
    int failed = 0;
    for (int64_t play = 100; play < 120; ++play)
    {
        if (pool.play(backend, request(play, 10, cue), t0) == PlayResult::failed) ++failed;
    }
    CHECK(failed == 0 && backend.playing() == 8);
}

void formats()
{
    FakeBackend backend;
    Pool pool(2);
    auto mono = sound(1, 48000);
    auto stereo = sound(2, 44100);
    CHECK(pool.play(backend, request(1, 10, mono), t0) == PlayResult::started);
    CHECK(pool.play(backend, request(2, 20, stereo), t0) == PlayResult::started);
    CHECK(backend.created == 2);
    backend.finish_all();
    auto other = sound(1, 22050);
    // Pool full, idle voices of other formats: one is recreated for the new format.
    CHECK(pool.play(backend, request(3, 30, other), t0) == PlayResult::started);
    CHECK(backend.destroyed == 1 && backend.created == 3 && pool.voices() == 2);
    // Pool full and busy with other formats: the oldest is stolen and recreated.
    CHECK(pool.play(backend, request(4, 20, stereo), t0) == PlayResult::started);
    CHECK(pool.play(backend, request(5, 10, mono), t0) == PlayResult::stolen);
    CHECK(pool.voices() == 2 && backend.playing() == 2);
}

void stopping_and_memory()
{
    FakeBackend backend;
    Pool pool(4);
    auto cue = sound(1, 48000);
    auto other = sound(1, 48000);
    pool.play(backend, request(1, 10, cue), t0);
    pool.play(backend, request(2, 10, cue), t0);
    pool.play(backend, request(3, 20, other), t0);
    CHECK(cue.use_count() == 3);
    // Another session can't stop this session's plays by id.
    pool.stop_play(backend, 2, 1, t0);
    CHECK(pool.playing() == 3);
    pool.stop_sound(backend, 10, t0);
    CHECK(pool.playing() == 1 && backend.playing() == 1);
    // Halted voices may still be read by the current audio pass: samples stay referenced.
    CHECK(cue.use_count() == 3 && pool.retired() == 2);
    pool.reap(backend, t0 + 100ms);
    CHECK(cue.use_count() == 3);
    pool.reap(backend, t0 + Pool::retire_after);
    CHECK(cue.use_count() == 1 && pool.retired() == 0);
    // A voice that finished on its own lets go at once.
    backend.finish_all();
    pool.reap(backend, t0 + 1s);
    CHECK(other.use_count() == 1 && pool.playing() == 0);
}

void preparing()
{
    FakeBackend backend;
    Pool pool(2);
    pool.prepare(backend, Format{1, 48000});
    pool.prepare(backend, Format{1, 48000});
    CHECK(backend.created == 1 && pool.voices() == 1 && pool.playing() == 0);
    auto cue = sound(1, 48000);
    CHECK(pool.play(backend, request(1, 10, cue), t0) == PlayResult::started);
    CHECK(backend.created == 1); // the prepared voice plays
    pool.prepare(backend, Format{2, 44100});
    pool.prepare(backend, Format{1, 22050}); // no room left
    CHECK(backend.created == 2 && pool.voices() == 2);
}

void failures_and_clear()
{
    FakeBackend backend;
    Pool pool(2);
    auto cue = sound(1, 48000);
    backend.fail_create = true;
    CHECK(pool.play(backend, request(1, 10, cue), t0) == PlayResult::failed);
    CHECK(pool.voices() == 0);
    backend.fail_create = false;
    backend.fail_start = true;
    CHECK(pool.play(backend, request(2, 10, cue), t0) == PlayResult::failed);
    CHECK(pool.playing() == 0 && pool.retired() == 1); // a half-started buffer keeps its samples
    backend.fail_start = false;
    CHECK(pool.play(backend, request(3, 10, cue), t0) == PlayResult::started);
    pool.clear(backend);
    CHECK(pool.voices() == 0 && pool.retired() == 0 && backend.destroyed == 1 && cue.use_count() == 1);

    // Recreating for another format fails: the dead slot is dropped.
    Pool small(1);
    FakeBackend second;
    small.play(second, request(1, 10, cue), t0);
    second.finish_all();
    second.fail_create = true;
    CHECK(small.play(second, request(2, 20, sound(2, 48000)), t0) == PlayResult::failed);
    CHECK(small.voices() == 0);
    Pool zero(0);
    CHECK(zero.capacity() == 1);
}
} // namespace

int main()
{
    reuse_and_growth();
    overlap_and_stealing();
    formats();
    stopping_and_memory();
    preparing();
    failures_and_clear();
    return LexTests::finish("SoundBridgeVoicePoolTests");
}
