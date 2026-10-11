-- Offline checks of an extension's load-time verifications (lua/main_*.lua,
-- shipped as Scripts/main.lua with the shared Scripts/lex_verify.lua): the
-- MCS setting gates them, they are off unless settings.ini says LoadTime=1, and
-- a failure inside them never escapes main.lua. The in-game checks themselves
-- need the game. Run from the repository root:
--
--   lua5.4 shared/tests/verify_gate.lua bridge-files

local extension = assert(arg[1], "usage: verify_gate.lua <extension-folder>")

local function read(path)
    local file = assert(io.open(path, "rb"))
    local text = file:read("a")
    file:close()
    return text
end

local function scriptOf(folder)
    local listing = assert(io.popen("ls '" .. folder .. "/lua'"))
    local found
    for name in listing:lines() do
        if name:match("^main_.+%.lua$") then
            assert(found == nil, folder .. "/lua has more than one main_*.lua")
            found = folder .. "/lua/" .. name
        end
    end
    listing:close()
    return assert(found, folder .. "/lua has no main_*.lua")
end

local script = {
    source = scriptOf(extension),
    global = assert(read(extension .. "/mod.json"):match('"id"%s*:%s*"([^"]+)"'), "mod.json has no id"),
}
local HARNESS = "shared/lua/lex_verify.lua"

local base = assert(os.getenv("TMPDIR") or "/tmp") .. "/lex-verify-" .. tostring(os.time()) .. "-"
    .. tostring(math.random(1, 1 << 30))

local function shell(command)
    assert(os.execute(command), command)
end

local function write(path, text)
    local file = assert(io.open(path, "wb"))
    file:write(text)
    file:close()
end

-- Installs the script as <mod>/Scripts/main.lua with the given settings.ini
-- (nil for none) and runs it with a bridge global that records any access and
-- then raises. Returns whether the bridge was touched, whether main.lua raised,
-- and the printed text.
local function run(script, settings)
    local mod = base .. "/" .. script.global
    shell("rm -rf '" .. mod .. "' && mkdir -p '" .. mod .. "/Scripts'")
    write(mod .. "/Scripts/main.lua", read(script.source))
    write(mod .. "/Scripts/lex_verify.lua", read(HARNESS))
    if settings then write(mod .. "/settings.ini", settings) end

    local touched, printed = false, {}
    local stub = setmetatable({}, {
        __index = function() touched = true; error("stub bridge") end,
        __call = function() touched = true; error("stub bridge") end,
    })
    local saved = { print = print, global = _G[script.global], LoopAsync = LoopAsync, path = package.path }
    -- UE4SS puts the mod's Scripts folder on the module path.
    package.path = mod .. "/Scripts/?.lua;" .. package.path
    package.loaded.lex_verify = nil
    _G[script.global] = stub
    print = function(text) printed[#printed + 1] = text end
    LoopAsync = function() end
    local chunk = assert(loadfile(mod .. "/Scripts/main.lua"))
    local ok = pcall(chunk)
    print, _G[script.global], LoopAsync = saved.print, saved.global, saved.LoopAsync
    package.path = saved.path
    return touched, ok, table.concat(printed)
end

local passed, total = 0, 0
local function test(name, fn)
    total = total + 1
    local ok, err = xpcall(fn, debug.traceback)
    if ok then passed = passed + 1 else print("FAIL " .. name .. "\n" .. tostring(err)) end
end

do
    local label = script.source .. ": "
    local function off(settings)
        local touched, ok, printed = run(script, settings)
        assert(ok, "main.lua raised")
        assert(not touched, "the bridge was used while verifications are off")
        assert(printed == "", "printed while off: " .. printed)
    end

    test(label .. "off without settings.ini", function() off(nil) end)
    test(label .. "off when LoadTime=0", function() off("[Verifications]\nLoadTime=0\n") end)
    test(label .. "off for an unreadable value", function() off("[Verifications]\nLoadTime=yes\n") end)
    test(label .. "off when the key is in another section", function() off("[Other]\nLoadTime=1\n") end)
    test(label .. "off for an empty file", function() off("") end)

    for _, settings in ipairs({
        "[Verifications]\nLoadTime=1\n",
        "[verifications]\r\n  loadtime = 1 \r\n",
        "[General]\nScale=2\n\n[Verifications]\nLoadTime=1.0",
    }) do
        test(label .. "on for " .. string.format("%q", settings), function()
            local touched, ok, printed = run(script, settings)
            assert(touched, "the bridge was not used while verifications are on")
            assert(ok, "a failure inside the verifications escaped main.lua")
            assert(printed:find("FAIL", 1, true), "the failure was not reported: " .. printed)
        end)
    end
end

shell("rm -rf '" .. base .. "'")
print(string.format("%s: load-time verification gate tests: %d of %d passed", extension, passed, total))
if passed ~= total then os.exit(1) end
