#pragma once

// Level ranks, module and category names, the [Levels] section of config.ini
// and the resolution of a <module>.<category>'s effective level. Portable.

#include <LogBridgeNativeContract.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UE4SSLuaLogBridge::Core
{
inline constexpr int rank_trace = 1;
inline constexpr int rank_warn = 4;
inline constexpr int rank_critical = 6;
inline constexpr int rank_off = 7;
inline constexpr int default_rank = rank_warn;

inline constexpr std::array<std::string_view, 7> level_names{
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF"};

inline std::string_view level_name(int rank)
{
    if (rank < 1 || rank > rank_off) return "?";
    return level_names[static_cast<std::size_t>(rank - 1)];
}

inline std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

inline std::string lower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The four categories every logger has, in this order.
inline constexpr std::array<std::string_view, 4> default_categories{"main", "config", "ui", "input"};

// The rank a level name (any case) or a rank digit 1..7 gives; surrounding
// spaces are ignored.
inline std::optional<int> parse_level(std::string_view text)
{
    const auto name = lower(trim(text));
    for (std::size_t i = 0; i < level_names.size(); ++i)
    {
        if (name == lower(level_names[i])) return static_cast<int>(i + 1);
    }
    if (name.size() == 1 && name[0] >= '1' && name[0] <= '7') return name[0] - '0'; // the menu stores ranks
    if (name == "warning") return rank_warn;
    if (name == "none") return rank_off;
    return std::nullopt;
}

// A module name: 1..64 of letters, digits, '_', '-' and inner spaces; not
// "global" (the settings key of the global level). Dots separate the category.
inline bool valid_module_name(std::string_view name)
{
    if (name.empty() || name.size() > static_cast<std::size_t>(NativeContract::max_module_name)) return false;
    if (name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != ' ') return false;
    }
    return lower(name) != "global";
}

// A module name from a mod folder name: characters a name can't hold become '_'.
inline std::string module_name_from_folder(std::string_view folder)
{
    std::string name;
    for (char c : folder)
    {
        const bool kept = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        name.push_back(kept ? c : '_');
    }
    name = std::string(trim(name));
    if (name.size() > static_cast<std::size_t>(NativeContract::max_module_name)) name.resize(NativeContract::max_module_name);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (!valid_module_name(name)) return "Mod";
    return name;
}

// A category name: a letter, then up to 31 letters, digits, '_' or '-'; it is
// also a file name, so Windows device names are refused.
inline bool valid_category_name(std::string_view name)
{
    if (name.empty() || name.size() > static_cast<std::size_t>(NativeContract::max_category_name)) return false;
    if (!std::isalpha(static_cast<unsigned char>(name.front()))) return false;
    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    }
    const auto folded = lower(name);
    static constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
    for (auto device : devices)
    {
        if (folded == device) return false;
    }
    if (folded.size() == 4 && (folded.starts_with("com") || folded.starts_with("lpt")) &&
        std::isdigit(static_cast<unsigned char>(folded[3])))
    {
        return false;
    }
    return true;
}

// A settings key: "" or "global", "<module>" or "<module>.<category>".
// Returns the folded key ("" for global), or nullopt.
inline std::optional<std::string> level_key(std::string_view key)
{
    key = trim(key);
    if (key.empty() || lower(key) == "global") return std::string();
    const auto dot = key.find('.');
    if (dot == std::string_view::npos) return valid_module_name(key) ? std::optional(lower(key)) : std::nullopt;
    const auto module = key.substr(0, dot);
    const auto category = key.substr(dot + 1);
    if (!valid_module_name(module) || !valid_category_name(category)) return std::nullopt;
    return lower(module) + "." + lower(category);
}

// Whether a [Levels] value says "no level of its own": 0 (what the menu's
// Inherit stores) or "inherit", any case.
inline bool inherits(std::string_view text)
{
    const auto value = lower(trim(text));
    return value == "0" || value == "inherit";
}

using LevelTable = std::unordered_map<std::string, int>;

// Levels from config.ini: `[UE4SSLuaEXtensions.Logs] GlobalLevel=4` (the menu's
// row) is the global level (key ""), and `[Levels]` holds `Module=DEBUG` and
// `Module.category=TRACE`. Sections and keys are case-insensitive; other
// sections, comments, unknown keys and unknown levels are ignored
// (configuration never blocks the bridge), each unknown [Levels] entry and an
// unknown GlobalLevel noted in `ignored` when given. A [Levels] value of 0 or
// "inherit" sets nothing and is not noted. `names`, when given, receives the
// keys of [Levels] entries with a level or Inherit, as written, first spelling
// of each folded key, in file order.
inline LevelTable parse_level_settings(std::string_view text, std::vector<std::string>* ignored = nullptr,
    std::vector<std::string>* names = nullptr)
{
    LevelTable table;
    std::unordered_set<std::string> seen;
    bool in_levels = false;
    bool in_own = false;
    while (!text.empty())
    {
        const auto end = text.find('\n');
        auto line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[')
        {
            const auto close = line.find(']');
            const auto name = close == std::string_view::npos ? std::string() : lower(trim(line.substr(1, close - 1)));
            in_levels = name == "levels";
            in_own = name == "ue4ssluaextensions.logs";
            continue;
        }
        if (!in_levels && !in_own) continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        if (in_own)
        {
            if (lower(trim(line.substr(0, equals))) != "globallevel") continue;
            const auto rank = parse_level(line.substr(equals + 1));
            if (rank) table[""] = *rank;
            else if (ignored) ignored->emplace_back(line);
            continue;
        }
        const auto key = level_key(line.substr(0, equals));
        const auto value = line.substr(equals + 1);
        const bool inherit = inherits(value);
        const auto rank = inherit ? std::nullopt : parse_level(value);
        if (!key || key->empty() || (!rank && !inherit)) // the global level lives in the bridge's own section
        {
            if (ignored) ignored->emplace_back(line);
            continue;
        }
        if (names && !seen.contains(*key))
        {
            seen.insert(*key);
            names->emplace_back(trim(line.substr(0, equals)));
        }
        if (rank) table[*key] = *rank;
        else table.erase(*key); // a later Inherit undoes an earlier level, as a later level would
    }
    return table;
}

// Where an effective level comes from (ListLoggers). A module's: a runtime
// override, [Levels], its registered `level` option or the global level. A
// category's: its own override or [Levels] entry, else its module.
enum class LevelSource : uint8_t { override_, levels, option, global, module };

inline std::string_view source_name(LevelSource source)
{
    static constexpr std::array<std::string_view, 5> names{"override", "levels", "option", "global", "module"};
    return names[static_cast<std::size_t>(source)];
}

struct ResolvedLevel
{
    int rank{};
    LevelSource source{};
};

// Effective levels. Order, first found: runtime override of the category, of
// the module; settings of the category, of the module; the module's registered
// default; the global runtime override; the global setting; WARN.
//
// It also keeps the registry of the modules and categories registered this
// session (every Lua mod's, since this is the one native object they share)
// and the keys config.ini's [Levels] names, for ListLoggers.
class LevelResolver
{
public:
    // Replaces the settings; true when anything changed. names: the [Levels]
    // keys as written (parse_level_settings' `names`).
    bool set_settings(LevelTable table, std::vector<std::string> names = {})
    {
        std::scoped_lock lock(mutex_);
        if (table == settings_ && names == settings_names_) return false;
        settings_ = std::move(table);
        settings_names_ = std::move(names);
        ++generation_;
        return true;
    }

    // Sets (rank) or clears (nullopt) a runtime override. key as level_key returns.
    void set_override(const std::string& key, std::optional<int> rank)
    {
        std::scoped_lock lock(mutex_);
        if (rank) overrides_[key] = *rank;
        else overrides_.erase(key);
        ++generation_;
    }

    // A module's registered default (0 = none). The strongest default wins:
    // the lowest rank registered under that module name.
    void set_default(const std::string& module, int rank)
    {
        if (rank <= 0) return;
        std::scoped_lock lock(mutex_);
        const auto key = lower(module);
        const auto found = defaults_.find(key);
        if (found != defaults_.end() && found->second <= rank) return;
        defaults_[key] = rank;
        ++generation_;
    }

    // Records a registered module (with the default categories) and, when not
    // empty, one of its categories. The first spelling of a module is kept;
    // categories are folded. A new name changes the generation.
    void note(std::string_view module, std::string_view category = {})
    {
        std::scoped_lock lock(mutex_);
        const auto key = lower(module);
        auto found = known_index_.find(key);
        bool changed = false;
        if (found == known_index_.end())
        {
            Known entry{std::string(module), {}};
            for (auto name : default_categories) entry.categories.emplace_back(name);
            found = known_index_.emplace(key, known_.size()).first;
            known_.push_back(std::move(entry));
            changed = true;
        }
        if (!category.empty())
        {
            auto& categories = known_[found->second].categories;
            const auto folded = lower(category);
            if (std::find(categories.begin(), categories.end(), folded) == categories.end())
            {
                categories.push_back(folded);
                changed = true;
            }
        }
        if (changed) ++generation_;
    }

    // An empty category resolves the module itself.
    int resolve(std::string_view module, std::string_view category) const
    {
        std::scoped_lock lock(mutex_);
        return explain_locked(lower(module), lower(category)).rank;
    }

    // The effective rank of a level_key ("" = global).
    int resolve_key(const std::string& key) const
    {
        if (key.empty())
        {
            std::scoped_lock lock(mutex_);
            return global_locked();
        }
        const auto dot = key.find('.');
        if (dot == std::string::npos) return resolve(key, "");
        return resolve(key.substr(0, dot), key.substr(dot + 1));
    }

    // The effective rank of <module>.<category> (empty: the module) and where it comes from.
    ResolvedLevel explain(std::string_view module, std::string_view category) const
    {
        std::scoped_lock lock(mutex_);
        return explain_locked(lower(module), lower(category));
    }

    // Every registered module, in registration order, then the modules only
    // [Levels] names, in file order. One line per module, each followed by a
    // line per category (registered ones first, then those only [Levels]
    // names), fields separated by tabs:
    //   M <name> <rank> <source> <loaded 1|0>
    //   C <name> <rank> <source> <loaded 1|0>
    // Names never hold tabs or line breaks (valid_module_name, valid_category_name).
    std::string list() const
    {
        std::scoped_lock lock(mutex_);
        struct Row
        {
            std::string name;
            bool loaded;
            std::vector<std::pair<std::string, bool>> categories;
        };
        std::vector<Row> rows;
        std::unordered_map<std::string, std::size_t> index;
        for (const auto& known : known_)
        {
            Row row{known.name, true, {}};
            for (const auto& category : known.categories) row.categories.emplace_back(category, true);
            index.emplace(lower(known.name), rows.size());
            rows.push_back(std::move(row));
        }
        for (const auto& name : settings_names_)
        {
            const auto dot = name.find('.');
            const auto module = name.substr(0, dot);
            const auto key = lower(module);
            auto found = index.find(key);
            if (found == index.end())
            {
                found = index.emplace(key, rows.size()).first;
                rows.push_back(Row{module, false, {}});
            }
            if (dot == std::string::npos) continue;
            auto& categories = rows[found->second].categories;
            const auto category = lower(name.substr(dot + 1));
            const bool listed = std::any_of(categories.begin(), categories.end(),
                [&](const auto& entry) { return entry.first == category; });
            if (!listed) categories.emplace_back(category, false);
        }
        std::string out;
        const auto line = [&out](char kind, const std::string& name, ResolvedLevel level, bool loaded) {
            out.push_back(kind);
            out.push_back('\t');
            out.append(name);
            out.push_back('\t');
            out.append(std::to_string(level.rank));
            out.push_back('\t');
            out.append(source_name(level.source));
            out.push_back('\t');
            out.push_back(loaded ? '1' : '0');
            out.push_back('\n');
        };
        for (const auto& row : rows)
        {
            const auto module = lower(row.name);
            line('M', row.name, explain_locked(module, ""), row.loaded);
            for (const auto& [category, loaded] : row.categories)
            {
                line('C', category, explain_locked(module, category), loaded);
            }
        }
        return out;
    }

    int64_t generation() const
    {
        std::scoped_lock lock(mutex_);
        return generation_;
    }

private:
    struct Known
    {
        std::string name;
        std::vector<std::string> categories;
    };

    mutable std::mutex mutex_;
    LevelTable settings_;
    std::vector<std::string> settings_names_;
    LevelTable overrides_;
    std::unordered_map<std::string, int> defaults_;
    std::vector<Known> known_;
    std::unordered_map<std::string, std::size_t> known_index_;
    int64_t generation_ = 1;

    int global_locked() const
    {
        if (auto found = overrides_.find(std::string()); found != overrides_.end()) return found->second;
        if (auto found = settings_.find(std::string()); found != settings_.end()) return found->second;
        return default_rank;
    }

    // module and category folded; an empty category explains the module.
    ResolvedLevel explain_locked(const std::string& module, const std::string& category) const
    {
        const auto category_key = category.empty() ? std::string() : module + "." + category;
        const bool own = category.empty();
        for (const auto* table : {&overrides_, &settings_})
        {
            const auto source = table == &overrides_ ? LevelSource::override_ : LevelSource::levels;
            if (!category_key.empty())
            {
                if (auto found = table->find(category_key); found != table->end()) return {found->second, source};
            }
            if (auto found = table->find(module); found != table->end())
            {
                return {found->second, own ? source : LevelSource::module};
            }
        }
        if (auto found = defaults_.find(module); found != defaults_.end())
        {
            return {found->second, own ? LevelSource::option : LevelSource::module};
        }
        return {global_locked(), own ? LevelSource::global : LevelSource::module};
    }
};
} // namespace UE4SSLuaLogBridge::Core
