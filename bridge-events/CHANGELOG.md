# Changelog

## Unreleased

### Added

- Load-time verifications: in-game checks of the bridge API shipped as
  `Scripts/main.lua` in the mod folder, with a ModCore Settings page whose
  **Enable load-time verifications** toggle turns them on (off by default).
  Results go to UE4SS.log.

## v1.0.12

### Changes

- Capture live objects that were never weakly referenced. Unreal assigns their
  object-item serial lazily, so `lifetimes.captureObject` and `lifetimes.weak`
  rejected freshly created widgets as not live. They now initialize the serial
  through `KismetSystemLibrary` and validate once more, as generated
  InputActions already did.

## v1.0.11

### Changes

- Probe the UObject layout by walking the start of the object array by index
  instead of looking up `Class` objects by name. On Dawnwalker the name lookup
  returned nothing at initialization, so the probe checked no objects and
  disabled lifetimes for the process.
- Report a probe that found no live objects separately from a layout mismatch.

## v1.0.10

### Changes

- Package the mod folder as `0_ModCore_UE4SSLuaEventBridge`, matching the
  numbered ModCore folders so the bridge sorts before the mods that use it.
  Rename an existing `_ModCore_UE4SSLuaEventBridge` folder and its `mods.txt`
  entry when upgrading.
- Remove the boot-time retirement of a legacy `UE4SSLuaEventBridge` folder.
- Probe the UObject layout once, at Unreal initialization, instead of retrying
  from the game thread. A failed probe is a layout mismatch that a retry would
  not change, and a successful retry registered object-array listeners
  mid-game, where the engine's loading threads may be using them.

## v1.0.9

### Changes

- Root helper mapping contexts at construction so a game that clears its
  mappings cannot get them collected, and reuse emptied contexts instead of
  rooting new ones.
- Hold the helper subsystem through a weak handle, so a freed subsystem is never
  dereferenced.
- Add `input:Refresh()` to re-apply a scope's contexts after the game clears its
  mappings.
- Write failed input callbacks, which disable their binding, to UE4SS.log.
- Generate the embedded Lua API chunks from the file's size, so the API can
  grow without build changes.
- Run every Python test module in CI.

## v1.0.8

### Changes

- Add `lifetimes.weak(object)` handles. `get()` returns the wrapper only while
  its native lifetime is valid, so a wrapper kept past garbage collection is
  never dereferenced; off the game thread it fails without forgetting the
  object. Advertised as `weak_handles`.
- Make loss reporting opt-in per session through `lifetimes.reportLosses()` or
  the first `takeLost()` call. Sessions that never drain losses can no longer
  overflow the loss queue and fault their validations. Advertised as
  `loss_opt_in`.
- Retry a failed UObject layout probe from the game thread, at most once per
  second and ten times in all, instead of disabling lifetimes for the process.
- Report why lifetimes are unavailable in
  `GetCapabilities().object_lifetimes_reason`.
- Index lifetime observations by object address and index, with an empty-set
  fast path, so object-array listeners stay cheap as observations grow.
- Reference-count lifetime observations. `lifetimes.release(token)` and weak
  handle `release()` or collection end an observation without a reported loss
  once no capture holds it.
- Fall back in the bootstrap to the next compatible implementation, newest
  first, when automatic selection's choice fails to load, fails its ABI check,
  or does not start. Exact pins still never fall back.
- Write bootstrap selection decisions and failures to UE4SS.log, resolving
  UE4SS's output at runtime without a link-time dependency.
- Write native diagnostics to UE4SS.log as well as the debugger: probe outcome,
  legacy-folder migration, Lua-stop cleanup, loop-start and delivery-fault
  handler failures.

API compatibility is version 6. Losses that occur before a session opts in are
not reported; call `reportLosses()` at startup to receive all of them.

## v1.0.7

- Consolidate API, lifecycle, diagnostics, and build documentation.
- Correct queue-statistics documentation and include current capability fields.
- Extract version-specific release notes from the changelog.
- Add project metadata.
- Add per-session one-shot loop-start callbacks delivered before the input queue
  rate gate.
- Add session-isolated, non-owning UObject lifetime tokens with native
  create/delete invalidation, bounded loss queues, and fail-closed overflow.
- Gate UObject lifetime support behind a one-time runtime layout probe; failed
  address/index/slot/serial validation disables the capability and listeners.
- Embed verifiable Windows file and product version metadata in the DLL and
  reject packaging when it differs from the source release version.
- Rename the packaged mod folder to `_ModCore_UE4SSLuaEventBridge`; existing
  `_UE4SSLuaEventBridge` installs must rename the folder and `mods.txt` entry.
- Split the native code into an independent bootstrap `main.dll` and a
  versioned bridge implementation selected by `dlls/main.json`.
- Add exact and automatic implementation selection with filename, version,
  private-ABI, UE4SS-build, and Unreal-target validation before activation.
- Keep the selected implementation and process-wide claim resident when native
  bindings prevent safe unload, blocking activation of a second version.

API compatibility is version 5. The lifecycle APIs are additive and advertised
through `GetCapabilities()`.

Changes and compatibility notes for each version.

## v1.0.1

This update installs the bridge under `_UE4SSLuaEventBridge` so UE4SS loads it
before ordinary consumer mods. The Lua and native API names remain unchanged.

On boot, the bridge detects a sibling legacy `UE4SSLuaEventBridge` folder. If
that folder has no `deprecated.txt`, the bridge removes its `enabled.txt` marker
and writes `deprecated.txt` containing `_UE4SSLuaEventBridge`.
Existing deprecation markers are left unchanged, so the migration is idempotent.

## v1.0.0

UE4SSLuaEventBridge provides a native Enhanced Input event boundary for UE4SS Lua mods. Unreal recognizes action phases and Tap/Hold triggers, while Lua receives copied event data outside the native input-dispatch stack.

### Reliability and lifecycle

- Add target-scoped delivery-fault handling for queue rejection, allocation failure, and Lua callback failure. Faulted targets stop delivery and notify the consumer once so held state can fail closed.
- Strengthen subscription teardown, stopped-session isolation, component-destruction handling, late-callback suppression, cleanup retry ownership, and repeated Lua reload behavior.
- Add native lifecycle tests, simulated engine-boundary integration tests, and AddressSanitizer coverage for concurrent shutdown and queue ownership.
- Retain the DLL conservatively if Unreal may still own a cloned native binding, preventing calls through an unloaded vtable.

### Bounded delivery

- Bound the producer event queue and diagnostic trace queue, expose dispatch statistics, and reject overload explicitly instead of allowing silent unbounded growth.
- Preserve FIFO order across budgeted dispatch passes. Defaults allow 256 live callback attempts or 2 ms of dispatch work per eligible pass.
- Keep native input delegates Lua-free and retain the configurable 20 Hz queue-dispatch schedule.

### Packaging

- Unify runtime, build, archive, and release metadata around one product version.
- Add package manifests and checksums for verifying release contents.
- Preserve installed settings and user-controlled enablement during deployment.

API compatibility remains version 4. This build targets Windows x64, Unreal Engine 5.5, and UE4SS commit `97b7e501`.

Helper scopes must still be closed explicitly on the Unreal game thread before Lua shutdown. An off-thread Lua stop deactivates native callbacks safely but cannot remove helper-created mapping contexts after the Lua state is destroyed. Primitive `BindAction` consumers that own their actions and contexts are unaffected by this helper limitation.

## v0.3.4-rc.3

- Add eight offline lifecycle integration cases, including 100 fresh Lua sessions, late callbacks, cleanup retries, partial removal failures, and simulated component/subsystem destruction.
- Preserve user-controlled enablement, including an absent enabled.txt on fresh installations.

API compatibility remains 4. The native input implementation is unchanged apart from the product version. The archive includes enabled.txt; existing installation enablement remains user-controlled.

The lifecycle tests exercise the production Lua helper against simulated engine/native boundaries. They do not establish Unreal garbage-collection safety, native shutdown race safety, or resolve off-thread generated-context cleanup. No new gameplay acceptance or performance claim is made.

## v0.3.4-rc.2

### Changes

- Scheduled event dispatch defaults to 20 passes per second, with configurable rate, event-count and time budgets. Pending events retain FIFO order across passes.
- Bounded event and trace queues with explicit overflow rejection reporting and on-demand `GetDispatchStats()` counters. Defaults allow 256 events or 2 ms per pass and 65,536 queued producer events; slow callbacks cannot be preempted.
- On-demand binding snapshots, stronger binding identity checks, and improved lifecycle regression coverage.
- Consistent product/version metadata, package contents, and checksums.

### Compatibility and validation

Windows x64, Unreal Engine 5.5 and UE4SS commit `97b7e501` only. API version remains 4.

Automated native, Lua, and packaging checks accompany this release. No host-game acceptance run was performed, and no performance advantage is established.

### Known limitation

When a Lua mod stops off the game thread, its callbacks are deactivated but helper-generated mapping contexts can remain installed. Close helper input scopes on the game thread before stopping/reloading the owning Lua mod where possible. Automatic off-thread context removal remains unresolved; this prerelease does not claim to fix it.

The ZIP contains only `UE4SSLuaEventBridge/enabled.txt` and `UE4SSLuaEventBridge/dlls/main.dll`. Its SHA-256 checksum and source-commit/file-hash manifest are separate release assets.

## v0.3.3

### Changes since v0.3.2

- Added descriptive primitive failure returns while preserving the existing
  successful return values and `UnbindAll` completion flag.
- Added opt-in per-`OpenInput` debug tracing for generated Enhanced Input
  phases, native delegate entry, queue decisions, dequeue, and Lua callback
  completion or failure.
- Added stable helper scope/binding IDs, atomic event sequences, OS thread IDs,
  game-thread state, and rejection reasons to trace lines.
- Added `sequence` to primitive callback payloads and `scope_id`/`binding_id`
  to helper callback payloads.
- Delayed mapping-context activation until native subscriptions are ready and
  made debug observer cleanup part of the logical helper binding lifecycle.
- Raised the capability API to 4 and extended `GetCapabilities()` with
  `detailed_errors` and `debug_tracing`.

### Compatibility

- UE4SS 3.0.1 Beta #0, commit `97b7e501`
- Unreal Engine 5.5
- Windows x64

Debug tracing is disabled by default. It covers the generated Enhanced Input
action through native-to-Lua delivery; raw physical-key detection remains the
responsibility of the consuming mod. Use `event_seq` to correlate every trace
stage produced for one native action event.

## v0.3.2

### Changes since v0.2.10

- Hardened ABI validation, callback dispatch, teardown, native-binding ownership,
  and DLL lifetime behavior.
- Added `Helpers.OpenInput`, direct key binding with Enhanced Input Tap/Hold
  triggers, developer documentation, and a complete F10 sample mod.
- Rejected helper mutations outside the Unreal game thread.
- Synchronized Lua session aliases and retained inert stopped-session storage
  while queued or in-flight work may still reference it.
- Removed the unused generic event-registry scaffold from the source tree.

### Compatibility

- UE4SS 3.0.1 Beta #0, commit `97b7e501`
- Unreal Engine 5.5
- Windows x64

The release contains the installable mod ZIP and its SHA-256 checksum.
