package.path = "shared/tests/?.lua;" .. package.path
local expect = require("TestKit").expect

local counters = {
    targets_opened = 0,
    targets_closed = 0,
    contexts_added = 0,
    contexts_removed = 0,
    native_unbound = 0,
}
local nextObject = 0
local nextHandle = 0
local nativeTokens = {}
local nativeBindings = {}
local scopeTraces = {}
local failNextBind = false
local gameThread = true
local failContextAdd = false
local failContextRemove = false

local function object(kind, path)
    local value = { kind = kind, path = path, valid = true }
    function value:IsValid() return self.valid end
    function value:IsA(classPath)
        return classPath == "/Script/EnhancedInput." .. self.kind
            or (self.kind == "Class" and classPath == "/Script/CoreUObject.Class")
    end
    function value:GetFullName() return self.kind .. " " .. self.path end
    return value
end

local subsystemPath = "/Engine/Transient.TestSubsystem"
local subsystem = object("EnhancedInputLocalPlayerSubsystem", subsystemPath)
subsystem.active = {}
function subsystem:AddMappingContext(context, priority, _options)
    context.priority = priority
    self.active[context] = true
    counters.contexts_added = counters.contexts_added + 1
    if failContextAdd then error('injected failure after context installation') end
end
function subsystem:RemoveMappingContext(context, _options)
    if failContextRemove then error('injected context removal failure') end
    if self.active[context] then
        self.active[context] = nil
        counters.contexts_removed = counters.contexts_removed + 1
    end
end

local classes = {}
for _, name in ipairs({
    "InputMappingContext",
    "InputAction",
    "InputTriggerTap",
    "InputTriggerHold",
}) do
    classes["/Script/EnhancedInput." .. name] = object("Class", "/Script/EnhancedInput." .. name)
    classes["/Script/EnhancedInput." .. name].class_name = name
end

local failWeakInitialization = false
local weakInitializations = 0
local systemLibrary = object("KismetSystemLibrary", "/Script/Engine.Default__KismetSystemLibrary")
function systemLibrary:Conv_ObjectToSoftObjectReference(action)
    expect(gameThread and action.kind == "InputAction")
    if failWeakInitialization then error("injected reflected conversion failure") end
    weakInitializations = weakInitializations + 1
    action.weak_initialized = true
    -- Deliberately no result: the helper must not inspect or depend on it.
end
function StaticFindObject(path)
    if path == "/Script/Engine.Default__KismetSystemLibrary" then return systemLibrary end
    if path == subsystemPath then return subsystem end
    return classes[path] or object("Missing", path)
end

local contextsConstructed = 0
function StaticConstructObject(class, outer, _name, flags)
    -- Private contexts are rooted so a game-side mapping clear cannot collect them.
    if class.class_name == "InputMappingContext" then
        expect(flags == 0xC0, "private contexts must be transient and rooted")
        contextsConstructed = contextsConstructed + 1
    else
        expect(flags == 0x40, "generated objects must be transient")
    end
    nextObject = nextObject + 1
    local generated = object(class.class_name, "/Engine/Transient.Generated_" .. nextObject)
    generated.outer = outer
    if class.class_name == "InputMappingContext" then
        function generated:MapKey(action, key)
            self.action = action
            self.key = key
        end
        function generated:UnmapAll()
            self.action = nil
            self.key = nil
            self.unmapped = (self.unmapped or 0) + 1
        end
    end
    return generated
end

function FName(value) return { value = value } end

__UE4SSLuaEventBridge_SessionId = 17

function UE4SSLuaEventBridge_GetVersion() return "0.3.3" end
function UE4SSLuaEventBridge_GetCapabilities()
    return 6, true, true, true, true, true, true, true, true, "97b7e501", true, true, true, true
end
function UE4SSLuaEventBridge_InspectInputComponent(session, target)
    expect(session == 17)
    if not gameThread then return nil, "InspectInputComponent must run on the Unreal game thread" end
    return "snapshot target=" .. target
end
function UE4SSLuaEventBridge_IsInGameThread() return gameThread end
function UE4SSLuaEventBridge_GetDispatchStats() return 7,42,3,5 end
function UE4SSLuaEventBridge_OpenInputComponent(_session, ...)
    counters.targets_opened = counters.targets_opened + 1
    return counters.targets_opened
end
function UE4SSLuaEventBridge_CloseInputComponent(_session, _target)
    counters.targets_closed = counters.targets_closed + 1
    return true
end
function UE4SSLuaEventBridge_BindAction(...)
    local args = { ... }
    if failNextBind then
        failNextBind = false
        return nil, "injected bind failure"
    end
    nextHandle = nextHandle + 1
    nativeTokens[nextHandle] = args[69]
    nativeBindings[nextHandle] = {
        phase = args[68],
        debug = args[70] == 1,
        scope_id = args[71],
        binding_id = args[72],
        primary = args[73] == 1,
        trigger_kind = args[74],
    }
    return nextHandle
end
function UE4SSLuaEventBridge_Unbind(_session, handle)
    if nativeTokens[handle] == nil then return false, "unknown subscription handle" end
    nativeTokens[handle] = nil
    nativeBindings[handle] = nil
    counters.native_unbound = counters.native_unbound + 1
    return true
end
function UE4SSLuaEventBridge_UnbindAll(_session)
    local count = 0
    for handle in pairs(nativeTokens) do
        nativeTokens[handle] = nil
        nativeBindings[handle] = nil
        count = count + 1
    end
    return count, true
end
function UE4SSLuaEventBridge_TraceScope(_session, scopeId, stage, ...)
    scopeTraces[#scopeTraces + 1] = { scope_id = scopeId, stage = stage }
end

local dispatch = assert(loadfile("bridge-events/lua/bridge_api.lua"))()
local bridge = UE4SSLuaEventBridge
local Helpers = bridge.Helpers
local Trigger = Helpers.Trigger

expect(bridge.API_VERSION == 6)
expect(bridge.GetCapabilities().helpers == true)
expect(bridge.GetCapabilities().detailed_errors == true)
expect(bridge.GetCapabilities().debug_tracing == true)
expect(bridge.GetCapabilities().loop_start == true)
expect(bridge.GetCapabilities().object_lifetimes == true)
expect(bridge.GetCapabilities().weak_handles == true)
expect(type(bridge.lifetimes.weak) == "function")
expect(type(bridge.lifetimes.captureObject) == "function")
expect(type(bridge.lifetimes.captureAddress) == "function")
expect(bridge.lifetimes.capture == nil)

local invalidTarget, invalidTargetError = bridge.CloseInputComponent("bad")
expect(not invalidTarget and string.find(invalidTargetError, "positive integer", 1, true))
local invalidBind, invalidBindError = bridge.BindAction(1, "", "Triggered", function() end)
expect(invalidBind == nil and string.find(invalidBindError, "1..512", 1, true))
local missingRemoved, missingRemoveError = bridge.Unbind(999)
expect(not missingRemoved and missingRemoveError == "unknown subscription handle")

local readOnly = pcall(function() Trigger.Tap = "changed" end)
expect(not readOnly, "Trigger constants must be read-only")

gameThread = false
local offThreadScope, openThreadError = Helpers.OpenInput({
    component_path = "/Game/Test.Component",
    subsystem_path = subsystemPath,
})
expect(offThreadScope == nil and string.find(openThreadError, "game thread", 1, true))
expect(counters.targets_opened == 0,
    "off-thread OpenInput must not resolve or retain an Input Component")
gameThread = true

local scope, openError = Helpers.OpenInput({
    component_path = "/Game/Test.Component",
    subsystem_path = subsystemPath,
})
expect(scope ~= nil, openError)

gameThread = false
local objectsBeforeOffThreadBind = nextObject
local offThreadHandle, offThreadError = scope:Bind("F9", Trigger.Tap, function() end)
expect(offThreadHandle == nil and string.find(offThreadError, "game thread", 1, true))
expect(nextObject == objectsBeforeOffThreadBind,
    "off-thread Bind must not construct Unreal objects")
gameThread = true

local tapEvent
local tapHandle, tapError = scope:Bind("F10", Trigger.Tap, function(event)
    tapEvent = event
end, { threshold_seconds = 0.2 })
expect(tapHandle ~= nil, tapError)
expect(nativeBindings[tapHandle].debug == false,
    "debug-disabled scopes must not attach diagnostic metadata")

local holdEvent
local holdHandle, holdError = scope:Bind("F10", Trigger.Hold, function(event)
    holdEvent = event
end, { threshold_seconds = 0.5, one_shot = true })
expect(holdHandle ~= nil, holdError)
expect(counters.contexts_added == 2)

gameThread = false
local removedOffThread, unbindThreadError = scope:Unbind(tapHandle)
expect(not removedOffThread and string.find(unbindThreadError, "game thread", 1, true))
expect(counters.native_unbound == 0 and counters.contexts_removed == 0,
    "off-thread Unbind must not mutate native bindings or mapping contexts")
local closedOffThread, closeThreadError = scope:Close()
expect(not closedOffThread and string.find(closeThreadError, "game thread", 1, true))
expect(counters.native_unbound == 0 and counters.contexts_removed == 0,
    "off-thread Close must not mutate native bindings or mapping contexts")
gameThread = true

dispatch(nativeTokens[tapHandle], tapHandle, "/Engine/Transient.Tap", "Triggered",
    0.1, 0.0, 1.0, 0.0, 0.0, 0, 41)
expect(tapEvent.key == "F10")
expect(tapEvent.trigger == Trigger.Tap)
expect(tapEvent.sequence == 41)

dispatch(nativeTokens[holdHandle], holdHandle, "/Engine/Transient.Hold", "Triggered",
    0.75, 0.25, 1.0, 0.0, 0.0, 0, 42)
expect(holdEvent.trigger == Trigger.Hold)
expect(holdEvent.elapsed_processed == 0.75)

local removed, removeError = scope:Unbind(tapHandle)
expect(removed, removeError)
expect(counters.contexts_removed == 1)

failNextBind = true
local failedHandle, failedError = scope:Bind("F11", Trigger.Tap, function() end)
expect(failedHandle == nil and failedError == "injected bind failure")
expect(counters.contexts_added == 2)
expect(counters.contexts_removed == 1,
    "failed native Bind must not publish its mapping context")

local badHandle = assert(scope:Bind("F12", Trigger.Tap, function()
    error("injected callback failure")
end))
local callbackSucceeded = pcall(dispatch,
    nativeTokens[badHandle], badHandle, "/Engine/Transient.Bad", "Triggered",
    0.1, 0.0, 1.0, 0.0, 0.0, 0, 43)
expect(not callbackSucceeded)
-- The native update catch deactivates and reaps a failed callback before a
-- later explicit Unbind. Model the resulting already-absent native handle.
nativeTokens[badHandle] = nil
expect(scope:Unbind(badHandle) == true,
    "helper cleanup must tolerate a native callback-failure reap")

local closed, closeError = scope:Close()
expect(closed, closeError)
expect(counters.contexts_removed == 3)
expect(counters.targets_closed == 1)
expect(scope:Close() == true, "Close must be idempotent")

local secondScope = assert(Helpers.OpenInput({
    component_path = "/Game/Test.Component",
    subsystem_path = subsystemPath,
}))
assert(secondScope:Bind("F12", Trigger.Tap, function() end))
local allCount, allCompleted, allError = bridge.UnbindAll()
expect(allCount == 1 and allCompleted and allError == nil,
    "UnbindAll must count helper subscriptions and report completion")
expect(counters.targets_closed == 2)

local debugScope = assert(Helpers.OpenInput({
    component_path = "/Game/Test.Component",
    subsystem_path = subsystemPath,
    debug = true,
    debug_label = "HelperTests",
}))
expect(#scopeTraces == 1 and scopeTraces[1].stage == 1,
    "debug OpenInput must emit scope_opened")

local handlesBeforeDebugBind = nextHandle
local debugEvent
local debugHandle = assert(debugScope:Bind("F10", Trigger.Hold, function(event)
    debugEvent = event
end))
expect(nextHandle - handlesBeforeDebugBind == 4,
    "debug binding must observe Triggered, Started, Completed, and Canceled")
expect(nativeBindings[debugHandle].phase == 1 and nativeBindings[debugHandle].primary,
    "Triggered observer must be the logical primary binding")
local debugScopeId = nativeBindings[debugHandle].scope_id
local debugBindingId = nativeBindings[debugHandle].binding_id
expect(nativeBindings[debugHandle + 1].phase == 2)
expect(nativeBindings[debugHandle + 2].phase == 5)
expect(nativeBindings[debugHandle + 3].phase == 4)
for handle = debugHandle, debugHandle + 3 do
    expect(nativeBindings[handle].debug == true)
    expect(nativeBindings[handle].scope_id == debugScopeId)
    expect(nativeBindings[handle].binding_id == debugBindingId)
    expect(nativeBindings[handle].trigger_kind == 2)
end

dispatch(nativeTokens[debugHandle], debugHandle, "/Engine/Transient.DebugHold", "Triggered",
    1.0, 0.5, 1.0, 0.0, 0.0, 0, 99)
expect(debugEvent.sequence == 99)
expect(debugEvent.scope_id == debugScopeId)
expect(debugEvent.binding_id == debugBindingId)

local nativeUnboundBeforeDebug = counters.native_unbound
expect(debugScope:Unbind(debugHandle) == true)
expect(counters.native_unbound - nativeUnboundBeforeDebug == 4,
    "debug Unbind must remove every phase observer")
expect(debugScope:Close() == true)
expect(#scopeTraces == 2 and scopeTraces[2].stage == 2,
    "debug Close must emit scope_closed")

expect(bridge.GetCapabilities().binding_snapshot == true)
local initFailureScope = assert(Helpers.OpenInput({component_path="TestComponent",subsystem_path=subsystemPath}))
local beforeInitHandles, beforeInitContexts = nextHandle, counters.contexts_added
failWeakInitialization = true
local failedInit, failedInitError = initFailureScope:Bind("Q",Trigger.Tap,function() end)
expect(failedInit == nil and failedInitError:find("weak reference",1,true))
expect(nextHandle == beforeInitHandles and counters.contexts_added == beforeInitContexts)
failWeakInitialization = false
assert(initFailureScope:Close())
expect(weakInitializations > 0)
local diagnosticScope = assert(UE4SSLuaEventBridge.Helpers.OpenInput({component_path="TestComponent", subsystem_path=subsystemPath}))
expect(type(diagnosticScope:InspectBindings()) == "string")
local beforeSnapshot = counters.contexts_added
 gameThread = false
local snapshot, snapshotError = diagnosticScope:InspectBindings()
expect(snapshot == nil and snapshotError:find("game thread",1,true))
gameThread = true
expect(counters.contexts_added == beforeSnapshot)
expect(UE4SSLuaEventBridge.InspectInputComponent(-1) == nil)
assert(diagnosticScope:Close())
expect(diagnosticScope:InspectBindings() == nil)
local function countEntries(values)
    local n=0
    for _ in pairs(values) do n=n+1 end
    return n
end
local beforeContexts=countEntries(subsystem.active)
local stats=bridge.GetDispatchStats()
expect(stats.queued==7 and stats.queue_high_water==42 and stats.rejected_events==3 and stats.rejected_traces==5)
local beforeBindings=countEntries(nativeTokens)
for _=1,500 do
    local scope=assert(Helpers.OpenInput({component_path='TestComponent',subsystem_path=subsystemPath}))
    local calls=0
    local handle=assert(scope:Bind('A',Trigger.Tap,function() calls=calls+1 end))
    local token=nativeTokens[handle]
    dispatch(token,handle,'action','Triggered',0,0,1,0,0,0,1)
    expect(calls==1)
    assert(scope:Unbind(handle))
    dispatch(token,handle,'action','Triggered',0,0,1,0,0,0,2)
    expect(calls==1,'callback fired after unbind')
    assert(scope:Close())
    assert(scope:Close())
end
expect(countEntries(subsystem.active)==beforeContexts,'contexts accumulated during rebind cycles')
expect(countEntries(nativeTokens)==beforeBindings,'native handles accumulated during rebind cycles')
local rollback=assert(Helpers.OpenInput({component_path='TestComponent',subsystem_path=subsystemPath}))
failContextAdd=true
local bad,why=rollback:Bind('B',Trigger.Tap,function() end)
expect(bad==nil and why:find('failed to add',1,true))
failContextAdd=false
expect(countEntries(subsystem.active)==beforeContexts,'failed installation leaked a context')
expect(countEntries(nativeTokens)==beforeBindings,'failed installation leaked native handles')
local handle=assert(rollback:Bind('C',Trigger.Hold,function() end))
failContextRemove=true
local closed,closeError=rollback:Close()
expect(not closed and closeError:find('failed to remove',1,true))
failContextRemove=false
assert(rollback:Close())
expect(countEntries(subsystem.active)==beforeContexts,'retry did not remove retained context')
expect(countEntries(nativeTokens)==beforeBindings,'cleanup retry leaked native handles')
-- Native fault state is simulated here; production latch/overflow is tested in C++.
local faultState = {}
function UE4SSLuaEventBridge_EnableTargetDeliveryFaults(_session, target)
    if not gameThread then return false, "wrong thread" end
    if faultState[target] ~= nil then return false, "already registered" end
    faultState[target] = "healthy"
    return true
end
function UE4SSLuaEventBridge_IsTargetDeliveryValid(_session, target)
    if faultState[target] == "healthy" then return true end
    return false, faultState[target] or "unknown target"
end
expect(bridge.GetCapabilities().target_delivery_faults)
expect(not bridge.SetTargetDeliveryFaultHandler(0, function() end))
expect(not bridge.SetTargetDeliveryFaultHandler(99, false))
expect(not bridge.IsTargetDeliveryValid(-1))
local guarded = assert(bridge.OpenInputComponent("Guarded"))
local faults, calls, held = 0, 0, false
local epoch = 1
local effects = {}
local function faultHandler(event)
    expect(event.target == guarded and event.reason == "queue_capacity_exceeded")
    faults = faults + 1; held = false; epoch = epoch + 1
end
gameThread = false
expect(not bridge.SetTargetDeliveryFaultHandler(guarded, faultHandler))
gameThread = true
assert(bridge.SetTargetDeliveryFaultHandler(guarded, faultHandler))
expect(not bridge.SetTargetDeliveryFaultHandler(guarded, function() error("replacement ran") end))
local started = assert(bridge.BindAction(guarded, "Action", "Started", function()
    calls = calls + 1; held = true
    local capturedEpoch = epoch
    effects[#effects+1] = function()
        if epoch == capturedEpoch and bridge.IsTargetDeliveryValid(guarded) then calls = calls + 100 end
    end
end))
local token = nativeTokens[started]
dispatch(token, started, "Action", "Started", 0,0,1,0,0,0,1)
expect(held and calls == 1)
faultState[guarded] = "queue_capacity_exceeded"
-- Already scheduled work fails closed even BEFORE invalidation callback delivery.
effects[1](); expect(calls == 1)
dispatch(-1, guarded, "queue_capacity_exceeded")
expect(not held and faults == 1 and not bridge.IsTargetDeliveryValid(guarded))
dispatch(token, started, "Action", "Started", 0,0,1,0,0,0,2)
expect(not held and calls == 1, "stale Started relatched after fault")
dispatch(-1, guarded, "queue_capacity_exceeded"); expect(faults == 1)
assert(bridge.Unbind(started)); assert(bridge.CloseInputComponent(guarded))
local throwing = assert(bridge.OpenInputComponent("Throwing"))
assert(bridge.SetTargetDeliveryFaultHandler(throwing, function() error("injected fault handler failure") end))
faultState[throwing] = "callback_error"
expect(not pcall(dispatch,-1,throwing,"callback_error"))
expect(pcall(dispatch,-1,throwing,"callback_error"))
expect(not bridge.IsTargetDeliveryValid(throwing))
assert(bridge.CloseInputComponent(throwing))
local closedTarget = assert(bridge.OpenInputComponent("Closed"))
assert(bridge.SetTargetDeliveryFaultHandler(closedTarget, function() error("closed target callback") end))
assert(bridge.CloseInputComponent(closedTarget)); dispatch(-1,closedTarget,"callback_error")
local stoppedTarget = assert(bridge.OpenInputComponent("Stopped"))
assert(bridge.SetTargetDeliveryFaultHandler(stoppedTarget, function() error("stopped callback") end))
bridge.UnbindAll(); dispatch(-1,stoppedTarget,"callback_error")
print("Lua helper tests passed")
