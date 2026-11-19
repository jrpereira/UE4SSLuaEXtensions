-- Lua-layer tests for UE4SSLuaLogBridge (bridge-logs/lua/logs_api.lua),
-- against a fake native table that follows the native contract.
-- Run from the repository root with lua5.4.
package.path = "bridge-logs/tests/support/?.lua;shared/tests/?.lua;" .. package.path
local Fake = require("FakeLogNatives")

local API_PATH = "bridge-logs/lua/logs_api.lua"

local Kit = require("TestKit")
local expect, expectFailure, expectRaise = Kit.expect, Kit.expectFailure, Kit.expectRaise

local function fresh()
    local fake = Fake.new()
    fake.install()
    local chunk = assert(loadfile(API_PATH))
    chunk()
    return fake, UE4SSLuaLogBridge
end

local suite = Kit.suite("UE4SSLuaLogBridge Lua tests")
local function test(name, fn)
    suite.run(name, function()
        local fake, bridge = fresh()
        fn(fake, bridge, bridge())
        if #fake.violations > 0 then
            error("native contract violations: " .. table.concat(fake.violations, ", "))
        end
    end)
end

local function last(fake)
    return fake.lines[#fake.lines]
end

-- Loading ---------------------------------------------------------------------------

test("load hides natives and the session id", function(_, bridge, logs)
    expect(__UE4SSLuaLogBridge_SessionId == nil)
    for name in pairs(Fake.SHAPES) do
        expect(_G["UE4SSLuaLogBridge_" .. name] == nil, name .. " is still global")
    end
    expect(type(bridge) == "table" and bridge() == logs)
    expectRaise("read-only", function() bridge.Register = 1 end)
    expectRaise("read-only", function() logs.Register = 1 end)
    expect(getmetatable(bridge) == false and getmetatable(logs) == false)
    expect(bridge.API_VERSION == 1 and logs.API_VERSION == 1)
end)

test("version, capabilities and stats", function(_, bridge, logs)
    expect(bridge.GetVersion() == "1.0.0" and logs:GetVersion() == "1.0.0")
    local caps = logs.GetCapabilities()
    expect(caps.api == 1 and caps.journal_bytes == 4194304 and caps.max_message_bytes == 65536 and caps.frame == true)
    expect(#caps.levels == 7 and caps.default_categories[1] == "main" and #caps.default_categories == 4)
    local stats = bridge.GetStats()
    expect(stats.written == 0 and stats.generation == 1 and stats.loggers == 0)
end)

-- Registering -------------------------------------------------------------------------

test("register by name, with defaults", function(fake, _, logs)
    local log = assert(logs.Register("Preymonition"))
    expect(log.name == "Preymonition" and tostring(log) == "UE4SSLuaLogBridge logger Preymonition")
    local options = fake.loggers[1]
    expect(options.target == 0 and options.frequency == 0 and options.interval == 1000 and options.thread == 0)
    expect(options.rank == 0)
    expect(logs.Register("preymonition") == log, "one logger per name in a Lua state")
    expect(fake.counts.Register == 1)
    expectRaise("read-only", function() log.debug = nil end)
end)

test("register without a name uses the mod folder", function(fake, _, logs)
    local log = assert(logs:Register({ target = "module" }))
    expect(log.name == Fake.MOD and fake.loggers[1].target == 1)
    expect(logs.Register() == log and fake.counts.Register == 1)
end)

test("register options", function(fake, _, logs)
    assert(logs.Register("A", { target = "Module", frequency = "periodic", interval = 2.5, thread = "worker",
        level = "info", categories = { "combat", "AI" } }))
    local options = fake.loggers[1]
    expect(options.target == 1 and options.frequency == 2 and options.interval == 2500 and options.thread == 1)
    expect(options.rank == 3)
    local log = logs.Register("A")
    expect(log.combat and log.ai and log.combat.name == "combat")
    assert(logs.Register("B", { frequency = "frame", level = 5 }))
    expect(fake.loggers[2].frequency == 1 and fake.loggers[2].rank == 5)
end)

test("register refusals", function(_, _, logs)
    expectFailure("invalid", logs.Register(42))
    expectFailure("invalid", logs.Register("A", "x"))
    expectFailure("invalid", logs.Register("A", { colour = "red" }))
    expectFailure("invalid", logs.Register("A", { target = "disk" }))
    expectFailure("invalid", logs.Register("A", { frequency = "sometimes" }))
    expectFailure("invalid", logs.Register("A", { thread = 1 }))
    expectFailure("invalid", logs.Register("A", { interval = 0 }))
    expectFailure("invalid", logs.Register("A", { interval = 0 / 0 }))
    expectFailure("invalid", logs.Register("A", { level = "loud" }))
    expectFailure("invalid", logs.Register("A", { categories = "combat" }))
    expectFailure("invalid", logs.Register("A", { categories = { "con" } }))
    expectFailure("invalid", logs.Register("A", { categories = { "2d" } }))
    expectFailure("invalid", logs.Register("bad/name"))
end)

-- Writing -----------------------------------------------------------------------------

test("levels filter before arguments are converted", function(fake, _, logs)
    local log = logs.Register("Mod")
    local converted = 0
    local probe = setmetatable({}, { __tostring = function() converted = converted + 1 return "probe" end })
    log.debug("hidden ", probe)
    expect(#fake.lines == 0 and converted == 0, "WARN is the default")
    log.warn("shown ", probe, " ", 3, " ", nil, " ", true)
    expect(converted == 1)
    local line = last(fake)
    expect(line.message == "shown probe 3 nil true" and line.level == "WARN" and line.category == "main")
    log.error("e")
    log.critical("c")
    expect(#fake.lines == 3 and last(fake).rank == 6)
end)

test("colon calls and write with a level", function(fake, _, logs)
    local log = logs.Register("Mod", { categories = { "combat" } })
    log:warn("colon")
    expect(last(fake).message == "colon")
    log.write("error", "dot ", 1)
    expect(last(fake).message == "dot 1" and last(fake).rank == 5)
    log:write("CRITICAL", "colon write")
    expect(last(fake).message == "colon write" and last(fake).rank == 6)
    log.write(5, "by rank")
    expect(last(fake).message == "by rank")
    expectFailure("invalid", log.write("loud", "x"))
    expectFailure("invalid", log.write("off", "x"))
    log.combat:error("category colon")
    expect(last(fake).category == "combat" and last(fake).message == "category colon")
end)

test("default and extra categories", function(fake, _, logs)
    local log = logs.Register("Mod")
    for _, name in ipairs({ "main", "config", "ui", "input" }) do
        expect(type(log[name]) == "table", name)
    end
    expect(log.main == log)
    log.config.error("bad value")
    expect(last(fake).category == "config")
end)

test("categories are registered explicitly", function(fake, _, logs)
    local log = logs.Register("Mod")
    expect(log.combat == nil and log.commbat == nil, "an unregistered category is no field")
    local ai, combat = log.register("AI", "combat")
    expect(ai == log.ai and combat == log.combat and ai.name == "ai")
    expect(log:register("ai") == ai, "registering again returns the same category")
    expect(log.register("main") == log)
    log.combat.error("hit")
    expect(last(fake).category == "combat")
    expect(log["bad name"] == nil and log.nul == nil and log.Write == nil)
    expectFailure("invalid", log.register("bad name"))
    expectFailure("invalid", log.register("nul"))
    expectFailure("invalid", log.register("write"))
    expectFailure("invalid", log.register("Enabled"))
    expectFailure("invalid", log.register("ok", 5))
    expect(log.ok == nil, "a refused register adds nothing")
    expect(logs.Register("Mod", { categories = { "loot" } }) == log and log.loot ~= nil,
        "categories in a later Register add to the logger")
    expectFailure("invalid", logs.Register("Mod", { categories = { "level" } }))
end)

test("enabled and level", function(fake, _, logs)
    local log = logs.Register("Mod", { categories = { "combat" } })
    expect(log.enabled("warn") and log:enabled("ERROR") and not log.enabled("info") and not log.enabled("loud"))
    expect(log.level == "WARN")
    fake.setLevel("mod.combat", 1)
    expect(log.combat.enabled("trace") and log.combat.level == "TRACE" and log.level == "WARN")
end)

test("NUL bytes and unprintable values", function(fake, _, logs)
    local log = logs.Register("Mod")
    log.warn("a\0b")
    expect(last(fake).message == "a\\0b")
    local bad = setmetatable({}, { __tostring = function() error("boom") end })
    log.warn(bad)
    expect(last(fake).message:find("^<unprintable"))
end)

-- Levels changing elsewhere -----------------------------------------------------------

test("a new generation from a write refreshes the cache", function(fake, _, logs)
    local log = logs.Register("Mod")
    log.debug("hidden")
    fake.setLevel("mod", 2)
    log.warn("written; carries the new generation")
    log.debug("now shown")
    expect(#fake.lines == 2 and last(fake).message == "now shown")
end)

test("filtered calls poll the generation", function(fake, _, logs)
    local log = logs.Register("Mod")
    log.debug("first")
    fake.setLevel("", 1)
    for _ = 1, 62 do log.trace("hidden") end -- with "first": 63 filtered calls
    expect(#fake.lines == 0)
    log.trace("the 64th filtered call polls")
    log.trace("shown")
    expect(#fake.lines == 1 and last(fake).message == "shown")
end)

test("registered defaults and per-category levels", function(fake, _, logs)
    local log = logs.Register("Mod", { level = "debug" })
    log.debug("shown")
    expect(#fake.lines == 1)
    fake.setLevel("mod.ui", 6)
    log.warn("refresh")
    log.ui.error("hidden")
    log.ui.critical("shown")
    expect(#fake.lines == 3 and last(fake).category == "ui")
end)

test("SetLevel and GetLevel", function(fake, _, logs)
    local log = logs.Register("Mod")
    expect(logs.SetLevel("Mod", "debug") == true)
    expect(fake.levels.mod == 2)
    log.debug("shown at once")
    expect(#fake.lines == 1)
    expect(logs.GetLevel("Mod") == "DEBUG" and logs.GetLevel() == "WARN")
    expect(logs:SetLevel(nil, 1) == true and fake.levels[""] == 1)
    expect(logs.SetLevel("Mod") == true and fake.levels.mod == nil)
    expectFailure("invalid", logs.SetLevel("Mod", "loud"))
    expectFailure("invalid", logs.SetLevel(5, "INFO"))
    expectFailure("invalid", logs.SetLevel("bad/key", "INFO"))
end)

test("Flush", function(fake, _, logs)
    expect(logs.Flush() == true and fake.flushed == 1)
end)

-- Refused threads -----------------------------------------------------------------------

test("a refused coroutine falls back to print", function(fake, _, logs)
    local log = logs.Register("Mod", { categories = { "combat" } })
    log.warn("prime the cache")
    local printed = {}
    local savedPrint = print
    print = function(text) printed[#printed + 1] = text end
    local co = coroutine.create(function()
        log.warn("from a coroutine")
        log.combat.error("category")
        expectFailure("invalid", logs.Flush())
    end)
    local ok, err = coroutine.resume(co)
    print = savedPrint
    expect(ok, tostring(err))
    expect(#fake.lines == 1)
    expect(printed[1] == "[Mod] WARN from a coroutine\n", printed[1])
    expect(printed[2] == nil or printed[2] == "[Mod:combat] ERROR category\n")
end)

suite.finish()
