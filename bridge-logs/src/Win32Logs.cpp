// UE4SSLuaLogBridge: files, folders, clocks and the mapped journal.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <UE4SSLuaABI.hpp>

#include <Win32Logs.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace UE4SSLuaLogBridge::Win32
{
namespace
{
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

// Invalid UTF-8 becomes U+FFFD rather than dropping the line.
std::wstring widen_lossy(const std::string& text)
{
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), needed);
    return wide;
}

void send_ue4ss(const std::string& line)
{
    RC::Output::send(widen_lossy(line + "\n"));
}

// "C:/a/b" -> "\\?\C:\a\b", so long paths work.
std::wstring native_path(const std::string& path)
{
    auto wide = widen(path);
    if (wide.empty()) return {};
    for (auto& c : wide)
    {
        if (c == L'/') c = L'\\';
    }
    if (wide.size() >= 2 && wide[1] == L':') return L"\\\\?\\" + wide;
    return wide;
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

bool write_all(HANDLE handle, const std::string& text)
{
    std::size_t done = 0;
    while (done < text.size())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - done, 1u << 24));
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + done, chunk, &written, nullptr) || written == 0) return false;
        done += written;
    }
    return true;
}

HANDLE open_append(const std::wstring& path)
{
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
} // namespace

Folders folders(const void* address_in_module)
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
    Folders found;
    found.mods = narrow(mods);
    found.own = found.mods + "/" + narrow(segments[segments.size() - 4]);
    return found;
}

bool make_folder(const std::string& path)
{
    const auto native = native_path(path);
    if (native.empty()) return false;
    if (CreateDirectoryW(native.c_str(), nullptr)) return true;
    const DWORD attributes = GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes)
{
    const auto native = native_path(path);
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<std::string> content;
    if (GetFileSizeEx(handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) <= max_bytes)
    {
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (bytes.empty() || ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        {
            bytes.resize(read);
            content = std::move(bytes);
        }
    }
    CloseHandle(handle);
    return content;
}

int64_t file_stamp(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(native_path(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>(stamp.QuadPart ^ (static_cast<uint64_t>(data.nFileSizeLow) << 1));
}

bool write_file(const std::string& path, const std::string& text)
{
    HANDLE handle = CreateFileW(native_path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(handle, text);
    CloseHandle(handle);
    return ok;
}

bool append_file(const std::string& path, const std::string& text)
{
    HANDLE handle = open_append(native_path(path));
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(handle, text);
    CloseHandle(handle);
    return ok;
}

Core::CivilTime local_time()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return Core::CivilTime{now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds};
}

int64_t monotonic_ms()
{
    return static_cast<int64_t>(GetTickCount64());
}

MappedFile::~MappedFile()
{
    if (view_)
    {
        FlushViewOfFile(view_, 0);
        UnmapViewOfFile(view_);
    }
    if (mapping_) CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool MappedFile::open(const std::string& path, std::size_t bytes, std::string& why)
{
    HANDLE file = CreateFileW(native_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        why = "can't open " + path + " (win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    file_ = file;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != static_cast<LONGLONG>(bytes))
    {
        LARGE_INTEGER wanted{};
        wanted.QuadPart = static_cast<LONGLONG>(bytes);
        if (!SetFilePointerEx(file, wanted, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
        {
            why = "can't size " + path + " (win32 " + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    const auto high = static_cast<DWORD>(static_cast<uint64_t>(bytes) >> 32);
    const auto low = static_cast<DWORD>(static_cast<uint64_t>(bytes) & 0xFFFFFFFFu);
    mapping_ = CreateFileMappingW(file, nullptr, PAGE_READWRITE, high, low, nullptr);
    if (!mapping_)
    {
        why = "can't map " + path + " (win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes));
    if (!view_)
    {
        why = "can't map a view of " + path + " (win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

FileSink::~FileSink()
{
    std::scoped_lock lock(mutex_);
    for (auto& [_, handle] : handles_)
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle));
    }
    handles_.clear();
}

void FileSink::ue4ss(const std::string& line)
{
    send_ue4ss(line);
}

void FileSink::file(const std::string& folder, const std::string& category, const std::string& text)
{
    std::scoped_lock lock(mutex_);
    const auto path = folder + "/logs/" + category + ".log";
    auto found = handles_.find(path);
    if (found == handles_.end())
    {
        make_folder(folder + "/logs");
        const auto native = native_path(path);
        const auto previous = native_path(folder + "/logs/" + category + ".prev.log");
        if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            MoveFileExW(native.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        found = handles_.emplace(path, open_append(native)).first;
        if (found->second == INVALID_HANDLE_VALUE && !reported_)
        {
            reported_ = true;
            send_ue4ss("[UE4SSLuaLogBridge] can't open " + path + " (win32 " + std::to_string(GetLastError()) +
                "); its lines go to UE4SS.log instead");
        }
    }
    if (found->second != INVALID_HANDLE_VALUE && write_all(static_cast<HANDLE>(found->second), text)) return;
    // A file that can't be written: the lines still reach UE4SS.log.
    std::size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        send_ue4ss("[" + category + ".log] " + text.substr(start, end - start));
        start = end + 1;
    }
}
} // namespace UE4SSLuaLogBridge::Win32
