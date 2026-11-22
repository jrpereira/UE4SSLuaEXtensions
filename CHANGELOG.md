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
- A Logs section, "Logs Extension", lists under the global level a row for
  every mod that registered a logger with the Log Bridge, which also sets its
  `main` category, and one for each other category it registered, each set to
  Inherit or a level from Trace to Off. Levels apply at once. Mods and
  categories that did not register this session show as not loaded while
  `[Levels]` still sets a level for them, so it can be cleared. The section is
  republished when a mod or category registers later and shows the change the
  next time the menu opens. `[Levels]` values are stored as ranks, 0 for
  Inherit; level names written by hand become ranks, a `<Mod>.main` key folds
  into `<Mod>`, and Inherit keys for mods and categories that did not register
  are removed.
- The Log Bridge re-reads its config.ini when ModCore Settings applies the
  Logs page or reloads config (its Reload Config button, or the game window
  regaining focus).

## v1.3.3

### Changes

- Fix the extensions menu failing at startup on a fresh install with "cannot
  create the cache directory". If it still can't create its folders, the
  extensions keep working and are listed on their own, as before.
