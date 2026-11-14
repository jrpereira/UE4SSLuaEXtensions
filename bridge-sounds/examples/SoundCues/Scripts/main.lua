-- SoundCues: directional cues with UE4SSLuaSoundBridge, and a hands-on check of
-- the bridge in game. Copy the SoundCues folder into ue4ss/Mods and enable it.
--
--   F5  left        full left, pitch 1
--   F6  right       full right, pitch 1
--   F7  ahead       centre, pitch 1.2
--   F8  behind      centre, pitch 0.8, low-pass at 1200 Hz (muffled)
--   F9  burst       20 plays in one second, alternating full left and full right
--
-- The cue is sounds/cue.wav in this mod's folder. Without one, the script
-- writes a short beep there first. Each key also prints the bridge's
-- statistics to UE4SS.log, so a Lua reload (sounds released, then loaded
-- again) shows whether loaded sounds pile up.

local Bridge = UE4SSLuaSoundBridge
if type(Bridge) ~= "table" then
    print("[SoundCues] UE4SSLuaSoundBridge is not installed; nothing to do\n")
    return
end
local sounds = Bridge()

local function modFolder()
    local source = debug.getinfo(1, "S").source:gsub("^@", "")
    return source:match("^(.*)[/\\]Scripts[/\\][^/\\]+$")
end

-- A 150 ms 880 Hz beep with a fast decay, mono 16-bit at 48 kHz.
local function beep()
    local rate, frames, samples = 48000, 7200, {}
    for frame = 0, frames - 1 do
        local t = frame / rate
        local value = math.sin(2 * math.pi * 880 * t) * math.exp(-t * 25) * 0.6
        samples[#samples + 1] = string.pack("<i2", math.floor(value * 32767))
    end
    local data = table.concat(samples)
    local fmt = string.pack("<I2I2I4I4I2I2", 1, 1, rate, rate * 2, 2, 16)
    return "RIFF" .. string.pack("<I4", 4 + 8 + #fmt + 8 + #data) .. "WAVE"
        .. "fmt " .. string.pack("<I4", #fmt) .. fmt .. "data" .. string.pack("<I4", #data) .. data
end

local cue, code, message = sounds.Load("sounds/cue.wav")
if cue == nil and code == "not_found" then
    -- Write the beep with plain Lua io, then load it.
    local folder = modFolder()
    os.execute('mkdir "' .. folder:gsub("/", "\\") .. '\\sounds" 2>nul')
    local file = io.open(folder .. "/sounds/cue.wav", "wb")
    if file then
        file:write(beep())
        file:close()
    end
    cue, code, message = sounds.Load("sounds/cue.wav")
end
if cue == nil then
    print(string.format("[SoundCues] can't load sounds/cue.wav: %s %s\n", tostring(code), tostring(message)))
    return
end

local function stats(label)
    local s = sounds.GetStats()
    print(string.format("[SoundCues] %s: device %s, sounds %d (%d bytes), playing %d, started %d, dropped %d, stolen %d\n",
        label, s.device, s.sounds, s.sound_bytes, s.playing, s.started, s.dropped, s.stolen))
end

local CUES = {
    left = { pan = -1 },
    right = { pan = 1 },
    ahead = { pitch = 1.2 },
    behind = { pitch = 0.8, lowpass = 1200 },
}

local function play(name)
    local id, failed, why = cue:Play(CUES[name])
    if id == nil then print(string.format("[SoundCues] %s failed: %s %s\n", name, failed, why)) end
    stats(name)
end

RegisterKeyBind(Key.F5, function() play("left") end)
RegisterKeyBind(Key.F6, function() play("right") end)
RegisterKeyBind(Key.F7, function() play("ahead") end)
RegisterKeyBind(Key.F8, function() play("behind") end)
RegisterKeyBind(Key.F9, function()
    local count = 0
    LoopAsync(50, function()
        count = count + 1
        cue:Play(count % 2 == 1 and CUES.left or CUES.right)
        if count < 20 then return false end
        stats("burst")
        return true
    end)
end)

stats(string.format("loaded %s (%.3f s, %d Hz, %d channel(s))", cue.path, cue.duration, cue.rate, cue.channels))
