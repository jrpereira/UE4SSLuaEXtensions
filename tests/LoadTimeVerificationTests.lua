-- Offline checks for each product's load-time verification script
-- (bridge-*/lua/main_*.lua, shipped as Scripts/main.lua): the MCS setting gates
-- it, it is off unless settings.ini says LoadTime=1, and a failure inside it
-- never escapes main.lua. The in-game checks themselves need the game.
-- Run from the repository root with lua5.4.

local SCRIPTS = {
    { source = "bridge-events/lua/main_events.lua", global = "UE4SSLuaEventBridge" },
    { source = "bridge-files/lua/main_files.lua", global = "UE4SSLuaFileBridge" },
}

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

local function read(path)
    local file = assert(io.open(path, "rb"))
    local text = file:read("a")
    file:close()
    return text
end

-- Installs the script as <mod>/Scripts/main.lua with the given settings.ini
-- (nil for none) and runs it with a bridge global that records any access and
-- then raises. Returns whether the bridge was touched, whether main.lua raised,
-- and the printed text.
local function run(script, settings)
    local mod = base .. "/" .. script.global
    shell("rm -rf '" .. mod .. "' && mkdir -p '" .. mod .. "/Scripts'")
    write(mod .. "/Scripts/main.lua", read(script.source))
    if settings then write(mod .. "/settings.ini", settings) end

    local touched, printed = false, {}
    local stub = setmetatable({}, {
        __index = function() touched = true; error("stub bridge") end,
        __call = function() touched = true; error("stub bridge") end,
    })
    local saved = { print = print, global = _G[script.global], LoopAsync = LoopAsync }
    _G[script.global] = stub
    print = function(text) printed[#printed + 1] = text end
    LoopAsync = function() end
    local chunk = assert(loadfile(mod .. "/Scripts/main.lua"))
    local ok = pcall(chunk)
    print, _G[script.global], LoopAsync = saved.print, saved.global, saved.LoopAsync
    return touched, ok, table.concat(printed)
end

local passed, total = 0, 0
local function test(name, fn)
    total = total + 1
    local ok, err = xpcall(fn, debug.traceback)
    if ok then passed = passed + 1 else print("FAIL " .. name .. "\n" .. tostring(err)) end
end

for _, script in ipairs(SCRIPTS) do
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
print(string.format("Load-time verification gate tests: %d/%d passed", passed, total))
if passed ~= total then os.exit(1) end
