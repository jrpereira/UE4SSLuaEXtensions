// Level names, module and category names, [Levels] settings, resolution order
// and line formats.
#include <LogFormat.hpp>
#include <LogLevels.hpp>
#include <TestSupport.hpp>

using namespace UE4SSLuaLogBridge::Core;

namespace
{
void levels()
{
    CHECK(parse_level("TRACE") == 1 && parse_level("debug") == 2 && parse_level(" Info ") == 3);
    CHECK(parse_level("WARN") == 4 && parse_level("warning") == 4 && parse_level("Error") == 5);
    CHECK(parse_level("CRITICAL") == 6 && parse_level("off") == 7 && parse_level("none") == 7);
    CHECK(!parse_level("") && !parse_level("loud") && !parse_level("0") && !parse_level("8") && !parse_level("12"));
    CHECK(parse_level("4") == 4 && parse_level(" 7") == 7 && parse_level("1") == 1);
    CHECK(level_name(1) == "TRACE" && level_name(6) == "CRITICAL" && level_name(7) == "OFF" && level_name(0) == "?");
}

void names()
{
    CHECK(valid_module_name("Preymonition") && valid_module_name("0_ModCore_Settings") && valid_module_name("My Mod-2"));
    CHECK(!valid_module_name("") && !valid_module_name(" lead") && !valid_module_name("trail "));
    CHECK(!valid_module_name("a.b") && !valid_module_name("a/b") && !valid_module_name("Global"));
    CHECK(!valid_module_name(std::string(65, 'a')) && valid_module_name(std::string(64, 'a')));
    CHECK(module_name_from_folder("Foo.Bar") == "Foo_Bar" && module_name_from_folder("") == "Mod");
    CHECK(module_name_from_folder("global") == "Mod");

    CHECK(valid_category_name("main") && valid_category_name("combat") && valid_category_name("ai-2_x"));
    CHECK(!valid_category_name("") && !valid_category_name("2d") && !valid_category_name("a b"));
    CHECK(!valid_category_name("a.b") && !valid_category_name(std::string(33, 'a')));
    CHECK(!valid_category_name("con") && !valid_category_name("NUL") && !valid_category_name("com1"));
    CHECK(!valid_category_name("LPT9") && valid_category_name("com") && valid_category_name("console"));

    CHECK(level_key("") == std::string() && level_key("Global") == std::string() && level_key(" global ") == std::string());
    CHECK(level_key("Preymonition") == std::string("preymonition"));
    CHECK(level_key("Preymonition.Combat") == std::string("preymonition.combat"));
    CHECK(!level_key("a.b.c") && !level_key(".x") && !level_key("x.") && !level_key("bad/name"));
}

void settings()
{
    std::vector<std::string> ignored;
    const auto table = parse_level_settings(
        "[Settings]\nEnableVerifications=1\nGlobal=TRACE\n\n"
        "[Levels]\r\n; comment\r\nGlobal = info\r\nPreymonition=DEBUG\r\nPreymonition.combat=trace\r\n"
        "Broken=loud\r\nbad/key=INFO\r\nnoequals\r\n[Other]\nX=ERROR\n",
        &ignored);
    CHECK(table.size() == 3);
    CHECK(table.at("") == 3 && table.at("preymonition") == 2 && table.at("preymonition.combat") == 1);
    CHECK(ignored.size() == 2);
    CHECK(parse_level_settings("").empty() && parse_level_settings("[levels]\nA=ERROR").at("a") == 5);
}

void resolution()
{
    LevelResolver levels;
    CHECK(levels.resolve("Mod", "main") == default_rank && levels.resolve_key("") == default_rank);
    const auto start = levels.generation();

    levels.set_default("Mod", 3);
    CHECK(levels.resolve("Mod", "main") == 3 && levels.resolve("Other", "main") == 4);
    levels.set_default("Mod", 5); // a weaker default doesn't replace a stronger one
    CHECK(levels.resolve("Mod", "main") == 3);

    CHECK(levels.set_settings(parse_level_settings("[Levels]\nGlobal=ERROR\nmod=DEBUG\nMod.combat=TRACE\n")));
    CHECK(!levels.set_settings(parse_level_settings("[Levels]\nGlobal=ERROR\nmod=DEBUG\nMod.combat=TRACE\n")));
    CHECK(levels.resolve("Mod", "main") == 2 && levels.resolve("Mod", "combat") == 1);
    CHECK(levels.resolve("Other", "main") == 5); // global setting
    CHECK(levels.resolve_key("") == 5 && levels.resolve_key("mod") == 2 && levels.resolve_key("mod.combat") == 1);

    levels.set_override("mod", 6);
    CHECK(levels.resolve("Mod", "main") == 6);
    CHECK(levels.resolve("Mod", "combat") == 6); // a runtime module override beats every setting
    levels.set_override("mod", std::nullopt);
    levels.set_override("mod.combat", 7);
    CHECK(levels.resolve("Mod", "combat") == 7 && levels.resolve("Mod", "main") == 2);
    levels.set_override("", 1);
    CHECK(levels.resolve("Other", "main") == 1); // the global override beats the global setting
    CHECK(levels.resolve("Mod", "main") == 2);     // but not a module's own setting
    CHECK(levels.generation() > start);
}

void formats()
{
    const auto time = pack_time(CivilTime{2026, 11, 18, 14, 3, 22, 123});
    CHECK(format_time(time) == "2026-11-18 14:03:22.123");
    const auto back = unpack_time(time);
    CHECK(back.year == 2026 && back.month == 11 && back.day == 18 && back.millisecond == 123);
    CHECK(ue4ss_line("Mod", "main", 2, "hello\n") == "[Mod] DEBUG hello");
    CHECK(ue4ss_line("Mod", "combat", 5, "x") == "[Mod:combat] ERROR x");
    CHECK(file_line(time, 4, "line\r\n") == "2026-11-18 14:03:22.123 WARN line\n");
    CHECK(recovery_line(time, 1, "Mod", "ui", "m", true) == "2026-11-18 14:03:22.123 * TRACE [Mod:ui] m\n");
    CHECK(recovery_line(time, 3, "Mod", "main", "m", false) == "2026-11-18 14:03:22.123 INFO [Mod] m\n");
}
} // namespace

int main()
{
    levels();
    names();
    settings();
    resolution();
    formats();
    return LexTests::finish("LevelsTests");
}
