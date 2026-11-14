#pragma once

// WAV decoding: RIFF/WAVE with integer PCM (8, 16, 24 or 32 bit), IEEE float
// (32 or 64 bit) or WAVE_FORMAT_EXTENSIBLE carrying either, mono or stereo,
// 1 to 200 kHz. Everything becomes interleaved 32-bit float at the file's own
// rate. A data chunk that claims more bytes than the file holds is read up to
// the last whole frame, as players do.

#include <SoundData.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace UE4SSLuaSoundBridge::Core
{
namespace Wav
{
inline constexpr uint16_t format_pcm = 1;
inline constexpr uint16_t format_float = 3;
inline constexpr uint16_t format_extensible = 0xFFFE;
// The fixed tail of the KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT GUIDs, after
// their first two bytes (the format tag).
inline constexpr unsigned char subtype_tail[14] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

inline uint32_t u32(std::string_view bytes, std::size_t at)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
        static_cast<uint32_t>(p[3]) << 24;
}

inline uint16_t u16(std::string_view bytes, std::size_t at)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

inline float finite(double value)
{
    return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

inline float sample(const unsigned char* p, uint16_t tag, uint16_t bits)
{
    if (tag == format_float)
    {
        if (bits == 32)
        {
            float value;
            std::memcpy(&value, p, sizeof value);
            return finite(value);
        }
        double value;
        std::memcpy(&value, p, sizeof value);
        return finite(value);
    }
    switch (bits)
    {
    case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16: return static_cast<int16_t>(static_cast<uint16_t>(p[0] | p[1] << 8)) / 32768.0f;
    case 24:
    {
        int32_t value = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
            static_cast<uint32_t>(p[2]) << 24);
        return static_cast<float>(value >> 8) / 8388608.0f;
    }
    default:
    {
        const auto value = static_cast<int32_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
            static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
        return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    }
}
} // namespace Wav

// `max_ms` < 0: no length limit. A longer sound is `too_long`.
inline Outcome<SoundData> decode_wav(std::string_view bytes, int64_t max_ms = -1)
{
    using namespace Wav;
    const auto bad = [](ErrorCode code, std::string message) { return Failure{code, std::move(message)}; };
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE")
    {
        return bad(ErrorCode::unsupported, "not a WAV file (no RIFF/WAVE header)");
    }
    bool have_format = false;
    uint16_t tag = 0, channels = 0, block = 0, bits = 0;
    uint32_t rate = 0;
    std::string_view data;
    bool have_data = false;
    std::size_t at = 12;
    while (at + 8 <= bytes.size())
    {
        const auto id = bytes.substr(at, 4);
        const uint64_t size = u32(bytes, at + 4);
        const std::size_t body = at + 8;
        const std::size_t available = bytes.size() - body;
        if (id == "fmt ")
        {
            if (size < 16 || size > available) return bad(ErrorCode::unsupported, "malformed fmt chunk");
            tag = u16(bytes, body);
            channels = u16(bytes, body + 2);
            rate = u32(bytes, body + 4);
            block = u16(bytes, body + 12);
            bits = u16(bytes, body + 14);
            if (tag == format_extensible)
            {
                if (size < 40 || u16(bytes, body + 16) < 22) return bad(ErrorCode::unsupported, "malformed extensible fmt chunk");
                const auto* guid = reinterpret_cast<const unsigned char*>(bytes.data() + body + 24);
                if (std::memcmp(guid + 2, subtype_tail, sizeof subtype_tail) != 0 || guid[1] != 0)
                {
                    return bad(ErrorCode::unsupported, "unsupported extensible subformat");
                }
                tag = guid[0];
            }
            have_format = true;
        }
        else if (id == "data")
        {
            // Streaming writers may leave the size unset; read what is there.
            data = bytes.substr(body, static_cast<std::size_t>(std::min<uint64_t>(size, available)));
            have_data = true;
            break;
        }
        if (size > available) break;
        at = body + static_cast<std::size_t>(size) + (size & 1);
    }
    if (!have_format) return bad(ErrorCode::unsupported, "no fmt chunk");
    if (!have_data) return bad(ErrorCode::unsupported, "no data chunk");
    if (tag != format_pcm && tag != format_float)
    {
        return bad(ErrorCode::unsupported, "format tag " + std::to_string(tag) + " (compressed audio) isn't supported; use PCM or float WAV");
    }
    const bool bits_ok = tag == format_pcm ? (bits == 8 || bits == 16 || bits == 24 || bits == 32) : (bits == 32 || bits == 64);
    if (!bits_ok) return bad(ErrorCode::unsupported, std::to_string(bits) + "-bit samples aren't supported in this format");
    if (channels < 1 || channels > NativeContract::max_channels)
    {
        return bad(ErrorCode::unsupported, std::to_string(channels) + " channels; only mono and stereo are supported");
    }
    if (rate < NativeContract::min_sample_rate || rate > NativeContract::max_sample_rate)
    {
        return bad(ErrorCode::unsupported, "sample rate " + std::to_string(rate) + " Hz is outside 1000 to 200000 Hz");
    }
    if (block != channels * (bits / 8)) return bad(ErrorCode::unsupported, "block alignment doesn't match channels and bits");
    const uint64_t frames = data.size() / block;
    if (frames == 0) return bad(ErrorCode::unsupported, "the WAV file holds no samples");
    if (max_ms >= 0 && frames * 1000 > static_cast<uint64_t>(max_ms) * rate)
    {
        return bad(ErrorCode::too_long, "lasts " + std::to_string(frames * 1000 / rate) + " ms, more than the " +
            std::to_string(max_ms) + " ms allowed");
    }
    SoundData sound;
    sound.channels = channels;
    sound.rate = rate;
    sound.samples.resize(static_cast<std::size_t>(frames) * channels);
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t step = bits / 8;
    for (auto& value : sound.samples)
    {
        value = sample(p, tag, bits);
        p += step;
    }
    return sound;
}
} // namespace UE4SSLuaSoundBridge::Core
