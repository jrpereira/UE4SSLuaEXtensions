-- Vendored from UE4SSLuaEXtensions (owner). Do not edit copies; change the source and re-vendor.
-- Test double for UE4SSLuaEventBridge object lifetimes (API 6 weak handles). The bridge's
-- tests/LifecycleIntegrationTests.lua runs the same checks on it and on bridge_api.lua.
-- alive(object) is the native lifetime; by default the object's IsValid. As in the bridge:
--   weak(object) refuses a dead object and returns a handle with private state;
--   get() returns the same wrapper while its object lives, and nil for good once it died;
--   off the game thread, get() returns nil and why but keeps the wrapper;
--   identity() returns the lifetime token and address; release() returns its share once.
-- service.held counts unreleased handles; set service.gameThread=false to leave the game thread.
local M = {}
local DEAD = 'object must be a live UE4SS UObject wrapper'

local function isValid(object)
    local ok, value = pcall(function() return object:IsValid() end)
    return ok and value == true
end
local function address(object)
    local ok, value = pcall(function() return object:GetAddress() end)
    if ok and type(value) == 'number' then return value end
end

function M.service(alive)
    alive = alive or isValid
    local tokens, count = setmetatable({}, {__mode='k'}), 0
    local states = setmetatable({}, {__mode='k'})
    local service = {held=0, gameThread=true}
    local function token(object)
        if not tokens[object] then count = count + 1; tokens[object] = tostring(count) end
        return tokens[object]
    end
    local Handle = {__metatable=false}
    Handle.__index = Handle
    function Handle:get()
        local state = states[self]
        if state == nil or state.object == nil then return nil end
        if not service.gameThread then return nil, 'weak handles must be read on the Unreal game thread' end
        if alive(state.object) then return state.object end
        state.object = nil
    end
    function Handle:identity()
        local state = states[self]
        if state then return state.token, state.address end
    end
    function Handle:release()
        local state = states[self]
        if state == nil or state.released then return end
        state.object, state.released = nil, true
        service.held = service.held - 1
    end
    function service.captureObject(object)
        if object == nil or not alive(object) then return nil, DEAD end
        return token(object), address(object)
    end
    function service.weak(object)
        if not service.gameThread then return nil, 'weak handles must be created on the Unreal game thread' end
        if object == nil or not alive(object) then return nil, DEAD end
        local handle = setmetatable({}, Handle)
        states[handle] = {object=object, token=token(object), address=address(object)}
        service.held = service.held + 1
        return handle
    end
    -- A new token for object, as when its address is reused by another object.
    function service.renew(object) tokens[object] = nil end
    return service
end

-- Installs a service as the global UE4SSLuaEventBridge's lifetimes, creating that table
-- when absent, and returns the service.
function M.install(alive)
    local service = M.service(alive)
    local bridge = rawget(_G, 'UE4SSLuaEventBridge')
    if type(bridge) ~= 'table' then bridge = {}; rawset(_G, 'UE4SSLuaEventBridge', bridge) end
    bridge.lifetimes = service
    return service
end

-- A UE4SSLuaEventBridge table for a consumer's api.
-- bridge.loopStart() delivers pending onLoopStart callbacks, as the first update does.
function M.bridge(service)
    service = service or M.service()
    local pending = {}
    local bridge = {API_VERSION=6, lifetimes=service,
        GetCapabilities=function()
            return {api=6, object_lifetimes=true, weak_handles=true, loop_start=true}
        end}
    function bridge.onLoopStart(callback)
        local record = {callback=callback}
        pending[#pending+1] = record
        return function()
            local live = record.callback ~= nil
            record.callback = nil
            return live
        end
    end
    function bridge.loopStart()
        local ready = pending
        pending = {}
        for _, record in ipairs(ready) do
            local callback = record.callback
            record.callback = nil
            if callback then callback() end
        end
    end
    return bridge
end

return M
