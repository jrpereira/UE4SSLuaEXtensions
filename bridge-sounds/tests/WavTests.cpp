// WAV decoding: formats, conversion, malformed files and the length limit.
#include <SoundWav.hpp>
#include <TestSupport.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace UE4SSLuaSoundBridge::Core;

namespace
{
void put16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

void put32(std::string& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (8 * i) & 0xFF));
}

std::string fmt_chunk(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits)
{
    std::string out = "fmt ";
    put32(out, 16);
    put16(out, tag);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * (bits / 8));
    put16(out, static_cast<uint16_t>(channels * (bits / 8)));
    put16(out, bits);
    return out;
}

std::string extensible_chunk(uint16_t subtype, uint16_t channels, uint32_t rate, uint16_t bits)
{
    std::string out = "fmt ";
    put32(out, 40);
    put16(out, 0xFFFE);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * (bits / 8));
    put16(out, static_cast<uint16_t>(channels * (bits / 8)));
    put16(out, bits);
    put16(out, 22);
    put16(out, bits);
    put32(out, channels == 1 ? 0x4 : 0x3);
    put16(out, subtype);
    out.append(reinterpret_cast<const char*>(Wav::subtype_tail), sizeof Wav::subtype_tail);
    return out;
}

std::string riff(const std::string& chunks)
{
    std::string out = "RIFF";
    put32(out, static_cast<uint32_t>(4 + chunks.size()));
    out += "WAVE";
    out += chunks;
    return out;
}

std::string data_chunk(const std::string& bytes, uint32_t declared = UINT32_MAX)
{
    std::string out = "data";
    put32(out, declared == UINT32_MAX ? static_cast<uint32_t>(bytes.size()) : declared);
    out += bytes;
    if (bytes.size() & 1) out.push_back('\0');
    return out;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-6f;
}

void pcm16()
{
    std::string samples;
    put16(samples, 0);
    put16(samples, 0x4000);  // 0.5
    put16(samples, 0x8000);  // -1
    put16(samples, 0x7FFF);  // just under 1
    auto decoded = decode_wav(riff(fmt_chunk(1, 2, 44100, 16) + data_chunk(samples)));
    CHECK(decoded);
    if (!decoded) return;
    const auto& sound = decoded.value();
    CHECK(sound.channels == 2 && sound.rate == 44100 && sound.frames() == 2);
    CHECK(near(sound.samples[0], 0.0f) && near(sound.samples[1], 0.5f) && near(sound.samples[2], -1.0f));
    CHECK(sound.samples[3] < 1.0f && sound.samples[3] > 0.9999f);
    CHECK(sound.bytes() == 4 * sizeof(float));
    CHECK((sound.format() == Format{2, 44100}));
}

void other_depths()
{
    {
        const std::string samples{"\x80\x00\xFF", 3}; // 0, -1, ~1
        auto decoded = decode_wav(riff(fmt_chunk(1, 1, 8000, 8) + data_chunk(samples)));
        CHECK(decoded && decoded.value().frames() == 3);
        if (decoded)
        {
            CHECK(near(decoded.value().samples[0], 0.0f) && near(decoded.value().samples[1], -1.0f));
            CHECK(decoded.value().samples[2] > 0.99f);
        }
    }
    {
        const std::string samples{"\x00\x00\x40\x00\x00\x80", 6}; // 0.5, -1
        auto decoded = decode_wav(riff(fmt_chunk(1, 1, 48000, 24) + data_chunk(samples)));
        CHECK(decoded && decoded.value().frames() == 2);
        if (decoded) CHECK(near(decoded.value().samples[0], 0.5f) && near(decoded.value().samples[1], -1.0f));
    }
    {
        std::string samples;
        put32(samples, 0xC0000000u); // -0.5
        auto decoded = decode_wav(riff(fmt_chunk(1, 1, 48000, 32) + data_chunk(samples)));
        CHECK(decoded && near(decoded.value().samples[0], -0.5f));
    }
    {
        std::string samples(8, '\0');
        const float values[2] = {0.25f, std::nanf("")};
        std::memcpy(samples.data(), values, sizeof values);
        auto decoded = decode_wav(riff(fmt_chunk(3, 1, 48000, 32) + data_chunk(samples)));
        CHECK(decoded && near(decoded.value().samples[0], 0.25f));
        if (decoded) CHECK(decoded.value().samples[1] == 0.0f); // non-finite becomes silence
    }
    {
        std::string samples(8, '\0');
        const double value = -0.75;
        std::memcpy(samples.data(), &value, sizeof value);
        auto decoded = decode_wav(riff(fmt_chunk(3, 1, 48000, 64) + data_chunk(samples)));
        CHECK(decoded && near(decoded.value().samples[0], -0.75f));
    }
}

void extensible_and_chunks()
{
    std::string samples;
    put16(samples, 0x4000);
    auto decoded = decode_wav(riff(extensible_chunk(1, 1, 48000, 16) + data_chunk(samples)));
    CHECK(decoded && near(decoded.value().samples[0], 0.5f));

    std::string floats(4, '\0');
    const float half = 0.5f;
    std::memcpy(floats.data(), &half, sizeof half);
    decoded = decode_wav(riff(extensible_chunk(3, 1, 48000, 32) + data_chunk(floats)));
    CHECK(decoded && near(decoded.value().samples[0], 0.5f));

    // Unknown chunks (odd-sized, padded) before fmt and data are skipped.
    std::string list = "LIST";
    put32(list, 3);
    list += "abc";
    list.push_back('\0');
    decoded = decode_wav(riff(list + fmt_chunk(1, 1, 44100, 16) + data_chunk(samples)));
    CHECK(decoded && decoded.value().frames() == 1);

    // A data chunk claiming more than the file holds is read to the last whole frame.
    std::string three;
    put16(three, 1);
    put16(three, 2);
    three.push_back('\x07');
    decoded = decode_wav(riff(fmt_chunk(1, 1, 44100, 16) + "data" + std::string("\x00\x10\x00\x00", 4) + three));
    CHECK(decoded && decoded.value().frames() == 2);
}

void refusals()
{
    std::string samples;
    put16(samples, 1);
    const auto code = [](const Outcome<SoundData>& outcome) { return outcome ? ErrorCode::io : outcome.failure().code; };
    CHECK(code(decode_wav("not a wav file at all")) == ErrorCode::unsupported);
    CHECK(code(decode_wav("")) == ErrorCode::unsupported);
    CHECK(code(decode_wav(riff(data_chunk(samples)))) == ErrorCode::unsupported);                     // no fmt
    CHECK(code(decode_wav(riff(fmt_chunk(1, 1, 44100, 16)))) == ErrorCode::unsupported);              // no data
    CHECK(code(decode_wav(riff(fmt_chunk(2, 1, 44100, 16) + data_chunk(samples)))) == ErrorCode::unsupported); // ADPCM
    CHECK(code(decode_wav(riff(fmt_chunk(1, 6, 44100, 16) + data_chunk(samples)))) == ErrorCode::unsupported); // 5.1
    CHECK(code(decode_wav(riff(fmt_chunk(1, 1, 500, 16) + data_chunk(samples)))) == ErrorCode::unsupported);
    CHECK(code(decode_wav(riff(fmt_chunk(1, 1, 44100, 12) + data_chunk(samples)))) == ErrorCode::unsupported);
    CHECK(code(decode_wav(riff(fmt_chunk(3, 1, 44100, 16) + data_chunk(samples)))) == ErrorCode::unsupported);
    CHECK(code(decode_wav(riff(fmt_chunk(1, 1, 44100, 16) + data_chunk("")))) == ErrorCode::unsupported); // empty
    CHECK(code(decode_wav(riff(extensible_chunk(2, 1, 44100, 16) + data_chunk(samples)))) == ErrorCode::unsupported);
    auto bad = decode_wav(riff(fmt_chunk(2, 1, 44100, 16) + data_chunk(samples)));
    CHECK(!bad && bad.failure().message.find("PCM or float") != std::string::npos);
}

void length_limit()
{
    std::string second(2 * 1000, '\0'); // 1000 frames at 1000 Hz = 1 s
    const auto file = riff(fmt_chunk(1, 1, 1000, 16) + data_chunk(second));
    CHECK(decode_wav(file));
    CHECK(decode_wav(file, -1));
    CHECK(decode_wav(file, 1000));
    auto refused = decode_wav(file, 999);
    CHECK(!refused && refused.failure().code == ErrorCode::too_long);
    CHECK(!decode_wav(file, 0));
}
} // namespace

int main()
{
    pcm16();
    other_depths();
    extensible_and_chunks();
    refusals();
    length_limit();
    return LexTests::finish("SoundBridgeWavTests");
}
