// Path normalization, escapes, Unicode names and the location table.

#include <TestSupport.hpp>

#include <FileBridgePaths.hpp>

#include <string>

using namespace UE4SSLuaFileBridge;
using namespace UE4SSLuaFileBridge::Core;
using NativeContract::ErrorCode;

namespace
{
const std::string game = "D:/Steam/steamapps/common/The Blood of Dawnwalker";
const std::string user = "C:/Users/Me/AppData/Local/Dawnwalker";
const std::string mods = game + "/Dawnwalker/Binaries/Win64/ue4ss/Mods";

LocationSet locations(std::string_view mod = "MyMod")
{
    return make_locations(game, user, mods, mod);
}

std::string ok(std::string_view input, const LocationSet& set = locations())
{
    auto result = normalize(input, set, ascii_equal_fold);
    if (!result)
    {
        std::fprintf(stderr, "unexpected failure for %.*s: %s\n", static_cast<int>(input.size()), input.data(),
            result.failure().message.c_str());
        return "<failed>";
    }
    return result.value();
}

bool fails(std::string_view input, ErrorCode code, const LocationSet& set = locations())
{
    auto result = normalize(input, set, ascii_equal_fold);
    return !result && result.failure().code == code;
}

void location_table()
{
    const auto set = locations();
    CHECK(set.bound("game") && set.bound("game")->absolute == game);
    CHECK(set.bound("mod") && set.bound("mod")->absolute == mods + "/MyMod");
    CHECK(set.bound("moddata") && set.bound("moddata")->absolute == user + "/Saved/ModData/MyMod");
    CHECK(set.bound("temp") && set.bound("temp")->absolute == user + "/Saved/ModData/MyMod/temp");
    CHECK(set.bound("savegames") && set.bound("savegames")->absolute == user + "/Saved/SaveGames");
    CHECK(set.find("mods") && !set.bound("mods")); // pending: present but disabled

    const auto unbound = locations("bad/name");
    CHECK(unbound.find("mod") && unbound.find("mod")->absolute.empty());
    CHECK(!unbound.bound("moddata") && !unbound.bound("temp"));
    CHECK(unbound.bound("savegames"));
    CHECK(fails("mod/x.txt", ErrorCode::invalid, unbound));
    CHECK(fails("temp", ErrorCode::invalid, unbound));
    CHECK(!is_single_segment("..") && !is_single_segment("a:b") && !is_single_segment("CON"));
    CHECK(is_single_segment("Mod Name 2"));
}

void relative_forms()
{
    CHECK(ok("mod") == mods + "/MyMod");
    CHECK(ok("mod/cache/state.json") == mods + "/MyMod/cache/state.json");
    CHECK(ok("mod/cache/") == mods + "/MyMod/cache");
    CHECK(ok("mod//cache///x") == mods + "/MyMod/cache/x");
    CHECK(ok("mod/./a/./b") == mods + "/MyMod/a/b");
    CHECK(ok("mod/a/../b") == mods + "/MyMod/b");
    CHECK(ok("mod\\a\\b") == mods + "/MyMod/a/b");
    CHECK(ok("savegames/Autosave0.sav") == user + "/Saved/SaveGames/Autosave0.sav");
    CHECK(ok("game") == game);
    CHECK(ok("user/Saved") == user + "/Saved");
    CHECK(fails("mod/..", ErrorCode::outside_root));
    CHECK(fails("mod/../OtherMod", ErrorCode::outside_root));
    CHECK(fails("mod/a/../../b", ErrorCode::outside_root));
    CHECK(fails("game/..", ErrorCode::outside_root));
    CHECK(fails("Mod/x", ErrorCode::invalid));     // location names are exact, lowercase
    CHECK(fails("mods/x", ErrorCode::invalid));    // disabled location
    CHECK(fails("cache/x", ErrorCode::invalid));
    CHECK(fails("", ErrorCode::invalid));
}

void absolute_forms()
{
    CHECK(ok("d:/Steam/steamapps/common/The Blood of Dawnwalker/x") == game + "/x");
    CHECK(ok("D:\\Steam\\steamapps\\common\\The Blood of Dawnwalker\\Dawnwalker") == game + "/Dawnwalker");
    CHECK(ok("C:/users/me/appdata/local/dawnwalker/Saved") == "C:/users/me/appdata/local/dawnwalker/Saved");
    CHECK(ok(game + "/a/../b") == game + "/b");
    CHECK(fails(game + "/../The Blood of Dawnwalker/x", ErrorCode::outside_root)); // climbs above its root
    CHECK(fails("C:/Windows/System32", ErrorCode::outside_root));
    CHECK(fails("C:/", ErrorCode::outside_root));
    CHECK(fails("C:", ErrorCode::outside_root));
    CHECK(fails("C:foo", ErrorCode::invalid));
    CHECK(fails("C:/..", ErrorCode::outside_root));
    CHECK(fails("/Users/x", ErrorCode::invalid));
    CHECK(fails("\\Users\\x", ErrorCode::invalid));
}

void device_and_unc()
{
    CHECK(fails("\\\\server\\share\\x", ErrorCode::outside_root));
    CHECK(fails("//server/share/x", ErrorCode::outside_root));
    CHECK(fails("\\\\.\\pipe\\name", ErrorCode::outside_root));
    CHECK(fails("\\\\?\\C:\\x", ErrorCode::outside_root));
    CHECK(fails("\\?" "?\\C:\\x", ErrorCode::outside_root));
    CHECK(fails("mod/CON", ErrorCode::outside_root));
    CHECK(fails("mod/nul.txt", ErrorCode::outside_root));
    CHECK(fails("mod/Com1", ErrorCode::outside_root));
    CHECK(fails("mod/LPT9.log", ErrorCode::outside_root));
    CHECK(fails("mod/CON .txt", ErrorCode::outside_root));
    CHECK(ok("mod/COM10") == mods + "/MyMod/COM10");
    CHECK(ok("mod/console.txt") == mods + "/MyMod/console.txt");
}

void malformed()
{
    CHECK(fails("mod/a<b", ErrorCode::invalid));
    CHECK(fails("mod/a>b", ErrorCode::invalid));
    CHECK(fails("mod/a:stream", ErrorCode::invalid)); // alternate data streams
    CHECK(fails("mod/a\"b", ErrorCode::invalid));
    CHECK(fails("mod/a|b", ErrorCode::invalid));
    CHECK(fails("mod/a?", ErrorCode::invalid));
    CHECK(fails("mod/a*", ErrorCode::invalid));
    CHECK(fails("mod/a\tb", ErrorCode::invalid));
    CHECK(fails("mod/trailing ", ErrorCode::invalid));
    CHECK(fails("mod/trailing.", ErrorCode::invalid));
    CHECK(fails("mod/...", ErrorCode::invalid));
    CHECK(fails(std::string("mod/a\0b", 7), ErrorCode::invalid));
    CHECK(fails("mod/\xFF", ErrorCode::invalid));
    CHECK(ok("mod/.owner") == mods + "/MyMod/.owner");
}

void unicode_names()
{
    CHECK(ok("mod/caf\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC.txt") == mods + "/MyMod/caf\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC.txt");
    CHECK(ok("mod/\xF0\x9F\x98\x80") == mods + "/MyMod/\xF0\x9F\x98\x80");
    const auto set = make_locations(game, user, mods, "M\xC3\xB6" "d");
    CHECK(set.bound("mod") && set.bound("mod")->absolute == mods + "/M\xC3\xB6" "d");
}

void helpers()
{
    CHECK(normalize_system_path("C:\\a\\b\\") == "C:/a/b");
    CHECK(normalize_system_path("\\\\?\\c:\\a\\.\\b") == "C:/a/b");
    CHECK(normalize_system_path("c:") == "C:/");
    CHECK(normalize_system_path("\\\\?\\UNC\\server\\share").empty());
    CHECK(normalize_system_path("relative").empty());
    CHECK(is_within("C:/a/b", "c:/A", ascii_equal_fold));
    CHECK(!is_within("C:/ab", "C:/a", ascii_equal_fold));
    CHECK(is_proper_ancestor("C:/a", "C:/a/b", ascii_equal_fold));
    CHECK(!is_proper_ancestor("C:/a", "C:/a", ascii_equal_fold));
    CHECK(parent_path("C:/a/b") == "C:/a");
    CHECK(parent_path("C:/a") == "C:/");
    CHECK(leaf_name("C:/a/b.txt") == "b.txt");
    CHECK(extension_of("b.txt") == "txt");
    CHECK(extension_of("a.tar.gz") == "gz");
    CHECK(extension_of(".owner").empty());
    CHECK(extension_of("noext").empty());
    CHECK(child_path("C:/", "x") == "C:/x");
    CHECK(child_path("C:/a", "x") == "C:/a/x");
}
}

int main()
{
    location_table();
    relative_forms();
    absolute_forms();
    device_and_unc();
    malformed();
    unicode_names();
    helpers();
    return LexTests::finish("PathTests");
}
