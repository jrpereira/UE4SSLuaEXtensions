-- G0 spike driver (COORDINATOR/LEX/LEX_hooks.md §10, §12): a UEBridge eval script.
-- Needs the feature/hooks-spike build of UE4SSLuaEventBridge. Set PATH and ACTION,
-- then run:  python3 work/ue4ss_bridge.py --eval <this file>
--
-- ACTION: init | status | inspect | hook | counts | unhook | tick | tick_off
--   inspect, hook, counts and unhook need PATH; the others ignore it.
-- The native writes every line to UE4SS.log ("[UE4SSLuaEventBridge spike] ...") and
-- returns them; this script returns them with Lua's own view of the function, so the
-- ForEachProperty names and offsets sit next to the native walk.
local PATH = "/Script/UMG.Image:SetBrushFromAtlasInterface"
local ACTION = "inspect"

local out = { action = ACTION, path = PATH }
if UE4SSLuaEventBridge_HookSpike == nil then
    out.error = "UE4SSLuaEventBridge_HookSpike is not registered: install the spike build"
    return out
end

local needsFunction = { inspect = true, hook = true, counts = true, unhook = true }
local address = 0

local function text(value)
    local ok, result = pcall(function() return value:ToString() end)
    return ok and result or tostring(value)
end

local function listProperties(owner, list, depth)
    owner:ForEachProperty(function(property)
        local entry = string.rep("  ", depth) .. text(property:GetFName()) .. " @" ..
            tostring(property:GetOffset_Internal()) .. string.format(" address 0x%X", property:GetAddress())
        list[#list + 1] = entry
        if depth < 3 then
            local ok, struct = pcall(function() return property:GetStruct() end)
            if ok and struct ~= nil and struct:IsValid() then listProperties(struct, list, depth + 1) end
        end
    end)
end

if needsFunction[ACTION] then
    local found = StaticFindObject(PATH)
    if found == nil or not found:IsValid() then
        out.error = "not found: " .. PATH
        return out
    end
    address = found:GetAddress()
    out.address = string.format("0x%X", address)
    local okFlags, flags = pcall(function() return found:GetFunctionFlags() end)
    if okFlags then out.function_flags = string.format("0x%X", flags) end
    local properties = {}
    local okList, listError = pcall(listProperties, found, properties, 0)
    out.lua_properties = okList and properties or ("ForEachProperty failed: " .. tostring(listError))
end

local function run()
    local ok, result = pcall(UE4SSLuaEventBridge_HookSpike, address, ACTION)
    return ok and result or ("native call failed: " .. tostring(result))
end

-- inspect, hook, unhook and init must run on the game thread.
local onGameThread = UE4SSLuaEventBridge_IsInGameThread ~= nil and UE4SSLuaEventBridge_IsInGameThread()
if onGameThread or ACTION == "status" or ACTION == "counts" or ACTION == "tick" or ACTION == "tick_off" then
    out.native = run()
else
    ExecuteInGameThread(function() print(run()) end)
    out.native = "queued on the game thread; see UE4SS.log"
end
return out
