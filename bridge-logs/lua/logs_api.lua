-- UE4SSLuaLogBridge Lua layer (API 2). Embedded in the native DLL and run once
-- per Lua environment from on_lua_start. Public API: bridge-logs/docs/API.md.
-- Native boundary: bridge-logs/include/LogBridgeNativeContract.hpp.
local __session = __UE4SSLuaLogBridge_SessionId
__UE4SSLuaLogBridge_SessionId = nil
assert(math.type(__session) == "integer", "log bridge session id is missing")

local __API_VERSION = 2
local __NATIVE_PREFIX = "UE4SSLuaLogBridge_"
local __NATIVE_NAMES = {
    "GetVersion", "GetCapabilities", "GetStats", "Register", "Write", "Level", "Generation", "SetLevel", "GetLevel",
    "Flush", "ReloadSettings", "AddCategory", "ListLoggers",
}

-- Only this layer may reach the natives: capture each one and remove the
-- global (contract R5).
local __natives = {}
for _, name in ipairs(__NATIVE_NAMES) do
    local global = __NATIVE_PREFIX .. name
    local fn = _G[global]
    assert(type(fn) == "function", "log bridge native is missing: " .. global)
    __natives[name] = fn
    _G[global] = nil
end

local __LEVELS = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF" }
local __METHODS = { "trace", "debug", "info", "warn", "error", "critical" }
local __RANK = { warning = 4, none = 7 }
for rank, name in ipairs(__LEVELS) do __RANK[name:lower()] = rank end
local __DEFAULT_RANK = 4
local __OFF = 7
local __DEFAULT_CATEGORIES = { "main" }
local __TARGETS = { default = 0, module = 1 }
local __FREQUENCIES = { immediate = 0, frame = 1, periodic = 2 }
-- How long a cached level is trusted before the native generation is asked again.
local __REFRESH_SECONDS = 1
-- Wall-clock seconds since process start on Windows (MSVC's clock()).
local __clock = os.clock

local function __fail(message)
    return nil, "invalid", message
end

local function __readOnly()
    error("UE4SSLuaLogBridge tables are read-only", 2)
end

-- A native raises only when UE4SS refuses the calling Lua thread (contract
-- R6). That becomes nil, "invalid", message.
local function __finish(name, ok, ...)
    if not ok then return nil, "invalid", name .. ": not available here (" .. tostring((...)) .. ")" end
    return ...
end

local function __native(name, ...)
    return __finish(name, pcall(__natives[name], ...))
end

-- A level name (any case) or rank 1..7; nil when neither.
local function __rankOf(level)
    if math.type(level) == "integer" then return level >= 1 and level <= __OFF and level or nil end
    if type(level) == "string" then return __RANK[level:match("^%s*(.-)%s*$"):lower()] end
    return nil
end

local function __validCategory(name)
    if type(name) ~= "string" or #name > 32 or not name:match("^%a[%w_%-]*$") then return false end
    local folded = name:lower()
    if folded == "con" or folded == "prn" or folded == "aux" or folded == "nul" then return false end
    return not folded:match("^com%d$") and not folded:match("^lpt%d$")
end

-- Levels ----------------------------------------------------------------------

-- Every cached rank belongs to an epoch; a new generation from the native
-- side starts a new epoch, and each logger drops its cache when it sees one.
local __generation, __epoch = nil, 0
local __refreshedAt = __clock()

local function __seen(generation)
    if math.type(generation) == "integer" and generation ~= __generation then
        __generation = generation
        __epoch = __epoch + 1
    end
end

-- A level changed elsewhere (config.ini, the menu, another mod's SetLevel)
-- reaches this environment within __REFRESH_SECONDS, whatever the mod calls:
-- a line, enabled() or the level field.
local function __refresh()
    local now = __clock()
    if now - __refreshedAt < __REFRESH_SECONDS then return end
    __refreshedAt = now
    __seen((__native("Generation")))
end

-- Drops a logger's cached ranks when they belong to an older epoch.
local function __fresh(state)
    if state.epoch ~= __epoch then
        state.ranks = {}
        state.epoch = __epoch
    end
end

local function __threshold(state, category)
    __refresh()
    __fresh(state)
    local rank = state.ranks[category]
    if rank then return rank end
    local found, generation = __native("Level", __session, state.id, category)
    -- Refused (a coroutine the mod created): use the module's last known
    -- rank, not cached, so the main thread still asks for this category.
    if math.type(found) ~= "integer" then return state.known or __DEFAULT_RANK end
    __seen(generation)
    __fresh(state)
    state.ranks[category] = found
    if category == "main" or state.known == nil then state.known = found end
    return found
end

-- Writing ---------------------------------------------------------------------

-- Arguments are converted and joined (no separator, as mc_log) only here.
local function __message(count, ...)
    local parts = {}
    for index = 1, count do parts[index] = tostring((select(index, ...))) end
    local text = table.concat(parts)
    if text:find("\0", 1, true) then text = text:gsub("%z", "\\0") end
    return text
end

local function __emit(state, category, rank, count, ...)
    if rank < __threshold(state, category) then return end
    local ok, message = pcall(__message, count, ...)
    if not ok then message = "<unprintable: " .. tostring(message) .. ">" end
    local generation = __native("Write", __session, state.id, rank, category, message)
    if generation == nil then
        -- The bridge refused this Lua thread: the line still goes to UE4SS.log.
        local tag = category == "main" and state.name or state.name .. ":" .. category
        print("[" .. tag .. "] " .. __LEVELS[rank] .. " " .. message .. "\n")
        return
    end
    __seen(generation)
end

-- Loggers ---------------------------------------------------------------------

local __RESERVED = { write = true, enabled = true, register = true, name = true, level = true }
for _, method in ipairs(__METHODS) do __RESERVED[method] = true end

-- Why a category can't be registered under this name, or nil.
local function __categoryProblem(name)
    if not __validCategory(name) then
        return "bad category name " .. tostring(name)
            .. " (a letter, then up to 31 letters, digits, _ or -, not a device name)"
    end
    if __RESERVED[name:lower()] then return "category name " .. name .. " is a method name" end
end

-- The methods of one category, written into `into`; `self` is the table a
-- colon call passes.
local function __categoryMethods(state, category, self, into)
    for index, method in ipairs(__METHODS) do
        into[method] = function(first, ...)
            if first == self then return __emit(state, category, index, select("#", ...), ...) end
            return __emit(state, category, index, select("#", ...) + 1, first, ...)
        end
    end
    into.write = function(first, ...)
        local level
        if first == self then
            level = ...
            local rank = __rankOf(level)
            if not rank or rank == __OFF then return __fail("write: unknown level " .. tostring(level)) end
            return __emit(state, category, rank, select("#", ...) - 1, select(2, ...))
        end
        local rank = __rankOf(first)
        if not rank or rank == __OFF then return __fail("write: unknown level " .. tostring(first)) end
        return __emit(state, category, rank, select("#", ...), ...)
    end
    into.enabled = function(first, second)
        local rank = __rankOf(first == self and second or first)
        if not rank or rank == __OFF then return false end
        return rank >= __threshold(state, category)
    end
end

-- `level` reads as the effective level name, as mc_log's field does.
local function __levelField(state, category, key)
    if key == "level" then return __LEVELS[__threshold(state, category)] end
end

local function __newCategory(state, category)
    local methods = setmetatable({ name = category }, {
        __index = function(_, key) return __levelField(state, category, key) end,
    })
    local proxy = setmetatable({}, {
        __index = methods,
        __newindex = __readOnly,
        __metatable = false,
        __tostring = function() return "UE4SSLuaLogBridge category " .. state.name .. "." .. category end,
    })
    __categoryMethods(state, category, proxy, methods)
    return proxy
end

local __loggers = {}
local __defaultName

local function __newLogger(id, name)
    local state = { id = id, name = name, ranks = {}, epoch = __epoch, categories = {} }
    local methods = { name = name }
    -- Only registered categories are fields: a misspelt one reads as nil.
    setmetatable(methods, { __index = function(_, key) return __levelField(state, "main", key) end })
    local logger = setmetatable({}, {
        __index = methods,
        __newindex = __readOnly,
        __metatable = false,
        __tostring = function() return "UE4SSLuaLogBridge logger " .. name end,
    })
    __categoryMethods(state, "main", logger, methods)

    local function add(name)
        local key = name:lower()
        local found = state.categories[key]
        if not found then
            found = __newCategory(state, key)
            state.categories[key] = found
            methods[key] = found
            -- The bridge lists it for every mod (ListLoggers); main it knows already.
            __native("AddCategory", __session, state.id, key)
        end
        return found
    end
    -- register(name, ...) adds categories and returns them, in order.
    methods.register = function(...)
        local names = table.pack(...)
        local first = 1
        if names[1] == logger then first = 2 end
        for index = first, names.n do
            local why = __categoryProblem(names[index])
            if why then return __fail("register: " .. why) end
        end
        local found = {}
        for index = first, names.n do found[#found + 1] = add(names[index]) end
        return table.unpack(found)
    end
    state.categories.main = logger
    methods.main = logger
    return logger
end

local __REGISTER_OPTIONS = {
    target = true, frequency = true, interval = true, level = true, categories = true,
}

local function __choice(options, key, choices, default)
    local value = options[key]
    if value == nil then return choices[default] end
    local chosen = type(value) == "string" and choices[value:lower()] or nil
    if chosen == nil then return nil, "Register: option " .. key .. " is unknown: " .. tostring(value) end
    return chosen
end

local function __Register(name, options)
    if type(name) == "table" and options == nil then name, options = nil, name end
    if name ~= nil and type(name) ~= "string" then return __fail("Register: name must be a string") end
    if options == nil then options = {} end
    if type(options) ~= "table" then return __fail("Register: options must be a table") end
    for key in pairs(options) do
        if not __REGISTER_OPTIONS[key] then return __fail("Register: unknown option " .. tostring(key)) end
    end
    local target, why = __choice(options, "target", __TARGETS, "default")
    if target == nil then return __fail(why) end
    local frequency
    frequency, why = __choice(options, "frequency", __FREQUENCIES, "immediate")
    if frequency == nil then return __fail(why) end
    local interval = options.interval
    if interval == nil then interval = 1 end
    if type(interval) ~= "number" or interval ~= interval or interval <= 0 then
        return __fail("Register: option interval must be a number of seconds above 0")
    end
    local intervalMs = interval >= 1e9 and 1e12 or math.floor(interval * 1000 + 0.5)
    intervalMs = math.tointeger(intervalMs) or 3600000
    local rank = 0
    if options.level ~= nil then
        rank = __rankOf(options.level)
        if not rank then return __fail("Register: option level is unknown: " .. tostring(options.level)) end
    end
    local extra = options.categories
    if extra ~= nil then
        if type(extra) ~= "table" then return __fail("Register: option categories must be a list") end
        for _, category in ipairs(extra) do
            local problem = __categoryProblem(category)
            if problem then return __fail("Register: " .. problem) end
        end
    end

    local key = (name or __defaultName or ""):lower()
    local logger = __loggers[key]
    if not logger then
        local id, resolved, message = __native("Register", __session, name or "", target, frequency, intervalMs,
            rank)
        if id == nil then return nil, resolved, message end
        if name == nil then __defaultName = resolved end
        logger = __loggers[resolved:lower()]
        if not logger then
            logger = __newLogger(id, resolved)
            __loggers[resolved:lower()] = logger
        end
    end
    if extra then
        logger.register(table.unpack(extra))
    end
    return logger
end

-- Control ---------------------------------------------------------------------

local function __key(key, op)
    if key == nil then return "" end
    if type(key) ~= "string" then return nil, op .. ": key must be a string" end
    return key
end

local function __SetLevel(key, level)
    local checked, why = __key(key, "SetLevel")
    if checked == nil then return __fail(why) end
    local name = ""
    if level ~= nil then
        local rank = __rankOf(level)
        if not rank then return __fail("SetLevel: unknown level " .. tostring(level)) end
        name = __LEVELS[rank]
    end
    local ok, code, message = __native("SetLevel", checked, name)
    if ok == nil then return nil, code, message end
    __seen((__native("Generation")))
    return true
end

local function __GetLevel(key)
    local checked, why = __key(key, "GetLevel")
    if checked == nil then return __fail(why) end
    return __native("GetLevel", checked)
end

local function __Flush()
    local ok, code, message = __native("Flush")
    if ok == nil then return nil, code, message end
    return true
end

-- Asks the bridge to re-read config.ini; new levels arrive through the
-- generation like any other level change.
local function __ReloadSettings()
    local ok, code, message = __native("ReloadSettings")
    if ok == nil then return nil, code, message end
    return true
end

-- Every module registered this session by any Lua mod, then those config.ini's
-- [Levels] names that have not registered (loaded = false), each
-- { name, level, source, loaded, categories = { { name, level, source, loaded }, ... } },
-- and the generation they belong to.
local function __ListLoggers()
    local text, generation, message = __native("ListLoggers")
    if text == nil then return nil, generation, message end
    local list, current = {}, nil
    for line in text:gmatch("[^\n]+") do
        local kind, name, rank, source, loaded = line:match("^([MC])\t([^\t]+)\t(%d+)\t(%a+)\t([01])$")
        if kind then
            local entry = { name = name, level = __LEVELS[tonumber(rank)] or "WARN", source = source,
                loaded = loaded == "1" }
            if kind == "M" then
                entry.categories = {}
                list[#list + 1] = entry
                current = entry
            elseif current then
                current.categories[#current.categories + 1] = entry
            end
        end
    end
    return list, generation
end

local function __GetVersion()
    local version = __native("GetVersion")
    return version
end

-- The names of a name -> code map, in code order (codes run from 0).
local function __byCode(map)
    local names = {}
    for name, code in pairs(map) do names[code + 1] = name end
    return names
end

local function __GetCapabilities()
    local api, journal, maxMessage, frame = __native("GetCapabilities")
    if api == nil then return nil, journal, maxMessage end
    return {
        api = api,
        journal_bytes = journal,
        max_message_bytes = maxMessage,
        frame = frame == 1,
        levels = table.move(__LEVELS, 1, #__LEVELS, 1, {}),
        targets = __byCode(__TARGETS),
        frequencies = __byCode(__FREQUENCIES),
        default_categories = table.move(__DEFAULT_CATEGORIES, 1, #__DEFAULT_CATEGORIES, 1, {}),
    }
end

local function __GetStats()
    local written, delivered, pending, stalls, recovered, loggers, generation = __native("GetStats")
    if written == nil then return nil, delivered, pending end
    return {
        written = written,
        delivered = delivered,
        pending = pending,
        stalls = stalls,
        recovered = recovered,
        loggers = loggers,
        generation = generation,
    }
end

-- The environment -------------------------------------------------------------

local __environment

-- Accepts both env.Name(...) and env:Name(...).
local function __method(fn)
    return function(first, ...)
        if first == __environment then return fn(...) end
        return fn(first, ...)
    end
end

__environment = setmetatable({}, {
    __index = {
        API_VERSION = __API_VERSION,
        Register = __method(__Register),
        SetLevel = __method(__SetLevel),
        GetLevel = __method(__GetLevel),
        Flush = __method(__Flush),
        ReloadSettings = __method(__ReloadSettings),
        ListLoggers = __method(__ListLoggers),
        GetVersion = __method(__GetVersion),
        GetCapabilities = __method(__GetCapabilities),
        GetStats = __method(__GetStats),
    },
    __newindex = __readOnly,
    __metatable = false,
})

UE4SSLuaLogBridge = setmetatable({}, {
    __index = {
        API_VERSION = __API_VERSION,
        GetVersion = __GetVersion,
        GetCapabilities = __GetCapabilities,
        GetStats = __GetStats,
    },
    __call = function() return __environment end,
    __newindex = __readOnly,
    __metatable = false,
})
