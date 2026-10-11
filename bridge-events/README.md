# UE4SS Lua Event Bridge

UE4SSLuaEventBridge connects Unreal Enhanced Input to UE4SS Lua callbacks.
Bind existing Input Actions or create key bindings with native Tap/Hold triggers.
Unreal handles the stopwatch; your mod handles the action.

## Features

- Bind existing Input Actions or create private key bindings through Lua helpers.
- Receive copied event values outside Unreal's native input-dispatch stack.
- Use session-owned handles, explicit cleanup, and optional delivery-fault handlers.
- Inspect bindings and dispatch statistics on demand; enable tracing when needed.
- Tune bounded dispatch queues and per-pass budgets for your workload.

Supply exact live component and subsystem paths. Your mod owns target discovery
and rebinding when the game replaces those objects.

## Compatibility

Windows x64, Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit `97b7e501`.
Native layouts are ABI-pinned; nearby engine or UE4SS builds are not assumed
compatible. Check `GetCapabilities()` for API features rather than inferring
them from the product version.

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
    │       └── UE4SSLuaEventBridge-1.0.12.dll
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

## Documentation

- [Developer guide](docs/DEVELOPERS.md): API, examples, lifecycle, debugging, and tuning.
- [Maintainer guide](docs/BUILD.md): native architecture, build instructions, and tests.
- [Changelog](CHANGELOG.md): version history.
- [Complete sample](examples/EnhancedInputTapHold/Scripts/main.lua): rollback and cleanup.
