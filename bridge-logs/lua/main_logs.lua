-- UE4SSLuaLogBridge load-time verifications. Ships as Scripts/main.lua in the
-- bridge's own mod folder and runs only when the MCS setting "Enable load-time
-- verifications" is on (settings.ini: [Settings] EnableVerifications=1); it is off by
-- default. Results go to UE4SS.log as PASS, FAIL and SKIP lines, followed by a
-- SUMMARY line once the timed delivery checks finish. The harness is the
-- shared Scripts/lex_verify.lua.
--
-- It writes logs/lexverify*.log files into this mod's own folder and removes them again.

local Verify = require("lex_verify")
if not Verify.enabled() then return end

local V = Verify.new("[UE4SSLuaLogBridge verify] ")
local log, check, group, ok, refused, raises, skip, summary =
    V.log, V.check, V.group, V.ok, V.refused, V.raises, V.skip, V.summary
local modFolder = Verify.modFolder

local function read(name)
    local file = io.open(modFolder() .. "/logs/" .. name, "rb")
    if not file then return nil end
    local text = file:read("a")
    file:close()
    return text
end

local function cleanup()
    for _, name in ipairs({ "lexverify.log", "lexverify.prev.log", "lexverifyworker.log",
        "lexverifyworker.prev.log" }) do
        os.remove(modFolder() .. "/logs/" .. name)
    end
end

local WAIT_MS = 600

local function main()
    local Bridge = UE4SSLuaLogBridge
    if type(Bridge) ~= "table" then
        V.fail("UE4SSLuaLogBridge global", "missing; the native bridge did not start for this Lua state")
        summary()
        return
    end
    log("load-time verifications enabled; bridge " .. tostring(Bridge.GetVersion and Bridge.GetVersion()))
    local logs = Bridge()
    local stamp = tostring(os.time()) .. "-" .. tostring(math.random(1, 1 << 30))

    group("entry point", function()
        check("UE4SSLuaLogBridge() returns the environment", type(logs) == "table" and Bridge() == logs)
        local version = Bridge.GetVersion()
        check("GetVersion is x.y.z", type(version) == "string" and version:match("^%d+%.%d+%.%d+$") ~= nil, version)
        check("API_VERSION is 1", Bridge.API_VERSION == 1 and logs.API_VERSION == 1, Bridge.API_VERSION)
        local caps = Bridge.GetCapabilities()
        check("capabilities api is 1", caps.api == 1, caps.api)
        check("journal is 4 MiB", caps.journal_bytes == 4194304, caps.journal_bytes)
        log("engine tick: " .. tostring(caps.frame))
        local stats = logs.GetStats()
        check("GetStats fields", math.type(stats.written) == "integer" and math.type(stats.generation) == "integer")
        raises("assigning an environment field", nil, function() logs.Register = nil end)
    end)

    local fileLog, workerLog
    group("registering", function()
        fileLog = ok("Register a module-file logger", logs.Register("LexVerify", { target = "module",
            categories = { "lexverify" } }))
        workerLog = ok("Register a periodic logger", logs.Register("LexVerifyWorker", { target = "module",
            frequency = "periodic", interval = 0.1, level = "trace",
            categories = { "lexverifyworker" } }))
        ok("Register a frame logger", logs.Register("LexVerifyFrame", { frequency = "frame", level = "OFF" }))
        refused("unknown option", "invalid", logs.Register("LexVerify2", { colour = "red" }))
        refused("bad module name", "invalid", logs.Register("bad/name"))
        refused("global is not a module name", "invalid", logs.Register("global"))
        if fileLog then
            check("default categories", fileLog.main == fileLog and fileLog.config ~= nil and fileLog.ui ~= nil
                and fileLog.input ~= nil)
            check("one logger per name", logs.Register("lexverify") == fileLog)
            check("an unregistered category is no field", fileLog.lexverifx == nil)
            check("register adds a category", fileLog.register("extra") == fileLog.extra and fileLog.extra ~= nil)
            refused("a category named like a method", "invalid", fileLog.register("write"))
        end
    end)

    group("levels", function()
        if not fileLog then return skip("levels", "no logger") end
        check("default level is WARN", fileLog.level == "WARN", fileLog.level)
        check("SetLevel", logs.SetLevel("LexVerify.lexverify", "debug") == true)
        check("the category follows at once", fileLog.lexverify.enabled("debug") and fileLog.lexverify.level == "DEBUG")
        check("the module does not", not fileLog.enabled("debug"))
        check("GetLevel", logs.GetLevel("LexVerify.lexverify") == "DEBUG", logs.GetLevel("LexVerify.lexverify"))
        refused("SetLevel with an unknown level", "invalid", logs.SetLevel("LexVerify", "loud"))
        refused("SetLevel with a bad key", "invalid", logs.SetLevel("a.b.c", "INFO"))
    end)

    group("writing to a module file", function()
        if not fileLog then return skip("writing", "no logger") end
        fileLog.lexverify.debug("debug line ", stamp)
        fileLog.lexverify.trace("trace line ", stamp)
        fileLog.lexverify:warn("warn line ", stamp)
        check("Flush", logs.Flush() == true)
        local text = read("lexverify.log") or ""
        check("debug line written", text:find("DEBUG debug line " .. stamp, 1, true) ~= nil)
        check("trace line filtered", text:find("trace line " .. stamp, 1, true) == nil)
        check("warn line written", text:find("WARN warn line " .. stamp, 1, true) ~= nil)
        check("lines carry a timestamp", text:match("%d%d%d%d%-%d%d%-%d%d %d%d:%d%d:%d%d%.%d%d%d DEBUG") ~= nil)
        logs.SetLevel("LexVerify.lexverify", nil)
        check("SetLevel nil clears", fileLog.lexverify.level == "WARN", fileLog.lexverify.level)
    end)

    if workerLog then workerLog.lexverifyworker.trace("worker line ", stamp) end
    local before = logs.GetStats()
    local waited = 0
    LoopAsync(100, function()
        waited = waited + 100
        if waited < WAIT_MS then return false end
        group("worker delivery", function()
            if not workerLog then return skip("worker delivery", "no logger") end
            local text = read("lexverifyworker.log") or ""
            check("the worker wrote the periodic line", text:find("TRACE worker line " .. stamp, 1, true) ~= nil)
            local stats = logs.GetStats()
            log("written " .. stats.written .. ", delivered " .. stats.delivered .. ", pending " .. stats.pending
                .. ", stalls " .. stats.stalls .. ", recovered " .. stats.recovered)
            check("nothing left pending", stats.pending == 0, stats.pending)
            check("written counts up", stats.written >= before.written, stats.written)
        end)
        group("cleanup", function()
            cleanup()
            check("lexverify files removed", read("lexverify.log") == nil)
        end)
        summary()
        return true
    end)
end

V.run(main)
