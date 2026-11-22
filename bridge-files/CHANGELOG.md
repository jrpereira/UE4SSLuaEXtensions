# Changelog

## Unreleased

### Changes

- Breaking: settings move from `settings.ini` `[Settings]` to `config.ini`
  `[UE4SSLuaEXtensions.Files]` (`EnableVerifications`). `settings.ini`
  is no longer read, so switches return to their defaults until set again.

## v1.1.0

### Changes

- Breaking: the verification switch in `settings.ini` is now `[Settings]
  EnableVerifications`, replacing `[Verifications] LoadTime`. The old key is
  ignored, so verifications stay off until switched on again.
- `Tail` callbacks run on the game thread once per frame, like every
  UE4SSLuaEventBridge callback, so a mod that uses both bridges never has two
  of their callbacks running at once. With UE4SS's engine-tick hook turned
  off, they run off the game thread as before.
- The bridge stays loaded until the game exits.
- The default `Tail` delivery time limit drops from 2,000 to 500 microseconds
  per frame (`UE4SSLFB_MAX_DISPATCH_US` still overrides it).

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
- Load-time verifications: in-game checks of this API, turned on by the
  **Enable load-time verifications** toggle on its ModCore Settings page (off
  by default). Results go to UE4SS.log.
- ModCore Settings lists the bridge with its name, category and version.
