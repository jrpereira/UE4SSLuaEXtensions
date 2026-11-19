# UE4SS Lua Log Bridge

Leveled logging for UE4SS Lua mods, controlled per mod and per category, with
crash-safe delivery.

- Levels TRACE, DEBUG, INFO, WARN, ERROR and CRITICAL; WARN by default.
- Each mod registers a logger with categories (`main`, `config`, `ui`, `input`
  and any it registers).
- Levels change per mod, per category or globally, from settings.ini, the
  ModCore Settings menu or Lua, and apply while the game runs.
- Lines go to UE4SS.log or to the mod's own `logs/<category>.log`.
- Written at once, once per frame or every few seconds, on the game thread or
  on the bridge's own thread so disk writes never touch a frame.
- Every line lands in a memory-mapped journal before the call returns, so a
  crash loses nothing; the next start writes the previous session's lines to
  `logs/previous-session.log`.

```lua
local log = UE4SSLuaLogBridge().Register("MyMod", { target = "module", thread = "worker",
    categories = { "combat" } })
log.info("loaded ", count, " entries")
log.combat.debug("hit ", target)
```

Mod folder: `0_ModCore_UE4SSLuaLogBridge`. Requires UE4SS (commit 97b7e501,
the version Dawnwalker uses) on Windows x64.

- [API reference](docs/API.md)
- [Example](examples/LogDemo/Scripts/main.lua)

## Moving from mc_log

```lua
-- before
local log = require('mc_log').new({name = 'MyMod', path = root .. '/log_level.txt'})
-- after
local log = UE4SSLuaLogBridge().Register('MyMod')
```

Calls (`log.debug(...)`, `log:warn(...)`, `log.enabled('debug')`, `log.level`) and the
UE4SS.log line format stay the same. The level moves from `log_level.txt` to
`[Levels] MyMod=DEBUG` in the bridge's settings.ini. Declare the dependency in
`mod.json`:

```json
{ "id": "UE4SSLuaLogBridge", "version": "1.0.0+",
  "url": "https://github.com/jrpereira/UE4SSLuaEXtensions" }
```
