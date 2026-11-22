# UE4SS Lua Log Bridge API (API 2)

```lua
local Logs = UE4SSLuaLogBridge()   -- this Lua mod's environment
local log = Logs.Register("MyMod", { target = "module", frequency = "frame",
    categories = { "combat" } })
log.info("loaded ", count, " entries")
log.combat.debug("hit ", target, " for ", damage)
```

The bridge loads before Lua mods, so the global exists when a mod's `main.lua`
runs. Every function returns `nil, code, message` on failure (`invalid`, `io`
or `busy`); logging calls never raise.

## The global

| | |
|---|---|
| `UE4SSLuaLogBridge()` | this Lua mod's environment (the same table on every call) |
| `UE4SSLuaLogBridge.API_VERSION` | `2` (`ListLoggers` is new in 2) |
| `GetVersion()`, `GetCapabilities()`, `GetStats()` | as on the environment |

## The environment

### `Register([name], [options])` → logger

`name` is the module name levels are keyed by: 1 to 64 letters, digits, `_`,
`-` or inner spaces, not `global`. Without it, the mod's folder name is used
(characters a name can't hold become `_`). A name registers once per Lua mod;
registering it again returns the same logger and ignores the options except
`categories`.
`Register(options)` is `Register(nil, options)`.

| Option | Values | Default |
|---|---|---|
| `target` | `"default"`: UE4SS.log; `"module"`: `<mod folder>/logs/<category>.log` | `"default"` |
| `frequency` | `"immediate"`, `"frame"` (once per frame), `"periodic"` | `"immediate"` |
| `interval` | seconds between periodic writes, 0.05 to 3600 | `1` |
| `level` | the module's level when settings name none | none (WARN) |
| `categories` | a list of category names to register | |

A logging call returns at once and its line is written to its target shortly
after, without holding up the game: `immediate` writes each line (typically
under a millisecond later), `frame` once per frame, `periodic` every
`interval`. A line can
therefore reach UE4SS.log just after a `print` made right after it; call
`Flush()` first when that order matters. When the game reports no frames
(rare), `frame` writes every 16 ms instead.

`module` target files: on a file's first write in a session, the previous
session's file becomes `<category>.prev.log`. Lines read
`2026-11-18 14:03:22.123 INFO message`. UE4SS.log lines read
`[MyMod] INFO message`, or `[MyMod:combat] INFO message` for a category other
than `main`.

### `SetLevel(key, level)` → true

Overrides a level for this game session (not saved). `key`: `nil` or `""`
for the global level, `"MyMod"` for a module, `"MyMod.combat"` for one of its
categories; any case. `level`: a name (`"debug"`, any case) or a rank 1 to 7;
`nil` removes the override. It applies to every mod at once.

### `GetLevel(key)` → level name

The effective level of a key, as `SetLevel` takes it.

### `Flush()` → true

Writes every pending line now, before it returns.

### `ReloadSettings()` → true

Asks the bridge to re-read config.ini. It is read shortly after; when levels
changed, every logger sees them within a second, as with
`SetLevel`. ModCore Settings' extensions menu calls it for you (see Levels).

### `ListLoggers()` → list, generation

Every module registered this session, by any Lua mod, in registration order,
then each module that config.ini's `[Levels]` names but that has not
registered, in file order:

```lua
{ name = "MyMod", level = "DEBUG", source = "levels", loaded = true,
  categories = { { name = "main", level = "DEBUG", source = "module", loaded = true },
                 { name = "combat", level = "TRACE", source = "levels", loaded = true }, ... } }
```

`level` is the effective level (see Levels). A module's `source` says where it
comes from: `override` (`SetLevel`), `levels` (config.ini `[Levels]`),
`option` (the module's `level` option) or `global`. A category's is
`override` or `levels` when its own key decides, else `module`. A category
lists the defaults first, then those registered, then those only `[Levels]`
names (`loaded = false`). A module's spelling is its first registration's, or
the key's in `[Levels]` when it has not registered. `generation` changes
whenever a module or category registers or any level may have changed, like
`GetStats().generation`.

### `GetCapabilities()`

`{ api = 2, journal_bytes, max_message_bytes = 65536, frame = <engine tick>,
levels, targets, frequencies, default_categories }`.

### `GetStats()`

`{ written, delivered, pending, stalls, recovered, loggers, generation }`.
`stalls` counts logging calls that had to wait for pending lines to be
written first; `recovered` counts lines of the previous session that never
reached their target.

## Loggers and categories

A logger is its `main` category. Every logger has `main`, `config`, `ui` and
`input`; other categories exist only once registered, through the `categories`
option of `Register` (a later `Register` of the same name adds its categories)
or `log.register("combat", "ai")`, which returns them in order. A registered
category is a field of the logger (`log.combat`, or `log[name]`); an
unregistered one reads as `nil`, so a misspelt `log.commbat.debug(...)` raises
at once instead of logging to a category nobody watches. A category name is a
letter, then up to 31 letters, digits, `_` or `-`, not a Windows device name
and not a method name (`write`, `level`, ...); names are folded to lower case.

| Method (logger or category) | |
|---|---|
| `trace(...)`, `debug(...)`, `info(...)`, `warn(...)`, `error(...)`, `critical(...)` | arguments are converted with `tostring` and joined, without separators, only when the line is written |
| `write(level, ...)` | the level as a name or rank 1 to 6 |
| `enabled(level)` | whether a line at that level would be written |
| `level` | the effective level name (a field, read each time) |
| `name` | the module or category name |
| `register(name, ...)` | logger only: registers categories and returns them; `nil, "invalid", message` for a bad name, registering none |

`log:warn(...)` works as well as `log.warn(...)`. From a coroutine the mod
created, lines go to UE4SS.log whatever the target, and a category whose level the main thread has not asked for yet is
filtered at the module's last known level (WARN if none is known).

## Levels

TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL, and OFF to silence. A line is
written when its level is at or above the effective level of its
`<module>.<category>`, the first of:

1. a `SetLevel` override of the category, then of the module;
2. config.ini `[Levels]` of the category, then of the module;
3. the module's `level` option (the lowest one registered under that name);
4. a `SetLevel` override of the global level, then the global setting;
5. WARN.

Settings live in the bridge's own folder, `0_ModCore_UE4SSLuaLogBridge/config.ini`:

```ini
[UE4SSLuaEXtensions.Logs]
GlobalLevel=4

[Levels]
MyMod=DEBUG
MyMod.combat=TRACE
```

The ModCore Settings menu edits `GlobalLevel` (1 for TRACE to 7 for OFF; a
name works too) and, under it, each module listed by `ListLoggers` and its
categories, stored in `[Levels]` as ranks. A `[Levels]` value of `0` or
`inherit` (the menu's Inherit) sets no level, so the key falls through to the
next step above. The bridge reads the file when the game starts, and edits
apply when ModCore Settings applies the Logs page, when
ModCore Settings reloads config (its Reload Config button, or the game window
regaining focus), and on `ReloadSettings()`. Without ModCore Settings, an edit
applies on the next launch or after `ReloadSettings()`. Unknown keys and
levels are ignored and noted in UE4SS.log.

A level change reaches every logger within a second.

## Crash safety

Every line is saved to `0_ModCore_UE4SSLuaLogBridge/logs/journal.bin` (4 MiB)
before the logging call returns, and survives the game crashing or being
killed; only an operating-system crash or power loss loses a logged line.

At the next start, the previous session's retained lines go to
`logs/previous-session.log` (the newest 4 MiB, every target, oldest first),
with `*` marking lines that never reached their target; those of `module`
loggers are also appended to their own files. UE4SS.log of the crashed session
is gone by then (UE4SS starts it anew), so that file is where its last lines
are.

A full journal never drops a line: the logging call waits until pending lines
are written, then continues. Messages over 64 KiB are cut and end in
` [truncated]`.

A second game process running at the same time has no crash safety; UE4SS.log
says so.
