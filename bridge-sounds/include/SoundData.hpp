#pragma once

// Decoded sound data and the result type the portable core shares. A loaded
// sound is immutable and shared: the session that loaded it holds one
// reference, every voice playing it holds another, so releasing a sound never
// pulls samples from under the audio thread.

#include <SoundBridgeNativeContract.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace UE4SSLuaSoundBridge::Core
{
using NativeContract::ErrorCode;

struct Failure
{
    ErrorCode code{ErrorCode::io};
    std::string message;
};

template<class T>
class Outcome
{
public:
    Outcome(T value) : state_(std::move(value)) {}
    Outcome(Failure failure) : state_(std::move(failure)) {}
    explicit operator bool() const { return std::holds_alternative<T>(state_); }
    T& value() { return std::get<T>(state_); }
    const T& value() const { return std::get<T>(state_); }
    const Failure& failure() const { return std::get<Failure>(state_); }

private:
    std::variant<T, Failure> state_;
};

// A source voice serves one format; voices are pooled by it.
struct Format
{
    uint32_t channels{};
    uint32_t rate{};
    friend bool operator==(const Format&, const Format&) = default;
};

// Interleaved 32-bit float frames at the file's own rate.
struct SoundData
{
    std::vector<float> samples;
    uint32_t channels{};
    uint32_t rate{};

    [[nodiscard]] uint64_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    [[nodiscard]] std::size_t bytes() const { return samples.size() * sizeof(float); }
    [[nodiscard]] Format format() const { return Format{channels, rate}; }
};

// Per-play values after clamping.
struct PlayParams
{
    float pan{0.0f};      // -1 full left .. 1 full right
    float volume{1.0f};   // gain; 1 = as recorded
    float pitch{1.0f};    // playback rate
    float lowpass_hz{0.0f}; // 0 = no filter
};
} // namespace UE4SSLuaSoundBridge::Core
