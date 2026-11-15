// Sound paths: absolute as given, relative against the mod's folder, `..`
// anywhere; refusals only for robustness.
#include <SoundPaths.hpp>
#include <TestSupport.hpp>

#include <string>

using namespace UE4SSLuaSoundBridge::Core;

namespace
{
const std::string mod = "C:/Games/Mods/3_ModCore_Templates";

bool accepts(const std::string& path, const std::string& expected)
{
    auto result = normalize_sound_path(path);
    return result && result.value() == expected;
}

bool refuses(const std::string& path)
{
    auto result = normalize_sound_path(path);
    return !result && result.failure().code == ErrorCode::invalid && result.failure().message.find(path) == 0;
}

bool resolves(const std::string& path, const std::string& folder, const std::string& expected)
{
    auto normalized = normalize_sound_path(path);
    if (!normalized) return false;
    auto result = resolve_sound_path(normalized.value(), folder);
    return result && result.value() == expected;
}

bool unresolved(const std::string& path, const std::string& folder, ErrorCode code)
{
    auto normalized = normalize_sound_path(path);
    if (!normalized) return false;
    auto result = resolve_sound_path(normalized.value(), folder);
    return !result && result.failure().code == code;
}
} // namespace

int main()
{
    // Relative paths, as before.
    CHECK(accepts("sounds/cue.wav", "sounds/cue.wav"));
    CHECK(accepts("sounds\\cue.wav", "sounds/cue.wav"));
    CHECK(accepts("./sounds//cue.wav", "sounds/cue.wav"));
    CHECK(accepts("sounds/old/../cue.wav", "sounds/cue.wav"));
    CHECK(accepts("cue.wav", "cue.wav"));
    CHECK(accepts("sounds/cue.wav/", "sounds/cue.wav"));
    CHECK(accepts("sounds/caf\xC3\xA9.wav", "sounds/caf\xC3\xA9.wav"));
    CHECK(accepts("sounds/.hidden.wav", "sounds/.hidden.wav"));
    CHECK(accepts("sounds/COM10.wav", "sounds/COM10.wav"));
    CHECK(accepts("sounds/console.wav", "sounds/console.wav"));

    // `..` may leave the mod's folder.
    CHECK(accepts("../Other/cue.wav", "../Other/cue.wav"));
    CHECK(accepts("sounds/../../cue.wav", "../cue.wav"));
    CHECK(accepts("..\\..\\x\\..\\cue.wav", "../../cue.wav"));

    // Absolute paths load as given.
    CHECK(accepts("C:/Windows/Media/chord.wav", "C:/Windows/Media/chord.wav"));
    CHECK(accepts("c:\\Mods\\9_ModCore_Preymonition\\sounds\\cue_ding.wav", "C:/Mods/9_ModCore_Preymonition/sounds/cue_ding.wav"));
    CHECK(accepts("D:/a/../b/./cue.wav", "D:/b/cue.wav"));
    CHECK(is_absolute_sound_path("C:/x.wav"));
    CHECK(!is_absolute_sound_path("x.wav"));
    CHECK(!is_absolute_sound_path("../x.wav"));

    // Refusals: robustness only, all `invalid`.
    CHECK(refuses(""));
    CHECK(refuses("/sounds/cue.wav"));
    CHECK(refuses("\\sounds\\cue.wav"));
    CHECK(refuses("\\\\server\\share\\cue.wav"));
    CHECK(refuses("//server/share/cue.wav"));
    CHECK(refuses("\\\\.\\CON"));
    CHECK(refuses("\\\\?\\C:\\cue.wav"));
    CHECK(refuses("C:cue.wav"));
    CHECK(refuses("C:"));
    CHECK(refuses("C:/"));
    CHECK(refuses("C:/.."));
    CHECK(refuses("C:/../cue.wav"));
    CHECK(refuses("cue.wav:stream"));
    CHECK(refuses("C:/cue.wav:stream"));
    CHECK(refuses("1:/cue.wav"));
    CHECK(refuses("sounds/*.wav"));
    CHECK(refuses("sounds/cue?.wav"));
    CHECK(refuses("sounds/cue.wav."));
    CHECK(refuses("sounds /cue.wav"));
    CHECK(refuses(std::string("sounds/\tcue.wav")));
    CHECK(refuses("."));
    CHECK(refuses(".."));
    CHECK(refuses("sounds/.."));
    CHECK(refuses("sounds/../.."));
    CHECK(refuses("CON"));
    CHECK(refuses("sounds/con.wav"));
    CHECK(refuses("sounds/COM1.wav"));
    CHECK(refuses("sounds/lpt9"));
    CHECK(refuses("C:/NUL"));

    // Resolution against the mod's folder.
    CHECK(resolves("sounds/cue.wav", mod, mod + "/sounds/cue.wav"));
    CHECK(resolves("../9_ModCore_Preymonition/sounds/cue.wav", mod, "C:/Games/Mods/9_ModCore_Preymonition/sounds/cue.wav"));
    CHECK(resolves("../../../cue.wav", mod, "C:/cue.wav"));
    CHECK(resolves("E:/x.wav", "", "E:/x.wav"));
    CHECK(resolves("E:/x.wav", mod, "E:/x.wav"));
    CHECK(unresolved("../../../../cue.wav", mod, ErrorCode::invalid));
    CHECK(unresolved("sounds/cue.wav", "", ErrorCode::io));

    // Mod names usable as a folder.
    CHECK(single_folder_name("3_ModCore_Templates"));
    CHECK(!single_folder_name(""));
    CHECK(!single_folder_name(".."));
    CHECK(!single_folder_name("a/b"));
    CHECK(!single_folder_name("a\\b"));
    CHECK(!single_folder_name("C:/x"));
    CHECK(!single_folder_name("CON"));
    return LexTests::finish("SoundBridgePathTests");
}
