-- A mod logging through UE4SS Lua Log Bridge: a UE4SS.log logger for notable
-- events and a per-frame worker logger writing LogDemo/logs/<category>.log.
local Logs = UE4SSLuaLogBridge and UE4SSLuaLogBridge()
if not Logs then
    print("[LogDemo] UE4SS Lua Log Bridge is not installed\n")
    return
end

local log = assert(Logs.Register("LogDemo", { level = "info" }))
local trace = assert(Logs.Register("LogDemoTrace", {
    target = "module", frequency = "frame", thread = "worker", level = "trace", categories = { "combat" },
}))

log.info("loaded; bridge ", Logs.GetVersion())
log.config.warn("no settings file; using defaults")

local ticks = 0
LoopAsync(1000, function()
    ticks = ticks + 1
    trace.combat.trace("tick ", ticks)
    if trace.enabled("debug") then trace.debug("stats: ", Logs.GetStats().written, " lines so far") end
    return ticks >= 10
end)
