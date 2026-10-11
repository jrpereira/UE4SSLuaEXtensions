# Developer guide

- [Requirements and execution model](#requirements-and-execution-model)
- [Version and capabilities](#version-and-capabilities)
- [Locations](#locations)
- [Access environments](#access-environments)
- [Reading and writing](#reading-and-writing)
- [Save games](#save-games)
- [File sockets: Open and Tail](#file-sockets-open-and-tail)
- [Errors](#errors)
- [Lifecycle and cleanup](#lifecycle-and-cleanup)
- [Inspection and debugging](#inspection-and-debugging)
- [Limits and tuning](#limits-and-tuning)
- [Examples](#examples)

UE4SSLuaFileBridge gives each Lua mod file and folder access inside two roots:
the game's install folder and its user data folder. This guide explains the
concepts and shows how to use them. The [API reference](API.md) is the
contract: every function, option, return value, error code and path rule is
specified there and not repeated here.

The examples in this guide and in `examples/` are untested in the game until
the first in-game acceptance run.

## Requirements and execution model

The current build targets UE4SS 3.0.1 Beta #0 at commit `97b7e501`, Unreal
Engine 5.5 and Windows x64, the same pin as UE4SSLuaEventBridge.

Every Lua mod receives its own `UE4SSLuaFileBridge` table when its Lua state
starts. Everything a mod creates through it (environments, streams,
subscriptions) belongs to that mod's Lua environment and is closed when it
stops or reloads.

File operations are synchronous. They run on the calling thread and don't need
the game thread: call them from the mod's main chunk, `LoopAsync`,
`ExecuteInGameThread` or a hook callback. The bridge never calls into Unreal.
Large reads and writes on the game thread stall the frame, so keep them small
there.

Operations are not available inside coroutines that the mod creates itself
(`coroutine.create`, `coroutine.wrap`). UE4SS refuses native calls from a Lua
thread it didn't register, so every operation returns `nil, "invalid",
message` there. Do the file work outside the coroutine and pass results in.

The bridge is a seatbelt, not a lock. Every UE4SS Lua mod already has the full
standard library (`io`, `os`), so it can write anywhere the user can. The
bridge makes the safe path the easy one and refuses the obvious mistakes made
through it.

## Version and capabilities

```lua
local version = UE4SSLuaFileBridge.GetVersion()        -- "1.0.1"
local capabilities = UE4SSLuaFileBridge.GetCapabilities()
local api = UE4SSLuaFileBridge.API_VERSION             -- 1
```

Gate features on `GetCapabilities()` (`streams`, `tail`, `append_only` and so
on; see [API.md](API.md#getcapabilities)), not on the version number. `lock`,
`hash` and `temp_file` are `false` in 1.0.1: those features are deferred.

## Locations

Paths start from a named **location**. Two of them are roots, which bound every
path the bridge accepts; the others are presets inside them.

| Location | Is | Use it for |
|---|---|---|
| `game` | The game install folder (root) | Reading game files and other mods' folders |
| `user` | `%LOCALAPPDATA%\Dawnwalker` (root) | Reading user config and logs |
| `mod` | This mod's folder under `Mods/` | Files that ship with or belong to the mod |
| `moddata` | `<user>/Saved/ModData/<mod>` | Data that should outlive a mod update or reinstall |
| `temp` | `<moddata>/temp` | Scratch files; emptied each time the bridge starts |
| `savegames` | `<user>/Saved/SaveGames` | Reading, and when granted writing, save files |

A path is a location name followed by a relative path, `"mod/cache/state.json"`,
or an absolute path inside one of the roots. Paths come back absolute, with
forward slashes. `Path` builds one, and `Locations` describes them all:

```lua
local files = UE4SSLuaFileBridge()
local save = files.Path("savegames", "Autosave0.sav")
-- "C:/Users/me/AppData/Local/Dawnwalker/Saved/SaveGames/Autosave0.sav"

for name, location in pairs(files.Locations()) do
    print(name .. " = " .. tostring(location.path)
        .. (location.exists and "" or " (missing)") .. "\n")
end
```

`moddata` is created the first time something is written under it, with a
small `.owner` file that records which mod it belongs to. Mods can read
`.owner` but not change it. Escapes with `..`, absolute paths outside both
roots, device and network paths, and links that lead outside are refused with
`outside_root`. The [path rules](API.md#paths) give the details, including how
links are resolved.

## Access environments

An **environment** is the set of grants a piece of code works with. You get the
safe one by calling the bridge, and you derive others with `AddPath`:

```lua
local files = UE4SSLuaFileBridge()
```

The **safe set** reads both roots and reads and writes this mod's folder and
`temp` (with delete allowed in `temp` only). Writing anywhere else, including
`moddata` and `savegames`, needs a grant:

```lua
local files = UE4SSLuaFileBridge()
    .AddPath("mod/cache", { delete = true })                    -- rw inherited from mod
    .AddPath("moddata",   { mode = "rw" })
    .AddPath("mod/logs",  { mode = "ao", extensions = { "log" } })
```

Things to know:

- **Each `AddPath` returns a new environment.** The one you started from is
  unchanged, so a helper can take the safe set, add what it needs, and keep the
  result to itself. There is no global switch and nothing to reset: an
  environment works wherever its variable is visible.
- **The most specific grant wins.** Grants aren't combined, and the order of the
  chain doesn't matter. A missing `mode` or `extensions` is inherited from the
  grant that encloses the path, so a default never widens access.
- **Modes:** `stat` (exists, stat, list), `ro` (plus reading), `wo` (writing,
  appending, making folders), `rw` (both), and `ao`, append-only, which never
  truncates, overwrites or deletes. Every mode can check existence. See the
  [mode table](API.md#what-each-mode-allows).
- **`delete`** is separate and off by default. It enables `Remove`,
  `RemoveTree` and moving a file away. A few folders can never be removed: the
  locations themselves, any folder that contains one (such as `user/Saved`)
  unless that exact folder is granted `delete`, drive roots and the user
  profile. See the [delete floor](API.md#delete-floor).
- **`extensions`** limits which files a grant covers. A file the grant doesn't
  cover falls back to the enclosing grant, so granting `rw` on `*.sav` leaves
  the `*.png` thumbnails beside them read-only rather than unreadable.
- **Mistakes raise.** An unknown option or a bad value makes `AddPath` raise a
  Lua error at the line that built the environment, so a chain never fails
  later as "attempt to index a nil value".

Environment functions accept both `files.ReadText(path)` and
`files:ReadText(path)`.

A common shape is one environment per concern, built once at load:

```lua
local base  = UE4SSLuaFileBridge()
local cache = base.AddPath("moddata/cache", { mode = "rw", delete = true })
local logs  = base.AddPath("mod/logs", { mode = "ao" })
```

## Reading and writing

```lua
local files = UE4SSLuaFileBridge().AddPath("moddata", { mode = "rw" })

assert(files.MakeDir("moddata/profiles"))
local ok, code, message = files.WriteText("moddata/profiles/default.json", json)
if ok == nil then
    print("save failed: " .. code .. " " .. message .. "\n")
end

local text = files.ReadText("moddata/profiles/default.json")
```

- **`WriteText` is atomic by default.** It writes a temporary file beside the
  target, flushes it, and swaps it in, so a reader or a crash sees the old
  content or the new content, never half of each. `{ atomic = false }` writes
  in place when you need to (not under `savegames`). It writes the bytes as
  given, so it also writes binary content. If the game crashes mid-write, the
  temporary file (`<name>.xbtmp-<pid>-<n>`) can stay behind. The bridge removes
  such leftovers at start only inside `moddata` folders; in mod folders and
  `savegames` they are **not cleaned automatically** and show up in `List`, so
  skip or remove names containing `.xbtmp-` if that matters to you.
- **`ReadText`** removes a UTF-8 byte-order mark and changes nothing else.
  **`ReadBytes`** returns the bytes exactly and can read a range
  (`{ offset = 128, length = 64 }`). A single read is limited to 64 MiB.
- **`Append`** adds to the end of a file and creates it if needed. Each call is
  one append, so two writers appending lines don't overwrite each other.
- **Parent folders must exist** for `WriteText`, `Append`, `Open`, `Copy` and
  `Move`. `MakeDir` creates a folder and its missing parents.
- **`List`** returns one folder's entries, sorted by name, each with `name`,
  `type`, `size`, `modified`, `created` and `link`. **`Stat`** returns type,
  size, times and flags for one path. Times are seconds since the Unix epoch,
  like `os.time()`.
- **`List` reads directory-entry metadata; `Stat` reads the file itself.** NTFS
  can update directory entries lazily for hardlinked files, so a listed size or
  time may lag. Use `Stat` when exact times matter.
- **`Copy`** and **`Move`** work on files and refuse to replace an existing
  destination unless given `{ overwrite = true }`. **`Remove`** removes a file
  or an empty folder; **`RemoveTree`** removes a folder and its contents, and
  removes nothing if any entry inside is not covered by a `delete` grant. Links
  are removed and moved as links; their targets are left alone. Moving a link
  to another drive is refused with `invalid`, since copying it would follow
  the link.

## Save games

Save files need care. Steam Cloud may sync them, so a bad write can reach the
cloud copy, and the game may hold a save open while writing it.

```lua
local saves = UE4SSLuaFileBridge()
    .AddPath("savegames", { mode = "rw", extensions = { "sav" } })

local ok, code = saves.WriteText("savegames/Autosave0.sav", bytes)
if code == "busy" then
    -- The game holds the file. Try again later; the bridge never waits.
end
```

- **Every overwrite keeps one backup.** Replacing a file under `savegames` with
  `WriteText`, `Copy` or `Move` leaves the previous content in
  `<name>.bak` (`Autosave0.sav.bak`), replacing an older backup. This is not an
  option.
- Operations that couldn't keep a backup are refused with `invalid`: a
  non-atomic `WriteText`, and `Open` that truncates an existing file.
- **`busy`** means another process holds the file. The bridge returns it at once
  instead of waiting; retry later, for example from a `LoopAsync` callback.

## File sockets: Open and Tail

`Open` and `Tail` together make a **file socket**: one mod holds a file open and
writes to it; another is called back whenever the file grows. Mods can talk to
each other through a path, without sharing a Lua state.

The writer opens a stream. By default each `Write` is handed to the operating
system immediately, so readers see it at once, and others can read the file
while it is open:

```lua
local files = UE4SSLuaFileBridge()
local outbox = assert(files.Open("mod/outbox.txt", { append = false }))
outbox:Write("ready ", 1, "\n")
-- ... later
outbox:Close()
```

The reader subscribes. It receives complete lines (without the newline) by
default, starting from the end of the file:

```lua
local files = UE4SSLuaFileBridge()
local inbox = "game/Dawnwalker/Binaries/Win64/ue4ss/Mods/WriterMod/outbox.txt"

local subscription = assert(files.Tail(inbox, function(line, info)
    print("got " .. line .. "\n")
end, { from = "start" }))
```

- The callback runs from the bridge's update callback, **not on the game
  thread**. Schedule Unreal work with `ExecuteInGameThread`.
- Delivery is budgeted per pass and usually arrives within a frame or so of
  the writer's write. The file is the buffer: a slow reader falls behind but
  doesn't lose lines, unless the file is truncated or replaced first.
- `info.offset` is the line's byte offset. `info.reset` is `true` on the first
  line after the file was truncated or replaced (the reader starts again from
  the beginning). `info.partial` marks a piece of a line longer than 1 MiB.
- `{ chunks = true }` delivers raw bytes as read instead of lines.
- The file may not exist yet; `Tail` waits for it as long as its folder exists.
- Tailing needs read access only, so the safe set can tail any file in the two
  roots, including another mod's folder.
- An error raised by the callback closes the subscription and is written to
  UE4SS.log. If the bridge itself has to close it (for example the folder was
  removed), it logs the reason and sets `subscription.closed`; there is no
  callback for that.

This is a pipe, not a message queue: there are no acknowledgements, no
redelivery and no distribution among several readers. Each reader sees every
line the file contains from its starting point.

For logs, combine an append-only grant with a stream:

```lua
local logs = UE4SSLuaFileBridge().AddPath("mod/logs", { mode = "ao", extensions = { "log" } })
assert(logs.MakeDir("mod/logs"))
local log = assert(logs.Open("mod/logs/session.log"))
log:Write(os.date("%H:%M:%S"), " started\n")
```

## Errors

Every operation returns its value on success and `nil, code, message` on
failure. Test for `nil`, not for truthiness: `Exists` returns `false` for a
missing file, which is an answer, not an error.

```lua
local text, code, message = files.ReadText("mod/settings.ini")
if text == nil then
    if code == "not_found" then
        text = DEFAULT_SETTINGS
    else
        print("settings: " .. code .. " " .. message .. "\n")
    end
end
```

Use `code` for decisions and `message` for logs; the message wording may
change. The eight codes are `not_found`, `denied`, `exists`, `outside_root`,
`read_only`, `busy`, `io` and `invalid`. [API.md](API.md#error-codes) says when
each occurs. In short: `denied` is a missing grant (or Windows refusing),
`busy` is someone else holding the file, `io` includes every capacity limit,
and `invalid` is a bad argument, the wrong kind of target, or a call from the
wrong place.

`AddPath` is the one function that raises instead of returning, because a bad
grant is a programming error.

## Lifecycle and cleanup

- **Streams and subscriptions** are closed automatically when the mod's Lua
  environment stops or reloads. Close them yourself when you are done:
  `stream:Close()` and `subscription:Close()` are idempotent.
- A stream dropped without `Close` is closed after it is garbage-collected, at
  the latest when its Lua environment stops. Collection timing is not a
  schedule; close explicitly.
- **Environments** need no cleanup. Unreferenced ones are released when
  collected. A function taken from an environment
  (`local read = files.ReadText`) keeps that environment alive.
- **`temp`** is emptied when the bridge starts (once per game launch), not on a
  Lua reload.
- **`moddata`** survives uninstalling the mod. Its `.owner` file records which
  mod created it and when it was last written, so leftovers can be found and
  removed by hand.

## Inspection and debugging

`GetDispatchStats()` returns process-wide `Tail` delivery counters:

```lua
local stats = UE4SSLuaFileBridge.GetDispatchStats()
print(string.format("subscriptions=%d delivered=%d budget_exhausted_passes=%d\n",
    stats.subscriptions, stats.delivered, stats.budget_exhausted_passes))
```

A rising `budget_exhausted_passes` means callbacks have more to deliver than
each pass allows; readers are falling behind. Make callbacks cheaper or raise
the budget (below). Counters last for the bridge lifetime; read them on demand.

When something is refused, the `message` names the operation and the path, and
Windows failures include the Windows error number. Subscription closures and
callback errors are written to UE4SS.log. `Locations()` shows where each
location points and whether it exists.

### Load-time verifications

The bridge's mod folder carries `Scripts/main.lua`, a set of in-game checks
of this API (source: [`lua/main_files.lua`](../lua/main_files.lua)), and
`Scripts/lex_verify.lua`, the harness all extensions share
([`shared/lua/lex_verify.lua`](../../shared/lua/lex_verify.lua)). They run
only when **Enable load-time verifications** is on in the bridge's ModCore
Settings page (`settings.ini`: `[Verifications] LoadTime=1`). It is off by
default and takes effect on the next launch. Results are `PASS`, `FAIL` and
`SKIP` lines tagged `[UE4SSLuaFileBridge verify]` in UE4SS.log, ending with a
`SUMMARY` line once the `Tail` checks finish (at most 10 seconds). They write
only under `temp/lexverify`, `moddata/lexverify` and `mod/lexverify`, and
remove what they write; saves are only read.

## Limits and tuning

| Limit | Value |
|---|---:|
| Grants per environment | 256 |
| Live environments per mod | 4,096 |
| Open streams per mod | 64 |
| `Tail` subscriptions per mod | 64 |
| Bytes per read | 64 MiB |
| `Tail` line length before splitting | 1 MiB |

Going over a capacity limit is `io` (`AddPath` raises it). See
[API.md](API.md#limits).

`Tail` delivery uses the same dispatch scheduling as UE4SSLuaEventBridge, with
its own budget. Set these process environment variables before starting the
game; they are read once when the bridge starts, and invalid values use the
defaults:

| Variable | Default | Zero means |
|---|---:|---|
| `UE4SSLFB_MAX_EVENTS_PER_PASS` | 256 | No delivery-count limit |
| `UE4SSLFB_MAX_DISPATCH_US` | 2,000 | No time limit |

At least one line is delivered per pass that has data, and a slow callback can
still exceed the time allowance; the budget isn't preemptive.

File reads are capped too, so polling many busy files can't stall UE4SS's
update thread. Each pass (20 per second by default) reads at most 256 KiB in
total across all subscriptions and at most 64 KiB from any one file, visiting
subscriptions round-robin so one busy file can't starve the rest. These are
tuning values, not capacity limits: nothing fails when they are reached, and
the rest is read on later passes. They do bound throughput, to roughly 5 MiB/s
in total and 1.25 MiB/s per file. A file socket is for messages, not bulk
transfer.

## Examples

Copy an example folder into `Mods/`, enable it and the file bridge, and read its
lines in UE4SS.log. They are untested in the game until the first in-game
acceptance run.

- [FileRoundTrip](../examples/FileRoundTrip/Scripts/main.lua): makes a folder,
  writes, appends, reads, stats, lists and removes files under its own mod
  folder (including a non-ASCII file name), and shows three refusals: removing
  without a `delete` grant, writing a save without a grant, and reading outside
  the roots.
- [FileSocketWriter](../examples/FileSocketWriter/Scripts/main.lua) and
  [FileSocketReader](../examples/FileSocketReader/Scripts/main.lua): install
  both. The writer appends a line a second to its outbox; the reader tails it
  and logs each line.
