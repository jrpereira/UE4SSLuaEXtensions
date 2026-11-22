# Changelog

## Unreleased

### Changes

- Breaking: settings move from `settings.ini` `[Settings]` to `config.ini`
  `[UE4SSLuaEXtensions.Sounds]` (`EnableVerifications`). `settings.ini`
  is no longer read, so switches return to their defaults until set again.

## v1.1.0

### Changes

- `Load` accepts any path: an absolute path with a drive letter loads as given,
  and a relative path still resolves against the calling mod's folder, with
  `..` allowed to climb out of it. Code running in another mod's Lua state,
  such as a ModCore Templates template, can now load its own sounds.
- No folder or link containment: the `outside_root` error code is gone.
  Refusals remain only for paths that could stall the game or be misread:
  network and device paths, device names, streams, folders and files over
  64 MiB, all as `invalid`.
- `GetCapabilities().paths` is `"any"`, so a mod can detect the change.
- A sound's `path` is its normalized path as given, relative or absolute.

## v1.0.0

### Changes

- First release: sound playback for UE4SS Lua mods through a native bridge.
- `Load` reads and decodes a WAV file from the calling mod's own folder (PCM at
  8, 16, 24 or 32 bits, float at 32 or 64 bits, mono or stereo), with an
  optional `max_seconds` limit.
- `sound:Play` and `Play` start a play at once, with pan, volume, pitch and an
  optional low-pass filter per play; plays overlap freely.
- 32 voices; when all are busy the oldest play is taken over.
- Follows the default output device and restarts on a new one; plays are
  dropped quietly while no device exists.
- `Stop`, `sound:Stop`, `StopAll` and `sound:Release`; sounds are released when
  their Lua state stops, or when garbage collected.
- `GetVersion`, `GetCapabilities` and `GetStats`.
- Load-time verifications: in-game checks of this API, turned on by the
  **Enable load-time verifications** toggle on its ModCore Settings page (off
  by default). Results go to UE4SS.log.
