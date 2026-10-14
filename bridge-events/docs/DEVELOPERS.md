# Developer guide

- [Requirements and execution model](#requirements-and-execution-model)
- [Version and capabilities](#version-and-capabilities)
- [Library primitives](#library-primitives)
- [Target delivery faults](#opt-in-target-delivery-faults)
- [Input helpers](#input-helpers)
- [Complete Tap/Hold example](#complete-taphold-example)
- [Ownership, errors, and lifecycle](#ownership-errors-and-lifecycle)
- [Inspection and debugging](#inspection-and-debugging)
- [Dispatch limits and tuning](#dispatch-limits-and-tuning)
- [Function hooks](#function-hooks)

UE4SSLuaEventBridge exposes two API layers:

| Layer | Use it when | Ownership |
|---|---|---|
| Library primitives | The game or another mod already owns the Input Actions and mappings | The caller owns Unreal assets and mapping contexts |
| Input helpers | A mod wants to bind a physical key without shipping Input Action assets | The returned input scope owns transient actions, triggers, mappings, and native subscriptions |

Both layers are game-agnostic. The caller supplies exact live object paths; the
bridge does not discover a player controller, pawn, local player, input
component, or subsystem.

## Requirements and execution model

The current build targets:

- UE4SS 3.0.1 Beta #0 at commit `97b7e501`;
- Unreal Engine 5.5; and
- Windows x64.

Operations that resolve or mutate Unreal objects must run on the Unreal game
thread. Initialize and shut down inside `ExecuteInGameThread`:

```lua
ExecuteInGameThread(function()
    -- Open, bind, unbind, and close here.
end)
```

Every helper entry point checks the native UE4SS game-thread state before it
constructs, configures, registers, or removes an Unreal object. An off-thread
call returns an error without partially changing the input scope.

Native input execution never enters Lua. It copies event data into a queue,
and the bridge dispatches Lua callbacks later from its UE4SS update callback.
If a callback needs to change bindings, it must schedule that work with
`ExecuteInGameThread`.

The bridge installs no `ProcessEvent` hook. Helpers do invoke ordinary
reflected Enhanced Input functions through UE4SS to create and register their
private objects.

## Version and capabilities

```lua
local version = UE4SSLuaEventBridge.GetVersion()
local capabilities = UE4SSLuaEventBridge.GetCapabilities()
```

The current API reports:

```lua
{
    api = 7,
    enhanced_input = true,
    explicit_target = true,
    helpers = true,
    dynamic_input = true,
    trigger_tap = true,
    trigger_hold = true,
    detailed_errors = true,
    debug_tracing = true,
    binding_snapshot = true,
    target_delivery_faults = true,
    loop_start = true,
    object_lifetimes = true,
    object_lifetimes_reason = nil, -- a string when object_lifetimes is false
    weak_handles = true,
    loss_opt_in = true,
    target_ue4ss_commit = "97b7e501",
}
```

Gate optional features on `GetCapabilities()`. `GetVersion()` identifies the
product release; `API_VERSION` identifies the API contract. A version number
is useful metadata, but a poor crystal ball.

`object_lifetimes` is `true` only after a runtime ABI probe validates the
configured UObject internal-index and object-item pointer/serial offsets against
multiple live objects. The probe runs once, at Unreal initialization, and walks
the start of the object array by index instead of looking classes up by name,
which can come back empty at that point; a failure is a layout mismatch that a
later attempt would not change. A failed probe leaves the capability false,
skips listener registration, and makes every lifetime operation fail closed for
the process. While lifetimes are unavailable, `object_lifetimes_reason` says
why: the probe has not run, found no live objects, how many objects it
verified, or that the object array shut down. The probe outcome is also written to UE4SS.log.

## Loop start and object lifetimes

`onLoopStart(callback)` registers a one-shot callback for the owning Lua
session. It runs from the first native `on_update` after that session starts,
before the Enhanced Input queue's rate check. It is intended for work that must
wait until the initial Lua-module loading batch has returned. The callback runs
on UE4SS's update thread; schedule Unreal object work with
`ExecuteInGameThread`. The returned function cancels a callback that has not yet
run and returns whether cancellation succeeded. Registering after delivery is
an error. Callback failures are logged and do not prevent other sessions or
callbacks from running.

```lua
local unsubscribe = UE4SSLuaEventBridge.onLoopStart(function()
    ExecuteInGameThread(function()
        -- Resolve initial live objects here.
    end)
end)
```

The lifetime API assigns a non-owning decimal-string token to a live UObject
address:

```lua
local token, err = UE4SSLuaEventBridge.lifetimes.captureObject(object)
local trustedToken, trustedError =
    UE4SSLuaEventBridge.lifetimes.captureAddress(address)
local stillLive = UE4SSLuaEventBridge.lifetimes.valid(address, token)
local released = UE4SSLuaEventBridge.lifetimes.release(token)
local reporting = UE4SSLuaEventBridge.lifetimes.reportLosses()
local lostToken, lossError = UE4SSLuaEventBridge.lifetimes.takeLost()
```

Use `captureObject` for ordinary UE4SS UObject wrappers. It checks `IsValid()`
and obtains `GetAddress()` synchronously before native identity validation.
Unreal assigns object-item serials lazily, on first weak reference, so a live
object that was never weakly referenced has none; when native validation rejects
the object, `captureObject` and `weak` initialize its serial through
`KismetSystemLibrary:Conv_ObjectToSoftObjectReference` and validate once more.
`captureAddress` has no wrapper to initialize and does not retry. Use
`captureAddress` only for a trusted address obtained from a freshly resolved live
object; a retained numeric address cannot prove its own provenance. Call all
lifetime operations on the Unreal game thread. A token
does not root the object. Native object-array listeners invalidate observations
without entering Lua. Loss reporting is opt-in per session: after
`reportLosses()` or the first `takeLost()` call, each invalidated token is
queued, and `takeLost` drains one at a time. Losses before opting in are not
queued, so call `reportLosses()` at startup to receive all of them. Capturing a
live object again returns its existing token and adds a reference; `release`
drops one reference, and the last one ends the observation without a loss. Tokens and loss queues are isolated between Lua sessions and
are cleared when their session stops. Address or object-array index reuse gets a
new token because identity also includes the current object-item serial number.

Each session permits 4,096 live observations and 4,096 queued losses. Exceeding
the observation limit rejects a new capture; observations end when their object
dies. A session that has not opted in queues no losses and cannot overflow. For
an opted-in session, loss-queue overflow faults the session's lifetime service: all subsequent validations fail, capture is rejected,
and `takeLost` returns an error instructing the consumer to discard cached object
state and reload its Lua session. This fail-closed behavior avoids treating an
incomplete invalidation stream as trustworthy.

### Weak handles

Keep a weak handle instead of a UE4SS wrapper whenever an object must outlive
the current call. UE4SS `IsValid()` reads the object, so calling it on a wrapper
kept past garbage collection, as when a save loads from a running game, reads
freed memory. A weak handle checks the native lifetime first and never
dereferences a dead object.

```lua
local handle, err = UE4SSLuaEventBridge.lifetimes.weak(object)
local live = handle:get()             -- the wrapper while alive, otherwise nil
local token, address = handle:identity()
handle:release()                      -- forget the wrapper early
```

Pass `weak()` a fresh wrapper: a hook argument, a `FindAllOf` or
`StaticFindObject` result, or a creation notification. `weak()` calls
`IsValid()` once to prove the wrapper is live, so a wrapper that was already
cached across a garbage collection can crash there just as anywhere else.

Create and read handles on the game thread. Off the game thread `get()` returns
nil and an error but keeps the wrapper for a later game-thread read. Once the
object dies, `get()` returns nil for good, even if its address is reused.
`identity()` keeps returning the captured token and address, so handles can key
caches. Each handle uses one of the session's observations until its object
dies or every handle and capture for it is released; handles for the same
object share one observation. `release()`, or collecting the handle, returns
its share.

## Library primitives

Every Lua mod receives an isolated `UE4SSLuaEventBridge` table after its Lua state starts. Target and
subscription handles belong to the Lua session that created them.

The `UE4SSLuaEventBridge_*` globals are the native entry points behind this
table. They are internal: call the table's methods instead. Session isolation is
cooperative, since the native side trusts the session ID the table passes.

### `OpenInputComponent(componentPath)`

```lua
local target, err = UE4SSLuaEventBridge.OpenInputComponent(componentPath)
```

`componentPath` is the exact live object path of a
`UEnhancedInputComponent`, not a class or content asset path. The component is
resolved only during this call and retained as a native weak reference.

Returns a numeric target handle, or `nil, errorMessage`.

### `BindAction(target, actionPath, eventName, callback)`

```lua
local handle, err = UE4SSLuaEventBridge.BindAction(
    target,
    "/Game/Input/IA_Example.IA_Example",
    "Triggered",
    function(event)
        print(event.action, event.phase)
    end)
```

This binds an existing `UInputAction`. It does not create an action, trigger,
key mapping, or mapping context.

Supported event names are `Started`, `Ongoing`, `Triggered`, `Canceled`, and
`Completed`. The callback receives:

```lua
{
    subscription = 1,
    source_type = "enhanced_input",
    action = "/Game/Input/IA_Example.IA_Example",
    phase = "Triggered",
    sequence = 27,
    elapsed_processed = 0.501,
    elapsed_triggered = 0.0,
    value = { x = 1.0, y = 0.0, z = 0.0, type = 0 },
}
```

`value.type` uses Unreal's Enhanced Input values: Boolean `0`, Axis1D `1`,
Axis2D `2`, and Axis3D `3`. Both elapsed fields come from Enhanced Input; the
bridge does not measure time itself.

### `Unbind(handle)`

```lua
local removed, err = UE4SSLuaEventBridge.Unbind(handle)
```

Deactivates the callback and removes its native action-event binding. It
returns `true` when the calling session owns the handle and removal completes.
On failure it returns `false, errorMessage`.

### `CloseInputComponent(target)`

```lua
local closed, err = UE4SSLuaEventBridge.CloseInputComponent(target)
```

Unbinds subscriptions attached through the target and releases the target
handle. It never destroys the Unreal component.
On failure it returns `false, errorMessage`.

### `UnbindAll()`

```lua
local count, completed, err = UE4SSLuaEventBridge.UnbindAll()
```

Attempts to close all helper scopes in the current Lua session, then removes
any remaining native subscriptions. It returns the number of native
subscriptions removed. `completed` is `true` only when helper and native
cleanup both complete. On failure the third result contains the combined
cleanup errors. Prefer each helper scope's `Close()` method when its lifetime
is known.

The compatibility aliases `SubscribeEnhancedInput`, `Unsubscribe`, and
`UnsubscribeAll` remain available.

## Opt-in target delivery faults

API 4 exposes `GetCapabilities().target_delivery_faults == true` when the
following additive methods are available. Older runtimes must be capability-gated.
Use a dedicated primitive target for inputs that must stop after delivery loss.

```lua
local target = assert(bridge.OpenInputComponent(componentPath))
local ok, err = bridge.SetTargetDeliveryFaultHandler(target, function(fault)
    -- Runs on the UE4SS update thread, not the native input producer.
    -- Clear consumer held/display state and invalidate its local lifecycle epoch.
    resetInputState(fault.reason)
    -- Schedule owned mapping/context cleanup on the game thread.
end)
assert(ok, err)
-- Bind all phases, then activate the caller-owned mapping context.
```

`SetTargetDeliveryFaultHandler(target, callback)` returns `true`, or `false,
errorMessage`. It requires the game thread and a target owned by the current
Lua session. Register once, before the target has ever successfully bound an
action. There is no handler replacement, opt-out or automatic rearm.

The first queue-capacity rejection, queue allocation/production exception, or
Lua action-callback error permanently invalidates the opted-in target. Reasons
are `queue_capacity_exceeded`, `queue_exception`, and `callback_error`.
All its subscriptions stop producing/delivering events, including older queued
`Started` events. Non-opted-in targets retain their previous behavior; unrelated
targets are not invalidated. The first reason is retained and notifications
coalesce to one attempt per target.

The handler receives `{target = targetHandle, reason = reason}` independently
of event/trace queue capacity. The bridge checks pending fault notifications
before ordinary dispatch, including when no new event arrives, and between
queued deliveries. Closing the target or stopping its session cancels pending
notification. A throwing handler is reported to the native debugger and is not
retried; the target stays invalid. Notification can be delayed by the normal
20 Hz dispatch schedule, host scheduling or an already-running Lua callback.
Callbacks already executing are not preempted.

`IsTargetDeliveryValid(target)` returns `true` for an opted-in target with no
known delivery fault. Otherwise it returns `false, reason`; unknown, closed,
stopped, cross-session and non-opted-in targets also return false. This query
is thread-safe and does not access Unreal objects. It checks delivery state,
not component validity, key actuation, application focus or mapping activation.
Call it on demand immediately before a scheduled side effect, alongside the
consumer's captured lifecycle epoch and gameplay gates:

```lua
local capturedTarget, capturedEpoch = target, inputEpoch
ExecuteInGameThread(function()
    if inputEpoch ~= capturedEpoch then return end
    if not bridge.IsTargetDeliveryValid(capturedTarget) then return end
    -- Perform an otherwise validated effect here. This is not a physical-key check.
end)
```

Target IDs are never reused within the bridge lifetime. For recovery, explicitly
close the old target, reset consumer state, create a new target, register its
handler and recreate subscriptions before activating input. The caller retains
ownership of its Input Actions and mapping contexts and must clean them up.
This policy does not resolve off-thread context cleanup, focus loss, missing
engine phase events, or simultaneous physical-release/side-effect ordering.
It adds no timer or consumer polling requirement. Healthy-path notification
checks use an atomic read; only fault processing scans target records.

## Input helpers

Helpers create private transient Enhanced Input objects so the mod can bind an
Unreal key name directly:

```lua
local Helpers = UE4SSLuaEventBridge.Helpers
local Trigger = Helpers.Trigger
```

The supported trigger constants, `Trigger.Tap` and `Trigger.Hold`, are opaque
and read-only. Unreal evaluates both triggers.

### `Helpers.OpenInput(options)`

```lua
local input, err = Helpers.OpenInput({
    component_path = COMPONENT_PATH,
    subsystem_path = ENHANCED_INPUT_SUBSYSTEM_PATH,
    mapping_priority = 0,
    debug = false,
    debug_label = "MyMod",
})
```

Required options:

- `component_path`: exact live path of the `UEnhancedInputComponent` that
  receives generated action bindings.
- `subsystem_path`: exact live path of the
  `UEnhancedInputLocalPlayerSubsystem` that receives private mapping contexts.

`mapping_priority` is an optional signed 32-bit context priority and defaults
to `0`. `debug` is an optional boolean and defaults to `false`.
`debug_label` is an optional 1..64 byte label and defaults to `"input"`.
`OpenInput` performs no discovery. It resolves both supplied objects and
returns an input scope, or `nil, errorMessage`. It does not install a mapping
until `Bind` succeeds.

### `input:Bind(key, trigger, callback [, options])`

```lua
local handle, err = input:Bind("F10", Trigger.Tap, function(event)
    print(event.key .. " tapped\n")
end, {
    threshold_seconds = 0.2,
})
```

Each successful call creates and owns one transient Input Mapping Context,
Boolean Input Action, Tap or Hold trigger, key mapping, and native `Triggered`
subscription. A private context per binding makes removal independent and
avoids mutating a context that is already active.

Private contexts are created rooted (`RF_MarkAsRootSet`), so a game that clears
its mappings cannot get them garbage-collected while a scope still uses them.
Rooted contexts are never released: unbinding empties a context and keeps it
for the next `Bind`, so the number of rooted contexts stays at the peak number
of live bindings. Generated actions and triggers are not rooted. The subsystem
is held through a [weak handle](#weak-handles) when lifetimes are available.

Generated actions default to `bConsumeInput = false`, so Tap and Hold actions
on the same key can both be evaluated and the helper does not intentionally
consume the game's mapping.

Common options:

| Option | Type | Default | Effect |
|---|---|---:|---|
| `consume_input` | boolean | `false` | Sets the generated Input Action's consume-input flag |
| `trigger_when_paused` | boolean | Unreal default | Sets the generated Input Action's paused-trigger flag |

Tap options:

| Option | Type | Default | Effect |
|---|---|---:|---|
| `threshold_seconds` | non-negative number | Unreal default | Sets `TapReleaseTimeThreshold` |

Hold options:

| Option | Type | Default | Effect |
|---|---|---:|---|
| `threshold_seconds` | non-negative number | Unreal default | Sets `HoldTimeThreshold` |
| `one_shot` | boolean | Unreal default | Sets `bIsOneShot` |

```lua
local handle, err = input:Bind("F10", Trigger.Hold, function(event)
    print(string.format("held for %.0f ms\n", event.elapsed_processed * 1000))
end, {
    threshold_seconds = 0.5,
    one_shot = true,
})
```

With `one_shot = false`, Enhanced Input may emit `Triggered` repeatedly while
the key remains actuated. The helper does not suppress or synthesize events.

The helper callback contains the primitive event payload plus:

```lua
{
    key = "F10",
    trigger = Trigger.Hold,
    scope_id = 1,
    binding_id = 2,
}
```

The `action` field is the transient generated action path. It is an
implementation detail and is not stable across sessions; use `key` and
`trigger` instead.

### `input:Unbind(handle)`

```lua
local removed, err = input:Unbind(handle)
```

Removes the native subscription and its private mapping context, then releases
the action and trigger references. A handle from another scope is rejected.
Do not pass helper-owned handles to the primitive `Unbind`; doing so bypasses
the helper's mapping-context ownership.

### `input:Close()`

```lua
local closed, err = input:Close()
```

Closes all bindings owned by the scope, removes their private mapping
contexts, and closes the component target. `Close` is idempotent. If a cleanup
step fails, ownership is retained so a later game-thread call can retry it.
`Bind` after a successful close returns `nil, "input scope is closed"`.

Call `Close()` on the game thread before reload or shutdown. See
[session cleanup](#session-cleanup-and-rebinding) for automatic cleanup limits.

### `input:Refresh()`

```lua
local refreshed, err = input:Refresh()
```

Re-applies the scope's private mapping contexts. Games can remove every mapping
context, for example through `ClearAllMappings` during a map load; the helper's
bindings then stop firing until their contexts are applied again. Call
`Refresh()` on the game thread after such a clear, such as from a hook on the
game's mapping rebuild. Re-applying a context that is still applied is harmless.
It returns `false, err` when the subsystem is no longer live.

## Complete Tap/Hold example

The [sample entry point](../examples/EnhancedInputTapHold/Scripts/main.lua)
binds F10 to independent Tap and Hold actions, with failure rollback and explicit
shutdown. Copy the [sample directory](../examples/EnhancedInputTapHold) to
`Mods/EnhancedInputTapHold`, replace its two live object paths, and enable the
sample and bridge.

Set `DEBUG_BRIDGE = true` to trace action phases and delivery. Call
`EnhancedInputTapHold_Shutdown()` from your reload or shutdown path while the
Lua state is still alive.

## Ownership, errors, and lifecycle

Primitive return values are documented with each method. Helper option-contract
violations raise Lua errors; operational failures return `nil, errorMessage` or
`false, errorMessage`.

Helper creation and binding are transactional. A failure removes any newly
installed context. If removal fails, the scope retains a cleanup record for
`Close()` to retry; earlier successful bindings remain active. Generated objects
stay referenced while their contexts or bindings are owned by the scope.

`UnbindAll()` disables callback delivery before attempting game-thread cleanup.
If helper cleanup fails, native removal preserves targets for retry and clears
obsolete subscription handles when removal succeeds. Retry `Close()` or
`UnbindAll()` to finish cleanup. Failed attempts do not resume delivery.

A callback that raises an error disables its binding. The bridge writes the
action, phase and error to UE4SS.log, and also reports a delivery fault when a
handler is registered.

Deactivation rejects queued callbacks but cannot preempt a callback already
executing. Consumers that schedule additional work must also check their own
lifecycle state before that work runs. See [target delivery faults](#opt-in-target-delivery-faults)
for optional native delivery-health checks.

### Session cleanup and rebinding

Close scopes explicitly on the game thread before stopping or reloading Lua.
A game-thread Lua stop also attempts helper cleanup. An off-thread stop disables
callbacks and defers native binding removal until the next game-thread bridge
operation, but cannot remove helper mapping contexts after the Lua state dies.
Those contexts stay applied and rooted. Their actions do not consume input
unless `consume_input` was set, so they do not block the game's own mappings.
Cleanup is a lifecycle operation, not a farewell wish.

Callback closures live in Lua-owned tables and are released on bind failure,
unbind, target close, unbind-all, or callback failure. One dispatcher registry
reference remains until the Lua state closes. Session and child-state alias
lookup is synchronized; stopped session storage remains inert until bridge
destruction so in-flight references cannot outlive it.

If Unreal still owns a binding or clone when the bridge unloads, the DLL retains
a process-lifetime module reference to keep its vtable callable. Explicitly
removing owned bindings avoids this retention when no engine-owned clones remain.

When the game replaces an input component, close the old target, resolve the new
component path, and bind again. Discovery, gameplay gates, and rebinding remain
the consumer's responsibility.

## Inspection and debugging

### Load-time verifications

The bridge's mod folder carries `Scripts/main.lua`, a set of in-game checks
of this API (source: [`lua/main_events.lua`](../lua/main_events.lua)), and
`Scripts/lex_verify.lua`, the harness all extensions share
([`shared/lua/lex_verify.lua`](../../shared/lua/lex_verify.lua)). They run
only when **Enable load-time verifications** is on in the bridge's ModCore
Settings page (`settings.ini`: `[Verifications] LoadTime=1`). It is off by
default and takes effect on the next launch. Results are `PASS`, `FAIL`,
`SKIP` and `NOTE` lines tagged `[UE4SSLuaEventBridge verify]` in UE4SS.log,
with a `SUMMARY` line per stage:

- `load`: the API surface and argument checks, while the mod loads.
- `engine`: loop start, unknown handles and object lifetimes, on the game
  thread after the first update.
- `player`: input targets, bindings, snapshots and helper scopes, once a
  player controller with a pawn exists. For 20 seconds after it starts, a tap
  or hold of NumPad 9 also checks delivery; without one those checks are
  skipped, not failed. Every binding is removed afterwards.

### Optional debug tracing

Enable tracing on one helper scope when an Enhanced Input callback appears to
be missing:

```lua
local input, err = Helpers.OpenInput({
    component_path = COMPONENT_PATH,
    subsystem_path = ENHANCED_INPUT_SUBSYSTEM_PATH,
    mapping_priority = 10000,
    debug = true,
    debug_label = "MyMod",
})
```

Debug mode installs internal `Started`, `Completed`, and `Canceled` observers
in addition to the normal `Triggered` subscription. These observers only
report Enhanced Input phases; they never invoke the developer callback or
classify Tap versus Hold. `Ongoing` is intentionally omitted because it can
produce a trace line every input-processing frame.

Each trace is written through UE4SS's normal log output in this form:

```text
[UE4SSLuaEventBridge][trace] label="MyMod" stage="event_queued" scope=1 binding=2 key="F10" trigger="Hold" phase="Triggered" event_seq=17 thread_id=1248 game_thread=true reason="-"
```

Scope and binding IDs are monotonic and stable within one Lua session. Event
sequences are assigned atomically when a native delegate is entered. Lifecycle
records that do not belong to an input event use `event_seq=0`. Empty fields
are rendered as `"-"`.

| Stage | Meaning |
|---|---|
| `scope_opened` | `OpenInput` completed |
| `binding_created` | The primary native `Triggered` binding was attached |
| `enhanced_input_event` | Unreal emitted one of the observed action phases |
| `native_delegate_entered` | The bridge's native action delegate began |
| `event_queued` | A copied event was accepted for later Lua delivery |
| `event_rejected` | Delivery was refused; inspect `reason` |
| `event_dequeued` | The UE4SS update callback removed the event from the queue |
| `lua_callback_started` | The bridge began its protected Lua dispatcher call |
| `lua_callback_completed` | The Lua dispatcher returned successfully |
| `lua_callback_failed` | The Lua dispatcher raised an error |
| `callback_skipped` | A dequeued event no longer had an active binding or session |
| `binding_removed` | The primary native binding was detached |
| `scope_closed` | Helper mappings, subscriptions, and target were closed |

Possible reasons include `binding_inactive`, `dispatch_stopped`,
`queue_exception`, `lua_session_inactive`, and `unknown_lua_exception`.
Recognized Lua exceptions include their error text in `reason`. Every line also
records the current OS thread ID and whether UE4SS reports that thread as the
Unreal game thread.

Tracing begins at the generated Enhanced Input action. It does not detect or
report raw physical-key input, controller routing, gameplay state, or UI state.
Those belong to the consuming mod or a separate diagnostic layer.


## Binding snapshot API

Binding inspection is an additive API 4 capability, `binding_snapshot=true`.
Check `GetCapabilities()` before calling it. Compatibility is pinned to
UE4SS 97b7e501, Unreal 5.5 and Windows x64/MSVC; this API does not broaden it.

### Invocation

```lua
if UE4SSLuaEventBridge.GetCapabilities().binding_snapshot then
    ExecuteInGameThread(function()
        local text, err = input:InspectBindings()
        print(text or ("binding snapshot failed: " .. tostring(err)))
    end)
end
```

input is the caller's existing Helpers.OpenInput scope. Primitive equivalent: UE4SSLuaEventBridge.InspectInputComponent(targetHandle).
Both return one multiline string on successful inspection, or nil,error for invalid target/session, stopped backend/session, off-thread call, closed scope or failure to produce the snapshot. Invalid component/array/binding states are data in a successful snapshot. No debug=true requirement. No automatic logging, cleanup/reaping, object creation, discovery, polling, rebinding or callback execution.

### Schema 1

Header: target; game_thread=1; component_valid; component_index/serial; array_readable; array_invariants; array_size/capacity; entries_readable.
Per subscription: subscription; scope; binding (same IDs as debug traces); active; member; metadata_readable; expected_action_index/serial/valid; expected_trigger; expected_handle.
When metadata is readable: action_index/serial; action_valid; action_matches; trigger; trigger_matches; handle; handle_matches.
Footer: snapshot_end reported=N truncated=0/1. Numeric boolean fields are 0/1. action_valid is unknown on identity mismatch: an untrusted copied action index is never passed to object resolution.
Trigger enums: Triggered=1, Started=2, Ongoing=4, Canceled=8, Completed=16. No event_seq because this is not an input event.
Maximum 256 owned subscription rows (roughly <128 KiB text); original native array capacity must be below 65536, with valid size/capacity/pointer alignment before bounded copying. Readable=false means downstream conclusions are unavailable; member=0 with entries_readable=0 does not establish absence. Likewise metadata fields are omitted if not readable. Row order is unspecified.

### Safety and limits

Uses the pinned object-array resolver for bridge-recorded weak references. Copies object-item fields, native array and metadata with ReadProcessMemory, validating full read length. Never dereferences array entries; only reads metadata at addresses recorded during original attachment after pointer membership is established. Does not invoke virtual methods on diagnostic binding pointers. Expected action identity and native handle are captured at original attachment.
The component class/layout was verified at OpenInput/bind; diagnostic reads only the existing ABI-pinned array offset after resolving the recorded weak reference. This is not a compatibility detector for other engine versions.
Original pointer membership and matching metadata do not prove engine input-stack participation, action evaluation, or delegate invocation. Engine-created clones are not individually tracked by this diagnostic. Arbitrary native memory corruption and allocator address reuse cannot be comprehensively certified by a snapshot. Snapshot observes stored subscriptions with original live-binding records; it does not enumerate foreign bindings.

### Interpreting a snapshot

Capture on demand in the game thread and retain the complete header, rows and
footer together. Check component weak identity and array readability before
interpreting membership or action/trigger/handle matches. A truncated snapshot
does not describe all subscriptions. Debug observers can add subscriptions beyond
the bindings a caller explicitly requested.

An intact snapshot establishes stored binding state only. Investigating missing
input also requires independent evidence of action evaluation and input routing;
absence of a bridge callback alone does not identify where an event was lost.
The native fixture tests cover bounds, identity and read gates, and Lua fixtures
cover API forwarding and failure handling. They do not certify a live Unreal
object layout or replace integration testing for the target build.

## Dispatch limits and tuning

Configure these process environment variables before starting the game. Values
are read once at bridge construction; changes require a restart. Invalid or
out-of-range values use the defaults.

| Variable | Default | Accepted integers | Zero means |
|---|---:|---|---|
| `UE4SSLEB_QUEUE_CHECKS_PER_SECOND` | 20 | 1–1,000 | Invalid; uses default |
| `UE4SSLEB_MAX_EVENTS_PER_PASS` | 256 | 0–1,000,000 | No callback-count limit |
| `UE4SSLEB_MAX_DISPATCH_US` | 2,000 | 0–1,000,000 | No time limit |
| `UE4SSLEB_MAX_QUEUED_EVENTS` | 65,536 | 0–1,000,000 | Unbounded producer queue |

For a PowerShell launcher:

```powershell
$env:UE4SSLEB_QUEUE_CHECKS_PER_SECOND = '40'
# Start the game from this process so it inherits the variable.
```

An already-running Steam process may not inherit a newly set launcher variable.

UE4SS still invokes on_update at its own cadence. Each invocation reads a monotonic
clock and returns before touching either queue when the next pass is not due.
The bridge creates no timer thread, sleeps, or catch-up bursts.

Each eligible pass continues the oldest batch in order, subject to existing
inactive-session/subscription and callback-error handling. The default allowance
is 256 live callback attempts or 2,000 microseconds per pass. Canceled bindings
and stopped sessions consume time but do not consume the callback-count allowance.
A failed callback attempt still counts. The first-progress allowance applies to
one examined entry, so an all-canceled batch cannot bypass the time limit. Remaining events stay in the batch
for the next eligible pass; new batches never overtake them. There is no sleep
between events. The time allowance starts before batch acquisition and buffer
recycling. Dequeue tracing is performed only for each admitted event, inside the
same budget; acquiring a batch does not format traces for the entire batch.
The allowance is non-preemptive: preparation, lock waits, or one slow Lua callback
can exceed it. At least one event is processed per nonempty pass to avoid starvation.

Disabling both per-pass limits drains the available batch without a dispatch
budget. A full producer queue rejects newest events, preserves accepted-event
order, and logs the overflow even with tracing disabled. Treat rejection as an
input-delivery failure. A larger queue stores more backlog; it does not make a
slow callback faster.

`UE4SSLuaEventBridge.GetDispatchStats()` returns process-wide `queued` (producer
queue plus the retained consumer batch, excluding the event currently being
processed), `queue_high_water` (producer queue only), `rejected_events` and
`rejected_traces` counters. Canceled entries remain queued until discarded.
The producer limit and high-water mark still describe producer capacity; total
`queued` can exceed both while a previous batch is retained.
Counters last for the bridge lifetime. Read them on demand; no diagnostic polling
is added by the bridge. A rejection must be treated as an input delivery failure.

Trace buffering is capped at 1,024 lines. Each pass separately allows 32 lines or
250 microseconds of Lua logging work, while preserving buffered line order.
Excess diagnostics increment `rejected_traces`. Native debugger tracing remains
synchronous when enabled; keep debug tracing off for gameplay measurements.

The 50 ms interval is between pass starts, not a sleep after completion. Actual
delivery can take longer because of UE4SS scheduling and slow callbacks. A slow
pass does not trigger repeated catch-up passes. Sustained overload can still
increase event latency or cause explicit rejections. Fewer checks do not by themselves prove lower frame times. Test the new limits under the intended workload before deployment.

See the [maintainer guide](BUILD.md#testing-and-validation-limits) for validation boundaries.

For inputs that must stop after delivery loss, use
[target delivery faults](#opt-in-target-delivery-faults). Those notifications
are independent of the bounded event and trace queues.

## Function hooks

`UE4SSLuaEventBridge.Hooks` observes individual `UFunction`s, Blueprint
functions and events included, without any process-wide UE4SS setting. A
function nobody hooks costs nothing. Hook events are copied when the function
runs and delivered to your callback later, on the game thread, once per frame.
The callback sees a snapshot, not the live call: it cannot block, replace or
edit the function (v1 observes only). Design: `COORDINATOR/LEX/LEX_hooks.md`.

Gate on the capability first:

```lua
local caps = UE4SSLuaEventBridge.GetCapabilities()
-- caps.hooks (bool), caps.hooks_reason (why not), caps.hooks_bp, caps.hooks_native,
-- caps.hooks_sync (false in 1.1), caps.hooks_frame_delivery (engine-tick drain active)
```

An older build returns `false` for all of these. `Hooks.Hook` must run on the
game thread (`ExecuteInGameThread`).

### `Hooks.Hook(target, options)`

```lua
local Hooks = UE4SSLuaEventBridge.Hooks

local h, err = Hooks.Hook("/Game/UI/WBP_Warning.WBP_Warning_C:OnShowWarning", {
    pre  = function(call) end,          -- either or both
    post = function(call) end,
    params = { "IconType" },            -- optional selection; dotted paths for struct fields
    object = weakHandle,                -- optional: one instance (lifetimes.weak)
    object_names = { "Reticle" },       -- optional: object FName set
    outer_class = classObject,          -- optional: an outer must be (or derive from) this class
    outer_class_exact = false,
    object_paths = false,               -- also deliver object path names
    max_queued = 256,
    delivery = "queued",                -- "sync" is rejected until hooks_sync is true
})
```

`target` is a path `"/Package.Class:Function"` or a live `UFunction` wrapper.
A path that does not resolve yet becomes a **pending** hook: it is owned by the
session and attaches by itself when the class appears (the owner class's short
name is registered as a wake-up) and on a one-second retry while any hook is
pending. If the function is later deleted (level or class reload) the hook goes
back to pending and attaches to the new function. A path that resolves to
something that is not a `UFunction` fails at once.

Failures return `nil, message`: unknown option, wrong type, `delivery = "sync"`
(`"sync delivery needs hooks_sync"`), hooks disabled (`[Hooks] Enabled=0`), or a
native refusal such as `"property layout mismatch"`. A hook that stays refused
while pending shows the last reason in `h:Status()`.

Parameters are copied by name. Supported kinds: bool, 8..64-bit integers,
float, double, enum, `FName`, `FString` (up to 4 KiB), object pointers, structs
of those (depth 3) and fixed arrays. Other kinds (arrays, maps, delegates,
`FText`, soft pointers) arrive as `nil`; asking for one explicitly in `params`
fails at `Hook`.

### The callback argument

```lua
call = {
    hook = h, phase = "pre" | "post", sequence = n, game_thread = bool,
    function_path = "...",
    object = { address = n, index = n, serial = n, path = "..."? },
    params = { Name = value, Struct = { Field = value },
               Enum = { value = n, name = "..." }, Obj = { address, index, serial, path? } },
    out = { ... },            -- post only: out parameters
    result = value,           -- post only: ReturnValue
    params_available = bool,  -- false on the Blueprint-to-native path
    frame = n,                -- GetStats().ticks when the game made the call
    clock = n,                -- os.clock() at the moment of the call (estimated at delivery)
}
```

Delivery happens later in the frame, or in a later frame under load. Use
`frame` and `clock` when you need to know when the game made the call; read
`GetStats().ticks` or `os.clock()` yourself for delivery time.

When a native function is called from Blueprint bytecode its parameters are not
evaluated yet: `params_available` is `false`, `params` is `nil`, and the post
call carries only `result`. `object.serial` may be `0` for an object that was
never weakly referenced; capture it later with `lifetimes.captureAddress` on the
game thread. Do not compare wrappers with `==`.

A callback error is printed and the hook stays active. After 32 failures in a
row the hook is disabled and `h:Status()` reports `state = "failed"`. A success
resets the count.

### Handle and library calls

```lua
h:Status()   --> { state = "active"|"pending"|"orphaned"|"failed"|"unhooked", reason = s?,
                   calls, matched, delivered, dropped, reentries }
h:Unhook()   --> true | false, reason      -- also removes a pending hook; a repeat call returns false, reason
Hooks.Unhook(h)
Hooks.UnhookAll()   --> count, true | count, false, reason
Hooks.GetStats()    --> { queued, high_water, rejected, delivery = "tick"|"loop", ticks,
                          update_calls, entries, slots = { a, b }, pending }
Hooks.Schema(target) --> { { name, kind, offset, size, dim, flags, fields = {...} }, ... } | nil, reason
```

`calls` counts handler entries before filters, `matched` the ones that passed
them, `delivered` those that reached Lua, `dropped` those lost to the per-hook
or queue limit. Counters survive `Unhook`. `Schema` is a diagnostic: it lists
what the bridge would copy for a resolved function.

Hot reload unhooks everything the session owns when the Lua state stops. Call
`Hooks.UnhookAll()` yourself when you tear down earlier.

### Example

```lua
ExecuteInGameThread(function()
    local h, err = UE4SSLuaEventBridge.Hooks.Hook(
        "/Game/UI/WBP_Warning.WBP_Warning_C:OnShowWarning", {
            params = { "IconType" },
            pre = function(call) print("icon", call.params.IconType) end,
        })
    if not h then print("hook failed: " .. err) end
end)
```

See `examples/HookIndicator/main.lua` for a fuller case.

### Environment variables and stats

Read once at bridge construction, like the input ones; invalid values use the
defaults.

| Variable | Default | Meaning |
|---|---:|---|
| `UE4SSLEB_MAX_QUEUED_HOOK_EVENTS` | 8,192 | Producer queue for hook events, separate from input |
| `UE4SSLEB_MAX_HOOK_EVENTS_PER_PASS` | 64 | Callbacks per frame drain |
| `UE4SSLEB_MAX_HOOK_DISPATCH_US` | 500 | Time allowance per frame drain, in microseconds |
| `UE4SSLEB_HOOK_CHECKS_PER_SECOND` | 120 | Fallback drain rate (1-1,000), used only when the engine-tick detour is unavailable |

Hook events are drained from an engine-tick callback on the game thread, so a
slow callback costs frame time; the allowances bound the bridge's own work only.
Without the engine-tick detour (`HookEngineTick = 0`) delivery falls back to the
event-loop thread, `hooks_frame_delivery` is `false` and `GetStats().delivery`
is `"loop"`: Lua callbacks may then overlap UE4SS's game-thread Lua. Per-hook
`max_queued` (default 256) stops one hot hook from starving the rest. Watch
`dropped` in `h:Status()` and `rejected` in `GetStats()` to see a backlog.
Once your Blueprint hooks use this API, `HookProcessInternal = 0` is safe again.
