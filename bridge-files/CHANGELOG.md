# Changelog

## Unreleased

### Changes

- With UE4SS's engine-tick detour (the default), `Tail` callbacks run on the
  game thread, once per frame from the engine tick, the thread that also runs
  every UE4SSLuaEventBridge callback, so a mod that uses both bridges never
  has two of their callbacks in its Lua state at once. Files are still read
  from UE4SS's update thread. Without the detour, callbacks run from the
  update callback as before.
- With the engine tick, the bridge DLL stays loaded until the game exits:
  UE4SS cannot unregister the tick callback. Lua-mod hot reload is
  unaffected.
- The default `Tail` delivery time limit drops from 2,000 to 500 microseconds
  per pass, since a pass now runs every frame on the game thread
  (`UE4SSLFB_MAX_DISPATCH_US` still overrides it).

## v1.0.1

First release of UE4SSLuaFileBridge, file and folder access for UE4SS Lua mods
through a native bridge.

### Added

- Named locations: the `game` and `user` roots, and the `mod`, `moddata`,
  `temp` and `savegames` presets, with `Path`, `Locations` and lexical path
  helpers.
- Access environments: `UE4SSLuaFileBridge()` returns a safe environment
  (read-only on both roots, read-write on the mod's own folder and `temp`), and
  `AddPath` derives immutable environments with `stat`, `ro`, `wo`, `rw` or
  append-only `ao` grants, extension lists, `delete` and `recursive`.
- File operations: `Exists`, `Stat`, `List`, `MakeDir`, `ReadText`,
  `ReadBytes`, `WriteText` (atomic by default), `Append`, `Remove`,
  `RemoveTree`, `Copy` and `Move`, with errors returned as `nil, code, message`.
- `savegames` keeps one `.bak` on every overwrite and reports `busy` instead of
  waiting while the game holds a save open.
- Streams (`Open`) and file sockets (`Tail`): one mod writes, another is called
  back with each new line within a per-pass dispatch budget.
- `GetCapabilities`, `GetVersion` and `GetDispatchStats`.
- Path policy: `..` escapes, absolute paths outside both roots, device and UNC
  paths, and links that leave the roots are refused.
- Load-time verifications: in-game checks of this API, shipped as
  `Scripts/main.lua` with the shared `Scripts/lex_verify.lua` harness, and a
  ModCore Settings page whose **Enable load-time verifications** toggle turns
  them on (off by default). Results go to UE4SS.log.
- The package ships `mod.json` (with its version) and `mod_settings.ini`, so
  ModCore Settings lists the bridge with its name, category and version.
