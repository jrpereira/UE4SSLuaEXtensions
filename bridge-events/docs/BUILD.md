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
4. `on_update` drains the queue and invokes one dispatcher closure in the
   owning Lua state.
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
record is queued. `on_update` prints those records through the owning Lua
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
first `Hooks.Hook()` call in the process.

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
  an engine-tick post-callback registered at the first hook, with a frame-time
  budget. Without the engine-tick detour (`HookEngineTick = 0`) the drain falls
  back to `on_update` on the event-loop thread; `hooks_frame_delivery` and
  `GetStats().delivery` say which one is active. Input events keep their own
  queue and drain.
- **Lifetime.** Hook entries live in one process-wide table and are never freed
  while the DLL is resident. Unhooking restores both bytes (or the `Func`
  pointer) and leaves the entry inert so a frame already inside a handler
  finishes normally. A hook on a class that is not loaded yet stays pending and
  attaches when the class object appears. A deleted `UFunction` orphans its
  entry (nothing is written back) and the hook returns to pending. Stopping a
  Lua session unhooks every hook it owns.

Settings: `mod_settings.ini` has a toggle, **Enable function hooks**
(`[Hooks] Enabled`, default on). Off, `Hooks.Hook()` fails with a reason and
`GetCapabilities().hooks_reason` says why.

### Deliberate exclusions

These apply to the bridge as a whole:

- no player-controller or pawn discovery;
- no listener-driven Lua callback or automatic object discovery;
- no process-wide detour: no `ProcessEvent`, `ProcessInternal` or
  `ProcessLocalScriptFunction` hook; function hooks are per-function and
  reversible (see above);
- no C++-to-C++ interception: a call that never passes through a `UFunction` is
  not seen;
- no parameter or return-value edits in 1.1: hooks observe only;
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
