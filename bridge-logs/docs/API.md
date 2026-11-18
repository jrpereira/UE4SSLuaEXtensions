# UE4SS Lua Log Bridge API (API 1)

```lua
local Logs = UE4SSLuaLogBridge()   -- this Lua mod's environment
local log = Logs.Register("MyMod", { target = "module", frequency = "frame", thread = "worker" })
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
| `UE4SSLuaLogBridge.API_VERSION` | `1` |
| `GetVersion()`, `GetCapabilities()`, `GetStats()` | as on the environment |

## The environment

### `Register([name], [options])` → logger

`name` is the module name levels are keyed by: 1 to 64 letters, digits, `_`,
`-` or inner spaces, not `global`. Without it, the mod's folder name is used
(characters a name can't hold become `_`). A name registers once per Lua mod;
registering it again returns the same logger and ignores the options.
`Register(options)` is `Register(nil, options)`.

| Option | Values | Default |
|---|---|---|
| `target` | `"ue4ss"`: UE4SS.log; `"module"`: `<mod folder>/logs/<category>.log` | `"ue4ss"` |
| `frequency` | `"immediate"`, `"frame"` (once per frame), `"periodic"` | `"immediate"` |
| `interval` | seconds between periodic writes, 0.05 to 3600 | `1` |
| `thread` | `"game"`: written on the game thread; `"worker"`: on the bridge's own thread | `"game"` |
| `level` | the module's level when settings name none | none (WARN) |
| `categories` | a list of category names to create now | |

`thread = "worker"` keeps disk writes off the game thread entirely. With
`"game"` and `"immediate"` a line is written inside the logging call, as
`print` does. Without the engine tick (rare), `frame` becomes `periodic` at
16 ms and every non-immediate logger is written by the worker.

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

Writes every pending line now, from the calling thread.

### `GetCapabilities()`

`{ api = 1, journal_bytes, max_message_bytes = 65536, frame = <engine tick>,
levels, targets, frequencies, threads, default_categories }`.

### `GetStats()`

`{ written, delivered, pending, stalls, recovered, loggers, generation }`.
`stalls` counts writes that found the journal full and wrote everything
pending first; `recovered` counts lines of the previous session that never
reached their target.

## Loggers and categories

A logger is its `main` category. Every logger has `main`, `config`, `ui` and
`input`; any other field that is a valid category name makes that category on
first use (`log.combat`), as does `log.category("combat")`. A category name is
a letter, then up to 31 letters, digits, `_` or `-`, not a Windows device name;
names are folded to lower case. A category named like a method (`write`) is
reached only through `log.category`.

| Method (logger or category) | |
|---|---|
| `trace(...)`, `debug(...)`, `info(...)`, `warn(...)`, `error(...)`, `critical(...)` | arguments are converted with `tostring` and joined, without separators, only when the line is written |
| `write(level, ...)` | the level as a name or rank 1 to 6 |
| `enabled(level)` | whether a line at that level would be written |
| `level` | the effective level name (a field, read each time) |
| `name` | the module or category name |

`log:warn(...)` works as well as `log.warn(...)`. From a coroutine the mod
created, UE4SS refuses native calls; the line then goes to UE4SS.log through
`print`.

## Levels

TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL, and OFF to silence. A line is
written when its level is at or above the effective level of its
`<module>.<category>`, the first of:

1. a `SetLevel` override of the category, then of the module;
2. settings.ini of the category, then of the module;
3. the module's `level` option (the lowest one registered under that name);
4. a `SetLevel` override of the global level, then the global setting;
5. WARN.

Settings live in the bridge's own folder, `0_ModCore_UE4SSLuaLogBridge/settings.ini`:

```ini
[Levels]
Global=WARN
MyMod=DEBUG
MyMod.combat=TRACE
```

The ModCore Settings menu edits `Global`. The file is re-read within half a
second of a change. Unknown keys and levels are ignored and noted in UE4SS.log.

The Lua layer keeps levels it has asked for, so a filtered call makes no native
call; a written line tells it when any level changed, and filtered calls check
every 64 calls.

## Crash safety

Every line is copied into `0_ModCore_UE4SSLuaLogBridge/logs/journal.bin`, a 4
MiB file mapped into memory, before the logging call returns. Windows writes a
mapped file's pages even when the game crashes or is killed, so only an
operating-system crash or power loss loses a logged line.

At the next start, the previous session's retained lines go to
`logs/previous-session.log` (the newest 4 MiB, every target, oldest first),
with `*` marking lines that never reached their target; those of `module`
loggers are also appended to their own files. UE4SS.log of the crashed session
is gone by then (UE4SS starts it anew), so that file is where its last lines
are.

A full journal never drops a line: the writing call first writes everything
pending, then continues. Messages over 64 KiB are cut and end in
` [truncated]`.

A second game process gets no journal file and keeps it in memory, without
crash safety; UE4SS.log says so.
