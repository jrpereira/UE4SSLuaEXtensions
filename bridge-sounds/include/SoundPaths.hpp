#pragma once

// Lexical rules for a sound path: relative to the calling mod's folder,
// read-only, never leaving it. The native side then checks the resolved file
// (links followed) against the folder's own final path.

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
} // namespace Paths

// Returns the normalized relative path ("sounds/cue.wav") or the refusal.
inline Outcome<std::string> normalize_sound_path(std::string_view path)
{
    const auto refuse = [&](ErrorCode code, std::string_view why) {
        return Failure{code, std::string(path) + ": " + std::string(why)};
    };
    if (path.empty()) return refuse(ErrorCode::invalid, "the path is empty");
    if (path.front() == '/' || path.front() == '\\')
    {
        return refuse(ErrorCode::invalid, "must be relative to the mod's folder, not absolute");
    }
    for (char c : path)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) return refuse(ErrorCode::invalid, "contains a control character");
        if (c == ':') return refuse(ErrorCode::invalid, "':' isn't allowed (no drives or streams)");
        if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            return refuse(ErrorCode::invalid, "contains a wildcard or reserved character");
        }
    }
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size())
    {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = path.size();
        const auto segment = path.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") continue;
        if (segment == "..")
        {
            if (segments.empty()) return refuse(ErrorCode::outside_root, "climbs out of the mod's folder");
            segments.pop_back();
            continue;
        }
        if (segment.back() == '.' || segment.back() == ' ')
        {
            return refuse(ErrorCode::invalid, "a segment ends in a dot or a space");
        }
        if (Paths::reserved_device(segment)) return refuse(ErrorCode::outside_root, "names a device");
        segments.emplace_back(segment);
    }
    if (segments.empty()) return refuse(ErrorCode::invalid, "names the mod's folder, not a file");
    std::string normalized;
    for (const auto& segment : segments)
    {
        if (!normalized.empty()) normalized.push_back('/');
        normalized += segment;
    }
    return normalized;
}
} // namespace UE4SSLuaSoundBridge::Core
