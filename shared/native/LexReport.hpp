#pragma once

// Writes "[<tag>] <message>" to the debugger and to UE4SS.log. Messages are
// UTF-8 and reach UE4SS.log as UTF-16, so non-ASCII mod and path names
// survive. Never throws.

#include <UE4SSLuaABI.hpp>

#ifdef _WIN32

#include <string>
#include <string_view>

#if defined(_MSC_VER)
#define LEX_WINAPI __stdcall
#else
#define LEX_WINAPI
#endif
extern "C" __declspec(dllimport) void LEX_WINAPI OutputDebugStringA(const char* message);
extern "C" __declspec(dllimport) int LEX_WINAPI MultiByteToWideChar(
    unsigned int code_page, unsigned long flags, const char* text, int length, wchar_t* wide, int wide_length);
#undef LEX_WINAPI

namespace Lex
{
// Invalid UTF-8 becomes U+FFFD rather than being dropped.
inline std::wstring utf8_to_wide(std::string_view text)
{
    constexpr unsigned int utf8 = 65001;
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(utf8, 0, text.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(utf8, 0, text.data(), length, wide.data(), needed);
    return wide;
}

inline void report(std::string_view tag, std::string_view message)
{
    try
    {
        std::string line;
        line.reserve(tag.size() + message.size() + 4);
        line.append("[").append(tag).append("] ").append(message).push_back('\n');
        OutputDebugStringA(line.c_str());
        RC::Output::send(utf8_to_wide(line));
    }
    catch (...)
    {
    }
}
}

#endif
