# UE4SSLuaSoundBridge Lua API (1.0.0)

API version 1. Every Lua mod gets the global `UE4SSLuaSoundBridge` when the
bridge is installed; without the bridge the global is `nil`.

## Conventions

- Functions return their result, or `nil, code, message` on failure. They never
  raise, except when assigning to a bridge table, which is read-only.
- `code` is one of the stable codes below; `message` is English text for logs
  and its wording isn't part of the API.
- Environment functions accept both `sounds.Load(...)` and `sounds:Load(...)`.
  Sound methods need a colon: `cue:Play()`.
- Calls work from every Lua thread UE4SS runs (game thread, hooks,
  `LoopAsync`). UE4SS refuses native calls from a coroutine the mod created;
  those calls fail with `invalid`.

### Error codes

| Code | Meaning |
|---|---|
| `not_found` | The file doesn't exist. |
| `denied` | The system refused to open the file. |
| `outside_root` | The path leaves the mod's folder (`..`, a link pointing elsewhere) or names a device. |
| `busy` | Another program holds the file open exclusively. |
| `io` | Reading failed, the mod's folder is unknown, or a limit was reached. |
| `invalid` | A bad argument, option or path, a folder instead of a file, a file over 64 MiB, or a released sound. |
| `unsupported` | Not a WAV file, or a WAV format the bridge doesn't decode. |
| `too_long` | Longer than the `max_seconds` given to `Load`. |

## Entry point and version

### `UE4SSLuaSoundBridge()`

Returns this Lua state's environment: the same table on every call.

### `UE4SSLuaSoundBridge.API_VERSION`, `sounds.API_VERSION`

`1`.

### `GetVersion()`

The product version, `"x.y.z"`. Also `sounds.GetVersion()`.

### `GetCapabilities()`

A new table on each call:

| Field | Value |
|---|---|
| `api` | `1` |
| `backend` | `"xaudio2"` |
| `max_voices` | `32`: plays that can sound at once |
| `max_file_bytes` | `67108864` (64 MiB) |
| `max_channels` | `2` |
| `formats` | `{"pcm8", "pcm16", "pcm24", "pcm32", "float32", "float64"}` |
| `pan`, `volume`, `pitch` | `{min =, max =}`: `-1..1`, `0..4`, `0.5..2` |
| `lowpass` | `{min = 20}` |
| `stop` | `true`: `Stop`, `sound:Stop` and `StopAll` exist |
| `target_ue4ss_commit` | `"97b7e501"` |

A mod that needs a later feature checks for its key here.

### `GetStats()`

A new table on each call, for diagnostics:

| Field | Meaning |
|---|---|
| `sounds` | Sounds loaded by every Lua state together |
| `sound_bytes` | Their decoded size in bytes |
| `playing` | Voices playing now |
| `started` | Plays started since the game started |
| `dropped` | Plays dropped: no output device, or the voice couldn't start |
| `stolen` | Plays that took over the oldest voice because all were busy |
| `device` | `"ready"`, `"none"` (no output device) or `"stopped"` (nothing loaded yet) |
| `device_changes` | Times playback restarted on a new default device |

## Loading

### `sounds.Load(path, options?)`

Reads and decodes one WAV file and returns a sound.

`path` is relative to the calling mod's own folder. Put sounds in a `sounds/`
subfolder by convention: `"sounds/cue.wav"`. `/` and `\` both separate
folders; `.` and `..` inside the folder are resolved. Refused:

- empty, absolute (`/x.wav`, `C:/x.wav`) and drive-relative paths, and `:`
  anywhere (`invalid`);
- wildcards (`* ? " < > |`), control characters, and folder or file names
  ending in a dot or a space (`invalid`);
- a `..` that climbs out of the mod's folder, device names such as `CON`, and a
  file whose real location (links followed) is outside the folder
  (`outside_root`);
- files over 64 MiB (`invalid`).

`options`:

| Option | Meaning |
|---|---|
| `max_seconds` | Refuse a sound longer than this with `too_long`. Default: no limit. |

The bridge itself sets no length limit; a mod that loads files chosen by its
user can set one to keep cues short.

Accepted WAV files: RIFF/WAVE with PCM at 8, 16, 24 or 32 bits, IEEE float at
32 or 64 bits, or `WAVE_FORMAT_EXTENSIBLE` carrying either; mono or stereo; 1
to 200 kHz. Everything else is `unsupported`, with the reason in the message.
Loading decodes the whole file at once, so do it at start-up or attach, not
while the cue is needed.

### Sound fields

Read-only:

| Field | Meaning |
|---|---|
| `path` | The normalized path, relative to the mod's folder |
| `frames` | Sample frames |
| `channels` | `1` or `2` |
| `rate` | Sample rate in Hz |
| `duration` | Seconds, `frames / rate` |
| `released` | `true` after `Release` |

### `sound:Release()`

Stops the sound's plays and frees it. Returns `true`, also when already
released. A sound that is no longer referenced is released by garbage
collection; when its Lua state stops or reloads, all its sounds are released.

## Playing

### `sound:Play(options?)`, `sounds.Play(sound, options?)`

Starts one play and returns its id, an integer. It returns at once: no file
access, no decoding, no wait on the audio engine. Plays of the same sound are
independent and may overlap.

`options`:

| Option | Default | Range | Meaning |
|---|---|---|---|
| `pan` | `0` | `-1..1` | Full left to full right |
| `volume` | `1` | `0..4` | Gain; `1` is as recorded. Above 1 may clip. |
| `pitch` | `1` | `0.5..2` | Playback rate; the length changes with it |
| `lowpass` | none | `>= 20` | Low-pass cutoff in Hz; `nil` or `false` for none |

Values outside their range are clamped. A non-number, NaN or unknown option is
`invalid`.

- **Pan.** A mono sound pans with a constant-power law: full left is the sound
  at full level in the left channel only; centre is about 0.71 in each, so the
  loudness stays even across the field. A stereo sound pans as balance: centre
  plays both channels as recorded and the far side fades out. On surround
  output only the front pair is used.
- **Low-pass.** The filter's top is a sixth of the output rate (8 kHz at
  48 kHz); a cutoff at or above it plays unfiltered.

When all 32 voices are busy, a new play takes over the oldest one. Without an
output device, plays are dropped; `Play` still returns an id and `GetStats()`
counts them under `dropped`.

### `sounds.Stop(play)`

Stops one play early. Returns `true`, also for a play that already ended. A
mod can only stop its own plays.

### `sound:Stop()`

Stops every play of this sound. Returns `true`.

### `sounds.StopAll()`

Stops every play of every sound this Lua state loaded. Returns `true`.

## Detection and fallback

```lua
local Bridge = rawget(_G, "UE4SSLuaSoundBridge")
local caps = Bridge and Bridge.GetCapabilities()
if not (caps and caps.api >= 1) then
    -- no sound bridge: use the game's own sounds
end
```

## Limits

| Limit | Value |
|---|---|
| Voices playing at once | 32 (shared by every mod) |
| File size | 64 MiB |
| Sounds per Lua state | 4096 |
| Channels | 2 |
| Sample rate | 1 to 200 kHz |
