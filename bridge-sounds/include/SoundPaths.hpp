#pragma once

// Lexical rules for a sound path. Any file may be loaded: an absolute path
// (with a drive letter) as given, a relative one against the calling mod's
// folder, `..` allowed anywhere. Refusals are for robustness only: paths that
// could block the game thread (devices, network shares) or that Windows would
// read differently than written.

#include <SoundData.hpp>

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace UE4SSLuaSoundBridge::Core
{
namespace Paths
{
inline bool reserved_device(std::string_view segment)
{
    // The stem before the first dot decides (CON.wav is still CON).
    const auto stem = segment.substr(0, segment.find('.'));
    std::string upper;
    for (char c : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    while (!upper.empty() && upper.back() == ' ') upper.pop_back();
    static constexpr std::array<std::string_view, 6> names{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (const auto name : names)
    {
        if (upper == name) return true;
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' &&
        upper[3] <= '9';
}

inline bool separator(char c)
{
    return c == '/' || c == '\\';
}

// "C:/..." or "C:\...": the only absolute form accepted.
inline bool drive_absolute(std::string_view path)
{
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
        separator(path[2]);
}
} // namespace Paths

// True for a normalized path that names its drive ("C:/...").
inline bool is_absolute_sound_path(std::string_view normalized)
{
    return Paths::drive_absolute(normalized);
}

// Returns the normalized path or the refusal. Separators become '/', `.` and
// empty segments go, `..` folds into the segment before it. An absolute path
// stays absolute ("C:/Sounds/cue.wav"; the drive letter upper-cased); a
// relative one stays relative and keeps the `..` it can't fold
// ("../Other/cue.wav").
inline Outcome<std::string> normalize_sound_path(std::string_view path)
{
    const auto refuse = [&](std::string_view why) {
        return Failure{ErrorCode::invalid, std::string(path) + ": " + std::string(why)};
    };
    if (path.empty()) return refuse("the path is empty");
    if (Paths::separator(path.front()))
    {
        if (path.size() >= 2 && Paths::separator(path[1]))
        {
            return refuse("network and device paths (\\\\server, \\\\.\\, \\\\?\\) aren't loaded: they can block the game");
        }
        return refuse("a rooted path needs its drive letter (C:/...)");
    }
    const bool absolute = Paths::drive_absolute(path);
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        const char c = path[i];
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) return refuse("contains a control character");
        if (c == ':' && !(absolute && i == 1))
        {
            return refuse(i == 1 ? "a drive needs a separator after it (C:/...)"
                                 : "':' is only allowed after a drive letter (no streams)");
        }
        if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            return refuse("contains a wildcard or reserved character");
        }
    }

    std::vector<std::string> segments;
    std::size_t start = absolute ? 3 : 0;
    while (start <= path.size())
    {
        std::size_t end = start;
        while (end < path.size() && !Paths::separator(path[end])) ++end;
        const auto segment = path.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") continue;
        if (segment == "..")
        {
            if (!segments.empty() && segments.back() != "..")
            {
                segments.pop_back();
            }
            else if (absolute)
            {
                return refuse("climbs above the drive's root");
            }
            else
            {
                segments.emplace_back(segment);
            }
            continue;
        }
        if (segment.back() == '.' || segment.back() == ' ') return refuse("a segment ends in a dot or a space");
        if (Paths::reserved_device(segment)) return refuse("names a device");
        segments.emplace_back(segment);
    }
    if (segments.empty() || segments.back() == "..") return refuse("names a folder, not a file");

    std::string normalized;
    if (absolute)
    {
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(path[0]))));
        normalized += ":";
    }
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        if (i > 0 || absolute) normalized.push_back('/');
        normalized += segments[i];
    }
    return normalized;
}

// The absolute path a normalized sound path names: itself when absolute,
// otherwise joined to `mod_folder` (an absolute folder path) and normalized
// again, so a leading `..` climbs out of the mod's folder.
inline Outcome<std::string> resolve_sound_path(std::string_view normalized, std::string_view mod_folder)
{
    if (is_absolute_sound_path(normalized)) return std::string(normalized);
    if (!Paths::drive_absolute(mod_folder))
    {
        return Failure{ErrorCode::io, std::string(normalized) + ": this mod's folder is unknown; use an absolute path"};
    }
    return normalize_sound_path(std::string(mod_folder) + "/" + std::string(normalized));
}

// A mod's name is usable as its folder: exactly one plain folder name.
inline bool single_folder_name(std::string_view name)
{
    auto checked = normalize_sound_path(name);
    return checked && checked.value() == name && name != ".." && name.find('/') == std::string_view::npos &&
        !is_absolute_sound_path(name);
}
} // namespace UE4SSLuaSoundBridge::Core
