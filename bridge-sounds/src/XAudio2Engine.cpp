// UE4SSLuaSoundBridge: the XAudio2 engine. One audio thread owns XAudio2, the
// mastering voice and the voice pool; other threads only queue commands.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mmdeviceapi.h>
#include <xaudio2.h>

#include <SoundEngine.hpp>
#include <SoundMix.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace UE4SSLuaSoundBridge
{
namespace
{
using Clock = std::chrono::steady_clock;

std::string hex(HRESULT result)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(result));
    return text;
}

// Calls `changed` when the default render device for games changes (also to
// none). Runs on an MMDevice thread: it only raises a flag.
class DeviceWatcher final : public IMMNotificationClient
{
public:
    explicit DeviceWatcher(std::function<void()> changed) : changed_(std::move(changed)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient))
        {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole) changed_();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
    std::function<void()> changed_;
};

// XAudio2 reports a lost device or a failed engine here; the engine must then
// be rebuilt. Runs on XAudio2's thread: it only raises a flag.
class EngineWatcher final : public IXAudio2EngineCallback
{
public:
    explicit EngineWatcher(std::function<void(HRESULT)> failed) : failed_(std::move(failed)) {}
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override { failed_(error); }

private:
    std::function<void(HRESULT)> failed_;
};

// The voice pool's backend (SoundVoicePool.hpp).
struct Backend
{
    using Voice = IXAudio2SourceVoice*;

    IXAudio2* xaudio{};
    IXAudio2MasteringVoice* master{};
    uint32_t output_channels{};
    uint32_t output_rate{};
    DWORD channel_mask{};

    Voice create(const Core::Format& format)
    {
        WAVEFORMATEX wave{};
        wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wave.nChannels = static_cast<WORD>(format.channels);
        wave.nSamplesPerSec = format.rate;
        wave.wBitsPerSample = 32;
        wave.nBlockAlign = static_cast<WORD>(format.channels * sizeof(float));
        wave.nAvgBytesPerSec = format.rate * wave.nBlockAlign;
        IXAudio2SourceVoice* voice = nullptr;
        const float max_ratio =
            static_cast<float>(NativeContract::max_pitch_micro) / static_cast<float>(NativeContract::micro);
        if (FAILED(xaudio->CreateSourceVoice(&voice, &wave, XAUDIO2_VOICE_USEFILTER, max_ratio))) return nullptr;
        return voice;
    }

    void destroy(Voice voice) { voice->DestroyVoice(); }

    bool start(Voice voice, const Core::SoundData& data, const Core::PlayParams& params)
    {
        const auto matrix = Core::output_matrix(data.channels, output_channels, channel_mask, params.pan);
        if (FAILED(voice->SetOutputMatrix(nullptr, data.channels, output_channels, matrix.data()))) return false;
        if (FAILED(voice->SetFrequencyRatio(params.pitch))) return false;
        if (FAILED(voice->SetVolume(params.volume))) return false;
        const auto filter = Core::lowpass_filter(params.lowpass_hz, output_rate);
        XAUDIO2_FILTER_PARAMETERS parameters{LowPassFilter, filter.frequency, filter.one_over_q};
        if (FAILED(voice->SetFilterParameters(&parameters))) return false;
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(data.bytes());
        buffer.pAudioData = reinterpret_cast<const BYTE*>(data.samples.data());
        if (FAILED(voice->SubmitSourceBuffer(&buffer))) return false;
        return SUCCEEDED(voice->Start(0));
    }

    void halt(Voice voice)
    {
        voice->Stop(0);
        voice->FlushSourceBuffers();
    }

    bool finished(Voice voice)
    {
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued == 0;
    }
};

struct Command
{
    enum class Kind
    {
        play,
        prepare,
        stop_play,
        stop_sound,
    };
    Kind kind{Kind::play};
    Core::PlayRequest request;
    int64_t owner{};
    int64_t id{};
    Core::Format format;
};

enum DeviceState : int
{
    device_stopped = 0,
    device_none = 1,
    device_ready = 2,
};
} // namespace

struct Engine::Impl
{
    Report report;

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Command> queue;
    bool quit{false};
    bool device_changed{false};
    bool engine_failed{false};
    HRESULT failure{S_OK};
    bool started{false};
    std::thread thread;

    std::atomic<int64_t> playing{0};
    std::atomic<int64_t> plays_started{0};
    std::atomic<int64_t> dropped{0};
    std::atomic<int64_t> stolen{0};
    std::atomic<int64_t> device_changes{0};
    std::atomic<int> device{device_stopped};

    // Audio thread only.
    Backend backend;
    Core::VoicePool<Backend> pool{static_cast<std::size_t>(NativeContract::max_voices)};
    EngineWatcher engine_watcher{[this](HRESULT error) { raise_failure(error); }};
    bool announced_missing{false};

    explicit Impl(Report r) : report(std::move(r)) {}

    void post(Command command)
    {
        {
            std::scoped_lock lock(mutex);
            queue.push_back(std::move(command));
        }
        wake.notify_one();
    }

    void raise_device_change()
    {
        {
            std::scoped_lock lock(mutex);
            device_changed = true;
        }
        wake.notify_one();
    }

    void raise_failure(HRESULT error)
    {
        {
            std::scoped_lock lock(mutex);
            engine_failed = true;
            failure = error;
        }
        wake.notify_one();
    }

    bool open()
    {
        IXAudio2* xaudio = nullptr;
        HRESULT result = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result))
        {
            report("XAudio2 is unavailable (" + hex(result) + "); sounds are dropped");
            return false;
        }
        IXAudio2MasteringVoice* master = nullptr;
        result = xaudio->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
            nullptr, AudioCategory_GameEffects);
        if (FAILED(result))
        {
            xaudio->Release();
            if (!announced_missing)
            {
                report("no audio output device (" + hex(result) + "); plays are dropped until one is available");
                announced_missing = true;
            }
            return false;
        }
        xaudio->RegisterForCallbacks(&engine_watcher);
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        DWORD mask = 0;
        master->GetChannelMask(&mask);
        backend = Backend{xaudio, master, details.InputChannels, details.InputSampleRate, mask};
        announced_missing = false;
        device.store(device_ready);
        report("audio output ready: " + std::to_string(details.InputChannels) + " channel(s) at " +
            std::to_string(details.InputSampleRate) + " Hz");
        return true;
    }

    void close()
    {
        pool.clear(backend);
        if (backend.master) backend.master->DestroyVoice();
        if (backend.xaudio)
        {
            backend.xaudio->UnregisterForCallbacks(&engine_watcher);
            backend.xaudio->Release();
        }
        backend = Backend{};
        playing.store(0);
        device.store(device_none);
    }

    void apply(Command& command, Clock::time_point now)
    {
        const bool ready = backend.xaudio != nullptr;
        switch (command.kind)
        {
        case Command::Kind::play:
        {
            if (!ready)
            {
                dropped.fetch_add(1);
                return;
            }
            const auto result = pool.play(backend, command.request, now);
            if (result == Core::PlayResult::failed)
            {
                dropped.fetch_add(1);
                return;
            }
            plays_started.fetch_add(1);
            if (result == Core::PlayResult::stolen) stolen.fetch_add(1);
            return;
        }
        case Command::Kind::prepare:
            if (ready) pool.prepare(backend, command.format);
            return;
        case Command::Kind::stop_play:
            if (ready) pool.stop_play(backend, command.owner, command.id, now);
            return;
        case Command::Kind::stop_sound:
            if (ready) pool.stop_sound(backend, command.id, now);
            return;
        }
    }

    void run()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        DeviceWatcher* watcher = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator))))
        {
            watcher = new DeviceWatcher([this] { raise_device_change(); });
            if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher)))
            {
                report("device change notifications are unavailable; a new default device is used after an engine error");
                watcher->Release();
                watcher = nullptr;
            }
        }

        device.store(device_none);
        open();
        auto retry_at = Clock::now() + std::chrono::seconds(1);
        std::vector<Command> work;
        for (;;)
        {
            bool rebuild = false;
            HRESULT error = S_OK;
            {
                std::unique_lock lock(mutex);
                const auto wait = backend.xaudio ? std::chrono::milliseconds(100) : std::chrono::milliseconds(1000);
                wake.wait_for(lock, wait, [this] { return quit || !queue.empty() || device_changed || engine_failed; });
                if (quit) break;
                work.swap(queue);
                rebuild = device_changed || engine_failed;
                if (engine_failed) error = failure;
                device_changed = false;
                engine_failed = false;
            }
            const auto now = Clock::now();
            if (rebuild)
            {
                if (error != S_OK) report("audio engine error " + hex(error) + "; restarting on the default device");
                else report("default audio device changed; restarting on it");
                close();
                device_changes.fetch_add(1);
                open();
                retry_at = now + std::chrono::seconds(1);
            }
            else if (!backend.xaudio && now >= retry_at)
            {
                open();
                retry_at = now + std::chrono::seconds(1);
            }
            for (auto& command : work) apply(command, now);
            work.clear();
            if (backend.xaudio) pool.reap(backend, Clock::now());
            playing.store(static_cast<int64_t>(pool.playing()));
        }

        close();
        device.store(device_stopped);
        if (enumerator)
        {
            if (watcher)
            {
                enumerator->UnregisterEndpointNotificationCallback(watcher);
                watcher->Release();
            }
            enumerator->Release();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

Engine::Engine(Report report) : impl_(std::make_unique<Impl>(std::move(report))) {}

Engine::~Engine()
{
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake.notify_one();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void Engine::start()
{
    std::scoped_lock lock(impl_->mutex);
    if (impl_->started) return;
    impl_->started = true;
    impl_->thread = std::thread([impl = impl_.get()] {
        try
        {
            impl->run();
        }
        catch (const std::exception& error)
        {
            impl->report(std::string("the audio thread stopped: ") + error.what());
            impl->device.store(device_stopped);
        }
        catch (...)
        {
            impl->report("the audio thread stopped: unknown exception");
            impl->device.store(device_stopped);
        }
    });
}

void Engine::play(Core::PlayRequest request)
{
    Command command;
    command.kind = Command::Kind::play;
    command.request = std::move(request);
    impl_->post(std::move(command));
}

void Engine::prepare(Core::Format format)
{
    Command command;
    command.kind = Command::Kind::prepare;
    command.format = format;
    impl_->post(std::move(command));
}

void Engine::stop_play(int64_t owner, int64_t play)
{
    Command command;
    command.kind = Command::Kind::stop_play;
    command.owner = owner;
    command.id = play;
    impl_->post(std::move(command));
}

void Engine::stop_sound(int64_t sound)
{
    Command command;
    command.kind = Command::Kind::stop_sound;
    command.id = sound;
    impl_->post(std::move(command));
}

Engine::Stats Engine::stats() const
{
    Stats stats;
    stats.playing = impl_->playing.load();
    stats.started = impl_->plays_started.load();
    stats.dropped = impl_->dropped.load();
    stats.stolen = impl_->stolen.load();
    stats.device_changes = impl_->device_changes.load();
    switch (impl_->device.load())
    {
    case device_ready: stats.device = "ready"; break;
    case device_none: stats.device = "none"; break;
    default: stats.device = "stopped"; break;
    }
    return stats;
}
} // namespace UE4SSLuaSoundBridge
