-- Observe a Blueprint indicator widget without UE4SS RegisterHook and without
-- HookProcessInternal. All paths below are placeholders: replace them with the
-- exact function paths from UE4SS Live View or an object dump of your game.
local Hooks = UE4SSLuaEventBridge.Hooks

-- Blueprint event on the indicator widget class: /Package.Class:Function
local ICON_CHANGED = "/Game/UI/WBP_ExampleIndicator.WBP_ExampleIndicator_C:OnIconTypeChanged"
-- An event without parameters; its pre and post phases fire together.
local CLEARED = "/Game/UI/WBP_ExampleIndicator.WBP_ExampleIndicator_C:NotifyIndicatorCleared"

local handles = {}

local function start()
    local caps = UE4SSLuaEventBridge.GetCapabilities()
    if not caps.hooks then
        print("function hooks unavailable: " .. tostring(caps.hooks_reason))
        return
    end

    -- A path that does not resolve yet is not an error: the hook stays pending
    -- and attaches when the widget class loads, and again after a reload.
    local icon, err = Hooks.Hook(ICON_CHANGED, {
        params = { "IconType" },       -- copy only what you need
        object_paths = true,           -- also deliver each widget's path
        pre = function(call)
            -- Runs on the game thread, later in the frame the function was called.
            print(string.format("icon %s on %s (sequence %d)",
                tostring(call.params.IconType), tostring(call.object and call.object.path),
                call.sequence))
        end,
    })
    if not icon then print("hook failed: " .. err) else handles[#handles + 1] = icon end

    local cleared
    cleared, err = Hooks.Hook(CLEARED, {
        post = function(call) print("indicator cleared", call.sequence) end,
    })
    if not cleared then print("hook failed: " .. err) else handles[#handles + 1] = cleared end
end

ExecuteInGameThread(start)

-- Counters tell you whether calls arrive, and whether any were dropped.
RegisterKeyBind(Key.F9, function()
    ExecuteInGameThread(function()
        for _, handle in ipairs(handles) do
            local s = handle:Status()
            print(s.state, "calls", s.calls, "delivered", s.delivered, "dropped", s.dropped)
        end
    end)
end)

-- Hooks are removed with the Lua state on hot reload; to remove them earlier:
--   Hooks.UnhookAll()
