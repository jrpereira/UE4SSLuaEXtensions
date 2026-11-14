#pragma once

// Per-play mixing values: clamping, the output matrix for a pan, and the
// low-pass filter setting. Pure functions; the XAudio2 backend applies them.

#include <SoundData.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace UE4SSLuaSoundBridge::Core
{
inline float from_micro(int64_t value, int64_t low, int64_t high)
{
    return static_cast<float>(static_cast<double>(std::clamp(value, low, high)) / NativeContract::micro);
}

// The native Play arguments (millionths and hertz) as clamped values.
inline PlayParams play_params(int64_t pan, int64_t volume, int64_t pitch, int64_t lowpass_hz)
{
    using namespace NativeContract;
    PlayParams params;
    params.pan = from_micro(pan, min_pan_micro, max_pan_micro);
    params.volume = from_micro(volume, min_volume_micro, max_volume_micro);
    params.pitch = from_micro(pitch, min_pitch_micro, max_pitch_micro);
    params.lowpass_hz = lowpass_hz <= 0 ? 0.0f : static_cast<float>(std::clamp<int64_t>(lowpass_hz, min_lowpass_hz, 1000000));
    return params;
}

inline constexpr uint32_t speaker_front_left = 0x1;
inline constexpr uint32_t speaker_front_right = 0x2;

// Index of a speaker among the output's channels, from its channel mask.
inline int channel_index(uint32_t mask, uint32_t speaker)
{
    if ((mask & speaker) == 0) return -1;
    int index = 0;
    for (uint32_t bit = 1; bit < speaker; bit <<= 1)
    {
        if (mask & bit) ++index;
    }
    return index;
}

// XAudio2 output matrix: the level from source channel s to output channel d
// is at [s + d * source]. Mono sources pan with a constant-power law (full
// left: 1 left, 0 right; centre: 0.707 each), so loudness holds across the
// field. Stereo sources pan as balance (centre: both channels as recorded;
// the far side fades linearly). Only the front pair is fed; a mono output
// receives the plain mix.
inline std::vector<float> output_matrix(uint32_t source, uint32_t outputs, uint32_t mask, float pan)
{
    std::vector<float> matrix(static_cast<std::size_t>(source) * outputs, 0.0f);
    if (source == 0 || outputs == 0) return matrix;
    if (outputs == 1)
    {
        for (uint32_t s = 0; s < source; ++s) matrix[s] = 1.0f / static_cast<float>(source);
        return matrix;
    }
    int left = channel_index(mask, speaker_front_left);
    int right = channel_index(mask, speaker_front_right);
    if (left < 0 || right < 0 || static_cast<uint32_t>(std::max(left, right)) >= outputs)
    {
        left = 0;
        right = 1;
    }
    pan = std::clamp(pan, -1.0f, 1.0f);
    const auto at = [&](uint32_t s, int d) -> float& { return matrix[s + static_cast<std::size_t>(d) * source]; };
    if (source == 1)
    {
        const double angle = (static_cast<double>(pan) + 1.0) * 3.14159265358979323846 / 4.0;
        at(0, left) = static_cast<float>(std::cos(angle));
        at(0, right) = static_cast<float>(std::sin(angle));
        return matrix;
    }
    at(0, left) = pan > 0.0f ? 1.0f - pan : 1.0f;
    at(1, right) = pan < 0.0f ? 1.0f + pan : 1.0f;
    return matrix;
}

// XAudio2 state-variable filter values. {LowPass, 1.0, 1.0} is documented as
// acoustically a bypass, so no filter and a cutoff at or above the filter's
// top (a sixth of the output rate) both use it.
struct FilterSetting
{
    bool bypass{true};
    float frequency{1.0f};
    float one_over_q{1.0f};
};

inline FilterSetting lowpass_filter(float cutoff_hz, uint32_t output_rate)
{
    FilterSetting setting;
    if (cutoff_hz <= 0.0f || output_rate == 0) return setting;
    const double ratio = static_cast<double>(cutoff_hz) / static_cast<double>(output_rate);
    if (ratio >= 1.0 / 6.0) return setting;
    setting.bypass = false;
    setting.frequency = static_cast<float>(2.0 * std::sin(3.14159265358979323846 * ratio));
    return setting;
}
} // namespace UE4SSLuaSoundBridge::Core
