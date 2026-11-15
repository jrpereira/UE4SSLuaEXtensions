#pragma once

// File access for sound loading. No <windows.h> in this header
// (src/Win32Sounds.cpp has it).

#include <SoundData.hpp>

#include <cstdint>
#include <string>

namespace UE4SSLuaSoundBridge::Win32
{
// The UE4SS Mods folder as UTF-8 with '/' separators: four segments above the
// DLL holding `address_in_module` (Mods/<folder>/dlls/versions/<dll>). Empty
// when it can't be found.
std::string mods_folder(const void* address_in_module);

// Reads the file at `absolute` (a resolved "C:/..." path; links followed, no
// folder restriction). `shown` names it in messages. Folders, non-disk files
// and files over max_bytes are `invalid`.
Core::Outcome<std::string> read_sound_file(const std::string& absolute, const std::string& shown, int64_t max_bytes);
} // namespace UE4SSLuaSoundBridge::Win32
