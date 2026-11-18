#pragma once

// Line formats. Portable.
//   UE4SS.log:     [Module] LEVEL message          (category main)
//                  [Module:category] LEVEL message
//   module files:  2026-11-18 14:03:22.123 LEVEL message
//   recovery file: 2026-11-18 14:03:22.123 LEVEL [Module:category] message
// Times are local wall-clock times packed as yyyymmddhhmmssmmm in an int64.

#include <LogLevels.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace UE4SSLuaLogBridge::Core
{
inline constexpr std::string_view main_category = "main";

struct CivilTime
{
    int year{}, month{}, day{}, hour{}, minute{}, second{}, millisecond{};
};

inline int64_t pack_time(const CivilTime& t)
{
    return ((((((static_cast<int64_t>(t.year) * 100 + t.month) * 100 + t.day) * 100 + t.hour) * 100 + t.minute) * 100 +
                t.second) *
               1000 +
        t.millisecond);
}

inline CivilTime unpack_time(int64_t packed)
{
    CivilTime t;
    t.millisecond = static_cast<int>(packed % 1000);
    packed /= 1000;
    t.second = static_cast<int>(packed % 100);
    packed /= 100;
    t.minute = static_cast<int>(packed % 100);
    packed /= 100;
    t.hour = static_cast<int>(packed % 100);
    packed /= 100;
    t.day = static_cast<int>(packed % 100);
    packed /= 100;
    t.month = static_cast<int>(packed % 100);
    t.year = static_cast<int>(packed / 100);
    return t;
}

inline std::string format_time(int64_t packed)
{
    const auto t = unpack_time(packed);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.year % 10000, t.month, t.day, t.hour,
        t.minute, t.second, t.millisecond);
    return buffer;
}

// One trailing newline is dropped: print-style callers add their own.
inline std::string_view message_body(std::string_view message)
{
    if (!message.empty() && message.back() == '\n') message.remove_suffix(1);
    if (!message.empty() && message.back() == '\r') message.remove_suffix(1);
    return message;
}

inline std::string source_tag(std::string_view module, std::string_view category)
{
    std::string tag("[");
    tag.append(module);
    if (!category.empty() && category != main_category) tag.append(":").append(category);
    tag.append("]");
    return tag;
}

// Without the newline: UE4SS adds its own.
inline std::string ue4ss_line(std::string_view module, std::string_view category, int rank, std::string_view message)
{
    std::string line = source_tag(module, category);
    line.append(" ").append(level_name(rank)).append(" ").append(message_body(message));
    return line;
}

inline std::string file_line(int64_t time, int rank, std::string_view message)
{
    std::string line = format_time(time);
    line.append(" ").append(level_name(rank)).append(" ").append(message_body(message)).append("\n");
    return line;
}

// `marker` is "*" for a line that never reached its target.
inline std::string recovery_line(int64_t time, int rank, std::string_view module, std::string_view category,
    std::string_view message, bool undelivered)
{
    std::string line = format_time(time);
    line.append(undelivered ? " * " : " ").append(level_name(rank)).append(" ").append(source_tag(module, category));
    line.append(" ").append(message_body(message)).append("\n");
    return line;
}
} // namespace UE4SSLuaLogBridge::Core
