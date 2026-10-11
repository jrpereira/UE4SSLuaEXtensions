-- lex_verify: the harness every extension's load-time verifications share.
-- The build ships it as Scripts/lex_verify.lua beside each extension's
-- Scripts/main.lua, which loads it with require("lex_verify").
--
--   local Verify = require("lex_verify")
--   if not Verify.enabled() then return end
--   local V = Verify.new("[UE4SSLuaFileBridge verify] ")
--   V.run(function() ... V.check("name", condition) ... end)
--
-- Verify.enabled() reads [Verifications] LoadTime from the mod's settings.ini
-- (written by the MCS toggle). A missing file or an unreadable value means
-- off: configuration never blocks an extension.

local Verify = {}

-- The mod folder: the parent of the Scripts folder holding this file.
function Verify.modFolder()
    local source = debug.getinfo(1, "S").source
    local scripts = source:gsub("^@", ""):match("^(.*)[/\\]")
    return scripts and scripts:match("^(.*)[/\\][^/\\]+$")
end

function Verify.enabled()
    local folder = Verify.modFolder()
    local file = folder and io.open(folder .. "/settings.ini", "r")
    if not file then return false end
    local section, value
    for line in file:lines() do
        line = line:match("^%s*(.-)%s*$")
        local name = line:match("^%[(.-)%]$")
        if name then
            section = name:lower()
        elseif section == "verifications" then
            local key, raw = line:match("^([^=]-)%s*=%s*(.*)$")
            if key and key:lower() == "loadtime" then value = raw end
        end
    end
    file:close()
    return tonumber(value) == 1
end

-- A harness writing "<tag>PASS|FAIL|SKIP|NOTE ..." lines to UE4SS.log.
function Verify.new(tag)
    local V = { counts = { passed = 0, failed = 0, skipped = 0 } }
    local counts = V.counts

    function V.log(text)
        print(tag .. text .. "\n")
    end

    function V.note(text)
        V.log("NOTE " .. text)
    end

    function V.pass(name)
        counts.passed = counts.passed + 1
        V.log("PASS " .. name)
    end

    function V.fail(name, detail)
        counts.failed = counts.failed + 1
        V.log("FAIL " .. name .. (detail ~= nil and (": " .. tostring(detail)) or ""))
    end

    function V.skip(name, why)
        counts.skipped = counts.skipped + 1
        V.log("SKIP " .. name .. ": " .. why)
    end

    function V.check(name, condition, detail)
        if condition then V.pass(name) else V.fail(name, detail) end
        return condition
    end

    -- Runs one group; an error inside it fails that group only.
    function V.group(name, fn)
        local ok, err = xpcall(fn, debug.traceback)
        if not ok then V.fail(name .. " (aborted)", err) end
    end

    -- An operation that must succeed: returns its value, or nil after a FAIL.
    function V.ok(name, value, code, message)
        if value == nil then
            V.fail(name, tostring(code) .. " " .. tostring(message))
            return nil
        end
        V.pass(name)
        return value
    end

    -- An operation that must fail with nil, `expected`, message.
    function V.refused(name, expected, value, code, message)
        if value == nil and code == expected and type(message) == "string" then
            V.pass(name .. " -> " .. expected)
        else
            V.fail(name, "expected nil, " .. expected .. "; got " .. tostring(value) .. ", "
                .. tostring(code) .. ", " .. tostring(message))
        end
    end

    -- A call that must fail with nil or false and an error message.
    function V.rejects(name, value, message)
        if (value == nil or value == false) and type(message) == "string" then
            V.pass(name .. " rejected")
        else
            V.fail(name, "expected nil/false and a message; got " .. tostring(value) .. ", " .. tostring(message))
        end
    end

    -- A call that must raise an error; with `prefix`, one whose message contains it.
    function V.raises(name, prefix, fn, ...)
        local done, err = pcall(fn, ...)
        if done then
            V.fail(name, "expected an error" .. (prefix and (" containing " .. prefix) or ""))
        elseif prefix and not tostring(err):find(prefix, 1, true) then
            V.fail(name, "expected " .. prefix .. ", got " .. tostring(err))
        else
            V.pass(name .. " raises" .. (prefix and (" " .. prefix) or ""))
        end
        return err
    end

    function V.summary(stage)
        V.log(string.format("SUMMARY %spassed=%d failed=%d skipped=%d",
            stage and (stage .. " ") or "", counts.passed, counts.failed, counts.skipped))
    end

    -- Runs the verifications in one protected call: a failure no group caught
    -- is reported, never raised out of main.lua.
    function V.run(main, stage)
        local ok, err = xpcall(main, debug.traceback)
        if not ok then
            V.fail("verifications (aborted)", err)
            V.summary(stage)
        end
    end

    return V
end

return Verify
