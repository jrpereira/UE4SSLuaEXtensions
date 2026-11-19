-- A fake of the 10 UE4SSLuaLogBridge natives, following
-- bridge-logs/include/LogBridgeNativeContract.hpp. It checks every call's
-- arity and argument types (contract R2) and records violations instead of
-- crashing, refuses calls from coroutines like UE4SS (R6), and records lines.
local Fake = {}

local SESSION = 7
Fake.SESSION = SESSION
Fake.MOD = "FakeMod"

local SHAPES = {
    GetVersion = "", GetCapabilities = "", GetStats = "", Register = "isiiii", Write = "iiiss", Level = "iis",
    Generation = "", SetLevel = "ss", GetLevel = "s", Flush = "",
}
Fake.SHAPES = SHAPES

local NAMES = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF" }

function Fake.new()
    local fake = {
        calls = {},
        counts = {},
        violations = {},
        lines = {},     -- {logger=, name=, rank=, category=, message=}
        loggers = {},   -- id -> {name, target, frequency, interval, rank}
        levels = {},    -- folded key ("" global, "mod", "mod.cat") -> rank
        defaults = {},
        generation = 1,
        nextLogger = 1,
        flushed = 0,
    }

    local function check(name, ...)
        fake.counts[name] = (fake.counts[name] or 0) + 1
        local shape = SHAPES[name]
        local count = select("#", ...)
        if count ~= #shape then
            fake.violations[#fake.violations + 1] = name .. ": " .. count .. " arguments, expected " .. #shape
            return false
        end
        for index = 1, #shape do
            local value = select(index, ...)
            local want = shape:sub(index, index)
            local okType = want == "i" and math.type(value) == "integer" or want == "s" and type(value) == "string"
            if not okType then
                fake.violations[#fake.violations + 1] = name .. ": argument " .. index .. " is " .. type(value)
                return false
            end
        end
        if coroutine.isyieldable() then error("cannot call a native from a coroutine", 0) end
        return true
    end

    local function resolve(module, category)
        local m = module:lower()
        return fake.levels[m .. "." .. category] or fake.levels[m] or fake.defaults[m] or fake.levels[""] or 4
    end

    function fake.setLevel(key, rank)
        fake.levels[key] = rank
        fake.generation = fake.generation + 1
    end

    local natives = {}

    function natives.GetVersion(...)
        if not check("GetVersion", ...) then return nil, "invalid", "bad arguments" end
        return "1.0.0"
    end

    function natives.GetCapabilities(...)
        if not check("GetCapabilities", ...) then return nil, "invalid", "bad arguments" end
        return 1, 4194304, 65536, 1
    end

    function natives.GetStats(...)
        if not check("GetStats", ...) then return nil, "invalid", "bad arguments" end
        return #fake.lines, #fake.lines, 0, 0, 0, fake.nextLogger - 1, fake.generation
    end

    function natives.Register(...)
        if not check("Register", ...) then return nil, "invalid", "Register: bad arguments" end
        local session, name, target, frequency, interval, rank = ...
        if session ~= SESSION then return nil, "invalid", "Register: unknown session" end
        if name == "" then name = Fake.MOD end
        if not name:match("^[%w_][%w_ %-]*$") or #name > 64 then return nil, "invalid", "Register: bad module name" end
        local id = fake.nextLogger
        fake.nextLogger = id + 1
        fake.loggers[id] = { name = name, target = target, frequency = frequency, interval = interval,
            rank = rank }
        if rank > 0 then fake.defaults[name:lower()] = rank end
        return id, name
    end

    function natives.Write(...)
        if not check("Write", ...) then return nil, "invalid", "Write: bad arguments" end
        local session, logger, rank, category, message = ...
        local owner = fake.loggers[logger]
        if session ~= SESSION or not owner then return nil, "invalid", "Write: unknown logger" end
        fake.lines[#fake.lines + 1] = { logger = logger, name = owner.name, rank = rank, level = NAMES[rank],
            category = category, message = message }
        return fake.generation
    end

    function natives.Level(...)
        if not check("Level", ...) then return nil, "invalid", "Level: bad arguments" end
        local session, logger, category = ...
        local owner = fake.loggers[logger]
        if session ~= SESSION or not owner then return nil, "invalid", "Level: unknown logger" end
        return resolve(owner.name, category), fake.generation
    end

    function natives.Generation(...)
        if not check("Generation", ...) then return nil, "invalid", "bad arguments" end
        return fake.generation
    end

    function natives.SetLevel(...)
        if not check("SetLevel", ...) then return nil, "invalid", "SetLevel: bad arguments" end
        local key, level = ...
        if key:find("/", 1, true) then return nil, "invalid", "SetLevel: bad key" end
        local rank
        for index, name in ipairs(NAMES) do if name == level then rank = index end end
        if level ~= "" and not rank then return nil, "invalid", "SetLevel: bad level" end
        fake.setLevel(key:lower() == "global" and "" or key:lower(), rank)
        return true
    end

    function natives.GetLevel(...)
        if not check("GetLevel", ...) then return nil, "invalid", "GetLevel: bad arguments" end
        local key = ...
        if key == "" then return NAMES[fake.levels[""] or 4] end
        local module, category = key:match("^([^.]+)%.?(.*)$")
        return NAMES[resolve(module, category ~= "" and category or "\0")]
    end

    function natives.Flush(...)
        if not check("Flush", ...) then return nil, "invalid", "bad arguments" end
        fake.flushed = fake.flushed + 1
        return true
    end

    function fake.install()
        for name, fn in pairs(natives) do _G["UE4SSLuaLogBridge_" .. name] = fn end
        __UE4SSLuaLogBridge_SessionId = SESSION
    end

    return fake
end

return Fake
