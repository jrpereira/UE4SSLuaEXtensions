// Per-play values: clamping, pan matrices and the low-pass setting.
#include <SoundMix.hpp>
#include <TestSupport.hpp>

#include <cmath>
#include <vector>

using namespace UE4SSLuaSoundBridge::Core;
namespace Contract = UE4SSLuaSoundBridge::NativeContract;

namespace
{
bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

void clamping()
{
    auto params = play_params(0, Contract::micro, Contract::micro, 0);
    CHECK(near(params.pan, 0) && near(params.volume, 1) && near(params.pitch, 1) && params.lowpass_hz == 0);
    params = play_params(-5 * Contract::micro, 9 * Contract::micro, 9 * Contract::micro, 5);
    CHECK(near(params.pan, -1) && near(params.volume, 4) && near(params.pitch, 2) && near(params.lowpass_hz, 20));
    params = play_params(5 * Contract::micro, -1, 0, -3);
    CHECK(near(params.pan, 1) && near(params.volume, 0) && near(params.pitch, 0.5f) && params.lowpass_hz == 0);
    params = play_params(250000, 1500000, 800000, 1200);
    CHECK(near(params.pan, 0.25f) && near(params.volume, 1.5f) && near(params.pitch, 0.8f) && near(params.lowpass_hz, 1200));
}

void mono_pan()
{
    constexpr uint32_t stereo_mask = speaker_front_left | speaker_front_right;
    auto m = output_matrix(1, 2, stereo_mask, -1.0f);
    CHECK(m.size() == 2 && near(m[0], 1) && near(m[1], 0));
    m = output_matrix(1, 2, stereo_mask, 1.0f);
    CHECK(near(m[0], 0) && near(m[1], 1));
    m = output_matrix(1, 2, stereo_mask, 0.0f);
    CHECK(near(m[0], m[1]) && near(m[0] * m[0] + m[1] * m[1], 1)); // constant power
    m = output_matrix(1, 2, stereo_mask, 0.5f);
    CHECK(m[1] > m[0] && near(m[0] * m[0] + m[1] * m[1], 1));
    m = output_matrix(1, 2, stereo_mask, -7.0f); // clamped
    CHECK(near(m[0], 1) && near(m[1], 0));
}

void stereo_pan()
{
    constexpr uint32_t stereo_mask = speaker_front_left | speaker_front_right;
    // [s + d * source]: (L->L, R->L, L->R, R->R)
    auto m = output_matrix(2, 2, stereo_mask, 0.0f);
    CHECK(m.size() == 4 && near(m[0], 1) && near(m[1], 0) && near(m[2], 0) && near(m[3], 1));
    m = output_matrix(2, 2, stereo_mask, -1.0f);
    CHECK(near(m[0], 1) && near(m[3], 0));
    m = output_matrix(2, 2, stereo_mask, 0.5f);
    CHECK(near(m[0], 0.5f) && near(m[3], 1));
}

void layouts()
{
    // 5.1: FL FR FC LFE BL BR; only the front pair is fed.
    constexpr uint32_t surround = 0x3F;
    auto m = output_matrix(1, 6, surround, -1.0f);
    CHECK(m.size() == 6 && near(m[0], 1));
    for (std::size_t d = 1; d < 6; ++d) CHECK(near(m[d], 0));
    // A channel's index counts the mask bits below it.
    CHECK(channel_index(0x3F, speaker_front_right) == 1);
    CHECK(channel_index(0x6, speaker_front_right) == 0); // FR FC
    CHECK(channel_index(0x6, speaker_front_left) == -1);
    m = output_matrix(1, 2, 0, 1.0f); // unknown mask: channels 0 and 1
    CHECK(near(m[0], 0) && near(m[1], 1));
    m = output_matrix(2, 1, 0x4, 1.0f); // mono output: plain mix
    CHECK(m.size() == 2 && near(m[0], 0.5f) && near(m[1], 0.5f));
    m = output_matrix(1, 1, 0x4, -1.0f);
    CHECK(m.size() == 1 && near(m[0], 1));
}

void filters()
{
    auto f = lowpass_filter(0, 48000);
    CHECK(f.bypass && near(f.frequency, 1) && near(f.one_over_q, 1));
    f = lowpass_filter(8000, 48000); // a sixth of the rate: the top
    CHECK(f.bypass);
    f = lowpass_filter(1200, 48000);
    CHECK(!f.bypass && near(f.frequency, static_cast<float>(2 * std::sin(3.14159265358979 * 1200.0 / 48000.0))));
    CHECK(f.frequency > 0 && f.frequency < 1 && near(f.one_over_q, 1));
    f = lowpass_filter(1200, 0);
    CHECK(f.bypass);
}
} // namespace

int main()
{
    clamping();
    mono_pan();
    stereo_pan();
    layouts();
    filters();
    return LexTests::finish("SoundBridgeMixTests");
}
