# UE4SS Lua Extensions

UE4SS runs Lua mods inside the game, but Lua alone cannot reach everything a mod may need. Lua cannot ask Unreal's input system for native Tap and Hold triggers, cannot tell whether an object it still holds has been freed, has no notion of which files a mod should be allowed to touch, and cannot play a sound the game doesn't ship. UE4SS Lua Extensions are native C++ mods that add these abilities to UE4SS and expose them to other Lua mods through small Lua APIs.

There are three independent extensions. Each has its own mod folder, version and documentation. A mod depends on whichever it uses, and you can keep all of them installed. They do nothing on their own: install them when a mod asks for them.

## UE4SS Lua Event Bridge

Connects Unreal Enhanced Input to UE4SS Lua callbacks. Bind existing Input Actions or create key bindings with native Tap and Hold triggers: Unreal handles the stopwatch, your mod handles the action. Lua receives copied event values outside Unreal's native input-dispatch stack.

It also provides native object-lifetime validation and weak handles, so a mod can tell that an object has been freed instead of reading it, plus session-owned handles, explicit cleanup, binding inspection, dispatch statistics and configurable queue budgets.

Mod folder: `0_ModCore_UE4SSLuaEventBridge`. Documentation: [Event Bridge README](bridge-events/README.md).

## UE4SS Lua File Bridge

Gives Lua mods file and folder access inside the game's own folders. Each mod gets named locations (its own folder, a per-mod data folder, a scratch folder and the save folder), explicit write grants, atomic writes and file sockets that let mods talk through a path. Lua already has `io`; this is `io` with a seatbelt. The sandbox prevents mistakes; it is not a security boundary.

Mod folder: `0_ModCore_UE4SSLuaFileBridge`. Documentation: [File Bridge README](bridge-files/README.md).

## UE4SS Lua Sound Bridge

Lets Lua mods play their own WAV files, outside the game's audio engine. A mod loads sounds from its own folder once, then plays them at once with pan, volume, pitch and a low-pass filter chosen per play; plays overlap, and playback follows the default output device when headphones are plugged in or out. Built for cues that must land with what is on screen, such as a sound that tells which side an attack comes from.

Mod folder: `0_ModCore_UE4SSLuaSoundBridge`. Documentation: [Sound Bridge README](bridge-sounds/README.md).

## Installation

The Nexus download is one ZIP holding all three extensions. Extract it into the UE4SS `Mods` directory, or install it with your mod manager. It adds a folder for each extension and a third, Lua-only folder for the menu:

```text
Mods/
├── 0_ModCore_UE4SSLuaEXtensions/
├── 0_ModCore_UE4SSLuaEventBridge/
├── 0_ModCore_UE4SSLuaFileBridge/
└── 0_ModCore_UE4SSLuaSoundBridge/
```

`0_ModCore_UE4SSLuaEXtensions` lists the installed extensions in ModCore Settings' mod list as one module, ModCore Lua Extensions. Its page shows how many extensions are installed, then a section for each one holding that extension's own settings (from its mod_settings.ini). The extensions are then not listed as separate mods. Without ModCore Settings, or without this folder, the extensions work the same and are listed on their own.

Each extension folder's `docs` subfolder holds the README, developer guide, examples and license for mod authors.

Each extension ships an `enabled.txt` marker, so it loads without any `mods.txt` entry. The extensions are native DLL mods, which UE4SS loads before Lua mods, and their `0_` folder names sort ahead of the mods that use them. Keep only one active installation of each extension. Each extension is also released separately on [GitHub](https://github.com/jrpereira/UE4SSLuaEXtensions/releases).

## Compatibility

Windows x64 (the Sound Bridge needs Windows 10 or later), Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit `97b7e501`. Native layouts are pinned to that build; nearby engine or UE4SS builds are not assumed compatible. Check `GetCapabilities()` in each extension's Lua API for available features rather than inferring them from a version number.

## Documentation

- [Event Bridge README](bridge-events/README.md) and [developer guide](bridge-events/docs/DEVELOPERS.md)
- [File Bridge README](bridge-files/README.md), [developer guide](bridge-files/docs/DEVELOPERS.md) and [API reference](bridge-files/docs/API.md)
- [Sound Bridge README](bridge-sounds/README.md) and [API reference](bridge-sounds/docs/API.md)
- Source, issues and releases: [GitHub](https://github.com/jrpereira/UE4SSLuaEXtensions)
