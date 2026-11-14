// Sound paths: relative to the mod's folder, never leaving it.
#include <SoundPaths.hpp>
#include <TestSupport.hpp>

#include <string>

using namespace UE4SSLuaSoundBridge::Core;

namespace
{
bool accepts(const std::string& path, const std::string& expected)
{
    auto result = normalize_sound_path(path);
    return result && result.value() == expected;
}

bool refuses(const std::string& path, ErrorCode code)
{
    auto result = normalize_sound_path(path);
    return !result && result.failure().code == code && result.failure().message.find(path) == 0;
}
} // namespace

int main()
{
    CHECK(accepts("sounds/cue.wav", "sounds/cue.wav"));
    CHECK(accepts("sounds\\cue.wav", "sounds/cue.wav"));
    CHECK(accepts("./sounds//cue.wav", "sounds/cue.wav"));
    CHECK(accepts("sounds/old/../cue.wav", "sounds/cue.wav"));
    CHECK(accepts("cue.wav", "cue.wav"));
    CHECK(accepts("sounds/cue.wav/", "sounds/cue.wav"));
    CHECK(accepts("sounds/caf\xC3\xA9.wav", "sounds/caf\xC3\xA9.wav"));
    CHECK(accepts("sounds/.hidden.wav", "sounds/.hidden.wav"));

    CHECK(refuses("", ErrorCode::invalid));
    CHECK(refuses("/sounds/cue.wav", ErrorCode::invalid));
    CHECK(refuses("\\sounds\\cue.wav", ErrorCode::invalid));
    CHECK(refuses("C:/Windows/Media/chord.wav", ErrorCode::invalid));
    CHECK(refuses("C:cue.wav", ErrorCode::invalid));
    CHECK(refuses("cue.wav:stream", ErrorCode::invalid));
    CHECK(refuses("sounds/*.wav", ErrorCode::invalid));
    CHECK(refuses("sounds/cue?.wav", ErrorCode::invalid));
    CHECK(refuses("sounds/cue.wav.", ErrorCode::invalid));
    CHECK(refuses("sounds /cue.wav", ErrorCode::invalid));
    CHECK(refuses(std::string("sounds/\tcue.wav"), ErrorCode::invalid));
    CHECK(refuses(".", ErrorCode::invalid));
    CHECK(refuses("sounds/..", ErrorCode::invalid));

    CHECK(refuses("..", ErrorCode::outside_root));
    CHECK(refuses("../Other/cue.wav", ErrorCode::outside_root));
    CHECK(refuses("sounds/../../cue.wav", ErrorCode::outside_root));
    CHECK(refuses("CON", ErrorCode::outside_root));
    CHECK(refuses("sounds/con.wav", ErrorCode::outside_root));
    CHECK(refuses("sounds/COM1.wav", ErrorCode::outside_root));
    CHECK(refuses("sounds/lpt9", ErrorCode::outside_root));
    CHECK(accepts("sounds/COM10.wav", "sounds/COM10.wav"));
    CHECK(accepts("sounds/console.wav", "sounds/console.wav"));
    return LexTests::finish("SoundBridgePathTests");
}
