-- UE4SSLuaSoundBridge load-time verifications. Ships as Scripts/main.lua in the
-- bridge's own mod folder and runs only when the MCS setting "Enable load-time
-- verifications" is on (settings.ini: [Verifications] LoadTime=1); it is off by
-- default. Results go to UE4SS.log as PASS, FAIL and SKIP lines, followed by a
-- SUMMARY line once the timed playback checks finish. The harness is the
-- shared Scripts/lex_verify.lua.
--
-- It writes a few small WAV files named lexverify-*.wav into this mod's own
-- folder, plays them at volume 0 (silent) and removes them again.

local Verify = require("lex_verify")
if not Verify.enabled() then return end

local V = Verify.new("[UE4SSLuaSoundBridge verify] ")
local log, check, group, ok, refused, raises, skip, summary =
    V.log, V.check, V.group, V.ok, V.refused, V.raises, V.skip, V.summary
local modFolder = Verify.modFolder

-- A WAV file of `frames` frames of a quiet sine. tag 1 = PCM, 3 = float.
local function wav(channels, rate, bits, tag, frames)
    local samples = {}
    local formats = { [8] = "B", [16] = "<i2", [24] = "<i3", [32] = tag == 3 and "<f" or "<i4", [64] = "<d" }
    local format = formats[bits]
    for frame = 0, frames - 1 do
        local value = 0.25 * math.sin(frame * 2 * math.pi * 440 / rate)
        for _ = 1, channels do
            local encoded
            if tag == 3 then encoded = value
            elseif bits == 8 then encoded = math.floor(value * 127 + 128)
            else encoded = math.floor(value * ((1 << (bits - 1)) - 1)) end
            samples[#samples + 1] = string.pack(format, encoded)
        end
    end
    local data = table.concat(samples)
    local block = channels * bits // 8
    local fmt = string.pack("<I2I2I4I4I2I2", tag, channels, rate, rate * block, block, bits)
    return "RIFF" .. string.pack("<I4", 4 + 8 + #fmt + 8 + #data) .. "WAVE"
        .. "fmt " .. string.pack("<I4", #fmt) .. fmt .. "data" .. string.pack("<I4", #data) .. data
end

local written = {}

local function write(name, content)
    local file = assert(io.open(modFolder() .. "/" .. name, "wb"))
    file:write(content)
    file:close()
    written[#written + 1] = name
end

local function cleanup()
    for _, name in ipairs(written) do os.remove(modFolder() .. "/" .. name) end
end

local PLAY_WAIT_MS = 600

local function main()
    local Bridge = UE4SSLuaSoundBridge
    if type(Bridge) ~= "table" then
        V.fail("UE4SSLuaSoundBridge global", "missing; the native bridge did not start for this Lua state")
        summary()
        return
    end
    log("load-time verifications enabled; bridge " .. tostring(Bridge.GetVersion and Bridge.GetVersion()))
    local sounds = Bridge()
    local loaded = {}

    group("entry point", function()
        check("UE4SSLuaSoundBridge() returns the environment", type(sounds) == "table" and Bridge() == sounds)
        local version = Bridge.GetVersion()
        check("GetVersion is x.y.z", type(version) == "string" and version:match("^%d+%.%d+%.%d+$") ~= nil, version)
        check("API_VERSION is 1", Bridge.API_VERSION == 1 and sounds.API_VERSION == 1, Bridge.API_VERSION)
        local caps = Bridge.GetCapabilities()
        check("capabilities api is 1", caps.api == 1, caps.api)
        check("capabilities backend", caps.backend == "xaudio2", caps.backend)
        check("at least 8 voices", caps.max_voices >= 8, caps.max_voices)
        check("pitch range covers 0.5 to 2", caps.pitch.min <= 0.5 and caps.pitch.max >= 2)
        check("stop is supported", caps.stop == true)
        check("target_ue4ss_commit", caps.target_ue4ss_commit == "97b7e501", caps.target_ue4ss_commit)
        local stats = sounds:GetStats()
        check("GetStats fields", math.type(stats.sounds) == "integer" and math.type(stats.started) == "integer"
            and type(stats.device) == "string")
        raises("assigning an environment field", nil, function() sounds.Load = nil end)
    end)

    group("loading", function()
        write("lexverify-mono16.wav", wav(1, 44100, 16, 1, 4410))
        write("lexverify-stereo-float.wav", wav(2, 48000, 32, 3, 4800))
        write("lexverify-mono24.wav", wav(1, 48000, 24, 1, 2400))
        write("lexverify-mono8.wav", wav(1, 22050, 8, 1, 2205))
        write("lexverify-long.wav", wav(1, 8000, 16, 1, 16000))
        write("lexverify-text.wav", "this is not a WAV file\n")

        local before = sounds.GetStats().sounds
        local mono = ok("Load mono 16-bit", sounds.Load("lexverify-mono16.wav"))
        if mono then
            check("mono fields", mono.channels == 1 and mono.rate == 44100 and mono.frames == 4410)
            check("duration in seconds", math.abs(mono.duration - 0.1) < 1e-9, mono.duration)
            check("path is normalized", mono.path == "lexverify-mono16.wav", mono.path)
            loaded.mono = mono
        end
        local stereo = ok("Load stereo float", sounds:Load("./lexverify-stereo-float.wav"))
        if stereo then
            check("stereo fields", stereo.channels == 2 and stereo.rate == 48000 and stereo.frames == 4800)
            loaded.stereo = stereo
        end
        loaded.mono24 = ok("Load mono 24-bit", sounds.Load("lexverify-mono24.wav"))
        loaded.mono8 = ok("Load mono 8-bit", sounds.Load("lexverify-mono8.wav"))
        check("GetStats counts loaded sounds", sounds.GetStats().sounds >= before + 4)
        ok("Load within max_seconds", sounds.Load("lexverify-long.wav", { max_seconds = 2 }))
        refused("Load over max_seconds", "too_long", sounds.Load("lexverify-long.wav", { max_seconds = 1 }))
        refused("missing file", "not_found", sounds.Load("lexverify-missing.wav"))
        refused("not a WAV file", "unsupported", sounds.Load("lexverify-text.wav"))
        refused("a folder", "invalid", sounds.Load("Scripts"))
        refused("climbing out of the folder", "outside_root", sounds.Load("../x.wav"))
        refused("absolute path", "invalid", sounds.Load("C:/Windows/Media/chord.wav"))
        refused("rooted path", "invalid", sounds.Load("/x.wav"))
        refused("device name", "outside_root", sounds.Load("CON.wav"))
        refused("empty path", "invalid", sounds.Load(""))
        refused("unknown option", "invalid", sounds.Load("lexverify-mono16.wav", { loop = true }))
        refused("non-string path", "invalid", sounds.Load(42))
    end)

    group("playing", function()
        local mono = loaded.mono
        if not mono then return skip("playing", "no sound loaded") end
        local first = ok("Play silently", mono:Play({ volume = 0 }))
        check("Play returns an integer id", math.type(first) == "integer", first)
        for _, options in ipairs({
            { volume = 0, pan = -1 }, { volume = 0, pan = 1 }, { volume = 0, pitch = 1.2 },
            { volume = 0, pitch = 0.8, lowpass = 1200 }, { volume = 0, pan = -5, pitch = 9, lowpass = 1e9 },
        }) do
            ok("Play with options", mono:Play(options))
        end
        if loaded.stereo then ok("Play stereo through env.Play", sounds.Play(loaded.stereo, { volume = 0, pan = 0.5 })) end
        check("Stop a play", sounds.Stop(first) == true)
        check("Stop an unknown id", sounds.Stop(123456789) == true)
        check("sound:Stop", mono:Stop() == true)
        refused("Play with an unknown option", "invalid", mono:Play({ loop = true }))
        refused("Play with a non-number", "invalid", mono:Play({ pan = "left" }))
        refused("Play with NaN", "invalid", mono:Play({ volume = 0 / 0 }))
        refused("dot call on a sound", "invalid", mono.Play({ volume = 0 }))
        refused("Stop without an id", "invalid", sounds.Stop("x"))
        for index = 1, 40 do mono:Play({ volume = 0, pan = index % 2 == 0 and -1 or 1 }) end
        check("StopAll", sounds.StopAll() == true)
    end)

    group("releasing", function()
        local mono8 = loaded.mono8
        if not mono8 then return skip("releasing", "no sound loaded") end
        local before = sounds.GetStats().sounds
        check("Release", mono8:Release() == true and mono8.released == true)
        check("Release twice", mono8:Release() == true)
        check("GetStats drops the released sound", sounds.GetStats().sounds == before - 1)
        refused("Play after Release", "invalid", mono8:Play({ volume = 0 }))
    end)

    local startedBefore = sounds.GetStats().started
    if loaded.mono then loaded.mono:Play({ volume = 0 }) end
    local waited = 0
    LoopAsync(100, function()
        waited = waited + 100
        if waited < PLAY_WAIT_MS then return false end
        group("audio thread", function()
            local stats = sounds.GetStats()
            log("device " .. stats.device .. ", started " .. stats.started .. ", dropped " .. stats.dropped
                .. ", stolen " .. stats.stolen .. ", playing " .. stats.playing)
            if stats.device == "ready" then
                check("plays reach the device", stats.started > startedBefore, stats.started)
                check("a burst over the voice limit steals, never fails", stats.dropped == 0, stats.dropped)
            else
                skip("plays reach the device", "no audio output device (" .. stats.device .. ")")
            end
        end)
        group("cleanup", function()
            for _, sound in pairs(loaded) do sound:Release() end
            cleanup()
            check("lexverify files removed", io.open(modFolder() .. "/lexverify-mono16.wav", "rb") == nil)
        end)
        summary()
        return true
    end)
end

V.run(main)
