#pragma once

// Files, folders, clocks and the mapped journal for the log bridge. No
// <windows.h> in this header (src/Win32Logs.cpp has it). Paths are UTF-8 with
// '/' separators.

#include <LogCore.hpp>
#include <LogFormat.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace UE4SSLuaLogBridge::Win32
{
struct Folders
{
    std::string mods; // <...>/Mods
    std::string own;  // <...>/Mods/<this mod's folder>
};

// From the DLL holding `address_in_module` (Mods/<folder>/dlls/versions/<dll>).
// Empty strings when they can't be found.
Folders folders(const void* address_in_module);

// Creates one folder (its parent must exist); true when it exists afterwards.
bool make_folder(const std::string& path);
// A small file's bytes, or nullopt when it can't be read or is over max_bytes.
std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes);
// The file's last-write time (0 when missing).
int64_t file_stamp(const std::string& path);
// Replaces the file's content.
bool write_file(const std::string& path, const std::string& text);
// Appends to the file, creating it.
bool append_file(const std::string& path, const std::string& text);

Core::CivilTime local_time();
int64_t monotonic_ms();

// A file mapped read-write, shared with no other writer: a second game
// process gets an error and the bridge uses memory instead.
class MappedFile
{
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();
    // Opens or creates `path` at exactly `bytes`; `why` says what failed.
    bool open(const std::string& path, std::size_t bytes, std::string& why);
    [[nodiscard]] uint8_t* data() const { return view_; }

private:
    void* file_{};
    void* mapping_{};
    uint8_t* view_{};
};

// UE4SS.log through RC::Output, and <folder>/logs/<category>.log files kept
// open for appending. A file's previous session becomes <category>.prev.log on
// its first write.
class FileSink final : public Core::Sink
{
public:
    FileSink() = default;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    ~FileSink() override;
    void ue4ss(const std::string& line) override;
    void file(const std::string& folder, const std::string& category, const std::string& text) override;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, void*> handles_;
    bool reported_{};
};
} // namespace UE4SSLuaLogBridge::Win32
