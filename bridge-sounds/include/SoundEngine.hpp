#pragma once

// The audio engine: one thread that owns XAudio2, the output device and the
// voice pool. Callers only queue commands, so no call here waits on audio
// work. No <windows.h> in this header (src/XAudio2Engine.cpp has it).
//
// Device handling: the engine opens the default output device and follows
// default-device changes (IMMNotificationClient) and engine failures
// (IXAudio2EngineCallback::OnCriticalError) by rebuilding on the new default
// device. With no device, it retries every second and drops plays meanwhile.

#include <SoundVoicePool.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace UE4SSLuaSoundBridge
{
class Engine
{
public:
    using Report = std::function<void(std::string_view)>;

    struct Stats
    {
        int64_t playing{};
        int64_t started{};
        int64_t dropped{};
        int64_t stolen{};
        int64_t device_changes{};
        std::string device; // "ready", "none" or "stopped" (not started yet)
    };

    explicit Engine(Report report);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the audio thread once; later calls do nothing.
    void start();
    // Readies a voice for sounds of this format (after a Load).
    void prepare(Core::Format format);
    void play(Core::PlayRequest request);
    void stop_play(int64_t owner, int64_t play);
    void stop_sound(int64_t sound);
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace UE4SSLuaSoundBridge
