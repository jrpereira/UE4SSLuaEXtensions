#pragma once

// Level ranks, module and category names, the [Levels] section of settings.ini
// and the resolution of a <module>.<category>'s effective level. Portable.

#include <LogBridgeNativeContract.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

using LevelTable = std::unordered_map<std::string, int>;

// Levels from config.ini: `[UE4SSLuaEXtensions.Logs] GlobalLevel=4` (the menu's
// row) is the global level (key ""), and `[Levels]` holds `Module=DEBUG` and
// `Module.category=TRACE`. Sections and keys are case-insensitive; other
// sections, comments, unknown keys and unknown levels are ignored
// (configuration never blocks the bridge), each unknown [Levels] entry and an
// unknown GlobalLevel noted in `ignored` when given.
inline LevelTable parse_level_settings(std::string_view text, std::vector<std::string>* ignored = nullptr)
{
    LevelTable table;
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
        const auto rank = parse_level(line.substr(equals + 1));
        if (!key || key->empty() || !rank) // the global level lives in the bridge's own section
        {
            if (ignored) ignored->emplace_back(line);
            continue;
        }
        table[*key] = *rank;
    }
    return table;
}

// Effective levels. Order, first found: runtime override of the category, of
// the module; settings of the category, of the module; the module's registered
// default; the global runtime override; the global setting; WARN.
class LevelResolver
{
public:
    // Replaces the settings; true when anything changed.
    bool set_settings(LevelTable table)
    {
        std::scoped_lock lock(mutex_);
        if (table == settings_) return false;
        settings_ = std::move(table);
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

    // An empty category resolves the module itself.
    int resolve(std::string_view module, std::string_view category) const
    {
        const auto module_key = lower(module);
        const auto category_key = category.empty() ? std::string() : module_key + "." + lower(category);
        std::scoped_lock lock(mutex_);
        for (const auto* table : {&overrides_, &settings_})
        {
            if (!category_key.empty())
            {
                if (auto found = table->find(category_key); found != table->end()) return found->second;
            }
            if (auto found = table->find(module_key); found != table->end()) return found->second;
        }
        if (auto found = defaults_.find(module_key); found != defaults_.end()) return found->second;
        if (auto found = overrides_.find(std::string()); found != overrides_.end()) return found->second;
        if (auto found = settings_.find(std::string()); found != settings_.end()) return found->second;
        return default_rank;
    }

    // The effective rank of a level_key ("" = global).
    int resolve_key(const std::string& key) const
    {
        if (key.empty())
        {
            std::scoped_lock lock(mutex_);
            if (auto found = overrides_.find(key); found != overrides_.end()) return found->second;
            if (auto found = settings_.find(key); found != settings_.end()) return found->second;
            return default_rank;
        }
        const auto dot = key.find('.');
        if (dot == std::string::npos) return resolve(key, "");
        return resolve(key.substr(0, dot), key.substr(dot + 1));
    }

    int64_t generation() const
    {
        std::scoped_lock lock(mutex_);
        return generation_;
    }

private:
    mutable std::mutex mutex_;
    LevelTable settings_;
    LevelTable overrides_;
    std::unordered_map<std::string, int> defaults_;
    int64_t generation_ = 1;
};
} // namespace UE4SSLuaLogBridge::Core
