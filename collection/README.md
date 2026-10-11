# UE4SS Lua Extensions {{COLLECTION_VERSION}}

UE4SS runs Lua mods inside the game, but Lua alone cannot reach everything a mod
may need. Lua cannot ask Unreal's input system for native Tap and Hold triggers,
cannot tell whether an object it still holds has been freed, and has no notion of
which files a mod should be allowed to touch. UE4SS Lua Extensions are native C++
mods that add these abilities to UE4SS and expose them to other Lua mods through
small Lua APIs.

This package holds two independent extensions. Each has its own mod folder,
version, changelog and documentation. A mod depends on whichever it uses, and
you can keep both installed.

| Extension | Mod folder | Version |
| --- | --- | --- |
| UE4SS Lua Event Bridge | `0_ModCore_UE4SSLuaEventBridge` | {{EVENT_BRIDGE_VERSION}} |
| UE4SS Lua File Bridge | `0_ModCore_UE4SSLuaFileBridge` | {{FILE_BRIDGE_VERSION}} |

## UE4SS Lua Event Bridge

Connects Unreal Enhanced Input to UE4SS Lua callbacks. Bind existing Input
Actions or create key bindings with native Tap and Hold triggers: Unreal handles
the stopwatch, your mod handles the action. Lua receives copied event values
outside Unreal's native input-dispatch stack.

It also provides native object-lifetime validation and weak handles, so a mod can
tell that an object has been freed instead of reading it, plus session-owned
handles, explicit cleanup, binding inspection, dispatch statistics and
configurable queue budgets.

Documentation: `docs/UE4SSLuaEventBridge/README.md`, beside this file

## UE4SS Lua File Bridge

Gives Lua mods file and folder access inside the game's own folders. Each mod gets
named locations (its own folder, a per-mod data folder, a scratch folder and the
save folder), explicit write grants, atomic writes and file sockets that let mods
talk through a path. Lua already has `io`; this is `io` with a seatbelt. The
sandbox prevents mistakes; it is not a security boundary.

Documentation: `docs/UE4SSLuaFileBridge/README.md`

## Installation

Extract this ZIP into the UE4SS `Mods` directory. It adds both mod folders and a
third folder of shared documentation:

```text
Mods/
├── 0_ModCore_UE4SSLuaEXtensions/   this README, the LICENSE and docs/
├── 0_ModCore_UE4SSLuaEventBridge/
└── 0_ModCore_UE4SSLuaFileBridge/
```

`0_ModCore_UE4SSLuaEXtensions` is documentation only and is not a mod; you may
delete it after reading. For deterministic startup before the Lua mods that use
the extensions, list them first in `Mods/mods.txt`:

```text
0_ModCore_UE4SSLuaEventBridge : 1
0_ModCore_UE4SSLuaFileBridge : 1
```

Each extension also ships an `enabled.txt` marker. Keep only one active
installation of each extension.

## Compatibility

Windows x64, Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit `97b7e501`.
Native layouts are pinned to that build; nearby engine or UE4SS builds are not
assumed compatible. Check `GetCapabilities()` in each extension's Lua API for
available features rather than inferring them from a version number.

## Documentation

- `docs/UE4SSLuaEventBridge/`: README, changelog, developer guide and maintainer guide.
- `docs/UE4SSLuaFileBridge/`: README, changelog, developer guide and API reference.
- Source, issues and releases: https://github.com/jrpereira/UE4SSLuaEXtensions
