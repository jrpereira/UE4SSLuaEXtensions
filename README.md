# UE4SS Lua Extensions

UE4SS runs Lua mods inside the game, but Lua alone cannot reach everything a mod may need. Lua cannot ask Unreal's input system for native Tap and Hold triggers, cannot tell whether an object it still holds has been freed, has no notion of which files a mod should be allowed to touch, cannot play a sound the game doesn't ship, and has no log that survives a crash. UE4SS Lua Extensions are native C++ mods that add these abilities to UE4SS and expose them to other Lua mods through small Lua APIs.

There are four independent extensions. Each has its own mod folder, version and documentation. A mod depends on whichever it uses, and you can keep all of them installed. They do nothing on their own: install them when a mod asks for them.

## UE4SS Lua Event Bridge (1.3.0)

Connects Unreal Enhanced Input to UE4SS Lua callbacks. Bind existing Input Actions or create key bindings with native Tap and Hold triggers: Unreal handles the stopwatch, your mod handles the action. Lua receives copied event values outside Unreal's native input-dispatch stack.

It also provides native object-lifetime validation and weak handles, so a mod can tell that an object has been freed instead of reading it, plus session-owned handles, explicit cleanup, binding inspection, dispatch statistics, configurable queue budgets and a callback when the game window regains focus.

Mod folder: `0_ModCore_UE4SSLuaEventBridge`. Documentation: [Event Bridge README](bridge-events/README.md).

## UE4SS Lua File Bridge (1.1.0)

Gives Lua mods file and folder access inside the game's own folders. Each mod gets named locations (its own folder, a per-mod data folder, a scratch folder and the save folder), explicit write grants, atomic writes and file sockets that let mods talk through a path. Lua already has `io`; this is `io` with a seatbelt. The sandbox prevents mistakes; it is not a security boundary.

Mod folder: `0_ModCore_UE4SSLuaFileBridge`. Documentation: [File Bridge README](bridge-files/README.md).

## UE4SS Lua Sound Bridge (1.1.0)

Lets Lua mods play WAV files outside the game's audio engine. A mod loads sounds once, from its own folder or any path, then plays them at once with pan, volume, pitch and a low-pass filter chosen per play; plays overlap, and playback follows the default output device when headphones are plugged in or out. Built for cues that must land with what is on screen, such as a sound that tells which side an attack comes from.

Mod folder: `0_ModCore_UE4SSLuaSoundBridge`. Documentation: [Sound Bridge README](bridge-sounds/README.md).

## UE4SS Lua Log Bridge (1.0.0)

Leveled logging for Lua mods: TRACE to CRITICAL, WARN by default, set per mod, per category or globally from config.ini, the ModCore Settings menu or Lua, and applied while the game runs. Lines go to UE4SS.log or to the mod's own `logs/<category>.log`, written by the bridge's own thread. Every line reaches a memory-mapped journal before the call returns, so a crash loses nothing: the next start writes the previous session's lines to `logs/previous-session.log`.

Mod folder: `0_ModCore_UE4SSLuaLogBridge`. Documentation: [Log Bridge README](bridge-logs/README.md).

## Installation

The Nexus download is one ZIP holding all four extensions. Extract it into the UE4SS `Mods` directory, or install it with your mod manager. It adds a folder for each extension and a fifth, Lua-only folder for the menu:

```text
Mods/
├── 0_ModCore_UE4SSLuaEXtensions/
├── 0_ModCore_UE4SSLuaEventBridge/
├── 0_ModCore_UE4SSLuaFileBridge/
├── 0_ModCore_UE4SSLuaLogBridge/
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
- [Log Bridge README](bridge-logs/README.md) and [API reference](bridge-logs/docs/API.md)
- Source, issues and releases: [GitHub](https://github.com/jrpereira/UE4SSLuaEXtensions)
