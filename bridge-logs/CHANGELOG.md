# Changelog

## v1.0.0

### Changes

- First version: leveled logging for UE4SS Lua mods (TRACE to CRITICAL, WARN
  by default) with a logger per module and categories (`main`, `config`, `ui`,
  `input` and any the mod registers).
- Levels per module, per category and global, from config.ini (`[Levels]`),
  the ModCore Settings menu (global) or `SetLevel`, applied while the game runs.
- Targets: UE4SS.log, or `<mod folder>/logs/<category>.log` with the previous
  session kept as `<category>.prev.log`.
- Delivery immediate, once per frame or periodic, always on the bridge's own
  thread.
- A memory-mapped journal keeps every logged line through a crash; the next
  start writes the previous session's lines to `logs/previous-session.log` and
  completes undelivered module files.
