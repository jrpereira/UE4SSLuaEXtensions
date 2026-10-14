local __session = assert(__UE4SSLuaEventBridge_SessionId, "bridge session id is missing")
__UE4SSLuaEventBridge_SessionId = nil

local __callbacks = {}
local __bindings = {}
local __targetFaultHandlers = {}
local __loopStartCallbacks = {}
local __nextCallbackToken = 0

local function __forget(handle)
    local binding = __bindings[handle]
    if binding ~= nil then
        __callbacks[binding.token] = nil
        __bindings[handle] = nil
    end
end

local function __forgetTarget(target)
    local handles = {}
    for handle, binding in pairs(__bindings) do
        if binding.target == target then
            handles[#handles + 1] = handle
        end
    end
    for _, handle in ipairs(handles) do
        __forget(handle)
    end
end

local function __clearCallbacks()
    __callbacks = {}
    __bindings = {}
    __targetFaultHandlers = {}
    __loopStartCallbacks = {}
end

local function __traceback(message)
    if debug ~= nil and debug.traceback ~= nil then
        return debug.traceback(message, 2)
    end
    return tostring(message)
end

-- Hook messages (token <= -3) are routed here; the Hooks block below replaces
-- this no-op. Contract: COORDINATOR/LEX/LEX_hooks.md §9.2.
local __hookDispatch = function() end

local function __dispatchInput(
    token, handle, sourceAction, phaseName, elapsed, triggered, x, y, z, valueType,
    eventSequence)
    if token == -1 then
        local handler = __targetFaultHandlers[handle]
        __targetFaultHandlers[handle] = nil -- One notification, even if handler throws.
        __forgetTarget(handle)
        if handler ~= nil then handler({target = handle, reason = sourceAction}) end
        return
    end
    if token == -2 then
        local callback = __loopStartCallbacks[handle]
        __loopStartCallbacks[handle] = nil
        if callback == nil then return end
        local ok, callbackError = xpcall(callback, __traceback)
        if not ok then error(callbackError, 0) end
        return
    end
    if token == 0 then
        print(handle)
        return
    end
    local callback = __callbacks[token]
    if callback == nil then return end

    local ok, callbackError = xpcall(callback, __traceback, {
        subscription = handle,
        source_type = "enhanced_input",
        action = sourceAction,
        phase = phaseName,
        elapsed_processed = elapsed,
        elapsed_triggered = triggered,
        sequence = eventSequence,
        value = { x = x, y = y, z = z, type = valueType },
    })
    if not ok then
        __forget(handle)
        error(callbackError, 0)
    end
end

-- The one dispatcher closure the native side keeps per session. Input messages
-- are -2, -1, 0 and positive callback tokens; hook messages are -3 and below.
local function __dispatch(token, ...)
    if type(token) == "number" and token <= -3 then return __hookDispatch(token, ...) end
    return __dispatchInput(token, ...)
end

local function __packText(value, wordCount)
    local words = {}
    for i = 1, wordCount do words[i] = 0 end
    for i = 1, #value do
        local word = ((i - 1) // 8) + 1
        local shift = ((i - 1) % 8) * 8
        words[word] = words[word] | (string.byte(value, i) << shift)
    end
    local args = { #value }
    for i = 1, wordCount do args[#args + 1] = words[i] end
    return args
end

local function __packPath(path) return __packText(path, 64) end
local function __packDebugText(value) return __packText(value, 8) end

local function __positiveInteger(value)
    return type(value) == "number" and value % 1 == 0 and value > 0
end

local __phases = {
    Triggered = 1,
    Started = 2,
    Ongoing = 3,
    Canceled = 4,
    Completed = 5,
}

local function __bindAction(targetHandle, action, event, callback, trace)
    if not __positiveInteger(targetHandle) then
        return nil, "targetHandle must be a positive integer returned by OpenInputComponent"
    end
    if type(action) ~= "string" or #action < 1 or #action > 512 then
        return nil, "action must be a 1..512 byte InputAction object path"
    end
    if type(event) ~= "string" or __phases[event] == nil then
        return nil, "event must be Triggered, Started, Ongoing, Canceled, or Completed"
    end
    if type(callback) ~= "function" then
        return nil, "callback must be a function"
    end

    local packed = __packPath(action)
    local args = { __session, targetHandle }
    for i = 1, #packed do args[#args + 1] = packed[i] end
    args[#args + 1] = __phases[event]

    __nextCallbackToken = __nextCallbackToken + 1
    local token = __nextCallbackToken
    __callbacks[token] = callback
    args[#args + 1] = token

    if trace == nil then
        args[#args + 1] = 0
    else
        args[#args + 1] = 1
        args[#args + 1] = trace.scope_id
        args[#args + 1] = trace.binding_id
        args[#args + 1] = trace.primary and 1 or 0
        args[#args + 1] = trace.trigger_kind
        local packedKey = __packDebugText(trace.key)
        for i = 1, #packedKey do args[#args + 1] = packedKey[i] end
        local packedLabel = __packDebugText(trace.label)
        for i = 1, #packedLabel do args[#args + 1] = packedLabel[i] end
    end

    local handle, bindError =
        UE4SSLuaEventBridge_BindAction(table.unpack(args, 1, #args))
    if handle == nil then
        __callbacks[token] = nil
        return nil, bindError
    end
    __bindings[handle] = { token = token, target = targetHandle }
    return handle
end

local bridge = {
    API_VERSION = 6,
    GetVersion = UE4SSLuaEventBridge_GetVersion,
    GetDispatchStats = function()
        local queued, highWater, rejected, tracesRejected = UE4SSLuaEventBridge_GetDispatchStats()
        if queued == nil then return nil, "bridge unavailable" end
        return {queued=queued, queue_high_water=highWater, rejected_events=rejected,
            rejected_traces=tracesRejected}
    end,
    GetCapabilities = function()
        local api, enhancedInput, explicitTarget, helpers, dynamicInput,
            triggerTap, triggerHold, detailedErrors, debugTracing, target, bindingSnapshot,
            targetDeliveryFaults, loopStart, objectLifetimes, objectLifetimesReason =
            UE4SSLuaEventBridge_GetCapabilities()
        return {
            api = api,
            enhanced_input = enhancedInput,
            explicit_target = explicitTarget,
            helpers = helpers,
            dynamic_input = dynamicInput,
            trigger_tap = triggerTap,
            trigger_hold = triggerHold,
            detailed_errors = detailedErrors,
            debug_tracing = debugTracing,
            target_ue4ss_commit = target,
            binding_snapshot = bindingSnapshot == true,
            target_delivery_faults = targetDeliveryFaults == true,
            loop_start = loopStart == true,
            object_lifetimes = objectLifetimes == true,
            object_lifetimes_reason = objectLifetimes ~= true and objectLifetimesReason or nil,
            weak_handles = objectLifetimes == true,
            loss_opt_in = objectLifetimes == true,
        }
    end,
    SetTargetDeliveryFaultHandler = function(target, callback)
        if not __positiveInteger(target) then return false, "target must be a positive integer" end
        if type(callback) ~= "function" then return false, "callback must be a function" end
        local enabled, why = UE4SSLuaEventBridge_EnableTargetDeliveryFaults(__session, target)
        if not enabled then return false, why end
        __targetFaultHandlers[target] = callback
        return true
    end,
    IsTargetDeliveryValid = function(target)
        if not __positiveInteger(target) then return false, "target must be a positive integer" end
        return UE4SSLuaEventBridge_IsTargetDeliveryValid(__session, target)
    end,
    OpenInputComponent = function(componentPath)
        if type(componentPath) ~= "string" or #componentPath < 1 or #componentPath > 512 then
            return nil, "componentPath must be a 1..512 byte EnhancedInputComponent object path"
        end
        local packed = __packPath(componentPath)
        local args = { __session }
        for i = 1, #packed do args[#args + 1] = packed[i] end
        return UE4SSLuaEventBridge_OpenInputComponent(table.unpack(args, 1, #args))
    end,
    InspectInputComponent = function(targetHandle)
        if not __positiveInteger(targetHandle) then
            return nil, "targetHandle must be a positive integer returned by OpenInputComponent"
        end
        return UE4SSLuaEventBridge_InspectInputComponent(__session, targetHandle)
    end,
    CloseInputComponent = function(targetHandle)
        if not __positiveInteger(targetHandle) then
            return false, "targetHandle must be a positive integer returned by OpenInputComponent"
        end
        local closed, closeError =
            UE4SSLuaEventBridge_CloseInputComponent(__session, targetHandle)
        if closed then
            __forgetTarget(targetHandle)
            __targetFaultHandlers[targetHandle] = nil
        end
        return closed, closeError
    end,
    BindAction = function(targetHandle, action, event, callback)
        return __bindAction(targetHandle, action, event, callback, nil)
    end,
    Unbind = function(handle)
        if not __positiveInteger(handle) then
            return false, "handle must be a positive integer returned by BindAction"
        end
        local removed, unbindError = UE4SSLuaEventBridge_Unbind(__session, handle)
        if removed then
            __forget(handle)
        end
        return removed, unbindError
    end,
}

bridge.onLoopStart = function(callback)
    if type(callback) ~= "function" then error("callback must be a function", 2) end
    __nextCallbackToken = __nextCallbackToken + 1
    local token = __nextCallbackToken
    __loopStartCallbacks[token] = callback
    local registered, why = UE4SSLuaEventBridge_RegisterLoopStart(__session, token)
    if not registered then
        __loopStartCallbacks[token] = nil
        error(why or "failed to register loop-start callback", 2)
    end
    return function()
        if __loopStartCallbacks[token] == nil then return false end
        __loopStartCallbacks[token] = nil
        return UE4SSLuaEventBridge_CancelLoopStart(__session, token) == true
    end
end

-- Asks Unreal to initialize the object's weak identity. Unreal assigns object
-- serials lazily, on first weak reference, so a live object can still have
-- none. Do not use the pinned native AllocateSerialNumber wrapper: its C++
-- soft-reference copy faults. The reflected function takes one UObject and
-- returns a soft object ref; the result is deliberately discarded.
local function __initializeWeakIdentity(object)
    return pcall(function()
        local system = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
        if system == nil or not system:IsValid() then error("KismetSystemLibrary default object is unavailable") end
        system:Conv_ObjectToSoftObjectReference(object)
    end)
end

local __unidentifiedObject = "address is not a live UObject"

local function __captureObject(object)
    if object == nil then return nil, "object must be a live UE4SS UObject wrapper" end
    local checked, valid = pcall(function() return object:IsValid() end)
    if not checked or valid ~= true then
        return nil, "object must be a live UE4SS UObject wrapper"
    end
    local resolved, address = pcall(function() return object:GetAddress() end)
    if not resolved or not __positiveInteger(address) then
        return nil, "object did not provide a valid UObject address"
    end
    local token, why = UE4SSLuaEventBridge_LifetimeCapture(__session, address)
    if token == nil and why == __unidentifiedObject then
        -- A live object that was never weakly referenced has no serial yet.
        local initialized, initializeError = __initializeWeakIdentity(object)
        if not initialized then
            return nil, why .. "; failed to initialize its weak reference: " .. tostring(initializeError)
        end
        token, why = UE4SSLuaEventBridge_LifetimeCapture(__session, address)
    end
    if token == nil then return nil, why end
    return token, address
end

local __weakState = setmetatable({}, { __mode = "k" })
local __WeakHandle = {}
__WeakHandle.__index = __WeakHandle
__WeakHandle.__metatable = false

-- Returns the wrapper while its UObject lives, without reading object memory
-- first. Off the game thread it returns nil and an error but keeps the wrapper.
function __WeakHandle:get()
    local state = __weakState[self]
    if state == nil or state.object == nil then return nil end
    if UE4SSLuaEventBridge_IsInGameThread() ~= true then
        return nil, "weak handles must be read on the Unreal game thread"
    end
    if UE4SSLuaEventBridge_LifetimeValid(__session, state.address, state.token) == true then
        return state.object
    end
    state.object = nil
    return nil
end

-- Token and address of the captured lifetime; stable for the handle's life.
function __WeakHandle:identity()
    local state = __weakState[self]
    if state == nil then return nil end
    return state.token, state.address
end

-- Forgets the wrapper and returns the handle's share of its observation.
function __WeakHandle:release()
    local state = __weakState[self]
    if state == nil or state.released then return end
    state.object, state.released = nil, true
    UE4SSLuaEventBridge_LifetimeRelease(__session, state.token)
end
__WeakHandle.__gc = __WeakHandle.release

bridge.lifetimes = {
    captureAddress = function(address)
        if not __positiveInteger(address) then return nil, "address must be a positive integer" end
        return UE4SSLuaEventBridge_LifetimeCapture(__session, address)
    end,
    captureObject = function(object)
        local token, why = __captureObject(object)
        if token == nil then return nil, why end
        return token
    end,
    -- Drops one capture of token; the last capture ends the observation.
    release = function(token)
        if type(token) ~= "string" or token:match("^[1-9][0-9]*$") == nil then return false end
        return UE4SSLuaEventBridge_LifetimeRelease(__session, token) == true
    end,
    valid = function(address, token)
        if not __positiveInteger(address) or type(token) ~= "string" or
            token:match("^[1-9][0-9]*$") == nil then return false end
        return UE4SSLuaEventBridge_LifetimeValid(__session, address, token) == true
    end,
    -- Losses are queued only after the session opts in, here or by calling takeLost.
    reportLosses = function()
        return UE4SSLuaEventBridge_LifetimeReportLosses(__session) == true
    end,
    takeLost = function()
        return UE4SSLuaEventBridge_LifetimeTakeLost(__session)
    end,
    weak = function(object)
        if UE4SSLuaEventBridge_IsInGameThread() ~= true then
            return nil, "weak handles must be created on the Unreal game thread"
        end
        local token, address = __captureObject(object)
        if token == nil then return nil, address end
        local handle = setmetatable({}, __WeakHandle)
        __weakState[handle] = { object = object, address = address, token = token }
        return handle
    end,
}

bridge.SubscribeEnhancedInput = function(spec, callback)
    if type(spec) ~= "table" then return nil, "spec must be a table" end
    return bridge.BindAction(spec.target, spec.action, spec.event, callback)
end

bridge.Unsubscribe = bridge.Unbind

local __tap = {}
local __hold = {}
local __triggerValues = { Tap = __tap, Hold = __hold }
local Trigger = setmetatable({}, {
    __index = __triggerValues,
    __newindex = function()
        error("Helpers.Trigger is read-only", 2)
    end,
    __pairs = function()
        return next, __triggerValues, nil
    end,
})

local __scopeMethods = {}
local __scopeMetatable = {
    __index = __scopeMethods,
    __newindex = function()
        error("input scopes are read-only", 2)
    end,
}
local __scopeStates = setmetatable({}, { __mode = "k" })
local __inputScopes = {}
local __classCache = {}
-- RF_Transient, and RF_MarkAsRootSet for private mapping contexts: the game may
-- clear its mappings, which would otherwise leave a context to be collected
-- while a scope still uses it. Rooted contexts are never released, so closed
-- ones are emptied and reused instead of leaked.
local __transientFlags = 0x40
local __rootedFlags = 0xC0
local __contextPool = {}

-- Holds a long-lived object without dereferencing it after it dies, using a
-- weak handle when native lifetimes are available.
local function __keep(object)
    local handle = bridge.lifetimes.weak(object)
    if handle ~= nil then return { weak = handle } end
    return { object = object }
end
local __nextScopeId = 0
local __nextBindingId = 0

local function __onGameThread(operation)
    if UE4SSLuaEventBridge_IsInGameThread() then return true end
    return false, operation .. " must run on the Unreal game thread"
end

local function __validObject(object)
    if object == nil then return false end
    local ok, valid = pcall(function() return object:IsValid() end)
    return ok and valid
end

local function __findObject(path, classPath, label)
    local ok, object = pcall(StaticFindObject, path)
    if not ok then
        return nil, label .. " lookup failed: " .. tostring(object)
    end
    if not __validObject(object) then
        return nil, label .. " did not resolve to a live object: " .. path
    end

    local classOk, isExpectedClass = pcall(function() return object:IsA(classPath) end)
    if not classOk or not isExpectedClass then
        return nil, label .. " is not a " .. classPath .. ": " .. path
    end
    return object
end

local function __getClass(name)
    local cached = __classCache[name]
    if __validObject(cached) then return cached end

    local path = "/Script/EnhancedInput." .. name
    local ok, class = pcall(StaticFindObject, path)
    if not ok then
        return nil, "class lookup failed for " .. path .. ": " .. tostring(class)
    end
    if not __validObject(class) then
        return nil, "Enhanced Input class is unavailable: " .. path
    end
    __classCache[name] = class
    return class
end

local function __held(holder)
    if holder == nil then return nil end
    if holder.weak ~= nil then return holder.weak:get() end
    return __validObject(holder.object) and holder.object or nil
end

local function __construct(name, outer, flags)
    local class, classError = __getClass(name)
    if class == nil then return nil, classError end

    local ok, object = pcall(StaticConstructObject, class, outer, 0, flags or __transientFlags)
    if not ok then
        return nil, "failed to create transient " .. name .. ": " .. tostring(object)
    end
    if not __validObject(object) then
        return nil, "failed to create transient " .. name
    end
    return object
end

local function __objectPath(object)
    local ok, fullName = pcall(function() return object:GetFullName() end)
    if not ok or type(fullName) ~= "string" then
        return nil, "failed to read generated Input Action path"
    end
    local path = string.match(fullName, "^%S+ (.+)$")
    if path == nil or #path == 0 then
        return nil, "unexpected generated Input Action name: " .. fullName
    end
    return path
end

-- Empties a rooted context and keeps it for the next Bind.
local function __recycleContext(record)
    local context = record.context
    record.context = nil
    record.action = nil
    record.trigger_object = nil
    record.context_added = false
    if context ~= nil and __validObject(context) and pcall(function() context:UnmapAll() end) then
        __contextPool[#__contextPool + 1] = context
    end
end

local function __removeContext(state, record)
    if record.context == nil then return true end
    local subsystem = record.context_added and __held(state.subsystem) or nil
    if subsystem ~= nil then
        local ok, removeError = pcall(function()
            subsystem:RemoveMappingContext(record.context, {})
        end)
        if not ok then
            return false, "failed to remove private Input Mapping Context: " .. tostring(removeError)
        end
    end
    __recycleContext(record)
    return true
end

local function __removeRecord(state, record)
    local errors = {}
    for index = #record.native_handles, 1, -1 do
        local nativeHandle = record.native_handles[index]
        local removed, unbindError = bridge.Unbind(nativeHandle)
        local callbackReaped = nativeHandle == record.native_handle and record.callback_failed
        if removed or callbackReaped then
            table.remove(record.native_handles, index)
            if nativeHandle == record.native_handle then record.native_handle = nil end
        else
            errors[#errors + 1] = unbindError or "failed to unbind native action"
        end
    end

    local contextRemoved, contextError = __removeContext(state, record)
    if not contextRemoved then errors[#errors + 1] = contextError end
    if #errors > 0 then return false, table.concat(errors, "; ") end
    return true
end

local function __traceScope(state, stage)
    if not state.debug then return end
    local packedLabel = __packDebugText(state.debug_label)
    local args = { __session, state.scope_id, stage }
    for i = 1, #packedLabel do args[#args + 1] = packedLabel[i] end
    -- Trace output is observational. A logger failure must not change scope
    -- lifecycle, and the native trace writer has its own debugger fallback.
    pcall(UE4SSLuaEventBridge_TraceScope, table.unpack(args, 1, #args))
end

function __scopeMethods:Bind(key, trigger, callback, options)
    local state = __scopeStates[self]
    assert(state ~= nil, "invalid input scope")
    if state.closed then return nil, "input scope is closed" end
    local onGameThread, threadError = __onGameThread("input:Bind")
    if not onGameThread then return nil, threadError end
    assert(type(key) == "string" and #key > 0, "key must be a non-empty Unreal key name")
    assert(trigger == __tap or trigger == __hold, "unsupported Helpers.Trigger constant")
    assert(type(callback) == "function", "callback must be a function")
    assert(options == nil or type(options) == "table", "options must be a table")
    options = options or {}

    local supportedOptions = {
        threshold_seconds = true,
        one_shot = true,
        consume_input = true,
        trigger_when_paused = true,
    }
    for name in pairs(options) do
        assert(supportedOptions[name], "unsupported Bind option: " .. tostring(name))
    end
    if trigger == __tap then
        assert(options.one_shot == nil, "one_shot is only supported by Trigger.Hold")
    end

    if options.threshold_seconds ~= nil then
        assert(type(options.threshold_seconds) == "number"
            and options.threshold_seconds == options.threshold_seconds
            and options.threshold_seconds >= 0
            and options.threshold_seconds < math.huge,
            "threshold_seconds must be a non-negative number")
    end
    if options.one_shot ~= nil then
        assert(type(options.one_shot) == "boolean", "one_shot must be a boolean")
    end
    if options.consume_input ~= nil then
        assert(type(options.consume_input) == "boolean", "consume_input must be a boolean")
    end
    if options.trigger_when_paused ~= nil then
        assert(type(options.trigger_when_paused) == "boolean", "trigger_when_paused must be a boolean")
    end
    if state.debug then
        assert(#key <= 64, "debug-enabled key names must be at most 64 bytes")
    end

    __nextBindingId = __nextBindingId + 1
    local bindingId = __nextBindingId
    local triggerKind = trigger == __tap and 1 or 2

    local subsystem = __held(state.subsystem)
    if subsystem == nil then return nil, "Enhanced Input subsystem is no longer live" end
    local context = table.remove(__contextPool)
    local createError
    if context == nil or not __validObject(context) then
        context, createError = __construct("InputMappingContext", subsystem, __rootedFlags)
        if context == nil then return nil, createError end
    end
    local action
    action, createError = __construct("InputAction", context)
    if action == nil then return nil, createError end
    local triggerObject
    local triggerClass = trigger == __tap and "InputTriggerTap" or "InputTriggerHold"
    triggerObject, createError = __construct(triggerClass, action)
    if triggerObject == nil then return nil, createError end

    local record = {
        context = context,
        action = action,
        trigger_object = triggerObject,
        native_handle = nil,
        native_handles = {},
        context_added = false,
        callback_failed = false,
        binding_id = bindingId,
    }

    local configured, configureError = pcall(function()
        action.ValueType = 0
        action.bConsumeInput = options.consume_input == true
        if options.trigger_when_paused ~= nil then
            action.bTriggerWhenPaused = options.trigger_when_paused
        end

        if trigger == __tap then
            if options.threshold_seconds ~= nil then
                triggerObject.TapReleaseTimeThreshold = options.threshold_seconds
            end
        else
            if options.threshold_seconds ~= nil then
                triggerObject.HoldTimeThreshold = options.threshold_seconds
            end
            if options.one_shot ~= nil then
                triggerObject.bIsOneShot = options.one_shot
            end
        end
        action.Triggers = { triggerObject }
        context:MapKey(action, { KeyName = FName(key) })
    end)
    if not configured then
        local removed, removeError = __removeContext(state, record)
        if not removed then state.orphans[#state.orphans + 1] = record end
        local message = "failed to configure generated Enhanced Input binding: "
            .. tostring(configureError)
        return nil, message .. (removeError and "; " .. removeError or "")
    end

    -- Ask Unreal to initialize its own weak identity; native Bind verifies it.
    local initialized, initializeError = __initializeWeakIdentity(action)
    if not initialized then
        return nil, "failed to initialize generated InputAction weak reference: " .. tostring(initializeError)
    end

    local actionPath, pathError = __objectPath(action)
    if actionPath == nil then
        local removed, removeError = __removeContext(state, record)
        if not removed then state.orphans[#state.orphans + 1] = record end
        return nil, pathError .. (removeError and "; " .. removeError or "")
    end

    local trace = state.debug and {
        scope_id = state.scope_id,
        binding_id = bindingId,
        primary = true,
        trigger_kind = triggerKind,
        key = key,
        label = state.debug_label,
    } or nil

    local handle, bindError = __bindAction(state.target, actionPath, "Triggered", function(event)
        event.key = key
        event.trigger = trigger
        event.scope_id = state.scope_id
        event.binding_id = bindingId
        local callbackOk, callbackError = xpcall(callback, __traceback, event)
        if not callbackOk then
            record.callback_failed = true
            error(callbackError, 0)
        end
    end, trace)
    if handle == nil then
        local removed, removeError = __removeContext(state, record)
        if not removed then state.orphans[#state.orphans + 1] = record end
        return nil, bindError .. (removeError and "; " .. removeError or "")
    end

    record.native_handle = handle
    record.native_handles[#record.native_handles + 1] = handle

    if state.debug then
        for _, phase in ipairs({ "Started", "Completed", "Canceled" }) do
            local observerTrace = {
                scope_id = state.scope_id,
                binding_id = bindingId,
                primary = false,
                trigger_kind = triggerKind,
                key = key,
                label = state.debug_label,
            }
            local observerHandle, observerError = __bindAction(
                state.target, actionPath, phase, function() end, observerTrace)
            if observerHandle == nil then
                local removed, removeError = __removeRecord(state, record)
                if not removed then state.orphans[#state.orphans + 1] = record end
                local message = "failed to create debug " .. phase
                    .. " observer: " .. tostring(observerError)
                return nil, message .. (removeError and "; " .. removeError or "")
            end
            record.native_handles[#record.native_handles + 1] = observerHandle
        end
    end

    record.context_added = true
    local contextAdded, addError = pcall(function()
        subsystem:AddMappingContext(context, state.mapping_priority, {})
    end)
    if not contextAdded then
        local removed, removeError = __removeRecord(state, record)
        if not removed then state.orphans[#state.orphans + 1] = record end
        local message = "failed to add private Input Mapping Context: " .. tostring(addError)
        return nil, message .. (removeError and "; " .. removeError or "")
    end
    state.bindings[handle] = record
    return handle
end

function __scopeMethods:Unbind(handle)
    local state = __scopeStates[self]
    assert(state ~= nil, "invalid input scope")
    assert(type(handle) == "number", "handle must come from input:Bind")
    if state.closed then return false, "input scope is closed" end
    local onGameThread, threadError = __onGameThread("input:Unbind")
    if not onGameThread then return false, threadError end

    local record = state.bindings[handle]
    if record == nil then return false, "binding is not owned by this input scope" end
    local removed, removeError = __removeRecord(state, record)
    if not removed then return false, removeError end
    state.bindings[handle] = nil
    return true
end

-- Re-applies this scope's private contexts, for example after the game cleared
-- its mappings. Adding an applied context again is harmless.
function __scopeMethods:Refresh()
    local state = __scopeStates[self]
    assert(state ~= nil, "invalid input scope")
    if state.closed then return false, "input scope is closed" end
    local onGameThread, threadError = __onGameThread("input:Refresh")
    if not onGameThread then return false, threadError end
    local subsystem = __held(state.subsystem)
    if subsystem == nil then return false, "Enhanced Input subsystem is no longer live" end
    local errors = {}
    for _, record in pairs(state.bindings) do
        if record.context_added and record.context ~= nil then
            local ok, addError = pcall(function()
                subsystem:AddMappingContext(record.context, state.mapping_priority, {})
            end)
            if not ok then errors[#errors + 1] = tostring(addError) end
        end
    end
    if #errors > 0 then return false, "failed to re-apply private Input Mapping Context: " .. table.concat(errors, "; ") end
    return true
end

function __scopeMethods:InspectBindings()
    local state = __scopeStates[self]
    assert(state ~= nil, "invalid input scope")
    if state.closed then return nil, "input scope is closed" end
    return bridge.InspectInputComponent(state.target)
end

function __scopeMethods:Close()
    local state = __scopeStates[self]
    assert(state ~= nil, "invalid input scope")
    if state.closed then return true end
    local onGameThread, threadError = __onGameThread("input:Close")
    if not onGameThread then return false, threadError end

    local errors = {}
    local handles = {}
    for handle in pairs(state.bindings) do handles[#handles + 1] = handle end
    for _, handle in ipairs(handles) do
        local record = state.bindings[handle]
        local removed, removeError = __removeRecord(state, record)
        if removed then
            state.bindings[handle] = nil
        else
            errors[#errors + 1] = removeError
        end
    end

    local remainingOrphans = {}
    for _, record in ipairs(state.orphans) do
        local removed, removeError = __removeRecord(state, record)
        if not removed then
            remainingOrphans[#remainingOrphans + 1] = record
            errors[#errors + 1] = removeError
        end
    end
    state.orphans = remainingOrphans

    if #errors > 0 then return false, table.concat(errors, "; ") end
    local closed, closeError = bridge.CloseInputComponent(state.target)
    if not closed then
        return false, closeError or "failed to close Input Component"
    end

    __traceScope(state, 2)
    state.closed = true
    state.target = nil
    state.subsystem = nil
    __inputScopes[self] = nil
    return true
end

local Helpers = { Trigger = Trigger }

function Helpers.OpenInput(options)
    assert(type(options) == "table", "OpenInput expects an options table")
    local onGameThread, threadError = __onGameThread("Helpers.OpenInput")
    if not onGameThread then return nil, threadError end
    assert(type(options.component_path) == "string" and #options.component_path > 0,
        "component_path must be an exact live UEnhancedInputComponent object path")
    assert(type(options.subsystem_path) == "string" and #options.subsystem_path > 0,
        "subsystem_path must be an exact live UEnhancedInputLocalPlayerSubsystem object path")

    local mappingPriority = options.mapping_priority
    if mappingPriority == nil then mappingPriority = 0 end
    assert(type(mappingPriority) == "number" and mappingPriority % 1 == 0
        and mappingPriority >= -2147483648 and mappingPriority <= 2147483647,
        "mapping_priority must be a 32-bit integer")

    local debugEnabled = options.debug
    if debugEnabled == nil then debugEnabled = false end
    assert(type(debugEnabled) == "boolean", "debug must be a boolean")
    local debugLabel = options.debug_label
    if debugLabel == nil then debugLabel = "input" end
    assert(type(debugLabel) == "string" and #debugLabel > 0 and #debugLabel <= 64,
        "debug_label must be a 1..64 byte string")

    local target, openError = bridge.OpenInputComponent(options.component_path)
    if target == nil then return nil, openError end

    local subsystem, subsystemError = __findObject(
        options.subsystem_path,
        "/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem",
        "subsystem_path")
    if subsystem == nil then
        local closed, closeError = bridge.CloseInputComponent(target)
        if not closed then
            subsystemError = subsystemError .. "; "
                .. (closeError or "failed to close opened component")
        end
        return nil, subsystemError
    end

    local scope = setmetatable({}, __scopeMetatable)
    __nextScopeId = __nextScopeId + 1
    local state = {
        target = target,
        subsystem = __keep(subsystem),
        mapping_priority = mappingPriority,
        scope_id = __nextScopeId,
        debug = debugEnabled,
        debug_label = debugLabel,
        bindings = {},
        orphans = {},
        closed = false,
    }
    __scopeStates[scope] = state
    __inputScopes[scope] = true
    __traceScope(state, 1)
    return scope
end

bridge.Helpers = Helpers

local function __closeHelperScopes()
    local scopes = {}
    for scope in pairs(__inputScopes) do scopes[#scopes + 1] = scope end
    local records = {}
    for _, scope in ipairs(scopes) do
        local state = __scopeStates[scope]
        for _, record in pairs(state.bindings) do
            if record.native_handle ~= nil then records[#records + 1] = record end
        end
    end
    local errors = {}
    for _, scope in ipairs(scopes) do
        local called, closed, closeError = pcall(scope.Close, scope)
        if not called then
            errors[#errors + 1] = tostring(closed)
        elseif not closed then
            errors[#errors + 1] = tostring(closeError or "failed to close helper input scope")
        end
    end
    local nativeRemoved = 0
    for _, record in ipairs(records) do
        if record.native_handle == nil then nativeRemoved = nativeRemoved + 1 end
    end
    if #errors > 0 then return false, table.concat(errors, "; "), nativeRemoved end
    return true, nil, nativeRemoved
end

bridge.UnbindAll = function()
    local onGameThread, threadError = __onGameThread("UnbindAll")
    if not onGameThread then return 0, false, threadError end
    -- Stop delivery even if removal fails; native ownership remains retryable.
    __clearCallbacks()
    local helpersClosed, helperError, helperCount = __closeHelperScopes()
    local removeNative = helpersClosed and UE4SSLuaEventBridge_UnbindAll
        or UE4SSLuaEventBridge_UnbindAllPreserveTargets
    local count, completed, nativeError = removeNative(__session)
    count = count or 0
    if completed and not helpersClosed then
        -- Native bulk removal consumed these handles. Preserve context records
        -- and targets, but never retry an already-removed subscription.
        for scope in pairs(__inputScopes) do
            local state = __scopeStates[scope]
            local function removed(record)
                record.native_handle = nil
                record.native_handles = {}
            end
            for _, record in pairs(state.bindings) do removed(record) end
            for _, record in ipairs(state.orphans) do removed(record) end
        end
    end

    local errors = {}
    if not helpersClosed then
        errors[#errors + 1] = helperError or "failed to close helper input scopes"
    end
    if not completed then
        errors[#errors + 1] = nativeError or "native UnbindAll did not complete"
    end
    if #errors > 0 then
        return helperCount + count, false, table.concat(errors, "; ")
    end
    return helperCount + count, true
end

bridge.UnsubscribeAll = bridge.UnbindAll

-- Used by the native Lua-stop path only when shutdown occurs on the game thread.
__UE4SSLuaEventBridge_CloseHelperScopes = __closeHelperScopes

-- execute_string discards return values. The native stop caller must receive an
-- exception when cleanup fails, rather than silently discarding false,error.
__UE4SSLuaEventBridge_StopHelperScopes = function()
    local called, closed, cleanupError = pcall(__closeHelperScopes)
    if not called then cleanupError = closed end
    if not called or not closed then
        local message = "[UE4SSLuaEventBridge] Lua-stop helper cleanup incomplete: "
            .. tostring(cleanupError or "unknown cleanup failure")
        pcall(print, message)
        error(message, 0)
    end
    return true
end

-- Function hooks (COORDINATOR/LEX/LEX_hooks.md §9). Day-0 stub: the names are
-- the contract; Track L replaces the bodies and assigns __hookDispatch.
-- Native functions this block calls (registered per Lua state by BridgeMod.cpp):
--   UE4SSLuaEventBridge_HooksEnabled() -> bool, reason|nil
--   UE4SSLuaEventBridge_HookFunction(session, functionAddress, classAddress, phaseMask,
--       options..., schema...) -> hookId | nil, reason
--   UE4SSLuaEventBridge_Unhook(session, hookId) -> bool, reason|nil
--   UE4SSLuaEventBridge_HookStatus(session, hookId)
--       -> state, reason|nil, calls, matched, delivered, dropped, reentries
--   UE4SSLuaEventBridge_HookPendingName(session, token, shortClassName) -> bool
--   UE4SSLuaEventBridge_HookPendingForget(session, token)
--   UE4SSLuaEventBridge_HookSchema(functionAddress, schema...) -> flat triplets | nil, reason
--   UE4SSLuaEventBridge_GetHookStats()
--       -> queued, high_water, rejected, deliveryIsTick, ticks, update_calls, entries, slotA, slotB
local __hooksNotImplemented = "function hooks are not implemented in this build"
bridge.Hooks = {
    Hook = function() return nil, __hooksNotImplemented end,
    Unhook = function() return false, __hooksNotImplemented end,
    UnhookAll = function() return 0 end,
    Schema = function() return nil, __hooksNotImplemented end,
    GetStats = function() return nil, __hooksNotImplemented end,
}

-- Called by the native Lua-stop path; Track L makes it unhook every hook the
-- session owns and raise on failure, like __UE4SSLuaEventBridge_StopHelperScopes.
__UE4SSLuaEventBridge_StopHooks = function() return true end

UE4SSLuaEventBridge = bridge

return __dispatch
