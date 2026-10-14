# UE4SS Lua Event Bridge

UE4SSLuaEventBridge connects Unreal Enhanced Input and individual Unreal
functions to UE4SS Lua callbacks.
Bind existing Input Actions or create key bindings with native Tap/Hold triggers.
Unreal handles the stopwatch; your mod handles the action. Hook one Blueprint or
native function without turning on a process-wide UE4SS hook.

## Features

- Bind existing Input Actions or create private key bindings through Lua helpers.
- Receive copied event values outside Unreal's native input-dispatch stack.
- Use session-owned handles, explicit cleanup, and optional delivery-fault handlers.
- Inspect bindings and dispatch statistics on demand; enable tracing when needed.
- Hook individual `UFunction`s, Blueprint events included, and receive queued, frame-aligned calls with copied parameters (`Hooks`).
- Tune bounded dispatch queues and per-pass budgets for your workload.
- Hook individual Blueprint and native functions (`Hooks.Hook`), pre and post,
  with copied parameters, delivered to Lua once per frame on the game thread. A
  hook on a class that is not loaded yet waits and attaches when it appears. A
  function nobody hooks costs nothing.

Supply exact live component and subsystem paths. Your mod owns target discovery
and rebinding when the game replaces those objects.

## Compatibility

Windows x64, Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit `97b7e501`.
Native layouts are ABI-pinned; nearby engine or UE4SS builds are not assumed
compatible. Check `GetCapabilities()` for API features rather than inferring
them from the product version.

Function hooks (1.1.0) do not need `HookProcessInternal = 1` in
`UE4SS-settings.ini`. Once your mods have moved their Blueprint hooks to
`Hooks.Hook`, set `HookProcessInternal = 0`: UE4SS then stops looking up a
callback on every Blueprint function the game enters. Leave
`HookProcessLocalScriptFunction` at 0 and `HookEngineTick` at UE4SS's default
(on); the bridge delivers hook events from the engine tick, and falls back to a
slower event-loop drain without it (`GetCapabilities().hooks_frame_delivery`
tells you which). Mods that still use UE4SS `RegisterHook` on Blueprint
functions keep needing `HookProcessInternal = 1`.

## Installation layout

Download the versioned ZIP and checksum from
[GitHub Releases](https://github.com/jrpereira/UE4SSLuaEXtensions/releases).
Extract the ZIP into the UE4SS `Mods` directory to produce:

```text
Mods/
└── 0_ModCore_UE4SSLuaEventBridge/
    ├── dlls/
    │   ├── main.dll
    │   ├── main.json
    │   └── versions/
    │       └── UE4SSLuaEventBridge-1.0.13.dll
    ├── enabled.txt
    └── mod.json
```

`main.dll` is a small bootstrap. It reads `dlls/main.json`, validates the
selected implementation's embedded product, version, private ABI, UE4SS commit,
and Unreal target, then loads it from `dlls/versions`. A missing configuration
or an explicit `"version": "auto"` selects the highest compatible discovered
version. A malformed configuration or unavailable exact version fails closed.

Both DLLs carry Windows version resources and can be inspected without loading
them. Use the published SHA-256 checksum to verify the exact archive bytes.

For deterministic startup before Lua mods that consume the bridge, add this as
the first mod entry in `Mods/mods.txt`:

```text
0_ModCore_UE4SSLuaEventBridge : 1
```

The packaged `enabled.txt` marker also enables the bridge, but UE4SS loads marker
enabled mods in a separate pass with no defined ordering. A `mods.txt` entry is
therefore preferred when another mod needs the bridge during its Lua-state setup.

Keep only one active bridge installation.

## Minimal Lua example

Replace the component and subsystem placeholders with exact live object paths.
Run setup and cleanup on the Unreal game thread:

```lua
local input
ExecuteInGameThread(function()
    local bridge = UE4SSLuaEventBridge
    input = assert(bridge.Helpers.OpenInput({
        component_path = COMPONENT_PATH,
        subsystem_path = ENHANCED_INPUT_SUBSYSTEM_PATH,
    }))
    local handle, err = input:Bind("F10", bridge.Helpers.Trigger.Tap, function()
        print("F10 tapped\n")
    end)
    if not handle then
        local closed, closeError = input:Close()
        error(tostring(err) .. (closed and "" or ("; cleanup: " .. tostring(closeError))))
    end
end)

-- Call from your mod's shutdown/reload path while its Lua state is still alive.
function ShutdownBridgeInput()
    ExecuteInGameThread(function()
        if input then
            local closed, err = input:Close()
            if not closed then error(err) end
            input = nil
        end
    end)
end
```

Off-thread Lua shutdown disables callbacks but can leave helper-generated mapping
contexts installed. Explicit game-thread cleanup remains required. Offline tests
do not establish live-game acceptance or performance.

## Minimal hook example

Gate on the capability, hook by path, and unhook on shutdown. The path may name
a class that is not loaded yet; the hook then stays `pending`. `call.params`
holds copies of the parameters, not live objects:

```lua
local Hooks = UE4SSLuaEventBridge.Hooks
local caps = UE4SSLuaEventBridge.GetCapabilities()
local hook
if caps.hooks then
    local err
    hook, err = Hooks.Hook("/Script/Engine.PlayerController:ClientRestart", {
        post = function(call)
            print(string.format("%s %s #%d\n", call.phase, call.function_path, call.sequence))
        end,
    })
    if not hook then print("hook refused: " .. tostring(err) .. "\n") end
else
    print("function hooks unavailable: " .. tostring(caps.hooks_reason) .. "\n")
end

-- hook:Status().state is "active", "pending", "orphaned", "failed" or "unhooked".
-- When your mod reloads: hook:Unhook()  (or Hooks.UnhookAll()).
```

Hook callbacks observe a call after it was made (delivery is queued, once per
frame); they cannot block or change it. `hooks_sync` is false in 1.1.

## Documentation

- [Developer guide](docs/DEVELOPERS.md): API, examples, lifecycle, debugging, and tuning.
- [Maintainer guide](docs/BUILD.md): native architecture, build instructions, and tests.
- [Changelog](CHANGELOG.md): version history.
- [Complete sample](examples/EnhancedInputTapHold/Scripts/main.lua): rollback and cleanup.
