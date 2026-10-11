-- Lua-layer tests for UE4SSLuaFileBridge (bridge-files/lua/files_api.lua),
-- against a fake native table that follows the frozen native contract.
-- Run from the repository root with lua5.4.
package.path = "bridge-files/tests/support/?.lua;shared/tests/?.lua;" .. package.path
local Fake = require("FakeFileNatives")

local API_PATH = "bridge-files/lua/files_api.lua"
local THIS_FILE = "FilesApiTests.lua"

local Kit = require("TestKit")
local expect, expectFailure, expectRaise = Kit.expect, Kit.expectFailure, Kit.expectRaise

-- Loads a fresh Lua layer over a fresh fake; returns the fake, the global and
-- the dispatcher.
local function fresh()
    local fake = Fake.new()
    fake.install()
    local chunk = assert(loadfile(API_PATH))
    local dispatch = chunk()
    return fake, UE4SSLuaFileBridge, dispatch
end

local suite = Kit.suite("UE4SSLuaFileBridge Lua tests")
local function test(name, fn)
    suite.run(name, function()
        local fake, bridge, dispatch = fresh()
        fn(fake, bridge, dispatch)
        if #fake.violations > 0 then
            error("native contract violations: " .. table.concat(fake.violations, ", "))
        end
    end)
end

-- Creates environments that are unreachable once this function returns (a
-- block-local could survive in a dead stack slot).
local function garbage(env, ...)
    for index = 1, select("#", ...) do env.AddPath((select(index, ...))) end
end

local function countCalls(fake, name)
    return #fake.callsTo(name)
end

-- Loading --------------------------------------------------------------------

test("load hides natives and the session id and returns the dispatcher", function(fake, bridge, dispatch)
    expect(type(dispatch) == "function")
    expect(__UE4SSLuaFileBridge_SessionId == nil)
    for name in pairs(Fake.SHAPES) do
        expect(_G["UE4SSLuaFileBridge_" .. name] == nil, name .. " is still global")
    end
    expect(type(bridge) == "table" and bridge.API_VERSION == 1)
    expect(#fake.calls == 0, "loading must not call natives")
end)

test("load fails when a native is missing or the session id is absent", function()
    local fake = Fake.new()
    fake.install()
    UE4SSLuaFileBridge_TailClose = nil
    local ok, message = pcall(assert(loadfile(API_PATH)))
    expect(not ok and tostring(message):find("UE4SSLuaFileBridge_TailClose", 1, true))
    fake.install()
    __UE4SSLuaFileBridge_SessionId = nil
    ok = pcall(assert(loadfile(API_PATH)))
    expect(not ok)
end)

test("the global table is read-only and locked", function(_, bridge)
    expectRaise("read-only", function() bridge.GetVersion = nil end)
    expectRaise("read-only", function() bridge.Extra = 1 end)
    expect(getmetatable(bridge) == false)
    expect(bridge() == bridge(), "the safe environment is reused")
end)

-- Version, capabilities, statistics, locations -------------------------------

test("GetVersion, GetCapabilities and GetDispatchStats", function(_, bridge)
    local env = bridge()
    expect(bridge.GetVersion() == "1.0.1" and env.GetVersion() == "1.0.1" and env:GetVersion() == "1.0.1")
    local caps = bridge.GetCapabilities()
    expect(caps.api == 1 and caps.environments and caps.append_only and caps.streams and caps.tail)
    expect(caps.lock == false and caps.hash == false and caps.temp_file == false)
    expect(caps.max_read_bytes == 67108864 and caps.target_ue4ss_commit == "97b7e501")
    expect(table.concat(caps.locations, ",") == "game,user,mod,moddata,temp,savegames")
    expect(bridge.GetCapabilities() ~= caps, "a new table each call")
    local stats = env:GetDispatchStats()
    expect(stats.subscriptions == 2 and stats.delivered == 1840 and stats.budget_exhausted_passes == 3)
end)

test("Locations decodes records, including unbound locations", function(fake, bridge)
    fake.unbound.mod, fake.unbound.moddata, fake.unbound.temp = true, true, true
    local locations = bridge().Locations()
    expect(locations.game.path == "C:/Root/game" and locations.game.root == true and locations.game.exists == true)
    expect(locations.savegames.root == false)
    expect(locations.mod.path == nil and locations.mod.exists == false)
    expect(locations.temp.path == nil)
end)

-- Lexical helpers --------------------------------------------------------------

test("Join, Parent, Name, Stem and Extension", function(fake, bridge)
    local env = bridge()
    expect(env.Join("mod", "cache", "x.json") == "mod/cache/x.json")
    expect(env.Join("mod/", "/cache\\sub", "x.json") == "mod/cache/sub/x.json")
    expect(env.Join("C:/", "x") == "C:/x")
    expect(env.Join("mod/cache/") == "mod/cache")
    expectFailure("invalid", env.Join())
    expectFailure("invalid", env.Join("mod", 3))
    expectFailure("invalid", env.Join("mod", "a\0b"))
    expect(env.Parent("mod/a/b.txt") == "mod/a")
    expect(env.Parent("mod\\a\\b.txt") == "mod/a")
    expect(env.Parent("mod/a/") == "mod")
    expect(env.Parent("C:/x") == "C:")
    expectFailure("invalid", env.Parent("mod"))
    expectFailure("invalid", env.Parent("C:"))
    expectFailure("invalid", env.Parent("C:/"))
    expect(env.Name("mod/a/b.txt") == "b.txt" and env.Name("mod/a/") == "a")
    expect(env.Stem("mod/a/b.txt") == "b" and env.Stem("a.tar.gz") == "a.tar" and env.Stem(".owner") == ".owner")
    expect(env.Extension("b.txt") == "txt" and env.Extension("a.tar.gz") == "gz")
    expect(env.Extension(".owner") == "" and env.Extension("noext") == "")
    for _, helper in ipairs({ "Parent", "Name", "Stem", "Extension" }) do
        expectFailure("invalid", env[helper]({}))
    end
    expect(#fake.calls == 0, "lexical helpers never call natives")
end)

test("Path validates the location and normalizes natively", function(fake, bridge)
    local env = bridge()
    expect(env.Path("savegames", "Autosave0.sav") == "C:/Root/savegames/Autosave0.sav")
    expect(env.Path("mod") == "C:/Root/mod")
    expect(env:Path("mod", "a/b", "c") == "C:/Root/mod/a/b/c")
    expect(fake.lastCall().name == "Normalize" and fake.lastCall().args[2] == "mod/a/b/c")
    expectFailure("invalid", env.Path("nowhere", "x"))
    expectFailure("invalid", env.Path("C:/x"))
    expectFailure("invalid", env.Path(1))
    expectFailure("invalid", env.Path("mod", false))
    expectFailure("outside_root", env.Path("mod", "../OtherMod"))
    expect(env.Normalize("mod/x") == "C:/Root/mod/x")
    expectFailure("invalid", env.Normalize(nil))
end)

-- Call styles, arguments, errors ------------------------------------------------

test("environments accept dot and colon calls alike", function(fake, bridge)
    local env = bridge()
    fake.files["C:/Root/mod/a.txt"] = "hello"
    expect(env.ReadText("mod/a.txt") == "hello")
    expect(env:ReadText("mod/a.txt") == "hello")
    expect(env.Exists("mod/a.txt") == true and env:Exists("mod/b.txt") == false)
    local derived = env:AddPath("mod/cache", { delete = true })
    local again = env.AddPath("mod/cache", { delete = true })
    expect(fake.policies[2].path == "mod/cache" and fake.policies[3].path == "mod/cache")
    expect(derived.Exists("mod/a.txt") and again:Exists("mod/a.txt"))
    expect(env.ReadText == env.ReadText, "bound functions are cached")
end)

test("bad arguments return invalid before any native call", function(fake, bridge)
    local env = bridge()
    local before = #fake.calls
    expectFailure("invalid", env.ReadText(42))
    expectFailure("invalid", env.ReadText(nil))
    expectFailure("invalid", env.ReadText("mod/a\0b"))
    expectFailure("invalid", env.Exists({}))
    expectFailure("invalid", env.WriteText("mod/a", 12))
    expectFailure("invalid", env.WriteText("mod/a", "x", "atomic"))
    expectFailure("invalid", env.WriteText("mod/a", "x", { atomic = 1 }))
    expectFailure("invalid", env.WriteText("mod/a", "x", { atomics = false }))
    expectFailure("invalid", env.WriteText("mod/a", "x", { [1] = true }))
    expectFailure("invalid", env.Append("mod/a", nil))
    expectFailure("invalid", env.ReadBytes("mod/a", { offset = -1 }))
    expectFailure("invalid", env.ReadBytes("mod/a", { length = 1.5 }))
    expectFailure("invalid", env.ReadBytes("mod/a", { length = "2" }))
    expectFailure("invalid", env.Copy("mod/a", 5))
    expectFailure("invalid", env.Move(5, "mod/b"))
    expectFailure("invalid", env.Copy("mod/a", "mod/b", { overwrite = "yes" }))
    expectFailure("invalid", env.Open("mod/a", { flush = "always" }))
    expectFailure("invalid", env.Open("mod/a", { append = 0 }))
    expectFailure("invalid", env.Tail("mod/a", "not a function"))
    expectFailure("invalid", env.Tail("mod/a", function() end, { from = "middle" }))
    expectFailure("invalid", env.Tail("mod/a", function() end, { lines = true }))
    expect(#fake.calls == before, "no native may run for a bad argument")
end)

test("options map to the contract's flags and defaults", function(fake, bridge)
    local env = bridge()
    fake.files["C:/Root/mod/a"] = "0123456789"
    expect(env.ReadBytes("mod/a") == "0123456789")
    local args = fake.lastCall().args
    expect(args[4] == 0 and args[5] == -1)
    expect(env.ReadBytes("mod/a", { offset = 2.0, length = 3 }) == "234")
    args = fake.lastCall().args
    expect(math.type(args[4]) == "integer" and args[4] == 2 and args[5] == 3)
    assert(env.WriteText("mod/a", "x"))
    expect(fake.lastCall().args[6] == 0)
    assert(env.WriteText("mod/a", "x", { atomic = false }))
    expect(fake.lastCall().args[6] == 1)
    assert(env.Copy("mod/a", "mod/b"))
    expect(fake.lastCall().args[5] == 0)
    assert(env.Move("mod/a", "mod/b", { overwrite = true }))
    expect(fake.lastCall().name == "Move" and fake.lastCall().args[5] == 1)
    assert(env.Open("mod/s"))
    expect(fake.lastCall().args[4] == 0)
    assert(env.Open("mod/s", { append = false, create = false, flush = "manual" }))
    expect(fake.lastCall().args[4] == 7)
    assert(env.Tail("mod/t", function() end))
    expect(fake.lastCall().args[5] == 0)
    assert(env.Tail("mod/t", function() end, { chunks = true, from = "start" }))
    expect(fake.lastCall().args[5] == 3)
    for _, name in ipairs({ "MakeDir", "Remove", "RemoveTree" }) do
        expect(env[name]("mod/d") == true and fake.lastCall().name == name)
        expect(fake.lastCall().args[2] == Fake.SAFE_POLICY)
    end
end)

test("native failures pass through as nil, code, message", function(fake, bridge)
    local env = bridge()
    expectFailure("not_found", env.ReadText("mod/missing"))
    fake.failures.Remove = { "denied", "Remove C:/Root/mod: delete floor" }
    local message = expectFailure("denied", env.Remove("mod"))
    expect(message == "Remove C:/Root/mod: delete floor")
    fake.failures.WriteText = { "busy", "WriteText: sharing violation (win32 32)" }
    expectFailure("busy", env.WriteText("savegames/a.sav", "x"))
    expect(env.Exists("mod/none") == false, "false is a success value")
    fake.failures.Exists = { "denied", "Exists: denied" }
    expectFailure("denied", env.Exists("savegames/x"))
end)

test("Stat and List decode native results", function(fake, bridge)
    local env = bridge()
    fake.files["C:/Root/mod/a.json"] = "12345"
    local stat = env.Stat("mod/a.json")
    expect(stat.type == "file" and stat.size == 5 and stat.modified == 1791640800.25)
    expect(stat.created == 1791640000 and stat.readonly == false and stat.link == false)
    expectFailure("not_found", env.Stat("mod/none"))
    fake.listing = ""
    local empty = env.List("mod")
    expect(type(empty) == "table" and #empty == 0)
    fake.listing = "a.json\tfile\t10\t1791640800\t0\t1791640000\n"
        .. "sub\tdirectory\t0\t1791640000.5\t1\t1791630000.25\n"
        .. "\tother\t0\t0\t0\t0"
    local entries = env.List("mod")
    expect(#entries == 3)
    expect(entries[1].name == "a.json" and entries[1].type == "file" and entries[1].size == 10)
    expect(math.type(entries[1].size) == "integer" and entries[1].link == false)
    expect(entries[1].created == 1791640000)
    expect(entries[2].modified == 1791640000.5 and entries[2].link == true)
    expect(entries[2].created == 1791630000.25)
    expect(entries[3].name == "" and entries[3].type == "other", "empty fields are kept")
    -- Records from before `created` was added decode without it.
    fake.listing = "old.txt\tfile\t3\t1791640800\t0\nshort\tfile"
    entries = env.List("mod")
    expect(#entries == 2 and entries[1].name == "old.txt" and entries[1].created == nil)
    expect(entries[1].size == 3 and entries[1].modified == 1791640800)
    expect(entries[2].size == nil and entries[2].modified == nil and entries[2].link == false)
end)

-- AddPath -------------------------------------------------------------------------

test("AddPath sends the wire form of its options", function(fake, bridge)
    local env = bridge()
    env.AddPath("savegames", { mode = "rw", extensions = { "sav", "META" } })
    local args = fake.lastCall().args
    expect(args[2] == Fake.SAFE_POLICY and args[3] == "savegames")
    expect(args[4] == "rw" and args[5] == "sav;META" and args[6] == 4)
    env.AddPath("mod/cache")
    args = fake.lastCall().args
    expect(args[4] == "" and args[5] == "" and args[6] == 0, "missing mode and extensions inherit")
    env.AddPath("mod/logs", { mode = "wo", extensions = {}, delete = true, recursive = false })
    args = fake.lastCall().args
    expect(args[5] == "" and args[6] == 7, "an empty list is sent as set")
    local derived = env.AddPath("mod/x", { recursive = true, delete = false })
    expect(fake.lastCall().args[6] == 0)
    derived.AddPath("mod/y")
    expect(fake.lastCall().args[2] == fake.nextPolicy - 1, "a derived environment chains from its own policy")
end)

test("AddPath raises at the caller's line on bad input", function(fake, bridge)
    local env = bridge()
    local before = #fake.calls
    local cases = {
        { 42 },
        { "mod/a\0" },
        { "mod", "rw" },
        { "mod", { mode = "rwx" } },
        { "mod", { mode = true } },
        { "mod", { colour = "red" } },
        { "mod", { [1] = "rw" } },
        { "mod", { delete = "yes" } },
        { "mod", { recursive = 0 } },
        { "mod", { extensions = "sav" } },
        { "mod", { extensions = { "sav", 3 } } },
        { "mod", { extensions = { [1] = "sav", [3] = "png" } } },
        { "mod", { extensions = { "" } } },
        { "mod", { extensions = { ".sav" } } },
        { "mod", { extensions = { "a/b" } } },
        { "mod", { extensions = { "a\\b" } } },
        { "mod", { extensions = { "a;b" } } },
        { "mod", { extensions = { "a\tb" } } },
        { "mod", { extensions = { "a*" } } },
        { "mod", { extensions = { "a?" } } },
        { "mod", { delete = true, mode = "ro" } },
        { "mod", { delete = true, mode = "stat" } },
        { "mod", { delete = true, mode = "ao" } },
    }
    for index, case in ipairs(cases) do
        -- Not tail calls, so the mod's frame is still on the stack.
        local message = expectRaise("invalid: ", function()
            local derived = env.AddPath(case[1], case[2])
            return derived
        end)
        expect(message:find(THIS_FILE .. ":", 1, true), "case " .. index .. " not reported at the caller: " .. message)
        expectRaise("invalid: ", function() local derived = env:AddPath(case[1], case[2]); return derived end)
    end
    expect(#fake.calls == before)
    local message = expectRaise("outside_root: ", function() local derived = env.AddPath("mod/../x"); return derived end)
    expect(message:find(THIS_FILE, 1, true))
    expectRaise("invalid: ", function() return env.AddPath("nowhere") end)
end)

test("environments are immutable values", function(fake, bridge)
    local base = bridge()
    local derived = base.AddPath("mod/cache", { delete = true })
    expect(derived ~= base)
    expect(base.Exists("mod/x") == false and fake.lastCall().args[2] == Fake.SAFE_POLICY)
    expect(derived.Exists("mod/x") == false and fake.lastCall().args[2] == 2)
    expectRaise("read-only", function() base.ReadText = function() return "fake" end end)
    expectRaise("read-only", function() derived.policy = 1 end)
    expect(getmetatable(derived) == false)
    rawset(derived, "Exists", function() return "patched" end)
    expect(base.Exists("mod/x") == false, "rawset on one environment doesn't reach another")
    expect(derived.Nothing == nil)
end)

-- Garbage collection ----------------------------------------------------------------

test("collected environments queue a release that runs at the next call", function(fake, bridge)
    local env = bridge()
    garbage(env, "mod/cache")
    collectgarbage(); collectgarbage()
    expect(countCalls(fake, "ReleasePolicy") == 0, "no native runs inside __gc")
    env.Exists("mod/x")
    local releases = fake.callsTo("ReleasePolicy")
    expect(#releases == 1 and releases[1].args[2] == 2)
    local order = {}
    for _, call in ipairs(fake.calls) do order[#order + 1] = call.name end
    local joined = table.concat(order, ",")
    expect(joined:find("ReleasePolicy,Exists", 1, true), "the queue drains before the call: " .. joined)
    collectgarbage(); collectgarbage()
    env.Exists("mod/x")
    expect(countCalls(fake, "ReleasePolicy") == 1, "released once")
end)

test("a function taken from an environment keeps its policy alive", function(fake, bridge)
    local env = bridge()
    local read = (function() return env.AddPath("mod/cache").ReadText end)()
    collectgarbage(); collectgarbage()
    fake.files["C:/Root/mod/cache/a"] = "kept"
    expect(read("mod/cache/a") == "kept")
    expect(fake.lastCall().args[2] == 2 and countCalls(fake, "ReleasePolicy") == 0)
    read = nil
    collectgarbage(); collectgarbage()
    env.Exists("mod/x")
    expect(countCalls(fake, "ReleasePolicy") == 1)
end)

test("the safe environment is never released", function(fake, bridge)
    local _ = bridge().Exists("mod/x")
    collectgarbage(); collectgarbage()
    bridge().Exists("mod/x")
    expect(countCalls(fake, "ReleasePolicy") == 0)
    expect(countCalls(fake, "SafePolicy") == 1, "the safe id is fetched once")
end)

test("the policy limit collects, releases and retries once", function(fake, bridge)
    local env = bridge()
    garbage(env, "mod/a", "mod/b")
    fake.policyLimit = 2
    local kept = env.AddPath("mod/c")
    expect(kept ~= nil and countCalls(fake, "ReleasePolicy") == 2)
    fake.policyLimit = 1
    local message = expectRaise("io: ", function() local derived = env.AddPath("mod/d"); return derived end)
    expect(message:find(THIS_FILE, 1, true))
    expect(kept.Exists("mod/x") == false)
end)

test("collected streams are closed at the next call", function(fake, bridge)
    local env = bridge()
    ;(function()
        local stream = assert(env.Open("mod/log.txt"))
        assert(stream:Write("a"))
    end)()
    collectgarbage(); collectgarbage()
    expect(countCalls(fake, "StreamClose") == 0)
    env.Exists("mod/x")
    local closes = fake.callsTo("StreamClose")
    expect(#closes == 1 and closes[1].args[2] == 1 and fake.streams[1].open == false)
end)

-- Coroutines ---------------------------------------------------------------------------

test("calls from a mod's own coroutine return invalid", function(fake, bridge)
    local env = bridge()
    local results = table.pack(coroutine.wrap(function() return env.ReadText("mod/a") end)())
    expect(results[1] == nil and results[2] == "invalid" and results[3]:find("not available here", 1, true))
    expect(results.n == 3, "the internal refusal marker isn't returned to mods")
    local ok, message = coroutine.wrap(function()
        return pcall(env.AddPath, "mod/cache")
    end)()
    expect(not ok and tostring(message):find("invalid: ", 1, true))
    local stream = assert(env.Open("mod/s"))
    results = table.pack(coroutine.wrap(function() return stream:Write("x") end)())
    expect(results[1] == nil and results[2] == "invalid")
end)

test("a refused drain keeps the queue for the next call outside the coroutine", function(fake, bridge)
    local env = bridge()
    garbage(env, "mod/cache")
    collectgarbage(); collectgarbage()
    coroutine.wrap(function() return env.Exists("mod/x") end)()
    expect(countCalls(fake, "ReleasePolicy") == 0 and fake.policies[2] ~= nil, "UE4SS refused it")
    env.Exists("mod/x")
    expect(countCalls(fake, "ReleasePolicy") == 1 and fake.policies[2] == nil, "released outside the coroutine")
end)

test("closing a stream inside a coroutine queues the close", function(fake, bridge)
    local env = bridge()
    local stream = assert(env.Open("mod/s"))
    local results = table.pack(coroutine.wrap(function() return stream:Close() end)())
    expect(results[1] == nil and results[2] == "invalid")
    expect(stream:Close() == true, "already closed in Lua")
    expect(fake.streams[1].open == true)
    env.Exists("mod/x")
    expect(fake.streams[1].open == false)
end)

-- Content escape ---------------------------------------------------------------------

test("content with NUL and \\1 bytes round-trips through the escape", function(fake, bridge)
    local env = bridge()
    local binary = "\0\1\2abc\1\0\0\1\1end\0"
    assert(env.WriteText("mod/b.bin", binary))
    expect(fake.lastWrite.length == #binary)
    expect(not fake.lastWrite.escaped:find("\0", 1, true), "no NUL crosses the boundary")
    expect(env.ReadText("mod/b.bin") == binary)
    assert(env.Append("mod/b.bin", "\0tail"))
    expect(env.ReadBytes("mod/b.bin") == binary .. "\0tail")
    local all = {}
    for byte = 0, 255 do all[#all + 1] = string.char(byte) end
    all = table.concat(all)
    assert(env.WriteText("mod/all.bin", all))
    expect(env.ReadText("mod/all.bin") == all)
    assert(env.WriteText("mod/t.txt", "plain text\n"))
    expect(fake.lastWrite.escaped == "plain text\n", "text without NUL or \\1 is sent unchanged")
    assert(env.WriteText("mod/empty", ""))
    expect(env.ReadText("mod/empty") == "")
end)

-- Streams ------------------------------------------------------------------------------

test("stream writes, flushes, closes and is read-only", function(fake, bridge)
    local env = bridge()
    local stream = assert(env.Open("mod/log.txt"))
    expect(stream.path == "C:/Root/mod/log.txt")
    expect(stream:Write("a", 1, "\0", 2.5) == true)
    expect(fake.files["C:/Root/mod/log.txt"] == "a1\0" .. "2.5")
    expect(fake.streams[1].writes == 1, "the arguments are one write")
    local before = #fake.calls
    expect(stream:Write() == true and #fake.calls == before, "no arguments writes nothing")
    expectFailure("invalid", stream:Write("a", {}, "b"))
    expect(fake.streams[1].writes == 1, "nothing is written on a bad argument")
    expect(stream:Flush() == true)
    expectFailure("invalid", stream.Write("x"))
    expectFailure("invalid", stream.Close())
    expectRaise("read-only", function() stream.path = "elsewhere" end)
    expect(stream:Close() == true and stream:Close() == true)
    expect(countCalls(fake, "StreamClose") == 1)
    local message = expectFailure("invalid", stream:Write("late"))
    expect(message == "stream is closed")
    expectFailure("invalid", stream:Flush())
end)

test("Open failures pass through", function(fake, bridge)
    fake.failures.StreamOpen = { "io", "Open: 64 streams already open" }
    expectFailure("io", bridge().Open("mod/s"))
end)

-- Tail and the dispatcher ------------------------------------------------------------

test("data deliveries reach the callback with info", function(fake, bridge, dispatch)
    local got = {}
    local sub = assert(bridge().Tail("mod/outbox.txt", function(text, info) got[#got + 1] = { text, info } end))
    expect(sub.path == "C:/Root/mod/outbox.txt" and sub.closed == false)
    local token = fake.subscriptions[1].token
    expect(math.type(token) == "integer" and token > 0)
    dispatch(1, token, "hello", 0, 0)
    dispatch(1, token, "x\0y", 6, 1)
    dispatch(1, token, "piece", 10, 2)
    dispatch(1, token, "both", 20, 3)
    dispatch(1, 999, "unknown token", 0, 0)
    expect(#got == 4)
    expect(got[1][1] == "hello" and got[1][2].offset == 0 and got[1][2].reset == false and got[1][2].partial == false)
    expect(got[2][1] == "x\0y" and got[2][2].reset == true and got[2][2].partial == false)
    expect(got[3][2].partial == true and got[3][2].reset == false)
    expect(got[4][2].partial == true and got[4][2].reset == true)
end)

test("a callback error closes the subscription and propagates to the bridge", function(fake, bridge, dispatch)
    local calls = 0
    local sub = assert(bridge().Tail("mod/in", function() calls = calls + 1; error("boom") end))
    local token = fake.subscriptions[1].token
    local ok, message = pcall(dispatch, 1, token, "line", 0, 0)
    expect(not ok and tostring(message):find("boom", 1, true))
    expect(sub.closed == true and calls == 1)
    expect(pcall(dispatch, 1, token, "line", 4, 0), "later deliveries are ignored")
    expect(calls == 1)
    expect(sub:Close() == true and countCalls(fake, "TailClose") == 0)
end)

test("a native-side close sets sub.closed without a callback", function(fake, bridge, dispatch)
    local calls = 0
    local sub = assert(bridge().Tail("mod/in", function() calls = calls + 1 end))
    local token = fake.subscriptions[1].token
    dispatch(2, token, "not_found\tTail C:/Root/mod/in: folder removed", 0, 0)
    expect(sub.closed == true and calls == 0)
    dispatch(1, token, "late", 0, 0)
    expect(calls == 0)
    dispatch(2, token, "", 0, 0)
    expect(sub:Close() == true and countCalls(fake, "TailClose") == 0)
end)

test("sub:Close stops delivery and is idempotent", function(fake, bridge, dispatch)
    local calls = 0
    local sub = assert(bridge().Tail("mod/in", function() calls = calls + 1 end))
    local token = fake.subscriptions[1].token
    expect(sub:Close() == true and sub.closed == true)
    expect(sub:Close() == true and countCalls(fake, "TailClose") == 1)
    expect(fake.subscriptions[1].open == false)
    dispatch(1, token, "late", 0, 0)
    expect(calls == 0)
    expectFailure("invalid", sub.Close())
    expectRaise("read-only", function() sub.closed = false end)
end)

test("stop closes every subscription and stream and clears the queue", function(fake, bridge, dispatch)
    local env = bridge()
    local calls = 0
    local subA = assert(env.Tail("mod/a", function() calls = calls + 1 end))
    local subB = assert(env.Tail("mod/b", function() calls = calls + 1 end))
    local stream = assert(env.Open("mod/s"))
    garbage(env, "mod/cache")
    collectgarbage(); collectgarbage()
    dispatch(3, 0, "", 0, 0)
    expect(subA.closed and subB.closed)
    dispatch(1, fake.subscriptions[1].token, "late", 0, 0)
    expect(calls == 0)
    expectFailure("invalid", stream:Write("x"))
    expect(stream:Close() == true)
    env.Exists("mod/x")
    expect(countCalls(fake, "ReleasePolicy") == 0, "the native side frees policies at stop")
end)

test("a failed TailOpen leaves no callback behind", function(fake, bridge, dispatch)
    fake.failures.TailOpen = { "not_found", "Tail C:/Root/mod/none/x: folder missing" }
    local called = false
    expectFailure("not_found", bridge().Tail("mod/none/x", function() called = true end))
    dispatch(1, 1, "x", 0, 0)
    expect(not called)
end)

test("operations after the session stops return invalid", function(fake, bridge)
    local env = bridge()
    fake.failures.ReadText = { "invalid", "Lua session is not registered or is stopping" }
    expectFailure("invalid", env.ReadText("mod/a"))
end)

suite.finish()
