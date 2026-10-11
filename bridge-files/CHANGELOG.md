# Changelog

## Unreleased

### Added

- Load-time verifications: in-game checks of the bridge API shipped as
  `Scripts/main.lua` in the mod folder, with a ModCore Settings page whose
  **Enable load-time verifications** toggle turns them on (off by default).
  Results go to UE4SS.log.

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
