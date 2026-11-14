-- UE4SSLuaSoundBridge Lua layer (API 1). Embedded in the native DLL and run once
-- per Lua environment from on_lua_start. Public API: bridge-sounds/docs/API.md.
-- Native boundary: bridge-sounds/include/SoundBridgeNativeContract.hpp.
local __session = __UE4SSLuaSoundBridge_SessionId
__UE4SSLuaSoundBridge_SessionId = nil
assert(math.type(__session) == "integer", "sound bridge session id is missing")

local __API_VERSION = 1
local __NATIVE_PREFIX = "UE4SSLuaSoundBridge_"
local __NATIVE_NAMES = { "GetVersion", "GetCapabilities", "GetStats", "Load", "Release", "Play", "Stop", "StopSound" }

-- Natives crash the game on a wrong argument shape, so only this layer may
-- reach them: capture each one and remove the global (contract R5).
local __natives = {}
for _, name in ipairs(__NATIVE_NAMES) do
    local global = __NATIVE_PREFIX .. name
    local fn = _G[global]
    assert(type(fn) == "function", "sound bridge native is missing: " .. global)
    __natives[name] = fn
    _G[global] = nil
end

-- Per-play ranges (contract section "Limits"); values outside are clamped.
local __MICRO = 1000000
local __PAN_MIN, __PAN_MAX = -1, 1
local __VOLUME_MIN, __VOLUME_MAX = 0, 4
local __PITCH_MIN, __PITCH_MAX = 0.5, 2
local __LOWPASS_MIN, __LOWPASS_MAX = 20, 1000000

local function __fail(message)
    return nil, "invalid", message
end

-- A native raises only when UE4SS refuses the calling Lua thread (a coroutine
-- the mod created, contract R6). That becomes nil, "invalid", message.
local __refused = false

local function __finish(name, ok, ...)
    __refused = not ok
    if not ok then
        return nil, "invalid", name .. ": not available here (" .. tostring((...)) .. ")"
    end
    return ...
end

local function __raw(name, ...)
    return __finish(name, pcall(__natives[name], ...))
end

-- Garbage collection only queues sound ids; no native runs inside a
-- finalizer. The queue drains at the start of the next native call.
local __pending = {}

local function __drain()
    if #__pending == 0 then return end
    local work = __pending
    __pending = {}
    for index = 1, #work do
        __raw("Release", __session, work[index])
        if __refused then
            for rest = index, #work do __pending[#__pending + 1] = work[rest] end
            return
        end
    end
end

local function __native(name, ...)
    __drain()
    return __raw(name, ...)
end

local function __readOnly()
    error("UE4SSLuaSoundBridge tables are read-only", 2)
end

-- Number options: nil keeps the default; NaN and non-numbers are refused.
local function __number(op, options, key, default, low, high)
    local value = options[key]
    if value == nil then return default end
    if type(value) ~= "number" or value ~= value then
        return nil, op .. ": option " .. key .. " must be a number"
    end
    if value < low then return low end
    if value > high then return high end
    return value
end

local function __micro(value)
    return math.floor(value * __MICRO + 0.5)
end

local function __checkOptions(op, options, allowed)
    if options == nil then return {} end
    if type(options) ~= "table" then return nil, op .. ": options must be a table" end
    for key in pairs(options) do
        if type(key) ~= "string" or not allowed[key] then
            return nil, op .. ": unknown option " .. tostring(key)
        end
    end
    return options
end

-- Sounds ---------------------------------------------------------------------

local __sounds = setmetatable({}, { __mode = "k" })
local __Sound = {}

local function __soundState(op, sound)
    local state = __sounds[sound]
    if state == nil then return nil, op .. ": call it with a colon on a sound" end
    return state
end

local __PLAY_OPTIONS = { pan = true, volume = true, pitch = true, lowpass = true }

local function __play(sound, options)
    local state, message = __soundState("Play", sound)
    if state == nil then return __fail(message) end
    if state.released then return __fail("Play: the sound was released") end
    local opts
    opts, message = __checkOptions("Play", options, __PLAY_OPTIONS)
    if opts == nil then return __fail(message) end
    local pan, volume, pitch, why
    pan, why = __number("Play", opts, "pan", 0, __PAN_MIN, __PAN_MAX)
    if pan == nil then return __fail(why) end
    volume, why = __number("Play", opts, "volume", 1, __VOLUME_MIN, __VOLUME_MAX)
    if volume == nil then return __fail(why) end
    pitch, why = __number("Play", opts, "pitch", 1, __PITCH_MIN, __PITCH_MAX)
    if pitch == nil then return __fail(why) end
    local lowpass = 0
    if opts.lowpass ~= nil and opts.lowpass ~= false then
        lowpass, why = __number("Play", opts, "lowpass", 0, __LOWPASS_MIN, __LOWPASS_MAX)
        if lowpass == nil then return __fail(why) end
        lowpass = math.floor(lowpass + 0.5)
    end
    return __native("Play", __session, state.id, __micro(pan), __micro(volume), __micro(pitch), lowpass)
end

function __Sound.Play(self, options)
    return __play(self, options)
end

function __Sound.Stop(self)
    local state, message = __soundState("Stop", self)
    if state == nil then return __fail(message) end
    if state.released then return true end
    local ok, code, why = __native("StopSound", __session, state.id)
    if ok == nil then return nil, code, why end
    return true
end

function __Sound.Release(self)
    local state, message = __soundState("Release", self)
    if state == nil then return __fail(message) end
    if state.released then return true end
    state.released = true
    local ok, code, why = __native("Release", __session, state.id)
    if __refused then
        __pending[#__pending + 1] = state.id
        return nil, code, why
    end
    if ok == nil then return nil, code, why end
    return true
end

local __FIELDS = { path = true, frames = true, channels = true, rate = true, duration = true, released = true }

local __soundMeta = {
    __index = function(self, key)
        local state = __sounds[self]
        if __FIELDS[key] then return state and state[key] end
        return __Sound[key]
    end,
    __newindex = __readOnly,
    __metatable = false,
    __tostring = function(self)
        local state = __sounds[self]
        return "UE4SSLuaSoundBridge sound " .. tostring(state and state.path)
    end,
    __gc = function(self)
        local state = __sounds[self]
        if state and not state.released then
            state.released = true
            __pending[#__pending + 1] = state.id
        end
    end,
}

local __LOAD_OPTIONS = { max_seconds = true }

local function __Load(path, options)
    if type(path) ~= "string" or path:find("\0", 1, true) then
        return __fail("Load: path must be a string without NUL bytes")
    end
    local opts, message = __checkOptions("Load", options, __LOAD_OPTIONS)
    if opts == nil then return __fail(message) end
    local maxMs = -1
    if opts.max_seconds ~= nil then
        local seconds = opts.max_seconds
        if type(seconds) ~= "number" or seconds ~= seconds or seconds < 0 then
            return __fail("Load: option max_seconds must be a number >= 0")
        end
        maxMs = seconds >= 1e12 and -1 or math.floor(seconds * 1000 + 0.5)
    end
    local id, frames, channels, rate, normalized = __native("Load", __session, path, maxMs)
    if id == nil then return nil, frames, channels end
    local proxy = setmetatable({}, __soundMeta)
    __sounds[proxy] = {
        id = id,
        path = normalized,
        frames = frames,
        channels = channels,
        rate = rate,
        duration = frames / rate,
        released = false,
    }
    return proxy
end

local function __Stop(play)
    if math.type(play) ~= "integer" then return __fail("Stop: play must be an id returned by Play") end
    local ok, code, why = __native("Stop", __session, play)
    if ok == nil then return nil, code, why end
    return true
end

local function __StopAll()
    for _, state in pairs(__sounds) do
        if not state.released then
            local ok, code, why = __native("StopSound", __session, state.id)
            if ok == nil then return nil, code, why end
        end
    end
    return true
end

-- Version, capabilities, statistics ----------------------------------------------

local function __GetVersion()
    local version = __native("GetVersion")
    return version
end

local function __GetCapabilities()
    local api, voices, maxBytes, backend, commit = __native("GetCapabilities")
    if api == nil then return nil, voices, maxBytes end
    return {
        api = api,
        backend = backend,
        max_voices = voices,
        max_file_bytes = maxBytes,
        max_channels = 2,
        formats = { "pcm8", "pcm16", "pcm24", "pcm32", "float32", "float64" },
        pan = { min = __PAN_MIN, max = __PAN_MAX },
        volume = { min = __VOLUME_MIN, max = __VOLUME_MAX },
        pitch = { min = __PITCH_MIN, max = __PITCH_MAX },
        lowpass = { min = __LOWPASS_MIN },
        stop = true,
        target_ue4ss_commit = commit,
    }
end

local function __GetStats()
    local sounds, bytes, playing, started, dropped, stolen, device, changes = __native("GetStats")
    if sounds == nil then return nil, bytes, playing end
    return {
        sounds = sounds,
        sound_bytes = bytes,
        playing = playing,
        started = started,
        dropped = dropped,
        stolen = stolen,
        device = device,
        device_changes = changes,
    }
end

-- The environment -----------------------------------------------------------------

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
        Load = __method(__Load),
        Play = __method(function(sound, options) return __play(sound, options) end),
        Stop = __method(__Stop),
        StopAll = __method(__StopAll),
        GetVersion = __method(__GetVersion),
        GetCapabilities = __method(__GetCapabilities),
        GetStats = __method(__GetStats),
    },
    __newindex = __readOnly,
    __metatable = false,
})

UE4SSLuaSoundBridge = setmetatable({}, {
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
