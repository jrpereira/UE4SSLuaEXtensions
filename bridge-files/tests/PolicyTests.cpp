// Access environments: safe set, grants, inheritance, coverage, delete floor,
// .owner and savegames rules, and link resolution through a fake resolver.

#include <TestSupport.hpp>

#include <FileBridgePolicy.hpp>

#include <map>
#include <string>

using namespace UE4SSLuaFileBridge;
using namespace UE4SSLuaFileBridge::Core;
using NativeContract::Access;
using NativeContract::ErrorCode;

namespace
{
const std::string game = "D:/Games/Dawn";
const std::string user = "C:/Users/Me/AppData/Local/Dawnwalker";
const std::string mods = game + "/Dawnwalker/Binaries/Win64/ue4ss/Mods";
const std::string mod = mods + "/MyMod";
const std::string saves = user + "/Saved/SaveGames";
const std::string moddata = user + "/Saved/ModData/MyMod";

// Links: path -> target. Resolution replaces the longest link prefix; the
// leaf is left alone when follow_leaf is false.
class FakeResolver final : public Resolver
{
public:
    std::map<std::string, std::string> links;
    int calls = 0;

    Outcome<std::string> resolve(const std::string& lexical, bool follow_leaf) override
    {
        ++calls;
        if (!follow_leaf)
        {
            auto parent = resolve(parent_path(lexical), true);
            if (!parent) return parent;
            return child_path(parent.value(), leaf_name(lexical));
        }
        std::string path = lexical;
        for (int guard = 0; guard < 8; ++guard)
        {
            bool changed = false;
            for (const auto& [from, to] : links)
            {
                if (is_within(path, from, ascii_equal_fold))
                {
                    path = to + path.substr(from.size());
                    changed = true;
                    break;
                }
            }
            if (!changed) break;
        }
        if (path.rfind("UNC", 0) == 0) return fail(ErrorCode::outside_root, "UNC target");
        return path;
    }
};

struct Fixture
{
    LocationSet locations = make_locations(game, user, mods, "MyMod");
    FakeResolver resolver;
    CheckEnvironment env{locations, resolver, ascii_equal_fold, "C:/Users/Me"};
    Policy policy = safe_policy(locations);

    void add(std::string_view path, std::string_view mode, std::string_view extensions = "", int64_t flags = 0)
    {
        auto lexical = normalize(path, locations, ascii_equal_fold);
        CHECK(lexical.ok());
        if (!lexical) return;
        auto real = resolver.resolve(lexical.value(), true);
        auto grant = make_grant(std::string(path), lexical.value(), real.value(), mode, extensions, flags);
        CHECK(grant.ok());
        if (!grant) return;
        auto next = add_grant(policy, grant.value(), ascii_equal_fold);
        CHECK(next.ok());
        if (next) policy = next.value();
    }

    Status check(std::string_view path, Access access, TargetKind kind = TargetKind::file,
        bool follow_leaf = true, bool bypass = false)
    {
        auto lexical = normalize(path, locations, ascii_equal_fold);
        if (!lexical) return lexical.failure();
        Checker checker(policy, env);
        CheckRequest request{"Test", path, access, kind, bypass};
        auto result = checker.check(lexical.value(), follow_leaf, request);
        if (!result) return result.failure();
        return Done{};
    }

    bool allowed(std::string_view path, Access access, TargetKind kind = TargetKind::file)
    {
        return check(path, access, kind).ok();
    }

    bool refused(std::string_view path, Access access, ErrorCode code, TargetKind kind = TargetKind::file,
        bool follow_leaf = true, bool bypass = false)
    {
        auto status = check(path, access, kind, follow_leaf, bypass);
        return !status && status.failure().code == code;
    }
};

constexpr int64_t del = NativeContract::AddPathFlag::delete_allowed;
constexpr int64_t flat = NativeContract::AddPathFlag::non_recursive;
constexpr int64_t ext = NativeContract::AddPathFlag::extensions_set;

void safe_set()
{
    Fixture f;
    CHECK(f.allowed("game/Dawnwalker/x.pak", Access::read));
    CHECK(f.refused("game/Dawnwalker/x.pak", Access::write, ErrorCode::denied));
    CHECK(f.allowed("user/Saved/Config/x.ini", Access::read));
    CHECK(f.refused("savegames/a.sav", Access::write, ErrorCode::denied));
    CHECK(f.refused("moddata/a.json", Access::write, ErrorCode::denied));
    CHECK(f.allowed("mod/a.json", Access::write));
    CHECK(f.allowed("mod/a.json", Access::read));
    CHECK(f.refused("mod/a.json", Access::delete_entry, ErrorCode::denied));
    CHECK(f.allowed("temp/a.json", Access::delete_entry));
    CHECK(f.allowed("temp/sub", Access::make_dir, TargetKind::directory));
    CHECK(f.allowed("mod", Access::stat, TargetKind::directory));
}

void modes()
{
    Fixture f;
    f.add("mod/logs", "ao");
    CHECK(f.allowed("mod/logs/a.log", Access::append));
    CHECK(f.allowed("mod/logs/a.log", Access::stat));
    CHECK(f.allowed("mod/logs/sub", Access::make_dir, TargetKind::directory));
    CHECK(f.refused("mod/logs/a.log", Access::write, ErrorCode::denied));
    CHECK(f.refused("mod/logs/a.log", Access::read, ErrorCode::denied));
    f.add("mod/out", "wo");
    CHECK(f.allowed("mod/out/x", Access::write));
    CHECK(f.allowed("mod/out/x", Access::stat));
    CHECK(f.refused("mod/out/x", Access::read, ErrorCode::denied));
    f.add("mod/peek", "stat");
    CHECK(f.allowed("mod/peek/x", Access::stat));
    CHECK(f.refused("mod/peek/x", Access::read, ErrorCode::denied));
    CHECK(f.refused("mod/peek/x", Access::append, ErrorCode::denied));
}

void inheritance()
{
    Fixture f;
    f.add("mod/cache", "", "", del); // inherits rw from mod
    CHECK(f.allowed("mod/cache/x.bin", Access::write));
    CHECK(f.allowed("mod/cache/x.bin", Access::delete_entry));
    CHECK(f.allowed("mod/cache/x.bin", Access::move_source));
    CHECK(f.refused("mod/other.bin", Access::delete_entry, ErrorCode::denied));

    // Order doesn't matter: a child added before its parent still inherits.
    Fixture g;
    g.add("savegames/sub", "");
    g.add("savegames", "rw", "sav", ext);
    CHECK(g.allowed("savegames/sub/a.sav", Access::write));
    CHECK(g.refused("savegames/sub/a.png", Access::write, ErrorCode::denied)); // extensions inherited
    CHECK(g.allowed("savegames/sub/a.png", Access::read));                    // falls through to user ro
    CHECK(g.allowed("savegames/a.png", Access::read));

    // delete with an inherited mode that can't write is inert.
    Fixture h;
    h.add("game/Dawnwalker/Content", "", "", del);
    CHECK(h.refused("game/Dawnwalker/Content/x", Access::delete_entry, ErrorCode::denied));
}

void extensions_and_coverage()
{
    Fixture f;
    f.add("savegames", "rw", "sav;META", ext);
    CHECK(f.allowed("savegames/a.sav", Access::write));
    CHECK(f.allowed("savegames/a.SAV", Access::write));
    CHECK(f.allowed("savegames/a.meta", Access::write));
    CHECK(f.refused("savegames/a.png", Access::write, ErrorCode::denied));
    CHECK(f.allowed("savegames/sub", Access::make_dir, TargetKind::directory)); // folders always covered
    f.add("mod/none", "rw", "", ext);                                            // {} covers no files
    CHECK(f.allowed("mod/none/x.txt", Access::write));                           // falls through to mod rw
    f.add("game/Dawnwalker/Logs", "rw", "", flat);
    CHECK(f.allowed("game/Dawnwalker/Logs/a.log", Access::write));
    CHECK(f.refused("game/Dawnwalker/Logs/deep/a.log", Access::write, ErrorCode::denied));
    CHECK(f.allowed("game/Dawnwalker/Logs", Access::write, TargetKind::directory));
}

void replacement_and_validation()
{
    Fixture f;
    f.add("mod", "", "", del); // re-grant replaces the safe-set grant
    CHECK(f.allowed("mod/a.txt", Access::delete_entry) == false); // inherited from user: ro, so delete inert
    f.add("mod", "rw", "", del);
    CHECK(f.allowed("mod/a.txt", Access::delete_entry));
    CHECK(f.policy.grants.size() == 4);

    CHECK(!make_grant("x", "C:/x", "C:/x", "zz", "", 0));
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "", 1 << 7));
    CHECK(!make_grant("x", "C:/x", "C:/x", "ro", "", del));
    CHECK(!make_grant("x", "C:/x", "C:/x", "stat", "", del));
    CHECK(!make_grant("x", "C:/x", "C:/x", "ao", "", del));
    CHECK(make_grant("x", "C:/x", "C:/x", "", "", del)); // inherited mode with delete is allowed
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "a;;b", ext));
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "a.b", ext));
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "a/b", ext));
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "a*", ext));
    CHECK(!make_grant("x", "C:/x", "C:/x", "rw", "sav", 0)); // extensions without the flag
    auto lowered = make_grant("x", "C:/x", "C:/x", "rw", "SaV;Png", ext);
    CHECK(lowered && lowered.value().extensions.size() == 2 && lowered.value().extensions[0] == "sav");

    Policy big;
    for (int i = 0; i < 256; ++i)
    {
        PolicyGrant grant;
        grant.real = "C:/g" + std::to_string(i);
        big.grants.push_back(grant);
    }
    PolicyGrant one_more;
    one_more.real = "C:/extra";
    auto overflow = add_grant(big, one_more, ascii_equal_fold);
    CHECK(!overflow && overflow.failure().code == ErrorCode::io);
}

void delete_floor()
{
    Fixture f;
    f.add("user", "rw", "", del);
    CHECK(f.refused("user", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    CHECK(f.refused("savegames", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    CHECK(f.refused("moddata", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    CHECK(f.refused("temp", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    // user/Saved contains savegames and moddata: needs delete on exactly it.
    CHECK(f.refused("user/Saved", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    CHECK(f.allowed("user/Saved/Logs/a.log", Access::delete_entry));
    f.add("user/Saved", "rw", "", del);
    CHECK(f.allowed("user/Saved", Access::delete_entry, TargetKind::directory));
    CHECK(f.refused("savegames", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
    CHECK(f.allowed("savegames/a.sav", Access::delete_entry));

    Fixture g;
    g.add("game", "rw", "", del);
    CHECK(g.refused("game/Dawnwalker/Binaries/Win64/ue4ss/Mods", Access::delete_entry, ErrorCode::denied,
        TargetKind::directory)); // contains mod
    CHECK(g.refused("mod", Access::move_source, ErrorCode::denied, TargetKind::directory));
    // The safe set's more specific mod grant (rw, no delete) wins over game.
    CHECK(g.refused("mod/x", Access::move_source, ErrorCode::denied));
    CHECK(g.allowed("game/Dawnwalker/x", Access::move_source));
    CHECK(g.refused("game", Access::delete_entry, ErrorCode::denied, TargetKind::directory));
}

void owner_and_savegames()
{
    Fixture f;
    f.add("moddata", "rw", "", del);
    CHECK(f.allowed("moddata/.owner", Access::read));
    CHECK(f.allowed("moddata/.owner", Access::stat));
    CHECK(f.refused("moddata/.owner", Access::write, ErrorCode::denied));
    CHECK(f.refused("moddata/.owner", Access::append, ErrorCode::denied));
    CHECK(f.refused("moddata/.owner", Access::delete_entry, ErrorCode::denied));
    CHECK(f.allowed("moddata/state.json", Access::write));

    f.add("savegames", "rw");
    CHECK(f.refused("savegames/a.sav", Access::write, ErrorCode::invalid, TargetKind::file, true, true));
    CHECK(f.allowed("savegames/a.sav", Access::write));
    CHECK(f.check("mod/a.txt", Access::write, TargetKind::file, true, true).ok());
    // A truncating Open bypasses the backup whether or not the save exists yet.
    CHECK(f.refused("savegames/new.sav", Access::write, ErrorCode::invalid, TargetKind::file, true, true));
    CHECK(f.refused("savegames/sub/new.sav", Access::write, ErrorCode::invalid, TargetKind::file, true, true));
}

void links()
{
    Fixture f;
    // A junction inside mod pointing into game content: the real path decides.
    f.resolver.links[mod + "/content"] = game + "/Dawnwalker/Content";
    CHECK(f.allowed("mod/content/x.pak", Access::read));
    CHECK(f.refused("mod/content/x.pak", Access::write, ErrorCode::denied));
    // A link leaving both roots.
    f.resolver.links[mod + "/out"] = "E:/Elsewhere";
    CHECK(f.refused("mod/out/x", Access::read, ErrorCode::outside_root));
    f.resolver.links[mod + "/unc"] = "UNC/server/share";
    CHECK(f.refused("mod/unc/x", Access::read, ErrorCode::outside_root));
    // Remove acts on the link itself: its own path decides, not the target's.
    f.add("mod", "rw", "", del);
    CHECK(f.check("mod/out", Access::delete_entry, TargetKind::file, false).ok());
    CHECK(f.refused("mod/out", Access::delete_entry, ErrorCode::outside_root, TargetKind::file, true));
    // A grant made through a link applies to the real folder.
    Fixture g;
    g.resolver.links[mod + "/saves"] = saves;
    g.add("mod/saves", "rw");
    CHECK(g.allowed("savegames/a.sav", Access::write));
}

void memoization()
{
    Fixture f;
    f.add("mod/a", "rw");
    f.add("mod/b", "rw");
    Checker checker(f.policy, f.env);
    CheckRequest request{"RemoveTree", "mod", Access::stat, TargetKind::file, false};
    (void)checker.check_real(mod + "/a/1", request);
    const auto after_first = f.resolver.calls;
    for (int i = 0; i < 10; ++i) (void)checker.check_real(mod + "/a/" + std::to_string(i), request);
    CHECK(f.resolver.calls == after_first); // grants and locations resolved once
}
}

int main()
{
    safe_set();
    modes();
    inheritance();
    extensions_and_coverage();
    replacement_and_validation();
    delete_floor();
    owner_and_savegames();
    links();
    memoization();
    return LexTests::finish("PolicyTests");
}
