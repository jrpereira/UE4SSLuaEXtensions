# Changelog

## Unreleased

### Changes

- `onFocusRegained(callback)` runs a callback each time the game window comes
  back to the foreground, with the time it spent in the background. ModCore
  Settings uses it to reload config files edited while the game was in the
  background.
- Breaking: settings move from `settings.ini` `[Settings]` to `config.ini`
  `[UE4SSLuaEXtensions.Events]` (`EnableHooks` and `EnableVerifications`). `settings.ini`
  is no longer read, so switches return to their defaults until set again.

## v1.3.0

### Changes

- Breaking: `settings.ini` keeps both switches in one `[Settings]` section:
  `EnableHooks` replaces `[Hooks] Enabled` and `EnableVerifications` replaces
  `[Verifications] LoadTime`. Old keys are ignored, so hooks stay on and
  verifications stay off until switched on again.
- Breaking: `Hooks.Hook` is replaced by `Hooks.Install` and `Hooks.Request`,
  and `h:Unhook()`, `Hooks.Unhook` and `Hooks.UnhookAll` by `h:Remove()`,
  `Hooks.Remove` and `Hooks.RemoveAll`. There are no aliases. Arguments after
  the target are unchanged. A removed handle reports `state = "removed"`
  instead of `"unhooked"`.
- `Hooks.Install` attaches now or returns `nil, reason`. It never waits and
  does no background work. If a reload deletes the function, the hook becomes
  `orphaned` and stays so.
- `Hooks.Request` attaches now if it can, otherwise waits for the owner class
  to load, and attaches again after a reload deletes the function. Pending
  hooks no longer retry every second, which could cost about 33 ms each time
  in a loaded level; a request now retries only when an object of its class
  appears, then about 100 ms and 1 s later.
- Refusals that can never succeed (unknown parameter, unsupported parameter
  kind, a target that is not a `UFunction`, a property layout mismatch) return
  `nil, reason` from both calls. A pending request that meets one when its
  class loads turns `failed` and stops retrying, instead of retrying every
  second.
- A request that turns `failed` and an installed hook that turns `orphaned`
  are logged as a `WARN` line with the function path.
- `h:Remove()` is safe on any status: pending, active, orphaned, failed or
  already removed.
- Fix a race that could corrupt a mod's Lua state when input callbacks and
  function-hook callbacks for the same mod ran at the same time. Every bridge
  callback now runs on one thread.
- Input, delivery-fault, trace and `onLoopStart` callbacks run on the game
  thread once per frame, in the same frame as the input and after the frame's
  hook events. Callbacks may touch UObjects directly; `ExecuteInGameThread`
  wrappers keep working, one frame later. With UE4SS's engine-tick hook turned
  off every callback, hooks included, runs off the game thread as before.
- `onLoopStart` callbacks still wait until every Lua mod has loaded; they now
  arrive on the next frame after that.
- Breaking: `GetCapabilities().hooks_frame_delivery` is replaced by
  `frame_delivery`, which covers every callback. `GetDispatchStats()` adds
  `delivery = "tick"|"loop"`.
- `UE4SSLEB_MAX_DISPATCH_US` defaults to 500 microseconds instead of 2,000,
  since input is now delivered every frame. `UE4SSLEB_QUEUE_CHECKS_PER_SECOND`
  applies only with UE4SS's engine-tick hook turned off.
- Fix `BindAction` and `SubscribeEnhancedInput` refusing an InputAction asset
  the game had not used yet ("InputAction weak-reference initialization
  failed").
- `BindAction` and `SubscribeEnhancedInput` also accept a live `UInputAction`
  wrapper in place of its path, which skips the lookup.
- The bridge stays loaded until the game exits, so restarting UE4SS's C++ mods
  needs a game restart, as it already did once a hook had been used.

## v1.2.2

### Changes

- Fix a slowdown in 1.2.1 that made each `BindAction` about ten times slower
  (around 440 ms in a loaded level). Repeated `OpenInputComponent` and
  `BindAction` calls for objects already found are now fast. Error messages
  are those of 1.2.0, and paths are no longer required to start with `/`.

## v1.2.1

### Changes

- `OpenInputComponent` and `BindAction` are faster: each call took around
  45 ms in a loaded level. A path that is not a full object path (starting
  with `/`) is reported as not found.

## v1.2.0

### Changes

- Hook events carry the frame and time of the call itself. `call.frame` is the
  game's frame counter when the call was made and `call.clock` is
  `os.clock()` at that moment, so events can be compared against in-game
  stamps.

## v1.1.0

### Changes

- Add function hooks. `Hooks.Hook(target, options)` hooks one Blueprint or
  native `UFunction`, pre and post, by path or wrapper, with copies of the
  parameters. Filters by object, object name set and outer class. Events are
  delivered to Lua once per frame on the game thread, with per-hook limits.
  Observation only: no parameter edits, no blocking, and `delivery = "sync"`
  is refused.
- Hooks need no process-wide UE4SS hook, and are removed on unhook and when
  the Lua state stops.
- A hook on a class that is not loaded yet stays pending and attaches when the
  class loads; a hook whose function is deleted (level or class reload)
  returns to pending.
- Add capabilities `hooks`, `hooks_reason`, `hooks_bp`, `hooks_native`,
  `hooks_sync` and `hooks_frame_delivery`; API number 7.
- Add a ModCore Settings toggle, **Enable function hooks** (default on).
- Mods that moved their Blueprint hooks to this API can set
  `HookProcessInternal = 0` in `UE4SS-settings.ini`.
- Hooks stay off when the bridge can't confirm this game build's object
  layout at startup.
- Add function-hook checks to the load-time verifications.
- New environment variables `UE4SSLEB_MAX_QUEUED_HOOK_EVENTS`,
  `UE4SSLEB_MAX_HOOK_EVENTS_PER_PASS`, `UE4SSLEB_MAX_HOOK_DISPATCH_US` and
  `UE4SSLEB_HOOK_CHECKS_PER_SECOND` (only with UE4SS's engine-tick hook turned
  off).

### Limits

- A call from C++ to C++ that never goes through a `UFunction` is not seen.
- A native function called from Blueprint bytecode is reported without
  parameters (`params_available = false`) and, after the call, with its return
  value only.
- Arrays, maps, sets, delegates, interfaces, soft, weak and lazy pointers, and
  text parameters are delivered as `nil`.

## v1.0.13

### Changes

- Write non-ASCII text in UE4SS.log lines correctly. Mod and path names came
  out garbled.
- ModCore Settings now shows the bridge's name, category and version.
- Add load-time verifications: in-game checks of this API, turned on by the
  **Enable load-time verifications** toggle in ModCore Settings (off by
  default). Results go to UE4SS.log.

## v1.0.12

### Changes

- `lifetimes.captureObject` and `lifetimes.weak` accept freshly created
  objects, such as new widgets, which they rejected as not live.

## v1.0.11

### Changes

- Fix object lifetimes being unavailable on Dawnwalker.
- `object_lifetimes_reason` tells a startup check that found no live objects
  apart from a layout mismatch.

## v1.0.10

### Changes

- Package the mod folder as `0_ModCore_UE4SSLuaEventBridge`, matching the
  numbered ModCore folders so the bridge sorts before the mods that use it.
  Rename an existing `_ModCore_UE4SSLuaEventBridge` folder and its `mods.txt`
  entry when upgrading.
- The bridge no longer disables a legacy `UE4SSLuaEventBridge` folder at
  startup.
- Object lifetimes are checked once, at startup.

## v1.0.9

### Changes

- Helper mapping contexts survive a game that clears its mappings.
- Add `input:Refresh()` to re-apply a scope's contexts after the game clears its
  mappings.
- Write failed input callbacks, which disable their binding, to UE4SS.log.

## v1.0.8

### Changes

- Add `lifetimes.weak(object)` handles. `get()` returns the wrapper only while
  its object is alive, so a wrapper kept past garbage collection is never
  read; off the game thread it fails without forgetting the object.
  Advertised as `weak_handles`.
- Make loss reporting opt-in per session through `lifetimes.reportLosses()` or
  the first `takeLost()` call. Sessions that never drain losses can no longer
  overflow the loss queue and fault their validations. Advertised as
  `loss_opt_in`.
- Report why lifetimes are unavailable in
  `GetCapabilities().object_lifetimes_reason`.
- `lifetimes.release(token)` and weak handle `release()` or collection end an
  observation without a reported loss once no capture holds it.
- Write the bridge's startup outcome, Lua-stop cleanup, and loop-start and
  delivery-fault handler failures to UE4SS.log.

API compatibility is version 6. Losses that occur before a session opts in are
not reported; call `reportLosses()` at startup to receive all of them.

## v1.0.7

- Add per-session one-shot loop-start callbacks (`onLoopStart`).
- Add session-isolated UObject lifetime tokens that become invalid when their
  object is destroyed, with bounded loss queues.
- Object lifetimes are available only when the bridge confirms this game
  build's object layout at startup.
- Rename the packaged mod folder to `_ModCore_UE4SSLuaEventBridge`; existing
  `_UE4SSLuaEventBridge` installs must rename the folder and `mods.txt` entry.
- Only one version of the bridge can be active at a time.

API compatibility is version 5. The lifecycle APIs are additive and advertised
through `GetCapabilities()`.

## v1.0.1

The bridge installs under `_UE4SSLuaEventBridge` so UE4SS loads it before the
mods that use it. The Lua API names are unchanged. A legacy
`UE4SSLuaEventBridge` folder next to it is disabled at startup.

## v1.0.0

UE4SSLuaEventBridge delivers Enhanced Input events to UE4SS Lua mods. Unreal
recognizes action phases and Tap/Hold triggers; Lua receives copies of the
event data after Unreal's input handling.

### Reliability and lifecycle

- Add target-scoped delivery-fault handling for queue rejection, allocation
  failure and Lua callback failure. Faulted targets stop delivery and notify
  the consumer once so held state can be reset.
- More reliable cleanup when subscriptions end, sessions stop, components are
  destroyed and Lua mods reload.

### Bounded delivery

- Bound the event and trace queues, expose dispatch statistics, and reject
  overload explicitly instead of growing without limit.
- Events keep their order across dispatch passes. Defaults allow 256 callbacks
  or 2 ms of dispatch work per pass, 20 passes per second.

### Packaging

- Release ZIPs come with checksums.
- Installed settings and enablement are kept on update.

API compatibility remains version 4. This build targets Windows x64, Unreal
Engine 5.5, and UE4SS commit `97b7e501`.

Helper scopes must still be closed explicitly on the Unreal game thread before
Lua shutdown. An off-thread Lua stop deactivates callbacks but cannot remove
helper-created mapping contexts. Primitive `BindAction` consumers that own
their actions and contexts are unaffected.

## v0.3.4-rc.3

- Keep the user's enablement, including an absent enabled.txt on fresh
  installations.

API compatibility remains 4.

## v0.3.4-rc.2

### Changes

- Event dispatch defaults to 20 passes per second, with configurable rate,
  event-count and time budgets. Pending events keep their order across passes.
- Bounded event and trace queues with explicit overflow rejection and
  on-demand `GetDispatchStats()` counters. Defaults allow 256 events or 2 ms
  per pass and 65,536 queued events; slow callbacks cannot be preempted.
- On-demand binding snapshots.

### Compatibility

Windows x64, Unreal Engine 5.5 and UE4SS commit `97b7e501` only. API version
remains 4.

### Known limitation

When a Lua mod stops off the game thread, its callbacks are deactivated but
helper-generated mapping contexts can remain installed. Close helper input
scopes on the game thread before stopping the owning Lua mod.

## v0.3.3

### Changes

- Primitive calls return descriptive failure reasons, with the existing
  successful return values and `UnbindAll` completion flag kept.
- Opt-in per-`OpenInput` debug tracing of each input event from Unreal to the
  Lua callback.
- Trace lines carry stable helper scope and binding IDs, event sequences, OS
  thread IDs, game-thread state and rejection reasons.
- Added `sequence` to primitive callback payloads and `scope_id`/`binding_id`
  to helper callback payloads.
- Raised the capability API to 4 and extended `GetCapabilities()` with
  `detailed_errors` and `debug_tracing`.

### Compatibility

- UE4SS 3.0.1 Beta #0, commit `97b7e501`
- Unreal Engine 5.5
- Windows x64

Debug tracing is disabled by default. It covers the generated Enhanced Input
action up to the Lua callback; detecting raw physical keys remains the job of
the consuming mod. Use `event_seq` to correlate every trace line for one
action event.

## v0.3.2

### Changes

- Added `Helpers.OpenInput`, direct key binding with Enhanced Input Tap/Hold
  triggers, developer documentation, and a complete F10 sample mod.
- Helper changes outside the Unreal game thread are refused.
- More reliable callback delivery and cleanup.

### Compatibility

- UE4SS 3.0.1 Beta #0, commit `97b7e501`
- Unreal Engine 5.5
- Windows x64
