# Changelog

Changes to the package's own folder, `0_ModCore_UE4SSLuaEXtensions`. Each
extension keeps its own changelog in its folder.

## Unreleased

### Changes

- The Logs section lists, under the global level, every mod that registered
  a logger with the Log Bridge (1.0.0, API 2), its `main` category and each
  category it registered,
  each set to Inherit or a level from Trace to Off; mods named only in the
  bridge's `[Levels]` show as not loaded. The section is republished when a
  mod or category registers later and shows the change the next time the
  menu opens. `[Levels]` values are stored as ranks, 0 for Inherit; level
  names written by hand become ranks.
- The Log Bridge re-reads its config.ini when ModCore Settings applies the
  Logs page or reloads config (its Reload Config button, or the game window
  regaining focus).

- The package includes UE4SSLuaLogBridge, and the extensions menu lists it as
  a fourth section, "Logs Extension".
- Each extension's settings are stored in its own `config.ini` under
  `[UE4SSLuaEXtensions.<Slot>]`, keyed by the setting's own id. An extension
  without a `mod_settings.ini` keeps its load-time verification switch in
  `config.ini` `[UE4SSLuaEXtensions.<Slot>] EnableVerifications` instead of
  `settings.ini` `[Settings]`, which is no longer read, so the switch returns
  to its default until set again.

## v1.3.3

### Changes

- Fix the extensions menu failing at startup on a fresh install with "cannot
  create the cache directory". If it still can't create its folders, the
  extensions keep working and are listed on their own, as before.
