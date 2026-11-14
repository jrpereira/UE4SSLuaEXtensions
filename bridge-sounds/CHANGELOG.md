# Changelog

## v1.0.0

First release of UE4SSLuaSoundBridge, sound playback for UE4SS Lua mods
through a native bridge.

### Added

- `Load` reads and decodes a WAV file from the calling mod's own folder (PCM at
  8, 16, 24 or 32 bits, float at 32 or 64 bits, mono or stereo), with an
  optional `max_seconds` limit.
- `sound:Play` and `Play` start a play at once, with pan, volume, pitch and an
  optional low-pass filter per play; plays overlap freely.
- XAudio2 playback on its own audio thread with 32 voices; when all are busy
  the oldest play is taken over.
- Follows the default output device and restarts on a new one; plays are
  dropped quietly while no device exists.
- `Stop`, `sound:Stop`, `StopAll` and `sound:Release`; sounds are released when
  their Lua state stops or reloads, or when garbage collected.
- `GetVersion`, `GetCapabilities` and `GetStats`.
- Load-time verifications: in-game checks of this API, shipped as
  `Scripts/main.lua` with the shared `Scripts/lex_verify.lua` harness, and a
  ModCore Settings page whose **Enable load-time verifications** toggle turns
  them on (off by default). Results go to UE4SS.log.
