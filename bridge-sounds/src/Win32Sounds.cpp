// UE4SSLuaSoundBridge: reading sound files from a mod's own folder.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Win32Sounds.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace UE4SSLuaSoundBridge::Win32
{
namespace
{
using Core::Failure;
using NativeContract::ErrorCode;

std::wstring widen(const std::string& text)
{
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), needed);
    return wide;
}

std::string narrow(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int length = static_cast<int>(wide.size());
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string text(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, text.data(), needed, nullptr, nullptr);
    return text;
}

std::wstring backslashes(std::wstring path)
{
    for (auto& c : path)
    {
        if (c == L'/') c = L'\\';
    }
    return path;
}

class Handle
{
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle()
    {
        if (valid()) CloseHandle(handle_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    [[nodiscard]] HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

Failure last_failure(const std::string& what)
{
    const DWORD error = GetLastError();
    ErrorCode code = ErrorCode::io;
    switch (error)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH: code = ErrorCode::not_found; break;
    case ERROR_ACCESS_DENIED: code = ErrorCode::denied; break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: code = ErrorCode::busy; break;
    default: break;
    }
    return Failure{code, what + " (win32 " + std::to_string(error) + ")"};
}

std::wstring final_path(HANDLE handle)
{
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
    return {};
}

bool equal_fold(const wchar_t* a, std::size_t a_length, const wchar_t* b, std::size_t b_length)
{
    return CompareStringOrdinal(a, static_cast<int>(a_length), b, static_cast<int>(b_length), TRUE) == CSTR_EQUAL;
}

// `child` lies strictly inside `parent` (both final paths).
bool within(const std::wstring& child, const std::wstring& parent)
{
    if (parent.empty() || child.size() <= parent.size() + 1) return false;
    if (child[parent.size()] != L'\\') return false;
    return equal_fold(child.data(), parent.size(), parent.data(), parent.size());
}

std::wstring module_file_name(HMODULE module)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}
} // namespace

std::string mods_folder(const void* address_in_module)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address_in_module), &module))
    {
        return {};
    }
    auto path = module_file_name(module);
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    std::vector<std::wstring> segments;
    std::wstring current;
    for (wchar_t c : path)
    {
        if (c == L'\\' || c == L'/')
        {
            if (!current.empty()) segments.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty()) segments.push_back(current);
    // Mods/<folder>/dlls/versions/<dll>
    if (segments.size() < 6) return {};
    std::wstring mods;
    for (std::size_t i = 0; i + 4 < segments.size(); ++i)
    {
        if (!mods.empty()) mods.push_back(L'/');
        mods += segments[i];
    }
    return narrow(mods);
}

Core::Outcome<std::string> read_mod_file(const std::string& mod_folder, const std::string& relative, int64_t max_bytes)
{
    const auto folder = L"\\\\?\\" + backslashes(widen(mod_folder));
    const auto file = folder + L"\\" + backslashes(widen(relative));
    if (folder.size() <= 4 || file.size() <= folder.size() + 1)
    {
        return Failure{ErrorCode::invalid, relative + ": the path isn't valid UTF-8"};
    }

    Handle root(CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!root.valid()) return last_failure("the mod's folder " + mod_folder + " can't be opened");
    const auto root_final = final_path(root.get());
    if (root_final.empty()) return last_failure("the mod's folder " + mod_folder + " can't be resolved");

    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid())
    {
        auto failed = last_failure(relative + ": can't be opened");
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return Failure{ErrorCode::invalid, relative + ": is a folder"};
        }
        if (failed.code == ErrorCode::not_found) failed.message = relative + ": not found";
        return failed;
    }
    const auto file_final = final_path(handle.get());
    if (file_final.empty()) return last_failure(relative + ": can't be resolved");
    if (!within(file_final, root_final))
    {
        return Failure{ErrorCode::outside_root, relative + ": resolves outside the mod's folder"};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) return last_failure(relative + ": size unknown");
    if (size.QuadPart > max_bytes)
    {
        return Failure{ErrorCode::invalid, relative + ": larger than " + std::to_string(max_bytes / (1024 * 1024)) + " MiB"};
    }
    std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t done = 0;
    while (done < content.size())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - done, 1u << 24));
        DWORD read = 0;
        if (!ReadFile(handle.get(), content.data() + done, chunk, &read, nullptr)) return last_failure(relative + ": read failed");
        if (read == 0) break;
        done += read;
    }
    content.resize(done);
    return content;
}
} // namespace UE4SSLuaSoundBridge::Win32
