# Changelog

Changes to the package's own folder, `0_ModCore_UE4SSLuaEXtensions`. Each
extension keeps its own changelog in its folder.

## v1.4.0

### Changes

- The package includes UE4SSLuaLogBridge 1.0.0, alongside UE4SSLuaEventBridge
  1.4.0, UE4SSLuaFileBridge 1.2.0 and UE4SSLuaSoundBridge 1.2.0.
- The extensions' settings are the **Extensions** tab of the ModCore Settings
  page, between Templates and Developer Tools: the installed count, then a
  section for each extension. ModCore Lua Extensions' entry in the mod list
  opens the tab. A ModCore Settings without the tab lists the page on its own.
- Each extension's settings are stored in its own `config.ini` under
  `[UE4SSLuaEXtensions.<Slot>]`, keyed by the setting's id. `settings.ini` is
  no longer read, so switches set there return to their defaults until set
  again. An extension without a `mod_settings.ini` keeps its load-time
  verification switch there as `EnableVerifications`.
- The Logs section of the Extensions tab lists, under the global level, one row
  per mod that logs through the Log Bridge, setting its `main` level, and an
  extra row only for each category the mod registers. Each is set to Inherit
  or a level from Trace to Off and applies at once.
- The Log Bridge re-reads its config.ini when ModCore Settings applies the
  Logs page or reloads config (its Reload Config button, or the game window
  regaining focus).

## v1.3.3

### Changes

- Fix the extensions menu failing at startup on a fresh install with "cannot
  create the cache directory". If it still can't create its folders, the
  extensions keep working and are listed on their own, as before.
