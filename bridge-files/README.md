# UE4SS Lua File Bridge

UE4SSLuaFileBridge gives UE4SS Lua mods file and folder access inside the game's
own folders. Each mod gets named locations, explicit write grants, atomic writes
and file sockets that let mods talk through a path. Lua already has `io`; this
is `io` with a seatbelt.

## Features

- Named locations: the mod's own folder, a per-mod data folder that outlives it,
  a scratch folder emptied at start, and the game's save folder.
- Access environments: read-only by default outside the mod's own folder;
  writing elsewhere is granted per path, mode and extension.
- Atomic writes by default; save files keep one `.bak` on every overwrite.
- Append-only grants for logs, and write streams that others can read while open.
- File sockets: `Tail` calls a mod back with each line another mod appends.
- Errors as `nil, code, message` with eight stable codes.

The sandbox prevents mistakes; it isn't a security boundary. Any Lua mod can
still use the standard library to write anywhere the user can.

## Compatibility

Windows x64, Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit `97b7e501`,
the same pin as UE4SSLuaEventBridge. Native layouts are ABI-pinned; nearby
UE4SS builds are not assumed compatible. Check `GetCapabilities()` for API
features rather than inferring them from the product version.

## Installation layout

Download the versioned ZIP and checksum from
[GitHub Releases](https://github.com/jrpereira/UE4SSLuaEXtensions/releases)
(releases tagged `bridge-files/v*`). Extract the ZIP into the UE4SS `Mods`
directory to produce:

```text
Mods/
└── 0_ModCore_UE4SSLuaFileBridge/
    ├── dlls/
    │   ├── main.dll
    │   ├── main.json
    │   └── versions/
    │       └── UE4SSLuaFileBridge-1.0.1.dll
    ├── enabled.txt
    └── mod.json
```

`main.dll` is a small bootstrap. It reads `dlls/main.json`, validates the
selected implementation's embedded product, version, private ABI, UE4SS commit
and Unreal target, then loads it from `dlls/versions`. A missing configuration
or `"version": "auto"` selects the highest compatible discovered version. A
malformed configuration or unavailable exact version fails closed. `mod.json`
describes the module to ModCoreSettings.

Use the published SHA-256 checksum to verify the exact archive bytes.

For deterministic startup before the Lua mods that use the bridge, list it in
`Mods/mods.txt` ahead of them, after the event bridge if both are installed:

```text
0_ModCore_UE4SSLuaEventBridge : 1
0_ModCore_UE4SSLuaFileBridge : 1
```

The packaged `enabled.txt` marker also enables the bridge, but UE4SS loads
marker-enabled mods in a separate pass with no defined ordering. A `mods.txt`
entry is preferred when another mod needs the bridge during its Lua-state setup.

Keep only one active installation of the file bridge.

## Minimal Lua example

```lua
local files = UE4SSLuaFileBridge()          -- safe set: read-write on this mod's folder

local ok, code, message = files.WriteText("mod/state.json", '{"runs":1}')
if not ok then
    print(string.format("write failed: %s %s\n", code, message))
end

local text = files.ReadText("mod/state.json")

-- Saves are read-only until granted.
local saves = files.AddPath("savegames", { mode = "rw", extensions = { "sav" } })
```

File operations don't need the game thread. `Tail` callbacks run from the
bridge's update callback, not on the game thread. The examples are untested in
the game until the first in-game acceptance run.

## Documentation

- [Developer guide](docs/DEVELOPERS.md): concepts, worked examples, lifecycle,
  debugging and limits.
- [API reference](docs/API.md): every function, option, error code and path
  rule.
- [Changelog](CHANGELOG.md): version history.
- [FileRoundTrip sample](examples/FileRoundTrip/Scripts/main.lua): write, read,
  list and remove, plus a refusal.
- [File socket samples](examples/FileSocketWriter/Scripts/main.lua) and
  [reader](examples/FileSocketReader/Scripts/main.lua): one mod writes, another
  tails.
