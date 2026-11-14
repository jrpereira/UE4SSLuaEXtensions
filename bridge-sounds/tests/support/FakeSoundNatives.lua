-- A fake of the 8 UE4SSLuaSoundBridge natives, following
-- bridge-sounds/include/SoundBridgeNativeContract.hpp. It checks every call's
-- arity and argument types (contract R2) and records violations instead of
-- crashing, refuses calls from coroutines like UE4SS (R6), and keeps loaded
-- sounds and plays in memory.
local Fake = {}

local SESSION = 11
Fake.SESSION = SESSION

-- Argument shapes: i = integer, s = string, from native_functions.
local SHAPES = {
    GetVersion = "", GetCapabilities = "", GetStats = "",
    Load = "isi", Release = "ii", Play = "iiiiii", Stop = "ii", StopSound = "ii",
}
Fake.SHAPES = SHAPES

function Fake.new()
    local fake = {
        calls = {},
        violations = {},
        -- path -> {frames, channels, rate} or {code, message}
        files = {
            ["sounds/cue.wav"] = { frames = 4410, channels = 1, rate = 44100 },
            ["sounds/stereo.wav"] = { frames = 96000, channels = 2, rate = 48000 },
            ["sounds/long.wav"] = { frames = 480000, channels = 1, rate = 48000 },
            ["sounds/text.wav"] = { code = "unsupported", message = "not a WAV file" },
        },
        sounds = {},
        released = {},
        plays = {},
        stopped = {},
        stoppedSounds = {},
        nextSound = 100,
        nextPlay = 500,
    }

    local function check(name, ...)
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
                fake.violations[#fake.violations + 1] = name .. ": argument " .. index .. " is " .. tostring(value)
                return false
            end
        end
        if shape:sub(1, 1) == "i" and name ~= "GetVersion" and (...) ~= SESSION then
            fake.violations[#fake.violations + 1] = name .. ": wrong session " .. tostring((...))
        end
        return true
    end

    local natives = {}

    function natives.GetVersion() return "1.0.0" end

    function natives.GetCapabilities() return 1, 32, 67108864, "xaudio2", "97b7e501" end

    function natives.GetStats()
        local count = 0
        for _ in pairs(fake.sounds) do count = count + 1 end
        return count, count * 1000, 0, #fake.plays, 0, 0, "ready", 0
    end

    function natives.Load(_, path, maxMs)
        local normalized = path:gsub("\\", "/"):gsub("^%./", "")
        if normalized:find("..", 1, true) then return nil, "outside_root", "Load " .. path .. ": climbs out" end
        local file = fake.files[normalized]
        if not file then return nil, "not_found", "Load " .. path .. ": not found" end
        if file.code then return nil, file.code, "Load " .. path .. ": " .. file.message end
        if maxMs >= 0 and file.frames * 1000 > maxMs * file.rate then
            return nil, "too_long", "Load " .. path .. ": too long"
        end
        fake.nextSound = fake.nextSound + 1
        fake.sounds[fake.nextSound] = normalized
        fake.lastMaxMs = maxMs
        return fake.nextSound, file.frames, file.channels, file.rate, normalized
    end

    function natives.Release(_, sound)
        fake.sounds[sound] = nil
        fake.released[#fake.released + 1] = sound
        return true
    end

    function natives.Play(_, sound, pan, volume, pitch, lowpass)
        if not fake.sounds[sound] then return nil, "invalid", "Play: the sound was released" end
        fake.nextPlay = fake.nextPlay + 1
        fake.plays[#fake.plays + 1] = { id = fake.nextPlay, sound = sound, pan = pan, volume = volume, pitch = pitch,
            lowpass = lowpass }
        return fake.nextPlay
    end

    function natives.Stop(_, play)
        fake.stopped[#fake.stopped + 1] = play
        return true
    end

    function natives.StopSound(_, sound)
        fake.stoppedSounds[#fake.stoppedSounds + 1] = sound
        return true
    end

    function fake.install()
        __UE4SSLuaSoundBridge_SessionId = SESSION
        for name, fn in pairs(natives) do
            _G["UE4SSLuaSoundBridge_" .. name] = function(...)
                fake.calls[#fake.calls + 1] = name
                -- UE4SS refuses natives called from a coroutine the mod made (R6).
                local _, main = coroutine.running()
                if not main then error("calling a C function from a coroutine isn't supported") end
                if not check(name, ...) then return nil, "invalid", name .. ": bad arguments" end
                return fn(...)
            end
        end
    end

    function fake.callsTo(name)
        local found = {}
        for _, call in ipairs(fake.calls) do
            if call == name then found[#found + 1] = call end
        end
        return found
    end

    function fake.lastPlay() return fake.plays[#fake.plays] end

    return fake
end

return Fake
