# UE4SS Lua Sound Bridge

UE4SSLuaSoundBridge lets UE4SS Lua mods play their own sounds. A mod loads WAV
files from its own folder once, then plays them at once, as often as it likes,
with pan, volume, pitch and a low-pass filter chosen per play. Playback runs
through XAudio2, beside the game's own audio engine, so it works where the
game's sound banks can't help: sounds the game doesn't ship, and cues whose
direction must not lag the camera.

## Features

- Load once, play many times: `Load` reads and decodes a WAV file into memory;
  `Play` does no file I/O and never waits on the audio engine.
- Per play: pan from full left to full right, volume, pitch as a playback rate
  (0.5 to 2) and an optional low-pass filter.
- 32 voices; when all are busy a new play takes over the oldest instead of
  failing.
- PCM WAV at 8, 16, 24 or 32 bits and float WAV at 32 or 64 bits, mono or
  stereo, 1 to 200 kHz.
- Follows the default output device: unplugging headphones or switching
  devices restarts playback on the new default; with no device, plays are
  dropped quietly.
- Sounds belong to the Lua state that loaded them and are released when it
  stops or reloads.
- Errors as `nil, code, message` with eight stable codes.

Sounds are read only from the calling mod's own folder. By convention they go
in a `sounds/` subfolder, next to `Scripts/`.

The bridge doesn't follow the game's volume settings or pause; a mod that
needs that offers its own volume and stops playing while paused.

## Compatibility

Windows 10 or later (XAudio2 2.9 ships with Windows), Unreal Engine 5.5, and
UE4SS 3.0.1 Beta #0 at commit `97b7e501`, the same pin as the other UE4SS Lua
extensions. Check `GetCapabilities()` for API features rather than inferring
them from the product version.

## Installation layout

Download the versioned ZIP and checksum from
[GitHub Releases](https://github.com/jrpereira/UE4SSLuaEXtensions/releases)
(releases tagged `bridge-sounds/v*`). Extract the ZIP into the UE4SS `Mods`
directory to produce:

```text
Mods/
└── 0_ModCore_UE4SSLuaSoundBridge/
    ├── dlls/
    │   ├── main.dll
    │   ├── main.json
    │   └── versions/
    │       └── UE4SSLuaSoundBridge-1.0.0.dll
    ├── enabled.txt
    └── mod.json
```

`main.dll` is a small bootstrap that validates and loads the versioned
implementation from `dlls/versions`, as in the other extensions. The packaged
`enabled.txt` enables the bridge without a `mods.txt` entry, and its `0_`
folder name sorts ahead of the mods that use it.

Use the published SHA-256 checksum to verify the exact archive bytes. Keep
only one active installation of the sound bridge.

## Minimal Lua example

```lua
if type(UE4SSLuaSoundBridge) ~= "table" then return end   -- not installed: fall back
local sounds = UE4SSLuaSoundBridge()

local cue, code, message = sounds.Load("sounds/cue.wav", { max_seconds = 5 })
if not cue then
    print(string.format("cue unavailable: %s %s\n", code, message))
    return
end

cue:Play({ pan = -1 })                               -- full left
cue:Play({ pitch = 1.2 })                            -- higher, centre
cue:Play({ pitch = 0.8, lowpass = 1200 })            -- lower and muffled
```

`Play` returns at once from any Lua thread UE4SS runs (game thread, hooks,
`LoopAsync`); the sound starts on the next audio pass, about 10 ms later.

## Documentation

- [API reference](docs/API.md): every function, option, field and error code.
- [Changelog](CHANGELOG.md): version history.
- [SoundCues sample](examples/SoundCues/Scripts/main.lua): left, right, ahead
  and behind cues on F5 to F8, and a 20-play burst on F9.
