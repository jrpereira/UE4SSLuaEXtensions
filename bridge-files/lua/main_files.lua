-- UE4SSLuaFileBridge load-time verifications. Ships as Scripts/main.lua in the
-- bridge's own mod folder and runs only when the MCS setting "Enable load-time
-- verifications" is on (settings.ini: [Verifications] LoadTime=1); it is off by
-- default. Results go to UE4SS.log as PASS, FAIL and SKIP lines, followed by a
-- SUMMARY line once the asynchronous Tail checks finish. The harness (settings
-- gate, logging, groups) is the shared Scripts/lex_verify.lua.
--
-- Everything it writes stays in temp/lexverify, moddata/lexverify and
-- mod/lexverify and is removed again. Saves are only read, except for one
-- refused write that must not create a file.

local Verify = require("lex_verify")
if not Verify.enabled() then return end

local V = Verify.new("[UE4SSLuaFileBridge verify] ")
local log, pass, fail, skip, check, group = V.log, V.pass, V.fail, V.skip, V.check, V.group
local ok, refused, raises, summary = V.ok, V.refused, V.raises, V.summary
local modFolder = Verify.modFolder

-- Checks ---------------------------------------------------------------------------

local function main()
    local Bridge = UE4SSLuaFileBridge
    if type(Bridge) ~= "table" then
        fail("UE4SSLuaFileBridge global", "missing; the native bridge did not start for this Lua state")
        summary()
        return
    end
    log("load-time verifications enabled; bridge " .. tostring(Bridge.GetVersion and Bridge.GetVersion()))

    local safe = Bridge()
    local ROOT = "temp/lexverify"

    group("entry point", function()
        check("UE4SSLuaFileBridge() returns an environment", type(safe) == "table")
        local version = Bridge.GetVersion()
        check("GetVersion is x.y.z", type(version) == "string" and version:match("^%d+%.%d+%.%d+$") ~= nil, version)
        check("API_VERSION is 1", Bridge.API_VERSION == 1, Bridge.API_VERSION)
        check("env.GetVersion matches", safe.GetVersion() == version)

        local caps = Bridge.GetCapabilities()
        check("GetCapabilities returns a table", type(caps) == "table")
        check("capabilities api is 1", caps.api == 1, caps.api)
        for _, key in ipairs({ "environments", "append_only", "streams", "tail" }) do
            check("capability " .. key, caps[key] == true, caps[key])
        end
        for _, key in ipairs({ "lock", "hash", "temp_file" }) do
            check("deferred capability " .. key .. " is false", caps[key] == false, caps[key])
        end
        check("max_read_bytes is 64 MiB", caps.max_read_bytes == 67108864, caps.max_read_bytes)
        check("target_ue4ss_commit", caps.target_ue4ss_commit == "97b7e501", caps.target_ue4ss_commit)
        local named = {}
        for _, name in ipairs(caps.locations or {}) do named[name] = true end
        for _, name in ipairs({ "game", "user", "mod", "moddata", "temp", "savegames" }) do
            check("capabilities list location " .. name, named[name])
        end
        check("GetCapabilities returns a new table", Bridge.GetCapabilities() ~= caps)

        local stats = Bridge.GetDispatchStats()
        check("GetDispatchStats fields", type(stats) == "table" and math.type(stats.subscriptions) == "integer"
            and math.type(stats.delivered) == "integer" and math.type(stats.budget_exhausted_passes) == "integer")
        check("env:Exists colon form", safe:Exists("temp") == safe.Exists("temp"))
        raises("assigning an environment field", nil, function() safe.Exists = nil end)
    end)

    group("locations", function()
        local locations = safe.Locations()
        for _, name in ipairs({ "game", "user", "mod", "moddata", "temp", "savegames" }) do
            local entry = locations[name]
            check("location " .. name .. " has a path", entry and type(entry.path) == "string", name)
        end
        check("game and user are roots", locations.game.root == true and locations.user.root == true)
        check("mod is not a root", locations.mod.root == false)
        check("game root exists", locations.game.exists == true)
        check("mod folder exists", locations.mod.exists == true)
        local folder = modFolder() and modFolder():match("([^/\\]+)$")
        check("mod is this mod's folder", folder and locations.mod.path:sub(-#folder) == folder,
            tostring(locations.mod.path))
        check("temp lies inside moddata", locations.temp.path == locations.moddata.path .. "/temp",
            tostring(locations.temp.path))
    end)

    group("paths", function()
        local path = safe.Path("mod", "a/b", "c.txt")
        check("Path joins and normalizes", type(path) == "string" and path:match("^%u:/") ~= nil
            and path:sub(-10) == "/a/b/c.txt" and not path:find("\\", 1, true), path)
        refused("Path with an unknown location", "invalid", safe.Path("nowhere", "x"))
        refused("Path climbing out of its location", "outside_root", safe.Path("mod", "..", "..", "x"))
        check("Join", safe.Join("mod", "cache//", "x.json") == "mod/cache/x.json", safe.Join("mod", "cache//", "x.json"))
        check("Normalize drops . and doubled slashes",
            safe.Normalize("mod/./a//b/") == safe.Path("mod", "a", "b"), safe.Normalize("mod/./a//b/"))
        check("Parent", safe.Parent("mod/a/b.txt") == "mod/a")
        refused("Parent of a single segment", "invalid", safe.Parent("mod"))
        check("Name", safe.Name("mod/a/b.txt") == "b.txt")
        check("Stem", safe.Stem("mod/a.tar.gz") == "a.tar" and safe.Stem(".owner") == ".owner")
        check("Extension", safe.Extension("a.tar.gz") == "gz" and safe.Extension(".owner") == ""
            and safe.Extension("mod/noext") == "")
        refused("Join with a non-string", "invalid", safe.Join("mod", {}))

        refused("absolute path outside the roots", "outside_root", safe.ReadText("C:/Windows/win.ini"))
        refused("UNC path", "outside_root", safe.Exists("//server/share/x.txt"))
        refused("device path", "outside_root", safe.Exists("\\\\.\\pipe\\x"))
        refused("reserved device name", "outside_root", safe.Exists("mod/CON"))
        refused("relative path without a location", "invalid", safe.Exists("Scripts/main.lua"))
        refused("empty path", "invalid", safe.Exists(""))
        refused("drive-relative path", "invalid", safe.Exists("C:foo"))
        refused("segment with a wildcard", "invalid", safe.Exists("mod/a?b"))
        refused("segment ending in a dot", "invalid", safe.Exists("mod/a."))
        refused("alternate data stream", "invalid", safe.Exists("mod/a.txt:stream"))
        refused("climbing out of mod", "outside_root", safe.Exists("mod/../Other"))
        refused("NUL byte", "invalid", safe.Exists("mod/a\0b"))
    end)

    group("AddPath validation", function()
        raises("unknown option", "invalid:", safe.AddPath, "temp", { bogus = true })
        raises("unknown mode", "invalid:", safe.AddPath, "temp", { mode = "xx" })
        raises("delete with ro", "invalid:", safe.AddPath, "temp", { mode = "ro", delete = true })
        raises("delete with ao", "invalid:", safe.AddPath, "temp", { mode = "ao", delete = true })
        raises("extension with a dot", "invalid:", safe.AddPath, "temp", { extensions = { ".txt" } })
        raises("extensions not a list", "invalid:", safe.AddPath, "temp", { extensions = "txt" })
        raises("path outside the roots", "outside_root:", safe.AddPath, "C:/Windows")
        raises("unknown location", "invalid:", safe.AddPath, "nowhere/x")
        local derived = safe.AddPath("temp", { mode = "ro" })
        check("AddPath returns a new environment", derived ~= safe and type(derived) == "table")
    end)

    group("operations", function()
        local env = safe
        local dir = ROOT .. "/ops"
        if env.Exists(ROOT) then env.RemoveTree(ROOT) end
        ok("MakeDir with parents", env.MakeDir(dir .. "/a/b"))
        ok("MakeDir on an existing folder", env.MakeDir(dir .. "/a/b"))
        check("Exists folder", env.Exists(dir .. "/a/b") == true)
        check("Exists missing", env.Exists(dir .. "/missing") == false)

        local file = dir .. "/hello.txt"
        local content = "line one\nlínea dos\n"
        ok("WriteText atomic", env.WriteText(file, content))
        check("ReadText round trip", env.ReadText(file) == content)
        ok("Append", env.Append(file, "three\n"))
        check("Append adds at the end", env.ReadText(file) == content .. "three\n")
        ok("WriteText non-atomic", env.WriteText(file, "short", { atomic = false }))
        check("non-atomic WriteText truncates", env.ReadText(file) == "short")
        ok("Append creates a file", env.Append(dir .. "/new.log", "x"))

        env.WriteText(dir .. "/bom.txt", "\239\187\191abc")
        check("ReadText strips a UTF-8 BOM", env.ReadText(dir .. "/bom.txt") == "abc")
        check("ReadBytes keeps the BOM", env.ReadBytes(dir .. "/bom.txt") == "\239\187\191abc")
        check("ReadBytes offset and length", env.ReadBytes(dir .. "/bom.txt", { offset = 3, length = 2 }) == "ab")
        check("ReadBytes past the end", env.ReadBytes(dir .. "/bom.txt", { offset = 100 }) == "")
        refused("ReadBytes negative offset", "invalid", env.ReadBytes(dir .. "/bom.txt", { offset = -1 }))
        refused("WriteText unknown option", "invalid", env.WriteText(file, "x", { bogus = 1 }))
        refused("WriteText non-string text", "invalid", env.WriteText(file, {}))

        local binary = "\0\1\2\255\r\n\26"
        env.WriteText(dir .. "/binary.bin", binary)
        check("binary content round trip", env.ReadBytes(dir .. "/binary.bin") == binary)

        local unicode = dir .. "/café-ñ-日本.txt"
        ok("WriteText non-ASCII name", env.WriteText(unicode, "ü"))
        check("ReadText non-ASCII name", env.ReadText(unicode) == "ü")

        local info = env.Stat(file)
        check("Stat file", info and info.type == "file" and info.size == 5 and type(info.modified) == "number"
            and type(info.created) == "number" and info.readonly == false and info.link == false)
        local folderInfo = env.Stat(dir .. "/a")
        check("Stat folder", folderInfo and folderInfo.type == "directory" and folderInfo.size == 0)
        refused("Stat missing", "not_found", env.Stat(dir .. "/missing"))

        local entries = env.List(dir)
        local names, sorted = {}, true
        for index, entry in ipairs(entries or {}) do
            names[entry.name] = entry
            if index > 1 and entries[index - 1].name > entry.name then sorted = false end
        end
        check("List entries", names["hello.txt"] and names["a"] and names["café-ñ-日本.txt"] ~= nil)
        check("List is sorted by name", sorted)
        check("List entry fields", names["hello.txt"] and names["hello.txt"].type == "file"
            and names["hello.txt"].size == 5 and type(names["hello.txt"].created) == "number"
            and names["a"].type == "directory")
        local leftovers = false
        for name in pairs(names) do
            if name:find(".xbtmp-", 1, true) then leftovers = true end
        end
        check("no temporary siblings left by atomic writes", not leftovers)
        refused("List a file", "invalid", env.List(file))
        refused("List missing", "not_found", env.List(dir .. "/missing"))

        refused("MakeDir over a file", "exists", env.MakeDir(file))
        refused("ReadText a folder", "invalid", env.ReadText(dir .. "/a"))
        refused("ReadText missing", "not_found", env.ReadText(dir .. "/missing.txt"))
        refused("WriteText into a missing folder", "not_found", env.WriteText(dir .. "/nope/x.txt", "x"))
        refused("WriteText onto a folder", "invalid", env.WriteText(dir .. "/a", "x"))

        local copy = dir .. "/copy.txt"
        ok("Copy", env.Copy(file, copy))
        check("Copy content", env.ReadText(copy) == "short")
        refused("Copy onto an existing file", "exists", env.Copy(file, copy))
        env.WriteText(file, "newer")
        ok("Copy with overwrite", env.Copy(file, copy, { overwrite = true }))
        check("Copy overwrite content", env.ReadText(copy) == "newer")
        refused("Copy a folder", "invalid", env.Copy(dir .. "/a", dir .. "/a2"))

        local moved = dir .. "/moved.txt"
        ok("Move", env.Move(copy, moved))
        check("Move removes the source", env.Exists(copy) == false and env.ReadText(moved) == "newer")
        env.WriteText(copy, "again")
        refused("Move onto an existing file", "exists", env.Move(copy, moved))
        ok("Move with overwrite", env.Move(copy, moved, { overwrite = true }))
        check("Move overwrite content", env.ReadText(moved) == "again")

        refused("Remove a non-empty folder", "invalid", env.Remove(dir .. "/a"))
        ok("Remove a file", env.Remove(moved))
        ok("Remove an empty folder", env.Remove(dir .. "/a/b"))
        refused("Remove missing", "not_found", env.Remove(dir .. "/a/b"))
        refused("RemoveTree a file", "invalid", env.RemoveTree(file))
        ok("RemoveTree", env.RemoveTree(dir))
        check("RemoveTree removed everything", env.Exists(dir) == false)
    end)

    group("access modes", function()
        local base = ROOT .. "/modes"
        safe.MakeDir(base .. "/ro")
        safe.MakeDir(base .. "/stat")
        safe.MakeDir(base .. "/ao")
        safe.MakeDir(base .. "/wo")
        safe.WriteText(base .. "/ro/x.txt", "ro")
        safe.WriteText(base .. "/stat/x.txt", "stat")
        safe.WriteText(base .. "/wo/x.txt", "wo")

        local env = safe
            .AddPath(base .. "/ro", { mode = "ro" })
            .AddPath(base .. "/stat", { mode = "stat" })
            .AddPath(base .. "/ao", { mode = "ao" })
            .AddPath(base .. "/wo", { mode = "wo" })

        check("ro reads", env.ReadText(base .. "/ro/x.txt") == "ro")
        refused("ro write", "denied", env.WriteText(base .. "/ro/x.txt", "x"))
        refused("ro append", "denied", env.Append(base .. "/ro/x.txt", "x"))
        refused("ro MakeDir", "denied", env.MakeDir(base .. "/ro/sub"))

        check("stat Exists and Stat", env.Exists(base .. "/stat/x.txt") == true and env.Stat(base .. "/stat/x.txt") ~= nil)
        check("stat List", env.List(base .. "/stat") ~= nil)
        refused("stat read", "denied", env.ReadText(base .. "/stat/x.txt"))
        refused("stat ReadBytes", "denied", env.ReadBytes(base .. "/stat/x.txt"))

        ok("ao Append", env.Append(base .. "/ao/log.txt", "a\n"))
        ok("ao MakeDir", env.MakeDir(base .. "/ao/sub"))
        refused("ao WriteText", "denied", env.WriteText(base .. "/ao/log.txt", "x"))
        refused("ao read", "denied", env.ReadText(base .. "/ao/log.txt"))
        local stream = ok("ao Open append", env.Open(base .. "/ao/log.txt"))
        if stream then stream:Close() end
        refused("ao Open truncating", "denied", env.Open(base .. "/ao/log.txt", { append = false }))
        refused("ao Remove", "denied", env.Remove(base .. "/ao/log.txt"))

        ok("wo WriteText", env.WriteText(base .. "/wo/x.txt", "w"))
        refused("wo read", "denied", env.ReadText(base .. "/wo/x.txt"))
        refused("wo Remove without delete", "denied", env.Remove(base .. "/wo/x.txt"))
        refused("Copy from wo (needs read)", "denied", env.Copy(base .. "/wo/x.txt", base .. "/y.txt"))
        refused("Copy into ro", "denied", env.Copy(base .. "/ro/x.txt", base .. "/ro/y.txt"))
        refused("Move out of ro", "denied", env.Move(base .. "/ro/x.txt", base .. "/y.txt"))

        local regranted = safe.AddPath("temp", { mode = "ro" })
        refused("re-granting temp as ro replaces rw", "denied", regranted.WriteText(base .. "/z.txt", "x"))
        check("the original environment is unchanged", safe.WriteText(base .. "/z.txt", "x") == true)

        refused("safe set: no delete in mod", "denied", safe.Remove("mod/__lexverify_absent.txt"))
        refused("safe set: game root is read-only", "denied",
            safe.WriteText("game/lexverify.txt", "x"))
        refused("safe set: savegames is read-only", "denied",
            safe.WriteText("savegames/lexverify.sav", "x"))
        refused("safe set: moddata is read-only", "denied", safe.WriteText("moddata/x.txt", "x"))
        check("savegames is readable", type(safe.Exists("savegames")) == "boolean")
        safe.RemoveTree(base)
    end)

    group("mod folder grant", function()
        local env = safe.AddPath("mod/lexverify", { delete = true })
        ok("MakeDir in mod", env.MakeDir("mod/lexverify"))
        ok("WriteText in mod (safe set rw)", safe.WriteText("mod/lexverify/x.txt", "m"))
        refused("Remove in mod without delete", "denied", safe.Remove("mod/lexverify/x.txt"))
        ok("Remove with an inherited-mode delete grant", env.Remove("mod/lexverify/x.txt"))
        ok("Remove the folder", env.Remove("mod/lexverify"))
        -- Floor checks use Remove, which never deletes a non-empty folder, so a
        -- broken floor can't take anything with it.
        refused("delete floor: the mod location itself", "denied",
            safe.AddPath("mod", { delete = true }).Remove("mod"))
    end)

    group("moddata, .owner and inheritance", function()
        local env = safe.AddPath("moddata/lexverify", { mode = "rw", delete = true })
        ok("MakeDir moddata/lexverify", env.MakeDir("moddata/lexverify"))
        ok("WriteText in moddata", env.WriteText("moddata/lexverify/x.txt", "d"))
        local owner = env.ReadText("moddata/.owner")
        check(".owner exists and names the folder and version", owner and owner:find("folder=", 1, true)
            and owner:find("bridge_version=" .. Bridge.GetVersion(), 1, true)
            and owner:find("first_write=", 1, true) and owner:find("last_write=", 1, true), owner)
        local wide = safe.AddPath("moddata", { mode = "rw", delete = true })
        refused(".owner cannot be written", "denied", wide.WriteText("moddata/.owner", "x"))
        refused(".owner cannot be appended", "denied", wide.Append("moddata/.owner", "x"))
        refused(".owner cannot be removed", "denied", wide.Remove("moddata/.owner"))
        refused("delete floor: moddata itself", "denied", wide.Remove("moddata"))
        refused("delete floor: temp itself", "denied", safe.Remove("temp"))
        refused("delete floor: a folder containing locations", "denied",
            safe.AddPath("user/Saved", { mode = "rw" }).AddPath("user", { mode = "rw", delete = true })
                .Remove("user/Saved"))

        local ext = safe.AddPath("moddata/lexverify/ext", { mode = "rw", extensions = { "log" } })
        env.MakeDir("moddata/lexverify/ext")
        ok("extension grant allows .log", ext.WriteText("moddata/lexverify/ext/a.log", "x"))
        ok("extensions compare ignoring case", ext.WriteText("moddata/lexverify/ext/B.LOG", "x"))
        refused("extension grant skips .txt (falls back to user ro)", "denied",
            ext.WriteText("moddata/lexverify/ext/a.txt", "x"))

        local flat = safe.AddPath("moddata/lexverify/flat", { mode = "rw", recursive = false })
        ok("non-recursive grant: direct child folder", flat.MakeDir("moddata/lexverify/flat/sub"))
        ok("non-recursive grant: direct child file", flat.WriteText("moddata/lexverify/flat/a.txt", "x"))
        refused("non-recursive grant: grandchild", "denied", flat.WriteText("moddata/lexverify/flat/sub/a.txt", "x"))

        local inherit = safe.AddPath("moddata/lexverify/inh", { mode = "rw", extensions = { "sav" } })
            .AddPath("moddata/lexverify/inh/sub", {})
        env.MakeDir("moddata/lexverify/inh/sub")
        ok("inherited mode and extensions allow .sav", inherit.WriteText("moddata/lexverify/inh/sub/a.sav", "x"))
        refused("inherited extensions refuse .txt", "denied", inherit.WriteText("moddata/lexverify/inh/sub/a.txt", "x"))
        refused("delete is never inherited", "denied", inherit.Remove("moddata/lexverify/inh/sub/a.sav"))

        ok("RemoveTree moddata/lexverify", env.RemoveTree("moddata/lexverify"))
    end)

    group("savegames rules", function()
        local name = "savegames/__lexverify_never_written.sav"
        if not safe.Locations().savegames.exists then
            skip("savegames rules", "no SaveGames folder yet")
            return
        end
        local env = safe.AddPath("savegames", { mode = "rw", extensions = { "sav" } })
        refused("non-atomic WriteText under savegames", "invalid", env.WriteText(name, "x", { atomic = false }))
        check("refused write created no file", safe.Exists(name) == false)
        refused("extension outside the grant", "denied", env.WriteText("savegames/__lexverify.txt", "x"))
        -- Only reached if a refusal above failed and left a file behind.
        local cleanup = safe.AddPath("savegames", { mode = "rw", delete = true })
        for _, leftover in ipairs({ name, "savegames/__lexverify.txt" }) do
            if safe.Exists(leftover) then cleanup.Remove(leftover) end
        end
    end)

    group("streams", function()
        local dir = ROOT .. "/streams"
        safe.MakeDir(dir)
        local path = dir .. "/s.log"
        local stream = ok("Open", safe.Open(path))
        if not stream then return end
        check("stream.path is normalized", stream.path == safe.Normalize(path), stream.path)
        check("Write strings and numbers", stream:Write("a", 1, "\n") == true)
        check("Write nothing", stream:Write() == true)
        refused("Write a table", "invalid", stream:Write({}))
        refused("dot call on a stream", "invalid", stream.Write("x"))
        check("readers see writes at once", safe.ReadText(path) == "a1\n")
        refused("a second writer is busy", "busy", safe.Open(path))
        check("Flush", stream:Flush() == true)
        check("Close", stream:Close() == true)
        check("Close again", stream:Close() == true)
        refused("Write after Close", "invalid", stream:Write("x"))
        refused("Flush after Close", "invalid", stream:Flush())
        raises("assigning stream.path", nil, function() stream.path = "x" end)

        local again = ok("Open appends by default", safe.Open(path))
        if again then again:Write("b\n"); again:Close() end
        check("append kept earlier content", safe.ReadText(path) == "a1\nb\n")
        local trunc = ok("Open append=false", safe.Open(path, { append = false }))
        if trunc then trunc:Write("c\n"); trunc:Close() end
        check("append=false truncated", safe.ReadText(path) == "c\n")
        local manual = ok("Open flush=manual", safe.Open(path, { flush = "manual" }))
        if manual then
            manual:Write("d\n")
            manual:Flush()
            check("manual flush hands data over", safe.ReadText(path) == "c\nd\n")
            manual:Close()
        end
        refused("Open create=false on a missing file", "not_found", safe.Open(dir .. "/none.log", { create = false }))
        refused("Open unknown option", "invalid", safe.Open(path, { bogus = true }))
        refused("Open bad flush value", "invalid", safe.Open(path, { flush = "never" }))
        refused("Open in a missing folder", "not_found", safe.Open(dir .. "/nope/x.log"))
    end)

    group("coroutines", function()
        local value, code = coroutine.wrap(function() return safe.Exists("temp") end)()
        check("an operation inside a mod coroutine returns invalid", value == nil and code == "invalid",
            tostring(value) .. " " .. tostring(code))
        local done, err = coroutine.wrap(function() return pcall(safe.AddPath, "temp", {}) end)()
        check("AddPath inside a mod coroutine raises invalid", done == false and tostring(err):find("invalid", 1, true),
            tostring(err))
    end)

    -- Tail (asynchronous) ---------------------------------------------------------------

    local TAIL_TIMEOUT_MS = 10000
    local tail = {
        lines = {}, ends = {}, chunks = {}, rotated = nil, errorCalls = 0, phase = 1, done = false,
    }

    group("Tail", function()
        local dir = ROOT .. "/tail"
        safe.MakeDir(dir)
        local path = dir .. "/socket.txt"
        local deliveredBefore = Bridge.GetDispatchStats().delivered

        refused("Tail with a missing folder", "not_found", safe.Tail(dir .. "/nope/x.txt", function() end))
        refused("Tail with a non-function", "invalid", safe.Tail(path, 5))
        refused("Tail on a folder", "invalid", safe.Tail(dir, function() end))
        refused("Tail unknown option", "invalid", safe.Tail(path, function() end, { bogus = true }))
        refused("Tail bad from value", "invalid", safe.Tail(path, function() end, { from = "middle" }))

        local writer = ok("Open the socket writer", safe.Open(path, { append = false }))
        if not writer then tail.done = true return end
        writer:Write("alpha\nbeta\r\ngam")

        tail.subLines = ok("Tail lines from start", safe.Tail(path, function(line, info)
            tail.lines[#tail.lines + 1] = { line = line, offset = info.offset, reset = info.reset, partial = info.partial }
            if tail.phase == 1 and line == "delta" then
                -- Phase 2: replace the file. A new identity must restart at offset 0.
                tail.phase = 2
                writer:Close()
                safe.WriteText(path, "x\n")
            elseif tail.phase == 2 and line == "x" then
                tail.rotated = info
                tail.done = true
            end
        end, { from = "start" }))
        tail.subChunks = ok("Tail chunks from start", safe.Tail(path, function(text)
            tail.chunks[#tail.chunks + 1] = text
        end, { chunks = true, from = "start" }))
        tail.subError = ok("Tail with a failing callback", safe.Tail(path, function()
            tail.errorCalls = tail.errorCalls + 1
            error("deliberate verification error")
        end, { from = "start" }))

        writer:Write("ma\n")
        tail.subEnd = ok("Tail from end", safe.Tail(path, function(line, info)
            tail.ends[#tail.ends + 1] = { line = line, offset = info.offset }
        end))
        writer:Write("delta\n")

        if tail.subLines then
            check("sub.path", tail.subLines.path == safe.Normalize(path))
            check("sub.closed starts false", tail.subLines.closed == false)
            refused("dot call on a subscription", "invalid", tail.subLines.Close())
        end
        tail.deliveredBefore = deliveredBefore
        if not (tail.subLines and tail.subChunks and tail.subEnd) then tail.done = true end
    end)

    local function finishTail(timedOut)
        group("Tail results", function()
            if timedOut then fail("Tail delivery", "timed out after " .. TAIL_TIMEOUT_MS .. " ms") end
            local expected = { { "alpha", 0 }, { "beta", 6 }, { "gamma", 12 }, { "delta", 18 } }
            local lines = tail.lines
            local okLines = #lines >= 4
            for index, want in ipairs(expected) do
                local got = lines[index]
                if not got or got.line ~= want[1] or got.offset ~= want[2] or got.partial ~= false then okLines = false end
            end
            check("lines arrive in order with offsets, CRLF stripped, partial held", okLines,
                #lines .. " lines; first " .. tostring(lines[1] and lines[1].line))
            check("rotation restarts at offset 0 with reset", tail.rotated and tail.rotated.offset == 0
                and tail.rotated.reset == true)
            check("from=end skips earlier lines", #tail.ends >= 1 and tail.ends[1].line == "delta"
                and tail.ends[1].offset == 18, tostring(tail.ends[1] and tail.ends[1].line))
            local joined = table.concat(tail.chunks)
            check("chunks deliver the raw bytes", joined:sub(1, 24) == "alpha\nbeta\r\ngamma\ndelta\n", #joined .. " bytes")
            check("a failing callback runs once and closes its subscription",
                tail.errorCalls == 1 and tail.subError and tail.subError.closed == true, tail.errorCalls)
            check("dispatch stats count deliveries",
                Bridge.GetDispatchStats().delivered > (tail.deliveredBefore or 0))
            for _, key in ipairs({ "subLines", "subChunks", "subEnd" }) do
                local sub = tail[key]
                if sub then
                    check(key .. ":Close", sub:Close() == true and sub:Close() == true and sub.closed == true)
                end
            end
        end)
        group("cleanup", function()
            ok("RemoveTree " .. ROOT, safe.RemoveTree(ROOT))
        end)
        summary()
    end

    if tail.done and not tail.subLines then
        finishTail(false)
    else
        local waited = 0
        LoopAsync(100, function()
            waited = waited + 100
            if tail.done or waited >= TAIL_TIMEOUT_MS then
                finishTail(not tail.done)
                return true
            end
            return false
        end)
    end

end

V.run(main)
