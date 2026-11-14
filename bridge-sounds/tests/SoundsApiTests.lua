-- Lua-layer tests for UE4SSLuaSoundBridge (bridge-sounds/lua/sounds_api.lua),
-- against a fake native table that follows the native contract.
-- Run from the repository root with lua5.4.
package.path = "bridge-sounds/tests/support/?.lua;shared/tests/?.lua;" .. package.path
local Fake = require("FakeSoundNatives")

local API_PATH = "bridge-sounds/lua/sounds_api.lua"

local Kit = require("TestKit")
local expect, expectFailure, expectRaise = Kit.expect, Kit.expectFailure, Kit.expectRaise

local function fresh()
    local fake = Fake.new()
    fake.install()
    local chunk = assert(loadfile(API_PATH))
    chunk()
    return fake, UE4SSLuaSoundBridge
end

local suite = Kit.suite("UE4SSLuaSoundBridge Lua tests")
local function test(name, fn)
    suite.run(name, function()
        local fake, bridge = fresh()
        fn(fake, bridge, bridge())
        if #fake.violations > 0 then
            error("native contract violations: " .. table.concat(fake.violations, ", "))
        end
    end)
end

-- Loading the layer ---------------------------------------------------------------

test("load hides natives and the session id", function(fake, bridge, sounds)
    expect(__UE4SSLuaSoundBridge_SessionId == nil)
    for name in pairs(Fake.SHAPES) do
        expect(_G["UE4SSLuaSoundBridge_" .. name] == nil, name .. " is still global")
    end
    expect(type(bridge) == "table" and type(sounds) == "table")
    expect(bridge() == sounds, "the environment is one table per Lua state")
end)

test("global and environment are read-only and opaque", function(_, bridge, sounds)
    expectRaise("read-only", function() bridge.Load = 1 end)
    expectRaise("read-only", function() sounds.Load = 1 end)
    expect(getmetatable(bridge) == false and getmetatable(sounds) == false)
    expect(bridge.API_VERSION == 1 and sounds.API_VERSION == 1)
end)

test("version, capabilities and stats", function(_, bridge, sounds)
    expect(bridge.GetVersion() == "1.0.0" and sounds.GetVersion() == "1.0.0" and sounds:GetVersion() == "1.0.0")
    local caps = bridge.GetCapabilities()
    expect(caps.api == 1 and caps.max_voices == 32 and caps.backend == "xaudio2" and caps.stop == true)
    expect(caps.pitch.min == 0.5 and caps.pitch.max == 2 and caps.volume.max == 4 and caps.lowpass.min == 20)
    expect(caps.target_ue4ss_commit == "97b7e501" and caps.max_file_bytes == 67108864)
    expect(bridge.GetCapabilities() ~= caps, "a new table each call")
    local stats = sounds.GetStats()
    expect(stats.sounds == 0 and stats.device == "ready" and stats.started == 0)
end)

-- Load ------------------------------------------------------------------------------

test("Load returns a sound with its fields", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    expect(cue.path == "sounds/cue.wav" and cue.frames == 4410 and cue.channels == 1 and cue.rate == 44100)
    expect(math.abs(cue.duration - 0.1) < 1e-12)
    expect(cue.released == false)
    expect(fake.lastMaxMs == -1, "no limit by default")
    expect(tostring(cue):find("sounds/cue.wav", 1, true))
    expectRaise("read-only", function() cue.path = "x" end)
    expect(getmetatable(cue) == false)
    local colon = assert(sounds:Load(".\\sounds\\stereo.wav"))
    expect(colon.channels == 2 and colon.path == "sounds/stereo.wav")
end)

test("Load passes failures through", function(_, _, sounds)
    expectFailure("not_found", sounds.Load("sounds/missing.wav"))
    expectFailure("unsupported", sounds.Load("sounds/text.wav"))
    expectFailure("outside_root", sounds.Load("../x.wav"))
end)

test("Load checks its arguments before any native call", function(fake, _, sounds)
    expectFailure("invalid", sounds.Load(nil))
    expectFailure("invalid", sounds.Load(42))
    expectFailure("invalid", sounds.Load("a\0b.wav"))
    expectFailure("invalid", sounds.Load("sounds/cue.wav", "fast"))
    expectFailure("invalid", sounds.Load("sounds/cue.wav", { loop = true }))
    expectFailure("invalid", sounds.Load("sounds/cue.wav", { max_seconds = -1 }))
    expectFailure("invalid", sounds.Load("sounds/cue.wav", { max_seconds = "10" }))
    expectFailure("invalid", sounds.Load("sounds/cue.wav", { max_seconds = 0 / 0 }))
    expect(#fake.callsTo("Load") == 0)
end)

test("max_seconds becomes milliseconds", function(fake, _, sounds)
    assert(sounds.Load("sounds/long.wav", { max_seconds = 10 }))
    expect(fake.lastMaxMs == 10000)
    expectFailure("too_long", sounds.Load("sounds/long.wav", { max_seconds = 9.9994 }))
    assert(sounds.Load("sounds/cue.wav", { max_seconds = 0.1 }))
    expect(fake.lastMaxMs == 100)
    assert(sounds.Load("sounds/cue.wav", { max_seconds = math.huge }))
    expect(fake.lastMaxMs == -1)
end)

-- Play -------------------------------------------------------------------------------

test("Play sends defaults in millionths", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    local id = cue:Play()
    expect(math.type(id) == "integer")
    local play = fake.lastPlay()
    expect(play.id == id and play.pan == 0 and play.volume == 1000000 and play.pitch == 1000000 and play.lowpass == 0)
end)

test("Play converts and clamps its options", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    assert(cue:Play({ pan = -1, volume = 0.5, pitch = 1.2, lowpass = 1200.4 }))
    local play = fake.lastPlay()
    expect(play.pan == -1000000 and play.volume == 500000 and play.pitch == 1200000 and play.lowpass == 1200)
    assert(cue:Play({ pan = 7, volume = 99, pitch = 0.1, lowpass = 3 }))
    play = fake.lastPlay()
    expect(play.pan == 1000000 and play.volume == 4000000 and play.pitch == 500000 and play.lowpass == 20)
    assert(cue:Play({ pan = -math.huge, volume = -1, pitch = math.huge, lowpass = math.huge }))
    play = fake.lastPlay()
    expect(play.pan == -1000000 and play.volume == 0 and play.pitch == 2000000 and play.lowpass == 1000000)
    assert(cue:Play({ lowpass = false }))
    expect(fake.lastPlay().lowpass == 0)
end)

test("env.Play matches sound:Play", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    assert(sounds.Play(cue, { pan = 0.25 }))
    expect(fake.lastPlay().pan == 250000)
    assert(sounds:Play(cue, { pan = -0.25 }))
    expect(fake.lastPlay().pan == -250000)
end)

test("Play refuses bad options without a native call", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    expectFailure("invalid", cue:Play({ loop = true }))
    expectFailure("invalid", cue:Play({ pan = "left" }))
    expectFailure("invalid", cue:Play({ volume = 0 / 0 }))
    expectFailure("invalid", cue:Play({ lowpass = true }))
    expectFailure("invalid", cue:Play("loud"))
    expectFailure("invalid", cue.Play({ volume = 1 }))
    expectFailure("invalid", sounds.Play({}, {}))
    expect(#fake.callsTo("Play") == 0)
end)

-- Stop and Release -----------------------------------------------------------------------

test("Stop, sound:Stop and StopAll", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    local stereo = assert(sounds.Load("sounds/stereo.wav"))
    local id = assert(cue:Play())
    expect(sounds.Stop(id) == true and fake.stopped[1] == id)
    expect(sounds:Stop(id) == true)
    expectFailure("invalid", sounds.Stop("x"))
    expectFailure("invalid", sounds.Stop(1.5))
    expect(cue:Stop() == true and #fake.stoppedSounds == 1)
    expect(sounds.StopAll() == true and #fake.stoppedSounds == 3)
    expect(stereo.released == false)
end)

test("Release frees once and blocks further plays", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    expect(cue:Release() == true and cue.released == true)
    expect(cue:Release() == true and #fake.released == 1)
    expectFailure("invalid", cue:Play())
    expect(cue:Stop() == true)
    expect(#fake.callsTo("Play") == 0)
    local before = #fake.callsTo("StopSound")
    expect(sounds.StopAll() == true and #fake.callsTo("StopSound") == before, "released sounds are skipped")
end)

test("garbage collection releases at the next native call", function(fake, _, sounds)
    local function loadAndDrop()
        assert(sounds.Load("sounds/cue.wav"))
    end
    loadAndDrop()
    collectgarbage()
    collectgarbage()
    expect(#fake.released == 0, "no native call inside a finalizer")
    sounds.GetStats()
    expect(#fake.released == 1)
end)

test("a coroutine call fails as invalid and keeps a release queued", function(fake, _, sounds)
    local cue = assert(sounds.Load("sounds/cue.wav"))
    local results
    coroutine.wrap(function() results = table.pack(cue:Play()) end)()
    expect(results[1] == nil and results[2] == "invalid" and results[3]:find("not available here", 1, true))
    coroutine.wrap(function() results = table.pack(cue:Release()) end)()
    expect(results[1] == nil and results[2] == "invalid" and cue.released == true)
    expect(#fake.released == 0)
    sounds.GetStats()
    expect(#fake.released == 1, "the queued release drains on the main thread")
end)

suite.finish()
