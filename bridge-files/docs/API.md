# UE4SSLuaFileBridge Lua API (1.0.1)

Status: **reviewed, ready to freeze (Gate G-spec).** This file is the contract
the native product and the Lua helpers implement. It folds in L's review
(`bridge-files/docs/SPEC-REVIEW.md`, M1–M15), F's native contract and K's
decisions of 2026-10-10. Once K freezes it, any change goes through K to the
native, Lua and documentation tracks together. One point is still pending; see
[Open points for review](#open-points-for-review).

- [Conventions](#conventions)
- [Entry point and version](#entry-point-and-version)
- [Locations: roots and presets](#locations-roots-and-presets)
- [Paths](#paths)
- [Access environments](#access-environments)
- [Operations](#operations)
- [Streams: `Open`](#streams-open)
- [File sockets: `Tail`](#file-sockets-tail)
- [Error codes](#error-codes)
- [Deferred and out of scope](#deferred-and-out-of-scope)
- [Internal native contract](#internal-native-contract)
- [Open points for review](#open-points-for-review)

## Conventions

- **Names.** Functions are PascalCase, like UE4SSLuaEventBridge. Functions of an
  environment are bound to it and accept both call forms: `env.ReadText(path)`
  and `env:ReadText(path)` do the same thing (a path is never a table, so a
  colon call is recognized without ambiguity). Methods of a stream or a
  subscription are colon-only (`stream:Write(text)`, `sub:Close()`); a dot call
  returns `nil, "invalid", message`.
- **Results.** Every operation returns its value on success. On failure it
  returns `nil, code, message`:
  - `code` is one of the [error codes](#error-codes). It is stable and meant for
    program logic.
  - `message` is English text for logs. It names the operation and the path. Its
    wording isn't part of the contract.
  - An operation with no natural value returns `true`.
- **Argument errors** in operations (wrong type, unknown option key, bad option
  value) return `nil, "invalid", message`. They are never ignored. `AddPath` is
  the exception: it raises a Lua error (see [AddPath](#envaddpathpath-options)).
- **Options** are always an optional last table argument. An unknown key or a
  value of the wrong type or range is `invalid`.
- **Strings** are Lua byte strings. Paths are UTF-8. File contents are bytes,
  passed through unchanged unless an operation says otherwise.
- **Threads.** File operations are synchronous and run on the calling thread.
  They work from the mod's main Lua state, the game thread
  (`ExecuteInGameThread`), loop callbacks (`LoopAsync`) and hook callbacks, but
  large reads and writes on the game thread stall the frame. The bridge never
  calls into Unreal.
- **Coroutines.** Operations are **not available inside coroutines the mod
  creates itself** (`coroutine.create`, `coroutine.wrap`). UE4SS refuses native
  calls from a Lua thread it didn't register; there every operation returns
  `nil, "invalid", message` (`AddPath` raises). Call the bridge outside the
  coroutine and pass results in.
- **Callbacks** (`Tail`) run on the mod's root Lua state, on the game thread
  from the bridge's engine-tick callback when UE4SS's engine-tick detour exists
  (the normal case), otherwise from the bridge's update callback, not on the
  game thread, where Unreal work must be scheduled with `ExecuteInGameThread`.
- **Isolation.** Every Lua environment (each mod's Lua state) receives its own
  `UE4SSLuaFileBridge`. Environments, streams and subscriptions belong to the Lua
  environment that created them and are closed when it stops or reloads. After
  the Lua environment starts stopping, operations return `nil, "invalid",
  message`. The native functions behind the table are not reachable from mods.

## Entry point and version

```lua
local env = UE4SSLuaFileBridge()           -- the safe environment
local version = UE4SSLuaFileBridge.GetVersion()        -- "1.0.1"
local caps = UE4SSLuaFileBridge.GetCapabilities()
local api = UE4SSLuaFileBridge.API_VERSION             -- 1
```

`UE4SSLuaFileBridge` is a table with a `__call` metamethod. Calling it returns
the [safe environment](#the-safe-set) for the calling mod. Each call returns an
equivalent environment; whether it is the same table is unspecified.

`GetVersion()` returns the product version string. `API_VERSION` is the API
contract number, `1` for 1.0.1. Every environment also has `env.GetVersion`,
`env.GetCapabilities` and `env.GetDispatchStats`, identical to the global ones.

### `GetCapabilities()`

Returns a new table each call:

```lua
{
    api = 1,
    environments = true,     -- AddPath and access checks
    append_only = true,      -- mode "ao"
    streams = true,          -- Open
    tail = true,             -- Tail
    locations = { "game", "user", "mod", "moddata", "temp", "savegames" },
    lock = false,            -- deferred
    hash = false,            -- deferred
    temp_file = false,       -- deferred
    max_read_bytes = 67108864,
    target_ue4ss_commit = "97b7e501",
}
```

Gate optional features on these fields, not on the version number. Later
releases add keys; they don't change the meaning of existing ones.

### `GetDispatchStats()`

```lua
local stats = UE4SSLuaFileBridge.GetDispatchStats()
-- { subscriptions = 2, delivered = 1840, budget_exhausted_passes = 3 }
```

Process-wide `Tail` delivery counters, for parity with UE4SSLuaEventBridge:
active subscriptions, deliveries made, and dispatch passes that ran out of
budget with text still waiting. Counters last for the bridge lifetime. Read them
on demand; the bridge adds no polling. Never fails.

### Limits

| Limit | Value | Beyond it |
|---|---:|---|
| Grants per environment | 256 | `AddPath` raises `io: …` |
| Live environments per Lua environment | 4,096 | `AddPath` raises `io: …` (after one garbage collection and retry) |
| Open streams per Lua environment | 64 | `Open` returns `nil, "io", message` |
| `Tail` subscriptions per Lua environment | 64 | `Tail` returns `nil, "io", message` |
| Bytes per read (`max_read_bytes`) | 64 MiB | `invalid` |
| `Tail` line length | 1 MiB | Delivered in pieces with `info.partial = true` |

Every capacity limit is `io`: it is exhaustion, not a bad argument and not
another holder. Environments that are no longer referenced count until they
are collected.

**`Tail` read caps** are tuning, not capacity limits: nothing fails when they
are reached. On each dispatch pass (20 passes per second by default) the bridge
reads at most **256 KiB in total across all subscriptions** and at most **64 KiB
from any one subscription's file**. Subscriptions are polled round-robin,
starting each pass after the one where the previous pass stopped, so one busy
file can't starve the others. Text not read in a pass stays in the file and is
read on a later pass. Together with the [delivery budget](#envtailpath-fn-options),
this keeps polling from stalling UE4SS's update thread; it also bounds
throughput, to roughly 5 MiB/s in total and 1.25 MiB/s per file at the default
rate.

## Locations: roots and presets

A **location** is a named folder. Paths start from a location (see
[Paths](#paths)). There are two kinds.

**Roots** bound every path the bridge accepts. They are not presets, but their
names can start a path.

| Location | Resolves to |
|---|---|
| `game` | The game install folder, which holds `Dawnwalker/` and `Engine/` (for Steam, `…/steamapps/common/The Blood of Dawnwalker`). Worked out from the running executable, `<game>/Dawnwalker/Binaries/Win64/Dawnwalker.exe` |
| `user` | The game's user data folder, `%LOCALAPPDATA%\Dawnwalker`, from `SHGetKnownFolderPath(FOLDERID_LocalAppData)` plus the project name (the folder that holds `Binaries/Win64`). It holds saves, user config and logs |

**Presets** are worked out once when the bridge starts (`mod` when the Lua
environment starts). 1.0.1 ships exactly these:

| Preset | Resolves to | Rules |
|---|---|---|
| `mod` | `<game>/Dawnwalker/Binaries/Win64/ue4ss/Mods/<calling mod>` | The calling mod's own folder, bound per Lua environment |
| `moddata` | `<user>/Saved/ModData/<calling mod>` | Per-mod data that outlives the mod folder. Created on first use, with an [`.owner` marker](#the-owner-marker) |
| `temp` | `<moddata>/temp` | Scratch space. [Emptied when the bridge starts](#temp) |
| `savegames` | `<user>/Saved/SaveGames` | `*.sav`, `*.meta`, `*.png`. [Every overwrite keeps one `.bak`](#savegames). Expect `busy` |

`<calling mod>` is the mod's folder name under `Mods/`, as UE4SS reports it in
`on_lua_start`. The `Mods/` folder is the one that holds the bridge's own folder
(checked to lie inside `game`); the path above is the fallback. A mod name that
isn't a single valid path segment leaves `mod`, `moddata` and `temp`
**unbound**: [`Locations()`](#envlocations) reports them with `path = nil` and
`exists = false`, and any path through them is `invalid`.

### `env.Path(name, ...)`

```lua
local p, code, msg = env.Path("savegames", "Autosave0.sav")
-- "C:/Users/me/AppData/Local/Dawnwalker/Saved/SaveGames/Autosave0.sav"
```

Joins the given parts onto the location `name` and returns the
[normalized](#envnormalizepath) absolute path. Parts are strings and may
themselves contain `/`. Pure: it doesn't touch the file system and needs no
grant. The folder need not exist.

Errors: `invalid` (unknown location, bad part, malformed result),
`outside_root` (a `..` part climbs out of the location).

### `env.Locations()`

```lua
local locations = env.Locations()
-- {
--   game      = { path = "C:/…/The Blood of Dawnwalker", root = true,  exists = true },
--   user      = { path = "C:/…/AppData/Local/Dawnwalker", root = true, exists = true },
--   mod       = { path = "C:/…/Mods/MyMod",              root = false, exists = true },
--   moddata   = { path = "C:/…/Saved/ModData/MyMod",     root = false, exists = false },
--   temp      = { ... }, savegames = { ... },
-- }
```

Returns a new table describing every location, roots and presets: its absolute
path, whether it is one of the two roots, and whether its folder exists right
now. A location whose folder doesn't exist is still returned, with
`exists = false`; an [unbound](#locations-roots-and-presets) one has
`path = nil`. Needs no grant. Never fails.

### The `.owner` marker

When the bridge creates `moddata` for a mod, it writes `moddata/.owner`, a UTF-8
text file of `key=value` lines:

```text
folder=MyMod
first_write=2026-10-10T12:00:00Z
last_write=2026-10-10T12:00:00Z
bridge_version=1.0.1
```

`last_write` and `bridge_version` are refreshed by the first write under
`moddata` in each bridge session (not on every write). Times are UTC, ISO 8601.
Mods may read `.owner`. The bridge refuses to let a mod write, append to, move or
remove it (`denied`), whatever the grants say. It exists so leftover folders can
be traced to a mod later.

### `temp`

`temp` lives inside `moddata`, so using it creates `moddata` (and `.owner`) if
needed. When the bridge starts, before any Lua environment starts, it empties
`temp` in every `<user>/Saved/ModData/*` folder that has an `.owner` marker.
Folders without a marker are left alone. A Lua reload doesn't empty `temp`.

### `savegames`

- **Every overwrite keeps one backup.** Any operation that replaces an existing
  file under `savegames` (`WriteText`, `Copy` or `Move` with `overwrite = true`)
  goes through `ReplaceFileW` and leaves the previous content in
  `<name>.bak` (for example `Autosave0.sav.bak`), replacing any older `.bak`.
  This is a fixed rule, not an option. The `.bak` is written by the bridge and
  isn't checked against the grant's `extensions`.
- **Operations that can't keep a backup are refused** with `invalid`:
  `WriteText` with `atomic = false`, and `Open` with `append = false` on an
  existing file. `Append` and appending `Open` don't overwrite, so they are
  allowed if granted.
- **`busy`:** the game may hold a save open while writing it. A sharing
  violation returns `busy` at once; the bridge doesn't wait or retry.
- Steam Cloud may sync these files (not checked). A bad write can reach the
  cloud copy.
- The safe set can only read `savegames`. Writing needs an explicit grant.

## Paths

### Accepted forms

Every path argument accepts either form:

- **Location-relative:** a location name, optionally followed by `/` and a
  relative path: `"mod"`, `"mod/cache/state.json"`, `"savegames/Autosave0.sav"`.
  The first segment must be a location name (exact, lowercase). Any other
  relative path is `invalid`; there is no current directory.
- **Absolute:** a drive-letter path inside one of the two roots:
  `"C:/Users/me/AppData/Local/Dawnwalker/Saved/x.txt"`. `\` and `/` are both
  accepted.

### Rules

- **Encoding.** Paths are UTF-8 in Lua. Invalid UTF-8 or a NUL byte is
  `invalid`. The native side converts to UTF-16 and uses long-path (`\\?\`)
  forms internally; these never appear in Lua.
- **Returned paths** are absolute, with forward slashes, an uppercase drive
  letter, no `.` segments, no doubled or trailing slash. Case is kept as given;
  the bridge doesn't fold case or look up on-disk spelling.
- **Comparison** with grants and locations ignores case (Windows ordinal,
  case-insensitive).
- **`.` segments** are dropped.
- **`..` segments** are resolved lexically. A location-relative path whose `..`
  climbs above its location (`"mod/../OtherMod"`) is `outside_root`. An absolute
  path whose `..` climbs above its root is `outside_root`.
- **Outside the roots:** an absolute path not inside `game` or `user` is
  `outside_root`.
- **Device and UNC paths** are `outside_root`: `\\server\share\…`, `//…`,
  `\\.\…`, `\\?\…`, `\??\…`, and any segment that is a reserved device name
  (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without an
  extension). Named pipes are therefore unreachable.
- **Malformed paths** are `invalid`: empty string, drive-relative (`C:foo`), a
  segment containing `< > : " | ? *` or a control character (this also rules
  out alternate data streams), a segment ending in a space or a dot.
- **Links.** Symbolic links, junctions and mount points inside the roots are
  followed. Every access check is made against the **resolved real path**. A
  path whose resolution leaves both roots is `outside_root`, checked at the time
  of each call. **`Remove`, `RemoveTree` and `Move` (both ends) act on a link
  itself:** their last segment is not followed, and the check uses the link's
  own path (its parent resolved, the link's name appended). `RemoveTree` never
  enters links inside the tree. `Stat` follows links and reports `link`. The
  check isn't atomic with the operation; a junction swapped in between isn't
  detected.

### Lexical helpers

These work on strings only. They don't touch the file system, need no grant and
don't resolve locations, except `Normalize`.

| Function | Returns | Notes |
|---|---|---|
| `env.Join(a, b, ...)` | string | Joins parts with `/` and collapses separators. `Join("mod", "cache", "x.json")` → `"mod/cache/x.json"`. Doesn't resolve `..` or locations |
| `env.Normalize(path)` | string | The absolute form described under "Returned paths", with the location resolved. Errors: `invalid`, `outside_root` |
| `env.Parent(path)` | string | Everything before the last segment. `Parent("mod/a/b.txt")` → `"mod/a"`. A path with one segment (`"mod"`, `"C:"`) returns `nil, "invalid"` |
| `env.Name(path)` | string | Last segment. `"b.txt"` |
| `env.Stem(path)` | string | Last segment without its extension. `"b"`; `"a.tar.gz"` → `"a.tar"`; `".owner"` → `".owner"` |
| `env.Extension(path)` | string | Text after the last dot of the last segment, without the dot. `"txt"`; `"a.tar.gz"` → `"gz"`; none or a leading dot only (`".owner"`) → `""` |

A trailing slash is ignored by all of them. A non-string argument is `invalid`.

## Access environments

The sandbox prevents mistakes; it isn't a security boundary. Every UE4SS Lua mod
already has the full standard library (`io`, `os`, `package.loadlib`) and can
write anywhere the user can. The bridge checks only what goes through it.

### The safe set

`UE4SSLuaFileBridge()` returns an environment with these grants:

| Path | Mode | delete |
|---|---|---|
| `game` | `ro` | `false` |
| `user` | `ro` | `false` |
| `mod` | `rw` | `false` |
| `temp` | `rw` | `true` |

Writing to `moddata`, `savegames` or anything else must be granted explicitly.
`temp` allows deleting because scratch space that can't be cleaned is no
scratch space.

### `env.AddPath(path, options?)`

```lua
local env = UE4SSLuaFileBridge()
    .AddPath("mod/cache", { delete = true })                    -- inherits rw from mod
    .AddPath("savegames", { mode = "rw", extensions = { "sav" } })
    .AddPath("mod/logs",  { mode = "wo", extensions = { "log" } })
```

Returns a **new** environment: this one plus the grant. The original is
unchanged, so variants derived from one base can't interfere. There is no
global switch and nothing to reset: an environment's **scope is wherever the
variable holding it is visible**. A function taken from an environment
(`local read = env.ReadText`) keeps that environment's grants alive for as long
as it lives. Assigning a field of an environment raises an error; even a
`rawset` can't change its grants, which are held natively.

`path` is a location or a location-relative path (or an absolute path inside the
roots). It needn't exist. A grant on a file covers that file; a grant on a
folder covers the folder and, with `recursive`, everything below it.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `mode` | `"ro"`, `"wo"`, `"rw"`, `"ao"`, `"stat"` | inherited | What the grant allows (table below) |
| `extensions` | list of strings | inherited | File extensions the grant covers, without the dot, compared ignoring case. `{}` covers no files. Folders are always covered |
| `delete` | boolean | `false` | Allows `Remove`, `RemoveTree` and being the source of `Move` |
| `recursive` | boolean | `true` | `false` covers the path itself and its direct children only |

**Inheritance.** A grant with no `mode` (or no `extensions`) takes it from the
most specific **other** grant that covers the grant's own path, treated as a
folder, repeating upwards if that one inherits too. It is worked out at check
time from specificity, not from chain order, so a grant's effective values
don't depend on the file being checked. There always is a source, because the
safe set's root grants have explicit values. A default never widens access:
`delete` defaults to `false` and is never inherited, and `extensions` inherits
rather than defaulting to "any" (the safe set's root grants cover any
extension). Example: `AddPath("savegames/sub", {})` under
`AddPath("savegames", {mode = "rw", extensions = {"sav"}})` is `rw` on `*.sav`
only.

`delete = true` on a grant whose inherited mode has no write (`ro`, `stat`,
`ao`) has no effect: removing needs `delete` and an effective `wo` or `rw`.

**Validation.** `AddPath` raises a Lua error at the caller's line, with a
message starting `invalid:`, `outside_root:` or `io:`, when:

- an option key is unknown or not a string, or a value has the wrong type or an
  unknown value;
- `extensions` isn't a sequence of strings, or an entry is empty or contains
  `.`, `/`, `\`, `;`, a control character, or `< > : " | ? *`;
- `delete = true` is combined with an explicit `mode` of `"ro"`, `"stat"` or
  `"ao"` (rotation of an `ao` log needs a separate grant with `delete`);
- the path is malformed or outside the roots, or names an unbound location;
- a [limit](#limits) is reached (`io:`).

It raises rather than returning `nil, code, message` so that a bad grant fails
at the line that built it, not later as "attempt to index a nil value" in the
middle of a chain.

**Re-granting a path.** `AddPath` on a path that already has a grant in the
environment (compared after resolution, ignoring case) **replaces** that grant
in the new environment. This is how `AddPath("mod", { delete = true })` changes
a safe-set grant.

### What each mode allows

| Operation | `stat` | `ro` | `wo` | `rw` | `ao` | Also needs |
|---|:-:|:-:|:-:|:-:|:-:|---|
| `Exists`, `Stat`, `List` | yes | yes | yes | yes | yes | |
| `ReadText`, `ReadBytes`, `Tail` | | yes | | yes | | |
| `MakeDir` | | | yes | yes | yes | |
| `WriteText` | | | yes | yes | | |
| `Append`, `Open` (append) | | | yes | yes | yes | |
| `Open` with `append = false` | | | yes | yes | | |
| `Remove`, `RemoveTree` | | | yes | yes | | `delete` |
| `Copy` source | | yes | | yes | | |
| `Copy` destination | | | yes | yes | | |
| `Move` source | | | yes | yes | | `delete` |
| `Move` destination | | | yes | yes | | |

Every mode includes `stat`, so a writer can check what it is about to touch.
`MakeDir` checks each folder it would create. `ao` never truncates, overwrites
or deletes, so logs keep their history.

### How a call is checked

For each path an operation touches:

1. The path is normalized ([Paths](#paths)): `invalid` or `outside_root`.
2. It is resolved to its real path: the deepest existing ancestor is opened
   (links followed) and the rest appended. The leaf isn't followed for
   `Remove`, `RemoveTree`, `Move` and the bridge's own `.bak` and temporary
   siblings. Outside both roots: `outside_root`.
3. Grant paths are resolved the same way. Among the environment's grants that
   **cover** the real path, the most specific wins (the most path segments;
   paths are unique within an environment, so there are no ties). A grant
   covers a path when the path is the grant's path or below it (direct children
   only when `recursive = false`), and, for a file (existing or being created),
   when its extension is in the grant's effective `extensions`. Folders are
   always covered. A grant that doesn't cover the path is skipped, so the next
   enclosing grant decides. Grants are never combined.
4. The winning grant's effective `mode` and `delete` must allow the operation.
   Otherwise: `denied`.
5. Fixed rules regardless of grants: the [delete floor](#delete-floor) and the
   [`.owner`](#the-owner-marker) rule (`denied`), then the
   [`savegames`](#savegames) rules (`invalid`).

Checks are made natively: each environment carries a policy id, and the native
side checks every call against the resolved real path (normalization,
junctions, Unicode).

### Delete floor

Whatever the grants say, `Remove`, `RemoveTree` and `Move` (as source) refuse,
with `denied`:

- any location itself (`game`, `user`, `mod`, `moddata`, `temp`, `savegames`);
- any folder that contains a location (for example `user/Saved`, which holds
  `savegames` and every mod's `moddata`, or the `Mods` folder, which holds
  `mod`), **unless that exact folder is granted with `delete = true`**. A
  `delete` grant on an enclosing folder doesn't count;
- a drive root;
- the user profile root (`%USERPROFILE%`).

For `RemoveTree`, the floor applies to every folder in the tree, so removing a
folder that contains a location is refused as a whole.

Their contents can be removed when granted.

## Operations

All paths below accept either [form](#accepted-forms). "Mode" is what the
winning grant must allow (see the [table](#what-each-mode-allows)). Errors list
the codes specific to the operation; `invalid`, `outside_root`, `denied` and
`io` can come from any operation that touches the file system.

### `env.Exists(path)`

Returns `true` or `false`. A missing file or folder is `false`, not an error.
Mode: `stat`. Errors: `denied`, `outside_root`, `invalid`, `io`.

### `env.Stat(path)`

```lua
{
    type = "file",          -- "file", "directory" or "other"
    size = 1234,            -- bytes; 0 for a directory
    modified = 1791640800,  -- seconds since the Unix epoch (UTC), may have a fraction
    created = 1791640000,
    readonly = false,       -- the read-only attribute
    link = false,           -- the path's last segment is a symlink, junction or mount point
}
```

Follows links; `link` reports whether the final segment was one. Mode: `stat`.
Errors: `not_found`.

### `env.List(path)`

Returns an array of the folder's entries, sorted by name (byte order), without
`.` and `..`:

```lua
{
    { name = "a.json", type = "file", size = 10,
      modified = 1791640800, created = 1791640000, link = false },
    ...
}
```

Each entry has `name`, `type`, `size`, `modified`, `created` and `link`, with the
same meaning and units as in [`Stat`](#envstatpath) (times in seconds since the
Unix epoch, possibly fractional). The values come from the folder's directory
entries, which NTFS may update lazily for a file with several hard links; `Stat`
reads the file itself, so use `Stat` when exact sizes or times matter.

Not recursive in 1.0.1. Hidden and system entries are included. Names that
can't be returned as UTF-8 (unpaired UTF-16 surrogates, which Windows permits)
or that contain a tab or newline are skipped and logged once; `RemoveTree` still
removes them. Mode: `stat` on the folder. Errors: `not_found`, `invalid` (path is a file).

### `env.MakeDir(path)`

Creates the folder and any missing parents. Returns `true`, also when the folder
already exists. Every folder it would create must be allowed by its own winning
grant; this is checked before anything is created. Mode: `wo`, `rw` or `ao`.
Errors: `exists` (a file is in the way), `not_found` (the drive or root is
missing), `read_only`.

### `env.ReadText(path)`

Returns the file's contents. A leading UTF-8 BOM is removed. Nothing else is
changed: no newline conversion, no UTF-8 validation. Mode: `ro`. Errors:
`not_found`, `invalid` (a directory, or larger than `max_read_bytes`), `busy`.

### `env.ReadBytes(path, options?)`

Returns the bytes exactly as stored. Options: `offset` (integer ≥ 0, default
`0`), `length` (integer ≥ 0, default to the end). Reading past the end returns
the bytes available (possibly `""`). A single read may not exceed
`max_read_bytes`. Mode: `ro`. Errors: `not_found`, `invalid`, `busy`.

### `env.WriteText(path, text, options?)`

Writes `text` (any Lua string, so it also serves binary content) to the file,
creating or replacing it. It adds no BOM and converts no newlines. Returns
`true`. The parent folder must exist. Mode: `wo` or `rw`.

Options: `atomic` (boolean, default `true`).

**Atomic write (default).** Readers see the old content or the new content,
never a mix, and a failure leaves the target unchanged:

1. Create a temporary sibling `<name>.xbtmp-<pid>-<n>` in the same folder
   (`CREATE_NEW`, not shared).
2. Write all bytes, then `FlushFileBuffers`, then close.
3. If the target exists: `ReplaceFileW(target, temp, backup)`, with `backup`
   = `<name>.bak` under `savegames` and none elsewhere. The target's
   attributes, ACL and creation time are kept. Otherwise:
   `MoveFileExW(temp, target, MOVEFILE_WRITE_THROUGH)`.
4. On any failure, delete the temporary file and return the error.

The temporary sibling isn't checked against `extensions`. A crash can leave one
behind; its name makes it recognizable. At bridge start, leftovers are removed
only inside `<user>/Saved/ModData/*` folders that have an `.owner` marker.
Leftovers elsewhere (mod folders, `savegames`) are **not cleaned
automatically**: they stay until removed and appear in `List`.

**Non-atomic** (`atomic = false`): opens the target with truncation and writes in
place. A failure can leave a partial file. Refused under `savegames`
(`invalid`).

Errors: `not_found` (parent folder missing), `busy` (the target is open without
write/delete sharing), `read_only` (the target has the read-only attribute),
`invalid` (target is a directory).

### `env.Append(path, text)`

Appends `text` to the end of the file, creating it if missing. Each call is one
`FILE_APPEND_DATA` write, so concurrent appenders don't overwrite each other.
Returns `true`. The parent folder must exist. Mode: `wo`, `rw` or `ao`. Errors:
`not_found`, `busy`, `read_only`, `invalid`.

### `env.Remove(path)`

Removes a file or an empty folder. A link is removed as a link; its target is
untouched. Returns `true`. Mode: `wo` or `rw`, plus
`delete`. Errors: `not_found`, `denied` (including the
[delete floor](#delete-floor)), `invalid` (a folder that isn't empty; use
`RemoveTree`), `busy` (the file is open), `read_only`.

### `env.RemoveTree(path)`

Removes a folder and everything in it. Returns `true`. Links inside the tree are
removed as links; their targets are never entered. Before removing anything it
checks every entry against the environment; if any entry is denied, nothing is
removed and it returns `denied`. A failure part-way (for example `busy`)
returns that code and leaves the rest of the tree. Mode: `wo` or `rw`, plus
`delete`, on every entry. Errors: `not_found`, `denied`, `invalid` (path is a
file), `busy`, `read_only`.

### `env.Copy(from, to, options?)`

Copies a file. Options: `overwrite` (boolean, default `false`). The destination
is written like an atomic `WriteText` (temporary sibling, then replace; `.bak`
under `savegames`). Folders are not copied in 1.0.1. Returns `true`. Mode:
`ro` on `from`, `wo` or `rw` on `to`. Errors: `not_found`, `exists`
(destination exists and `overwrite` is false), `invalid` (a folder), `busy`,
`read_only`.

### `env.Move(from, to, options?)`

Moves or renames a file, or a link as a link. Options: `overwrite` (boolean, default `false`). Within
one volume it is a rename (`MoveFileExW`, or `ReplaceFileW` with `.bak` when
overwriting under `savegames`). Across volumes (the two roots may be on
different drives) it copies, flushes, then removes the source. A **link** can't
be moved across volumes, because the copy would follow it and leave a plain
file: that is refused with `invalid`. Folders are not moved in 1.0.1. Returns `true`. Mode: `wo` or `rw` plus `delete` on `from`;
`wo` or `rw` on `to`. Errors: `not_found`, `exists`, `invalid`, `busy`,
`read_only`, `denied` (including the delete floor on `from`).

## Streams: `Open`

```lua
local log = assert(env.Open("mod/logs/session.log"))
log:Write("started\n")
log:Close()
```

### `env.Open(path, options?)`

Opens a file for writing and returns a stream. The environment is checked when
the stream is opened, not on each write. Streams are write-only in 1.0.1.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `append` | boolean | `true` | `true` writes at the end. `false` truncates the file on open (needs `wo` or `rw`; refused under `savegames`) |
| `create` | boolean | `true` | Create the file if missing. `false` returns `not_found` instead |
| `flush` | `"write"`, `"manual"` | `"write"` | `"write"` hands every `Write` to the operating system at once, so readers and `Tail` see it immediately. `"manual"` buffers up to 64 KiB until `Flush`, `Close` or a full buffer |

Mode: `wo`, `rw` or `ao` (with `append = true` only). The parent folder must
exist. The file is opened with read and delete sharing
(`FILE_SHARE_READ | FILE_SHARE_DELETE`): others can read it, and it can be
renamed for rotation, while it is open. A second writer gets `busy`. Errors:
`not_found`, `busy`, `read_only`, `invalid`, and `io` when the Lua environment
already has 64 open streams.

### Stream methods

| Method | Returns | Notes |
|---|---|---|
| `stream:Write(text, ...)` | `true` | Writes the arguments in order, as one write. Strings and numbers are accepted (numbers as `tostring`); anything else is `invalid` and nothing is written. No arguments writes nothing and returns `true` |
| `stream:Flush()` | `true` | Hands buffered data to the operating system. It doesn't force it to disk (`FlushFileBuffers`) |
| `stream:Close()` | `true` | Flushes and closes. Calling it again returns `true` |
| `stream.path` | string | The normalized path, read-only |

After `Close`, `Write` and `Flush` return `nil, "invalid", "stream is closed"`.
Streams close automatically when their Lua environment stops or reloads. A
stream that is garbage-collected without `Close` is closed after it is
collected, at the latest when its Lua environment stops (the close runs at the
next bridge call from that Lua environment, never inside the collector). Close
streams explicitly when you are done; collection timing is not a schedule.

## File sockets: `Tail`

`Open` and `Tail` together make a **file socket**: one mod holds a file open and
writes; another is called back whenever the file grows. Mods can talk through
a path without sharing a Lua state.

```lua
-- Mod A
local out = assert(env.Open("mod/outbox.txt"))
out:Write("hello\n")

-- Mod B
local sub = assert(env.Tail("game/Dawnwalker/Binaries/Win64/ue4ss/Mods/ModA/outbox.txt",
    function(line, info)
        print("ModA says: " .. line)
    end))
-- later
sub:Close()
```

### `env.Tail(path, fn, options?)`

Subscribes `fn` to data appended to the file. Returns a subscription. Mode:
`ro` (or `rw`).

| Option | Type | Default | Meaning |
|---|---|---|---|
| `chunks` | boolean | `false` | `false` delivers complete lines; `true` delivers raw chunks as read |
| `from` | `"end"`, `"start"` | `"end"` | Where reading starts: at the current end of the file, or at its start |

**Callback.** `fn(text, info)`:

- Lines mode: `text` is one line without its terminator (`\n`, or `\r\n`). A
  partial line is held until its newline arrives.
- Chunks mode: `text` is the bytes read, unchanged.
- `info` is `{ offset = <integer>, reset = <boolean>, partial = <boolean> }`.
  `offset` is the byte offset of `text` in the file. `reset` is `true` on the
  first delivery after a truncation or rotation. `partial` is `true` for a piece
  of a line longer than 1 MiB (lines mode only); the line's last piece has
  `partial = false`.

**Truncation and rotation.** When the file becomes shorter than the read
position (truncated), or its identity (volume serial and file index) changes
(rotated, deleted and recreated), the read position goes back to `0` and a held
partial line is discarded. Identity is checked by path at most every 250 ms,
so a rotation is noticed within that time.

**Missing file.** If the file doesn't exist yet, `Tail` still succeeds provided
its folder exists; delivery starts from offset `0` when the file appears.
Folder missing: `not_found`.

**Delivery.** The bridge checks subscribed files from its update callback, using
the dispatch schedule and budget code it shares with UE4SSLuaEventBridge, and
calls `fn` on the mod's root Lua state within the per-pass dispatch budget:
on the game thread, once per frame from its engine-tick callback, when UE4SS's
engine-tick detour exists (the thread of every UE4SSLuaEventBridge callback),
otherwise from the update callback itself, not on the game thread. Text left
over when the budget runs out is delivered on the next pass, in order. The file itself is
the buffer: the bridge reads only what it delivers, so a slow subscriber falls
behind but loses nothing (unless the file is truncated or rotated first). Expect
delivery in the next frame or so after the writer's flush. A line longer than
1 MiB is delivered in 1 MiB pieces (`info.partial`).

The per-pass budget defaults to 256 deliveries or 500 microseconds, whichever
comes first, and at least one delivery per non-empty pass. Like the event
bridge, it is set by process environment variables read once at bridge start;
invalid values use the defaults:

| Variable | Default | Zero means |
|---|---:|---|
| `UE4SSLFB_MAX_EVENTS_PER_PASS` | 256 | No delivery-count limit |
| `UE4SSLFB_MAX_DISPATCH_US` | 500 | No time limit |

File reads per pass are also capped (256 KiB in total, 64 KiB per
subscription, round-robin); see [Limits](#limits).

**Errors in `fn`.** A callback that raises an error closes its subscription.
The bridge writes the path and the error to UE4SS.log.

**Closed by the bridge.** The bridge also closes a subscription when the file
can no longer be read (for example its folder is removed or access is lost). It
writes `<path>: <code> <message>` to UE4SS.log once. There is no callback for
this; check `sub.closed`.

**Subscription.**

| Member | Notes |
|---|---|
| `sub:Close()` | Stops delivery and returns `true`; calling it again returns `true`. A callback already running is not interrupted |
| `sub.closed` | Read-only boolean: `true` after `Close`, a callback error, or a close by the bridge |
| `sub.path` | The normalized path, read-only |

Subscriptions close automatically when their Lua environment stops or reloads.

Errors from `Tail`: `not_found`, `denied`, `outside_root`, `invalid` (`fn` is not
a function, the path is a folder), `io` (64 subscriptions already open).

## Error codes

| Code | When |
|---|---|
| `not_found` | The file or folder doesn't exist, or a parent folder an operation needs is missing (`ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`) |
| `denied` | The environment doesn't grant the operation (mode, extension, `delete`); the [delete floor](#delete-floor) or the `.owner` rule; or Windows refuses access (`ERROR_ACCESS_DENIED`, for example under `Program Files` without elevation). The message says which |
| `exists` | The destination exists and the operation won't replace it: `Copy` or `Move` without `overwrite`, or `MakeDir` where a file is in the way |
| `outside_root` | The path, after normalization or link resolution, is outside both roots; a `..` climbs out of its location or root; a device, UNC or reserved-name path |
| `read_only` | The target file has the read-only attribute (Windows reports `ERROR_ACCESS_DENIED`; the bridge checks the attribute), or the volume is write-protected (`ERROR_WRITE_PROTECT`). The bridge never clears the attribute |
| `busy` | Another process or handle holds the file in a conflicting way (`ERROR_SHARING_VIOLATION`, `ERROR_LOCK_VIOLATION`, `ERROR_USER_MAPPED_FILE`), for example the game writing a save. Returned at once, without waiting or retrying |
| `io` | Any other operating-system failure (disk full, a device error, a failed step of an atomic replace that `ReplaceFileW` reports, an internal error), and every [capacity limit](#limits): grants, environments, streams, subscriptions |
| `invalid` | A bad argument or option, a malformed path, an unknown or unbound location, the wrong kind of target (folder vs file), a folder not empty for `Remove` (`ERROR_DIR_NOT_EMPTY`), a read larger than `max_read_bytes`, an operation that would bypass the `savegames` backup, a link moved across volumes, a closed stream, a call from a coroutine the mod created, or a call while its Lua environment is stopping |

Policy refusals and Windows refusals both use `denied`; they differ in the
message. An operation that fails leaves no partial result unless its section
says otherwise (non-atomic `WriteText`, `RemoveTree`).

## Deferred and out of scope

**Deferred** (not in 1.0.1; `GetCapabilities()` reports `false`):

- `Lock`: advisory or byte-range locking between writers.
- `Hash`: content hashes of files.
- `TempFile`: unique temporary files with automatic cleanup.

**Out of scope** (decided 2026-10-10):

- **Queuing:** brokers, delivery guarantees, distribution among receivers.
  `Tail` delivers what is in the file; it doesn't acknowledge or redeliver.
- **Whiteboard messaging:** shared state between mods.
- **Windows named pipes** (`\\.\pipe\…`). They would need an explicit exception
  to the ban on device paths.

Also not in 1.0.1: reading streams, recursive `List`, copying or moving
folders, file watching beyond `Tail`, presets beyond the four above.

## Internal native contract

The boundary between the DLL and the Lua layer (native functions, argument
shapes, record formats, the content escape, dispatch and lifetime) is specified
in [`bridge-files/include/FileBridgeNativeContract.hpp`](../include/FileBridgeNativeContract.hpp).
It is internal: mods use only the API in this file.

## Open points for review

- **`mods` location:** pending Jorge's decision. 1.0.1 ships without it; until
  then another mod's folder is reached as `game/Dawnwalker/Binaries/Win64/ue4ss/Mods/<mod>/…`.
