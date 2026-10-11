// Text, escape, record, error-mapping and contract-table checks.

#include <TestSupport.hpp>

#include <FileBridgeNativeContract.hpp>
#include <FileBridgeOutcome.hpp>
#include <FileBridgeRecords.hpp>
#include <FileBridgeText.hpp>

#include <set>
#include <string>
#include <string_view>

using namespace UE4SSLuaFileBridge;
using namespace UE4SSLuaFileBridge::Core;
using NativeContract::ErrorCode;

namespace
{
void text()
{
    CHECK(valid_utf8("plain"));
    CHECK(valid_utf8("Fran\xC3\xA7" "ais \xE2\x82\xAC \xF0\x9F\x98\x80"));
    CHECK(!valid_utf8("\xC3"));               // truncated
    CHECK(!valid_utf8("\xC0\xAF"));           // overlong '/'
    CHECK(!valid_utf8("\xED\xA0\x80"));       // encoded surrogate
    CHECK(!valid_utf8("\xF4\x90\x80\x80"));   // above U+10FFFF
    CHECK(!valid_utf8("\x80"));

    const std::string sample = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
    const auto wide = utf8_to_utf16(sample);
    CHECK(wide && wide->size() == 6);
    CHECK(wide && (*wide)[3] == 0xD83D && (*wide)[4] == 0xDE00);
    const auto back = wide ? utf16_to_utf8(*wide) : std::nullopt;
    CHECK(back && *back == sample);
    CHECK(!utf16_to_utf8(std::u16string{u'a', static_cast<char16_t>(0xD800)}));
    CHECK(!utf16_to_utf8(std::u16string{static_cast<char16_t>(0xDC00), u'a'}));
    CHECK(!utf8_to_utf16("\xFF"));

    CHECK(ascii_equal_fold("SaveGames", "savegames"));
    CHECK(!ascii_equal_fold("SaveGame", "savegames"));

    CHECK(format_unix_ticks(0) == "0");
    CHECK(format_unix_ticks(17916408000000000LL) == "1791640800");
    CHECK(format_unix_ticks(17916408005000000LL) == "1791640800.5");
    CHECK(format_unix_ticks(17916408000000001LL) == "1791640800.0000001");
    CHECK(format_unix_ticks(-2500000) == "-0.25");
    CHECK(format_filetime(filetime_unix_epoch + 10000000) == "1");
}

void escape()
{
    std::string out;
    CHECK(NativeContract::decode_escaped("plain", 5, out) && out == "plain");
    CHECK(NativeContract::decode_escaped(std::string("a\1\2b\1\1", 6), 4, out) && out == std::string("a\0b\1", 4));
    CHECK(!NativeContract::decode_escaped("a\1", 1, out));      // dangling escape
    CHECK(!NativeContract::decode_escaped("a\1\3", 2, out));    // unknown escape
    CHECK(!NativeContract::decode_escaped("abc", 4, out));      // length mismatch
    CHECK(!NativeContract::decode_escaped("abc", -1, out));
    CHECK(NativeContract::decode_escaped("", 0, out) && out.empty());
}

void records()
{
    std::size_t skipped = 99;
    std::vector<ListEntry> entries{
        {"b.txt", "file", 10, filetime_unix_epoch + 20000000, false, filetime_unix_epoch + 15000000},
        {"a", "directory", 0, filetime_unix_epoch, true, filetime_unix_epoch - 2500000},
        {"bad\tname", "file", 1, filetime_unix_epoch, false, filetime_unix_epoch},
        {"also\nbad", "file", 1, filetime_unix_epoch, false, filetime_unix_epoch},
    };
    const auto encoded = encode_list(entries, skipped);
    CHECK(skipped == 2);
    // name, type, size, modified, link, created
    CHECK(encoded == "a\tdirectory\t0\t0\t1\t-0.25\nb.txt\tfile\t10\t2\t0\t1.5");
    CHECK(NativeContract::list_fields.size() == 6 && NativeContract::list_fields[5] == "created");
    CHECK(encode_list({}, skipped).empty() && skipped == 0);

    const auto locations = encode_locations({{"game", "C:/G", true, true}, {"mod", "", false, false}});
    CHECK(locations == "game\tC:/G\t1\t1\nmod\t\t0\t0");

    CHECK(strip_utf8_bom("\xEF\xBB\xBFtext") == "text");
    CHECK(strip_utf8_bom("text") == "text");
    CHECK(strip_utf8_bom("\xEF\xBB") == "\xEF\xBB");
}

void errors()
{
    CHECK(map_win32_error(2) == ErrorCode::not_found);
    CHECK(map_win32_error(3) == ErrorCode::not_found);
    CHECK(map_win32_error(5) == ErrorCode::denied);
    CHECK(map_win32_error(19) == ErrorCode::read_only);
    CHECK(map_win32_error(32) == ErrorCode::busy);
    CHECK(map_win32_error(33) == ErrorCode::busy);
    CHECK(map_win32_error(1224) == ErrorCode::busy);
    CHECK(map_win32_error(80) == ErrorCode::exists);
    CHECK(map_win32_error(183) == ErrorCode::exists);
    CHECK(map_win32_error(145) == ErrorCode::invalid);
    CHECK(map_win32_error(112) == ErrorCode::io); // disk full
    CHECK(NativeContract::to_string(ErrorCode::outside_root) == "outside_root");
}

// Counts top-level comma-separated items, ignoring commas in parentheses.
int slots(std::string_view arguments)
{
    if (arguments.empty()) return 0;
    int count = 1;
    int depth = 0;
    for (const char c : arguments)
    {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) ++count;
    }
    // "data" is two stack slots (byte_length, escaped).
    std::size_t at = 0;
    while ((at = arguments.find("data", at)) != std::string_view::npos)
    {
        ++count;
        at += 4;
    }
    return count;
}

void contract()
{
    std::set<std::string_view> names;
    for (const auto& function : NativeContract::native_functions)
    {
        CHECK(names.insert(function.name).second);
        CHECK(slots(function.arguments) == function.arity);
    }
    CHECK(names.size() == 26);
    CHECK(!names.count("Check") && !names.count("LocationOf") && !names.count("SessionInfo"));
    CHECK(names.count("GetDispatchStats") == 1);
    CHECK(NativeContract::implementation_magic == 0x4C464231u);

    bool mods_disabled = false;
    for (const auto& location : NativeContract::locations)
    {
        if (location.name == "mods") mods_disabled = !location.enabled;
    }
    CHECK(mods_disabled);
}
}

int main()
{
    text();
    escape();
    records();
    errors();
    contract();
    return LexTests::finish("CodecTests");
}
