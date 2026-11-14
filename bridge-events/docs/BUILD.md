# Maintainer guide

For Lua integration, see the [developer guide](DEVELOPERS.md).

- [Build requirements](#build-requirements)
- [Configure and build](#configure-and-build)
- [Portable checks](#portable-checks)
- [Native architecture](#native-architecture)
- [Function hooks](#function-hooks-110)
- [Testing and validation limits](#testing-and-validation-limits)

## Build requirements

- Visual Studio 2022 with the Desktop C++ workload
- CMake 3.22 or newer
- Ninja or the Visual Studio 2022 CMake generator

The build is self-contained. Its minimal declarations and import definition are
pinned to the supplied `UE4SS.dll` at commit `97b7e501`; it does not build or
link a second UE4SS checkout. The Dawnwalker CXX dump supplies the verified UE
5.5 object and `FInputActionInstance` layouts used by the backend.

## Configure and build

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUE4SSLEB_BUILD_TESTS=OFF
cmake --build build
```

Expected output:

```text
build\dist\0_ModCore_UE4SSLuaEventBridge\dlls\main.dll
build\dist\0_ModCore_UE4SSLuaEventBridge\dlls\main.json
build\dist\0_ModCore_UE4SSLuaEventBridge\dlls\versions\UE4SSLuaEventBridge-1.0.13.dll
```

The repository contains two CMake projects. `bootstrap` builds `main.dll` with
no UE4SS link dependency. `bridge-events` builds the versioned implementation and owns
the UE4SS, Unreal, input, Lua, and lifetime code. The root CMake project builds
and assembles both.

The build embeds Windows `VERSIONINFO` resources in both DLLs. They can be
checked without loading either module:

```powershell
./tools/test-dll-version.ps1 `
  -Dll build/dist/0_ModCore_UE4SSLuaEventBridge/dlls/main.dll `
  -ExpectedProductName 'UE4SSLuaEventBridge Bootstrap' `
  -ExpectedOriginalFilename 'main.dll'
```

The validator requires the numeric file version `MAJOR.MINOR.PATCH.0`, the
three-part product version from `Version.hpp`, the product name, and the
original filename to agree. Packaging runs the same check before hashing the
bootstrap and selected implementation and creating the archive.

`dlls/main.json` contains schema 1 and either an exact three-part version or
`auto`. Exact selection never falls back. Automatic selection scans only
`dlls/versions`, rejects metadata and compatibility mismatches without loading
them, and tries the compatible candidates from newest to oldest: one that fails
to load, fails the private ABI check, or does not start is unloaded and the next
is tried. Changing selection requires a complete UE4SS unload or process
restart.

The bootstrap writes its decisions, including the implementation it started,
to UE4SS.log as well as the debugger. It resolves UE4SS's output function at
runtime and keeps no link-time dependency on UE4SS.

If native Unreal objects still depend on implementation code during teardown,
the implementation reports that condition to the bootstrap. The bootstrap keeps
the DLL and its process-wide selection claim resident, so another bridge version
cannot activate before process exit.

A different UE4SS revision requires ABI validation against `97b7e501`.
“Close enough” is not an ABI guarantee.

## Portable checks

On Linux, the behavioral tests and ABI-facing syntax audit can be run without
the Windows SDK, through the repository's test runner:

```sh
bash tests/run.sh bridge-events
```

It runs the shared code's tests, the checks every extension shares
(`shared/tests`: versions and identity, the ABI syntax audit, the load-time
verification gate), then this extension's own C++, Lua and Python suites.
`--only cpp|lua|python|checks` narrows a run.

## Native architecture

### Purpose

Translate native Enhanced Input action events into Lua callbacks, and (from
1.1.0) selected `UFunction` calls into Lua callbacks, without installing a
process-wide detour, object scans, or key-state polling. Enhanced
Input remains responsible for trigger evaluation and value generation. A separate
lifetime service uses UE4SS's native UObject create/delete listeners only to
invalidate explicitly captured, session-owned observations; listeners never call Lua.
They unregister during `OnUObjectArrayShutdown` before their storage is destroyed.
Before listener registration, a one-time startup probe resolves live `UClass`
objects and requires stable address/index/slot/serial agreement for at least two
distinct object-array entries. Failure disables the lifetime capability and no
listener is registered. There is no periodic object scan.
Copied events are delivered through scheduled queue checks; see [dispatch limits and tuning](DEVELOPERS.md#dispatch-limits-and-tuning).

The backend is game-agnostic. A caller supplies both the exact live
`UEnhancedInputComponent` object path and the exact `UInputAction` object path.
The bridge does not discover player controllers or infer gameplay/UI state.

### Pinned compatibility boundary

The native adapter targets:

- UE4SS `3.0.1 Beta #0`, commit `97b7e501`
- Unreal Engine 5.5
- Windows x64 with the MSVC ABI

The verified UE 5.5 layout constants are:

| Type or field | Pinned value |
|---|---:|
| `UInputComponent` size | `0x140` |
| `UEnhancedInputComponent` size | `0x178` |
| action-event binding array offset | `0x140` |
| `UObject::ClassPrivate` offset | `0x10` |
| `FInputActionInstance` size | `0x60` |
| `FEnhancedInputActionEventBinding` size | `0x20` |

The required UE 5.5 virtual order after the virtual destructor is:

1. `Execute`
2. `Clone`
3. `SetShouldFireWithEditorScriptGuard`
4. `IsBoundToObject`
5. `GetUObject`

Declaration order is ABI-significant. Compile-time size checks cover the local
weak pointer, binding base, action-event binding, instance view, unique-pointer
return wrapper, and array view.

### Native binding route

`UEnhancedInputComponent::BindAction` is a template/non-reflected C++ API, so
the bridge appends an owned polymorphic binding to the component's native
action-event array. Before reading or mutating that array it verifies:

- the component still resolves through its weak object reference;
- the component's reflected size is exactly the pinned UE 5.5 size;
- array size/capacity invariants;
- a non-null, pointer-aligned allocation when capacity is non-zero; and
- a bounded capacity before growth.

A mismatch fails closed. All public operations that touch the component or
action require Unreal's game thread.

The pinned UE4SS build's imported `FWeakObjectPtr(UObject*)` construction path
is deliberately not used. The bridge creates and resolves the equivalent
index/serial pair locally using the verified object-array offsets.

### Ownership and event delivery

1. `OpenInputComponent` resolves the exact supplied component once and stores a
   weak reference behind a session-owned target handle.
2. `BindAction` resolves the exact supplied action, attaches the native binding,
   and stores a shared subscription record.
3. Native `Execute` copies the event value into a bridge-owned queue. It never
   re-enters Lua.
4. The Lua consumer drains the queue and invokes one dispatcher closure in the
   owning Lua state. Every bridge-to-Lua call of the process (input, delivery
   faults, traces, loop-start callbacks, hook events and notices) runs on one
   thread, so no two ever share a Lua state (`include/LuaDelivery.hpp`). With
   UE4SS's engine-tick detour, decided once in `on_unreal_init`, that thread is
   the game thread: one engine-tick post-callback, registered there, drains the
   hooks and then runs the Lua pass every frame, and `on_update` makes no Lua
   call; it only marks sessions started since its last pass ready for their
   loop-start callbacks, because Lua mods start on its thread. Without the
   detour, `on_update` runs all of it at `UE4SSLEB_QUEUE_CHECKS_PER_SECOND`.
   A `LuaDispatchGate` (`shared/dispatch`) marks each pass in flight; Lua stop
   off the consumer thread and unload wait for it, bounded.
5. The dispatcher looks up the individual callback in Lua-owned tables.
6. Unbind, target close, and unbind-all deactivate the subscription before
   detaching its native binding on the game thread.
7. Callback failure and off-thread Lua-stop deactivate immediately; the next
   game-thread bridge operation detaches the inactive binding.

Queued events retain the shared subscription record, but an inactive record is
discarded before its Lua session is dereferenced. If Unreal has already
destroyed a component, weak resolution fails and detach avoids dereferencing
the stale native binding pointer.

Native bindings and any engine-created clones share a dispatch state rather
than retaining a raw backend pointer. The state rejects new events during
shutdown and counts live polymorphic binding objects. If UE4SS unloads the C++
mod off-thread while any such object remains under Unreal ownership, the DLL
adds a process-lifetime module reference. This intentionally trades a bounded
module residency for avoiding a dangling vtable after `FreeLibrary`.

Debug trace records use a separate queue in the same shared dispatch state.
Native stages capture their event sequence and thread information before the
record is queued. The Lua pass prints those records through the owning Lua
session's existing dispatcher, independently of the developer callback.
`OutputDebugString` receives the same line immediately as a debugger fallback.
Trace queues stop accepting records during backend shutdown and are cleared
before retained Lua session storage is released.

### Function hooks (1.1.0)

The bridge installs no process-wide detour, not on `ProcessEvent`, not on
`ProcessInternal`, not on `ProcessLocalScriptFunction`. A hook is a pair of
small, reversible writes inside the one function being hooked, and a function
nobody hooks costs nothing: no detour, no lookup, no extra branch. Nothing is
read from the engine's native-function table, and nothing is written, until the
first `Hooks.Install()` or `Hooks.Request()` call in the process.

- **Blueprint functions: two bytes.** A function's bytecode (`UStruct::Script`,
  a heap array) ends in exactly one `EX_Return`, followed by `EX_Nothing` and
  `EX_EndOfScript` (`04 0B 53`). The bridge claims two unused opcode slots in
  the engine's `GNatives` table (the two highest indices among the entries that
  still hold the shared undefined-opcode pointer; no pair, no Blueprint hooks).
  Hooking saves `Script[0]` and the `EX_Return` byte and overwrites them with
  the two claimed opcodes, **A** (entry) and **B** (exit). Both are single-byte
  writes to heap memory: no page protection changes, no torn reads, and neither
  the bytecode length nor any jump offset changes. Handler A records the
  pre-call event, then runs the saved first opcode's native handler; the engine
  continues as if A were never there. Handler B records the post-call event
  (return value and out parameters are already in the frame), then points the
  frame at a static `{EX_Return, EX_Nothing}` pair so the engine's loop finishes
  normally. A function whose body is empty (`04 0B 53`) gets only B, which then
  reports pre and post together. Every entry path runs this loop with the
  parameters already in the frame: `ProcessEvent`, Blueprint-to-Blueprint calls
  and event stubs alike. A per-thread re-entry guard stops a Kismet loop that
  jumps back to offset 0 from reporting a second pre-call. A function that has
  no `Script`, a different tail, or the native flag is refused, never patched.
- **Native functions: one pointer.** The bridge saves the function's `Func`
  pointer and swaps in its own thunk, which always calls the saved pointer. If
  another hooker swapped `Func` after us, unhooking leaves the entry inert
  instead of restoring over their pointer. Calls that arrive from Blueprint
  bytecode have unevaluated parameters; they are reported without parameters
  and, after the call, with the return value only (`params_available = false`).
- **Handlers copy and enqueue.** Native handlers may run on any thread. They
  read parameters by fixed offset from a schema built once at hook time, copy
  bytes into a bounded hook queue of their own, and return. They do no
  reflection calls and never enter Lua.
- **Delivery.** The hook queue is drained on the game thread, once per frame, from
  the bridge's engine-tick post-callback, with a frame-time budget, before the
  frame's input pass. Without the engine-tick detour (`HookEngineTick = 0`) the
  drain falls back to `on_update` on the event-loop thread with every other
  bridge callback; `frame_delivery` and `GetStats().delivery` say which one is
  active. Input events keep their own queue and budget.
- **Lifetime.** Hook entries live in one process-wide table and are never freed
  while the DLL is resident. Unhooking restores both bytes (or the `Func`
  pointer) and leaves the entry inert so a frame already inside a handler
  finishes normally. A request (`Hooks.Request`) on a class that is not loaded
  yet stays pending and attaches when the class object appears. A deleted
  `UFunction` orphans its entry (nothing is written back); an installed hook
  stays orphaned and a request returns to pending. Stopping a Lua session
  unhooks every hook it owns.

Settings: `mod_settings.ini` has a toggle, **Enable function hooks**
(`[Hooks] Enabled`, default on). Off, `Hooks.Install()` and `Hooks.Request()` fail with a reason and
`GetCapabilities().hooks_reason` says why.

### Function hook design reference

The rules below are what the hook code relies on. Source comments point to
these headings.

#### Hook design rules

- **Inert until used.** Nothing is read from `GNatives`, nothing is written and
  no table is allocated until the first hook (or pending hook) in the process.
  The object listeners that already run for lifetimes add one atomic fast path
  (`hooks_started`, then the pending-name count).
- **Kill switch.** `[Hooks] Enabled` in the mod's `settings.ini` (written by
  ModCore Settings, default on) is read at Lua start. Off, hooking fails with a
  reason and `hooks_reason` says why. Configuration never blocks launch.
- **Separate queue.** Hook events have their own bounded queue, drain, budget
  and counters; an input overflow and a hook flood cannot affect each other.
- **No detours.** Each hooked function gets reversible writes inside itself
  only: bytes in its `Script`, or its `Func` pointer.

#### Hook threads and locks

- Handlers run on whatever thread calls the function (game thread, animation
  workers). They read memory, copy bytes and enqueue under the producer mutex;
  they never enter Lua.
- The hook table is read-mostly: handlers take a shared lock for a lookup by
  `UFunction*`; hooking and unhooking take it exclusively on the game thread.
- Hook events and notices are drained on the game thread once per frame, from
  the engine-tick post-callback that `on_unreal_init` registers when the detour
  exists (the plain `std::function<void(UEngine*, float)>` overload). The same
  callback then runs the input Lua pass ("Ownership and event delivery"). It
  has no unregister: it checks the `LuaDispatchGate`'s inert flag, which
  `prepare_for_unload` sets, and the DLL stays resident for the rest of the
  process whenever the engine tick exists, hooks used or not. It runs after the
  world and widget ticks and before Slate paints, so a call made during this
  frame's tick is delivered before the frame is drawn.
- Registering at the first hook instead would need a handover between a running
  `on_update` pass and the first tick, and would save only the DLL's
  unloadability, so the bridge registers at startup.
- Session stop and unload wait for a running pass with the gate's bounded
  `wait_idle` (and the hook runtime's `wait_for_drain`), never under a lock:
  UE4SS fires `on_lua_stop` while holding its own Lua action mutex, which a
  callback inside the pass may be waiting for.
- Without the engine-tick detour (`HookEngineTick = 0`, or its scan failed) the
  drain falls back to `on_update` on the event-loop thread with input delivery.
  There the bridge's Lua calls can overlap UE4SS's game-thread Lua;
  `frame_delivery = false` reports it. UE4SS takes no lock around its own
  game-thread Lua, so a bridge-private lock could not prevent that.

#### Blueprint patch

A script function's bytecode is run by `ProcessLocalScriptFunction`: step until
`EX_Return`, then one more step for the return expression. Every entry path
(`ProcessEvent`, Blueprint-to-Blueprint calls, event stubs into the ubergraph)
runs this loop with the parameters already in `Stack.Locals`.

- Hooking saves `Script[0]` and the `EX_Return` byte and writes opcode A over
  the first and opcode B over the second.
- Handler A (`exec` signature): `Stack.Code` already points past the opcode, as
  the original opcode's handler expects. It finds the entry by `Stack.Node`,
  runs the re-entry guard, records the pre-call event on a genuine entry, then
  calls `GNatives[saved first opcode]`.
- Handler B finds the entry, records the post-call event (return value and out
  parameters are already in `Locals`; by-ref outs through the frame's
  `OutParms`), pops the guard, then points `Stack.Code` at a static
  `{EX_Return, EX_Nothing}` pair so the loop finishes normally.
- An empty body (`Script` is exactly `04 0B 53`) gets only B, which reports pre
  and post together.
- Aborted execution leaves the loop before B: the post-call event is not
  delivered and the guard record is pruned later.
- Unhooking restores both bytes and leaves the entry inert with its saved
  bytes, so a frame already past A or B still finds what it needs.

#### Native Func swap

The bridge saves `Func` and installs its thunk. `UFunction::Invoke` sets
`Stack.CurrentNativeFunction` before calling `Func`, so the thunk finds its
entry there, and it always calls the saved pointer.

- `Stack.Node == function` (through `ProcessEvent`): `Locals` is the parameter
  block, read before and after the call.
- `Stack.Node != function` (from Blueprint bytecode): the parameters are still
  unevaluated bytecode in the caller's frame. Pre delivers no parameters; post
  delivers only the return value, read from `Result`
  (`params_available = false`).
- Unhooking restores `Func` only while it is still the thunk; if another hooker
  swapped it later, the entry goes inert and the thunk passes calls through.
- Refused: `FUNC_Native` not set, or `Func` null.

#### Re-entry guard

A Kismet loop whose first statement is at offset 0 jumps back to `Script[0]`
and would run A again. Per thread, a small list of `(FFrame*, entry)` records:
A first drops records whose frame lies below the current one (finished
callees); a record for this frame and entry means re-entry, which dispatches
the saved opcode without hooks; otherwise A pushes one. B pops it. The records
live in a fixed per-thread array (handlers must not allocate on animation
workers); a full array treats the call as genuine, so a later loop-back may
report a spurious pre-call, and counts it.

#### Opcode slots

`GNatives` has `EX_Max` (0x100) entries; unassigned opcodes all hold the same
undefined-opcode handler, the most frequent pointer in the table. At first use
the bridge takes the most frequent pointer with at least 8 occurrences as
"free", claims the two highest free indices as A and B, and writes its
handlers there. If another writer took a slot meanwhile, it chooses again from
the current table, a few times. No free pair: Blueprint hooks are unavailable
(`hooks_bp = false`) and native hooks still work. Slots are never released.

#### Bytecode assumptions

The Kismet compiler emits exactly one `EX_Return`, followed by `EX_Nothing` and
`EX_EndOfScript` (`04 0B 53`), and early returns jump to it. The bridge checks
this per function at hook time and refuses a function without `Script`, with a
different tail, with `EX_Return` at entry, or already carrying a hook opcode;
such a function is never patched.

#### Parameter schema

Built once when a hook attaches, on the game thread, by two sides:

- **Lua enumerates** with `ForEachProperty`, into struct properties to depth 3,
  and sends per property its name, `GetOffset_Internal()`, `GetAddress()`,
  parent index and, for enums, the value names.
- **Native verifies and completes**: it walks `GetChildProperties()` and then
  `FField::Next` at `HookLayout::ffield_next`, and requires the walk to
  reproduce Lua's list exactly (count, names, offsets, addresses); a mismatch
  refuses the hook with "property layout mismatch". It reads element size,
  array dimension and flags through the exported accessors and the kind from
  the field class name. The schema is immutable afterwards.

At call time the handler makes no reflection calls: it reads bytes from
`Locals` by offset into a compact payload. `FName` values become strings with
`FName::ToString`, `FString` values are copied up to 4 KiB, and object values
carry address, index and serial (serial 0 for never-referenced objects), plus
the path built from outer names when `object_paths` is set. `params` selects
top-level parameters or dotted struct fields (`"Struct.Field"`); unsupported
kinds arrive as `nil`, and asking for one explicitly fails the hook.

#### Native filters

Evaluated in the handler before any copying, in this order, with plain memory
reads at the probe-checked offsets (never UE4SS's dynamic-offset accessors):

1. `object`: the frame object's index and serial equal a weak identity from
   Lua.
2. `object_names`: the frame object's FName (comparison index and number) is
   in the set, resolved at hook time with `FNAME_Find`. Names that do not exist
   yet can never match.
3. `outer_class`: an outer up to 8 levels up has this class, or derives from it
   unless `outer_class_exact`.

#### Hook lifetime

- One process-wide table, at most 1,024 distinct functions. Entries are never
  freed while the DLL lives: inert after unhook, reused on rehook.
- For `Hooks.Request`, a path that does not resolve leaves the request
  **pending** on the Lua side, with the owner class's short name (after the
  last `.`, before `:`) registered natively as a wake-up name. No timer runs.
  `NotifyUObjectCreated` queues a wake-up notice (-3) when a created object has
  that name; Lua then retries that request a bounded number of times (next
  tick, then about 100 ms and 1 s later) and waits for the next wake-up. A
  Blueprint function found right after its class appears can still lack its
  `Script`; those bounded retries cover it. Refusals that cannot change
  (unknown parameter, unsupported kind, not a `UFunction`, layout mismatch)
  turn the request `failed`. `Hooks.Install` never registers a wake-up.
- `NotifyUObjectDeleted` on a hooked `UFunction` orphans its entry: nothing is
  written back, and the session gets a notice (-4). An installed hook becomes
  `orphaned`; a request returns to pending, retries in a bounded burst and
  waits for its class, so the reloaded function attaches again.
- Stopping a Lua session unhooks everything it owns; queued events for an
  inactive session are discarded at drain.

#### Layout probe

The startup probe verifies `InternalIndex` and the object-item layout, and for
hooks also `ClassPrivate` (0x10), `NamePrivate` (0x18) and `OuterPrivate`
(0x20) against the exported accessors on the same sample. Hooks need it to
pass, because filters and wake-ups read those offsets directly.

#### Hook queue and delivery

- Events carry entry, session, hook id, phase, sequence, capture frame and
  time, whether they ran on the game thread, the object identity and the
  payload. Producer queue `UE4SSLEB_MAX_QUEUED_HOOK_EVENTS` (default 8,192).
- Per-hook limit `max_queued` (default 256): above it an event is dropped and
  counted, so a hot hook cannot starve a rare one.
- The drain takes a snapshot batch within `UE4SSLEB_MAX_HOOK_EVENTS_PER_PASS`
  (64) and `UE4SSLEB_MAX_HOOK_DISPATCH_US` (500); the rest waits for the next
  frame, in order. A callback that calls a hooked function sees that event
  next frame. The fallback drain runs at `UE4SSLEB_HOOK_CHECKS_PER_SECOND`
  (120).
- `GetStats().ticks` counts engine-tick callbacks and increases after that
  frame's drain, so Lua running anywhere in one frame reads the same value.

#### Hook natives and messages

Registered per Lua state; the Lua block in `lua/bridge_api.lua` is the only
caller:

```
HooksEnabled() -> hooks, reason|nil, hooks_bp, hooks_native, hooks_sync
HookFunction(session, functionAddress, phaseMask, options, properties) -> hookId | nil, reason
Unhook(session, hookId) -> bool, reason|nil
HookStatus(session, hookId) -> state, reason|nil, calls, matched, delivered, dropped, reentries
HookPendingName(session, token, shortClassName) -> bool
HookPendingForget(session, token)
HookSchema(functionAddress, properties) -> (name, kind, "offset:size:dim:flags")... | nil, reason
GetHookStats() -> queued, high_water, rejected, deliveryIsTick, ticks, update_calls, entries, slotA, slotB
```

- Text (options and every name) is sent as `length, word...` with
  `ceil(length / 8)` little-endian words.
- `options` is `names=a|b|c; outer=<address>; exact=0|1; obj=<address>:<token>;
  paths=0|1; maxq=n; params=A.B|C` (`sync=1` is refused). Unknown keys are
  ignored; a malformed known key is an error.
- `properties` is `count`, then per property in pre-order: parent index (-1 for
  a parameter), address, offset, name, enum count and enum names (an empty name
  for a gap in the values).
- A native returns at most 20 values (`LUA_MINSTACK`; UE4SS exports no
  `lua_checkstack`), which is why `GetCapabilities` and `HooksEnabled` are
  separate natives.

The session's one dispatcher closure carries hook messages too: Lua routes
codes at or below -3 to the hooks block.

```
dispatcher(code, hookId, sequence, phase, gameThread, objAddress, objIndex, objSerial, objPath|nil,
           paramsAvailable, n, name1, tag1, value1, ..., nameN, tagN, valueN)
code: -10 event; -5 fault (hookId, reason); -4 function deleted (hookId, reason); -3 wake-up (hookId = pending token)
tags: b bool, i integer, n number, s string, e enum ("n|Name"), o object ("address:index:serial[:path]"), x unsupported (nil)
names: "Param", "Struct.Field", "Arr[i]", "out:Param", "ret", and "@frame" / "@age" for the capture frame and age
```

Events larger than a direct call fits are pushed in chunks through the
session's transport helpers and flushed in one dispatcher call.

### Deliberate exclusions

These apply to the bridge as a whole:

- no player-controller or pawn discovery;
- no listener-driven Lua callback or automatic object discovery;
- no process-wide detour: no `ProcessEvent`, `ProcessInternal` or
  `ProcessLocalScriptFunction` hook; function hooks are per-function and
  reversible (see above);
- no C++-to-C++ interception: a call that never passes through a `UFunction` is
  not seen;
- no parameter or return-value edits in this version: hooks observe only;
- no background component scanning or polling;
- no automatic rebinding after component replacement; and
- no game-specific UI, pause, or gameplay-state filtering.

The Lua caller owns target selection, rebinding, and unbinding policy. The same
holds for hooks: the bridge does not decide which functions matter.

### Helper layer boundary

`Helpers.OpenInput` is part of the bridge's embedded Lua API. It uses UE4SS object construction and ordinary reflected Enhanced
Input calls to create transient mapping contexts, Input Actions, and Tap/Hold
triggers. The ABI-pinned native backend remains responsible only for explicit
component targets, native action-event bindings, queued event delivery, and
safe detachment.

Each helper binding uses a private mapping context. This keeps rollback and
unbind ownership local to one handle and avoids editing an already-active
shared context. Mapping activation occurs only after all required native
subscriptions have attached. A debug-enabled binding adds phase observers that
share the logical helper binding ID and are removed as part of the same
lifecycle. Helper-created actions default to non-consuming input.

## Testing and validation limits

Run from the repository root with Lua 5.4:

```sh
bash tests/run.sh --only lua bridge-events
```

Without Bash, run both files directly from the repository root:

```sh
lua bridge-events/tests/LuaHelperTests.lua
lua bridge-events/tests/LifecycleIntegrationTests.lua
```

The build and release workflows invoke the same runner. A failed assertion
returns a nonzero exit code and fails the job.

### What is exercised

The lifecycle suite loads the production `bridge-events/lua/bridge_api.lua` into isolated
Lua environments. Each reload receives a fresh session ID and callback registry,
while its simulated engine retains shared context, target and subscription
registries. Assertions cover cleanup ownership, error propagation, callback
routing and absence of accumulated resources.

| Scenario | Regression checks |
| --- | --- |
| Off-thread helper shutdown | Rejects cleanup without reflected mutations; retains resources for a later game-thread retry. |
| Repeated reloads | 100 fresh environments; all four debug observers removed; cleanup is idempotent; old queued callbacks never invoke user code. |
| Mapping-context removal failures | Two consecutive failures are reported; retry ownership is retained; detached callbacks stay disabled; successful removal happens once. |
| Partial native removal failure | Remaining subscription survives for retry; already-removed context is not removed twice. |
| Component destruction | Helper teardown completes under the native dead-component unsubscribe contract; captured callbacks are ignored after teardown. |
| Late callbacks | An event captured before closing a scope cannot invoke that scope or a subsequently opened scope. |
| Subsystem destruction | Invalid subsystem is not called through during cleanup; subscriptions and targets are released. |
| Native-stop helper failure | Failed cleanup throws and attempts a Lua log message; the native stop path reports the exception and still detaches native actions. |
| Thrown scope cleanup errors | An unexpected exception from one scope does not abandon healthy scopes; the failed scope remains retryable while Lua is alive. |
| Public UnbindAll retries | Context failure, partial subscription failure, and bulk failure preserve retry ownership; captured callbacks stay disabled; later Close/UnbindAll releases all resources. |
| Multiple scopes | Failure in one scope does not prevent cleanup of other scopes; remaining work is retried. |
| Loop start | One-shot delivery, explicit cancellation, and callback release after delivery. |
| Object lifetime Lua surface | Decimal token preservation, validation, and per-session loss draining. |

### Native queue ownership coverage

`QueueBuffersTests` additionally runs 100 lifecycle cycles with real producer,
consumer and statistics-reader threads synchronized by barriers, without sleeps.
It uses the production buffer transfer, dispatch backlog, count publisher and
budgeted dispatcher. It checks producer refill during a retained batch, count
visibility before callbacks, canceled-entry draining, shared ownership release,
and clearing the producer queue at shutdown. These are native C++ queue tests;
the payload is not a UObject and this does not establish engine GC safety.

The Windows build and release jobs run the queue and backend suites with MSVC AddressSanitizer
and assertions enabled. Locally, run `tests/NativeQueueSanitizerTests.ps1` from an
x64 Visual Studio developer shell. Sanitizer failures fail the job. AddressSanitizer
checks memory access errors; it is not a data-race detector or an Unreal GC test.

### Native backend lifecycle coverage

On Windows, `NativeBackendLifecycleTests` compiles the production
`EnhancedInputBackend.cpp` with controlled UE imports and object-array fixtures.
It exercises actual native delegate allocation, attachment, execution, cloning,
queueing, deactivation, detachment and backend shutdown:

- 100 sessions bind, enqueue, unsubscribe and release retained native ownership.
- Engine-owned clones reject late delivery after original removal and remain
  safe to call and destroy after the backend has been destroyed.
- A producer executes 10,000 delegate calls while a second thread shuts down
  the backend; off-thread shutdown leaves engine-owned bindings untouched,
  rejects subsequent events and requires module retention until destruction.
- Destroying the component and its delegates before unsubscribe exercises the
  real dead-component branch without dereferencing freed native bindings.
- Failed native allocation publishes no binding and allows a subsequent retry.

Windows CTest includes this suite. CI also runs it alongside the queue test
under MSVC AddressSanitizer. The production native code runs against controlled stubs for UE object
discovery, allocation, thread identity, and object-array lifetime. See
[validation boundaries](#validation-boundary) for the integration limits.

### Lua delivery thread coverage

`LuaDeliveryTests` (portable) and `LuaDispatchGateTests` (shared/dispatch)
drive the consumer glue (`LuaConsumer`, `LuaPass`, `LoopStart`) and the gate
from two threads, an injected engine tick and `on_update`, against fake
sessions whose Lua side counts its entry depth. Input events, delivery faults,
traces, loop-start callbacks and the real `HookRuntime`'s notices are queued
while both run. The tests check that no two calls overlap, that `on_update`
makes no Lua call in tick mode and the tick none in loop mode, that loop-start
callbacks wait for `on_update`'s barrier and keep registration order, that a
Lua stop during a running pass waits for it, and that an inert gate ends every
pass.

On Windows, `NativeDeliveryThreadsTests` repeats the two-thread run with the
production `EnhancedInputBackend.cpp` (the stubs of the backend suite) and the
real hook runtime: the game thread executes the native delegates, queues hook
work and runs the tick while `on_update` runs alongside. CI runs it under MSVC
AddressSanitizer with the other native suites. The File Bridge counterpart is
`bridge-files/tests/TailDeliveryTests.cpp`.

### Target delivery-fault coverage

The native backend suite additionally verifies real bounded-queue rejection of a
terminal event, invalidation of a retained old batch, notification without further
input, rejection of new subscriptions on the faulted target, explicit new-target
recovery, unaffected legacy/other-target behavior, and an injected C++ vector
allocation exception. Closing a target or stopping its session suppresses its
pending notification. The Lua suite exercises one-shot notification, a throwing
fault handler, stale callback suppression, and a scheduled side effect rejected
by native-health/generation guards before and after notification. These tests do
not establish game-thread integration or physical focus/key-state semantics.

### Validation boundary

The Lua suites are **offline helper integration tests**, using simulated Unreal objects
and native exports. They execute the real Lua implementation, but do not execute
`BridgeMod::on_lua_stop`, native delegate detachment, engine garbage collection,
or concurrent engine threads. Component destruction models the native contract;
it does not establish that the pinned Unreal ABI implements it safely.

The off-thread case invokes the helper cleanup entry point with the thread guard
set to false. Native off-thread Lua shutdown currently skips that entry point and
deactivates subscriptions without removing generated contexts. Game-thread stop now
reports helper cleanup failures instead of discarding their return values, but
reporting does not preserve those contexts for cleanup after the Lua state dies. Passing this suite
does **not** resolve or certify that known lifecycle gap.

Native acceptance still requires an isolated compatible UE/UE4SS process:
stop Lua off-thread, reload it repeatedly, destroy/recreate the input component,
and deliver queued or in-flight events during teardown. Verify mapping-context
and delegate counts, absence of callbacks into stopped sessions, and absence of
crashes using evidence from that process. Do not treat simulated cleanup or
portable weak-pointer tests as proof of those runtime properties.
