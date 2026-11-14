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

// Reads <mod_folder>/<relative> (relative: SoundPaths-normalized). The file's
// final path (links followed) must lie inside the mod folder's final path,
// otherwise `outside_root`. Files over max_bytes are `invalid`.
Core::Outcome<std::string> read_mod_file(const std::string& mod_folder, const std::string& relative, int64_t max_bytes);
} // namespace UE4SSLuaSoundBridge::Win32
