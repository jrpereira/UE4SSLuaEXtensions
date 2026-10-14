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
    API_VERSION = 7,
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
            targetDeliveryFaults, loopStart, objectLifetimes, objectLifetimesReason,
            hooks, hooksReason, hooksBp, hooksNative, hooksSync, hooksFrameDelivery =
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
            hooks = hooks == true,
            hooks_reason = hooks ~= true and hooksReason or nil,
            hooks_bp = hooksBp == true,
            hooks_native = hooksNative == true,
            hooks_sync = hooksSync == true,
            hooks_frame_delivery = hooksFrameDelivery == true,
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

-- Function hooks (COORDINATOR/LEX/LEX_hooks.md §9).
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
--
-- Argument encodings chosen by this block where §9.2 leaves them open:
--   * options and every name are sent as `length, word...` with ceil(length / 8)
--     words (__packText), not a fixed word count.
--   * the schema list is `count`, then per property in pre-order:
--     parentIndex (0-based, -1 for a parameter), address, offset, name,
--     enumCount, enum names (an empty name for a gap in the values).
--   * `obj=<address>:<weak token>` identifies one instance from a weak handle.
local __hooksUnavailable = "function hooks are not available in this build"
local __hooksPhaseMask = { pre = 1, post = 2 }
local __hookRecords = {}      -- Lua hook id -> record
local __hookByNative = {}     -- native hook id -> record
local __hookState = setmetatable({}, { __mode = "k" })
local __nextHookId = 0
local __nextPendingToken = 0
local __hooksStopped = false
local __hookRetryQueued = false
local __hookTimerArmed = false
local __hookRetryMs = 1000
local __maxHookStrikes = 32
local __maxHookProperties = 512
local __maxStructDepth = 3
local __classPath = "/Script/CoreUObject.Class"
local __functionPath = "/Script/CoreUObject.Function"

local function __hookLog(message)
    pcall(print, "[UE4SSLuaEventBridge] " .. message)
end

local function __textOf(value)
    if type(value) == "string" then return value end
    if value == nil then return nil end
    local ok, text = pcall(function() return value:ToString() end)
    if ok and type(text) == "string" then return text end
    return nil
end

local function __packVariable(text)
    return __packText(text, (#text + 7) // 8)
end

local function __append(list, values)
    for i = 1, #values do list[#list + 1] = values[i] end
end

local function __isFunctionObject(object)
    local ok, result = pcall(function() return object:IsA(__functionPath) end)
    return ok and result == true
end

local function __objectAddress(object)
    local ok, address = pcall(function() return object:GetAddress() end)
    if ok and __positiveInteger(address) then return address end
    return nil
end

local function __functionPathOf(object)
    local ok, fullName = pcall(function() return object:GetFullName() end)
    if not ok or type(fullName) ~= "string" then return nil end
    return string.match(fullName, "^%S+ (.+)$")
end

-- "/Game/X.X_C:Fn" -> "X_C"
local function __ownerClassName(path)
    return string.match(path, "%.([^%./:]+):[^:%.]+$")
end

local function __enumNamesOf(enum)
    local names, highest = {}, -1
    pcall(function()
        enum:ForEachName(function(name, value)
            name = __textOf(name)
            if name ~= nil and type(value) == "number" and value % 1 == 0
                and value >= 0 and value <= 1023 then
                names[value + 1] = name
                if value > highest then highest = value end
            end
        end)
    end)
    for i = 1, highest + 1 do
        if names[i] == nil then names[i] = "" end
    end
    return names
end

-- Pre-order list of {parent, address, offset, name, enum}; raises on a bad property.
local function __collectProperties(owner, list, parent, depth)
    owner:ForEachProperty(function(property)
        if #list >= __maxHookProperties then
            error("function has more than " .. __maxHookProperties .. " properties", 0)
        end
        local name = __textOf(property:GetFName())
        local offset = property:GetOffset_Internal()
        local address = property:GetAddress()
        if name == nil or #name == 0 then error("a property has no name", 0) end
        if type(offset) ~= "number" or offset % 1 ~= 0 or offset < 0 then
            error("property " .. name .. " has no valid offset", 0)
        end
        if type(address) ~= "number" then error("property " .. name .. " has no address", 0) end
        local entry = { parent = parent, address = address, offset = offset, name = name, enum = {} }
        list[#list + 1] = entry
        local index = #list - 1

        local enumOk, enum = pcall(function() return property:GetEnum() end)
        if enumOk and __validObject(enum) then entry.enum = __enumNamesOf(enum) end
        if depth < __maxStructDepth then
            local structOk, struct = pcall(function() return property:GetStruct() end)
            if structOk and __validObject(struct) then
                __collectProperties(struct, list, index, depth + 1)
            end
        end
    end)
end

local function __schemaArgs(functionObject)
    local list = {}
    local ok, why = pcall(__collectProperties, functionObject, list, -1, 0)
    if not ok then return nil, "failed to enumerate properties: " .. tostring(why) end
    local args = { #list }
    for _, entry in ipairs(list) do
        args[#args + 1] = entry.parent
        args[#args + 1] = entry.address
        args[#args + 1] = entry.offset
        __append(args, __packVariable(entry.name))
        args[#args + 1] = #entry.enum
        for _, enumName in ipairs(entry.enum) do __append(args, __packVariable(enumName)) end
    end
    return args
end

local function __stringList(value, label)
    if type(value) ~= "table" or #value < 1 then
        return nil, label .. " must be a non-empty array of strings"
    end
    for i = 1, #value do
        local item = value[i]
        if type(item) ~= "string" or #item < 1 or #item > 256 or item:find("[;|=]") then
            return nil, label .. " entries must be 1..256 byte strings without ; | ="
        end
    end
    return table.concat(value, "|")
end

local __hookOptionNames = {
    pre = true, post = true, params = true, object = true, object_names = true,
    outer_class = true, outer_class_exact = true, object_paths = true,
    max_queued = true, delivery = true,
}

-- Returns phaseMask, packedOptionsText or nil, message.
local function __validateHookOptions(options)
    if type(options) ~= "table" then return nil, "options must be a table" end
    for name in pairs(options) do
        if not __hookOptionNames[name] then
            return nil, "unsupported Hook option: " .. tostring(name)
        end
    end
    local mask = 0
    if options.pre ~= nil then
        if type(options.pre) ~= "function" then return nil, "pre must be a function" end
        mask = mask | __hooksPhaseMask.pre
    end
    if options.post ~= nil then
        if type(options.post) ~= "function" then return nil, "post must be a function" end
        mask = mask | __hooksPhaseMask.post
    end
    if mask == 0 then return nil, "at least one of pre or post must be a function" end

    local parts = {}
    if options.object_names ~= nil then
        local text, why = __stringList(options.object_names, "object_names")
        if text == nil then return nil, why end
        parts[#parts + 1] = "names=" .. text
    end
    if options.outer_class ~= nil then
        local class = options.outer_class
        if type(class) == "string" then
            local ok, found = pcall(StaticFindObject, class)
            class = ok and found or nil
        end
        local isClass = false
        if class ~= nil and __validObject(class) then
            local ok, result = pcall(function() return class:IsA(__classPath) end)
            isClass = ok and result == true
        end
        if not isClass then return nil, "outer_class must be a live UClass object or path" end
        local address = __objectAddress(class)
        if address == nil then return nil, "outer_class did not provide a valid address" end
        parts[#parts + 1] = "outer=" .. address
    end
    if options.outer_class_exact ~= nil then
        if type(options.outer_class_exact) ~= "boolean" then
            return nil, "outer_class_exact must be a boolean"
        end
        if options.outer_class == nil then return nil, "outer_class_exact needs outer_class" end
        parts[#parts + 1] = "exact=" .. (options.outer_class_exact and 1 or 0)
    end
    if options.object ~= nil then
        local identity = type(options.object) == "table" and options.object.identity
        if type(identity) ~= "function" then
            return nil, "object must be a weak handle from lifetimes.weak"
        end
        local token, address = options.object:identity()
        if token == nil or not __positiveInteger(address) then
            return nil, "object weak handle is released"
        end
        parts[#parts + 1] = "obj=" .. address .. ":" .. token
    end
    if options.object_paths ~= nil then
        if type(options.object_paths) ~= "boolean" then return nil, "object_paths must be a boolean" end
        parts[#parts + 1] = "paths=" .. (options.object_paths and 1 or 0)
    end
    if options.max_queued ~= nil then
        local value = options.max_queued
        if not __positiveInteger(value) or value > 1000000 then
            return nil, "max_queued must be an integer in 1..1000000"
        end
        parts[#parts + 1] = "maxq=" .. value
    end
    if options.params ~= nil then
        local text, why = __stringList(options.params, "params")
        if text == nil then return nil, why end
        parts[#parts + 1] = "params=" .. text
    end
    if options.delivery ~= nil and options.delivery ~= "queued" then
        if options.delivery ~= "sync" then return nil, 'delivery must be "queued" or "sync"' end
        if bridge.GetCapabilities().hooks_sync ~= true then
            return nil, "sync delivery needs hooks_sync"
        end
        parts[#parts + 1] = "sync=1"
    end
    return mask, table.concat(parts, ";")
end

local __HookHandle = {}
__HookHandle.__index = __HookHandle
__HookHandle.__metatable = false

local function __zeroCounters()
    return { calls = 0, matched = 0, delivered = 0, dropped = 0, reentries = 0 }
end

local function __forgetPendingName(record)
    if record.token ~= nil and UE4SSLuaEventBridge_HookPendingForget ~= nil then
        pcall(UE4SSLuaEventBridge_HookPendingForget, __session, record.token)
    end
end

local function __snapshotCounters(record)
    if record.nativeId == nil or UE4SSLuaEventBridge_HookStatus == nil then return end
    local ok, state, reason, calls, matched, delivered, dropped, reentries =
        pcall(UE4SSLuaEventBridge_HookStatus, __session, record.nativeId)
    if ok and state ~= nil then
        record.counters = {
            calls = calls or 0, matched = matched or 0, delivered = delivered or 0,
            dropped = dropped or 0, reentries = reentries or 0,
        }
        return state, reason
    end
end

local function __pendingCount()
    local count = 0
    for _, record in pairs(__hookRecords) do
        if record.state == "pending" then count = count + 1 end
    end
    return count
end

local __retryPending

local function __armHookTimer()
    if __hookTimerArmed or __hooksStopped or ExecuteInGameThreadWithDelay == nil then return end
    __hookTimerArmed = true
    ExecuteInGameThreadWithDelay(__hookRetryMs, function()
        __hookTimerArmed = false
        if __hooksStopped then return end
        __retryPending()
    end)
end

local function __makePending(record, reason)
    record.state = "pending"
    record.reason = reason
    if record.token == nil then
        __nextPendingToken = __nextPendingToken + 1
        record.token = __nextPendingToken
    end
    if record.className ~= nil and UE4SSLuaEventBridge_HookPendingName ~= nil then
        pcall(UE4SSLuaEventBridge_HookPendingName, __session, record.token, record.className)
    end
    __armHookTimer()
end

-- Returns the UFunction, or nil, reason, fatal.
local function __resolveHookFunction(path)
    local ok, object = pcall(StaticFindObject, path)
    if not ok then return nil, "lookup failed: " .. tostring(object), true end
    if not __validObject(object) then return nil, "not found: " .. path, false end
    if not __isFunctionObject(object) then return nil, "target is not a UFunction: " .. path, true end
    return object
end

local function __attachHook(record, functionObject)
    local functionAddress = __objectAddress(functionObject)
    if functionAddress == nil then return nil, "function did not provide a valid address" end
    local outerOk, outer = pcall(function() return functionObject:GetOuter() end)
    if not outerOk or not __validObject(outer) then return nil, "function has no live owner class" end
    local classAddress = __objectAddress(outer)
    if classAddress == nil then return nil, "owner class did not provide a valid address" end
    local schema, schemaError = __schemaArgs(functionObject)
    if schema == nil then return nil, schemaError end

    local args = { __session, functionAddress, classAddress, record.phaseMask }
    __append(args, __packVariable(record.optionsText))
    __append(args, schema)
    local id, why = UE4SSLuaEventBridge_HookFunction(table.unpack(args, 1, #args))
    if id == nil then return nil, why or "native hook registration failed" end
    record.nativeId = id
    record.state = "active"
    record.reason = nil
    record.strikes = 0
    __hookByNative[id] = record
    __forgetPendingName(record)
    return true
end

__retryPending = function()
    __hookRetryQueued = false
    if __hooksStopped then return end
    local pending = {}
    for _, record in pairs(__hookRecords) do
        if record.state == "pending" then pending[#pending + 1] = record end
    end
    table.sort(pending, function(a, b) return a.id < b.id end)
    for _, record in ipairs(pending) do
        local ok, functionObject, why, fatal = pcall(__resolveHookFunction, record.path)
        if not ok then
            record.reason = tostring(functionObject)
        elseif functionObject == nil then
            if fatal then
                record.state, record.reason = "failed", why
                __forgetPendingName(record)
            else
                record.reason = why
            end
        else
            local attached, attachOk, attachError = pcall(__attachHook, record, functionObject)
            if not attached then
                record.reason = tostring(attachOk)
            elseif not attachOk then
                record.reason = attachError
            end
        end
    end
    if __pendingCount() > 0 then __armHookTimer() end
end

local function __scheduleRetry()
    if __hookRetryQueued or __hooksStopped or ExecuteInGameThread == nil then return end
    __hookRetryQueued = true
    ExecuteInGameThread(__retryPending)
end

local function __unhookRecord(record)
    if record.state == "unhooked" then return false, "hook is already unhooked" end
    if record.nativeId ~= nil then
        __snapshotCounters(record)
        local removed, why = UE4SSLuaEventBridge_Unhook(__session, record.nativeId)
        if not removed then return false, why or "native unhook failed" end
        __hookByNative[record.nativeId] = nil
        record.nativeId = nil
    end
    __forgetPendingName(record)
    record.state = "unhooked"
    record.reason = nil
    __hookRecords[record.id] = nil
    return true
end

local function __faultRecord(record, reason)
    if record.state == "unhooked" then return end
    if record.state == "active" then __snapshotCounters(record) end
    record.state = "failed"
    record.reason = reason
    if record.nativeId ~= nil then
        local ok, removed = pcall(UE4SSLuaEventBridge_Unhook, __session, record.nativeId)
        if ok and removed then
            __hookByNative[record.nativeId] = nil
            record.nativeId = nil
        end
    end
    __forgetPendingName(record)
    __hookLog("hook disabled (" .. tostring(record.path or "function") .. "): " .. tostring(reason))
end

local function __newHookRecord(path, options, mask, optionsText)
    __nextHookId = __nextHookId + 1
    local record = {
        id = __nextHookId, path = path, pre = options.pre, post = options.post,
        phaseMask = mask, optionsText = optionsText, state = "pending",
        strikes = 0, counters = __zeroCounters(),
        className = path ~= nil and __ownerClassName(path) or nil,
    }
    local handle = setmetatable({}, __HookHandle)
    record.handle = handle
    __hookState[handle] = record
    return record
end

function __HookHandle:Status()
    local record = __hookState[self]
    if record == nil then return nil end
    local state, reason = record.state, record.reason
    if record.state == "active" then
        local nativeState, nativeReason = __snapshotCounters(record)
        if type(nativeState) == "string" then state, reason = nativeState, nativeReason or reason end
    end
    local counters = record.counters
    return {
        state = state, reason = reason, calls = counters.calls, matched = counters.matched,
        delivered = counters.delivered, dropped = counters.dropped, reentries = counters.reentries,
    }
end

function __HookHandle:Unhook()
    local record = __hookState[self]
    if record == nil then return false, "not a hook handle" end
    return __unhookRecord(record)
end

local function __hooksReady()
    if UE4SSLuaEventBridge_HookFunction == nil or UE4SSLuaEventBridge_HooksEnabled == nil then
        return false, __hooksUnavailable
    end
    local enabled, why = UE4SSLuaEventBridge_HooksEnabled()
    if enabled ~= true then return false, why or "function hooks are disabled" end
    return true
end

local function __hookTarget(target)
    if type(target) == "string" then
        if #target < 1 or #target > 1024 or __ownerClassName(target) == nil then
            return nil, nil, "target path must look like /Package.Class:Function"
        end
        local functionObject, why, fatal = __resolveHookFunction(target)
        if functionObject == nil and fatal then return nil, nil, why end
        return functionObject, target
    end
    if target == nil or not __validObject(target) then
        return nil, nil, "target must be a path string or a live UFunction"
    end
    if not __isFunctionObject(target) then return nil, nil, "target is not a UFunction" end
    return target, __functionPathOf(target)
end

local Hooks = {}

function Hooks.Hook(target, options)
    local ready, readyError = __hooksReady()
    if not ready then return nil, readyError end
    if __hooksStopped then return nil, "the hook session is stopped" end
    if UE4SSLuaEventBridge_IsInGameThread() ~= true then
        return nil, "Hooks.Hook must run on the Unreal game thread"
    end
    local mask, optionsText = __validateHookOptions(options)
    if mask == nil then return nil, optionsText end

    local functionObject, path, targetError = __hookTarget(target)
    if targetError ~= nil then return nil, targetError end
    local record = __newHookRecord(path, options, mask, optionsText)
    if functionObject == nil then
        __hookRecords[record.id] = record
        __makePending(record, "waiting for " .. path)
        return record.handle
    end
    local attached, why = __attachHook(record, functionObject)
    if not attached then
        __hookState[record.handle] = nil
        return nil, why
    end
    __hookRecords[record.id] = record
    return record.handle
end

function Hooks.Unhook(handle)
    local record = type(handle) == "table" and __hookState[handle] or nil
    if record == nil then return false, "handle must be returned by Hooks.Hook" end
    return __unhookRecord(record)
end

function Hooks.UnhookAll()
    local records = {}
    for _, record in pairs(__hookRecords) do records[#records + 1] = record end
    table.sort(records, function(a, b) return a.id < b.id end)
    local count, errors = 0, {}
    for _, record in ipairs(records) do
        local called, removed, why = pcall(__unhookRecord, record)
        if called and removed then
            count = count + 1
        else
            errors[#errors + 1] = tostring(called and why or removed or "failed to unhook")
        end
    end
    if #errors > 0 then return count, false, table.concat(errors, "; ") end
    return count, true
end

function Hooks.GetStats()
    if UE4SSLuaEventBridge_GetHookStats == nil then return nil, __hooksUnavailable end
    local queued, highWater, rejected, isTick, ticks, updateCalls, entries, slotA, slotB =
        UE4SSLuaEventBridge_GetHookStats()
    if queued == nil then return nil, "bridge unavailable" end
    return {
        queued = queued, high_water = highWater, rejected = rejected,
        delivery = isTick and "tick" or "loop", ticks = ticks, update_calls = updateCalls,
        entries = entries, slots = { a = slotA, b = slotB }, pending = __pendingCount(),
    }
end

-- Flat (name, kind, meta) triplets -> tree. meta is "offset:size:dim:flags".
function Hooks.Schema(target)
    if UE4SSLuaEventBridge_HookSchema == nil then return nil, __hooksUnavailable end
    if UE4SSLuaEventBridge_IsInGameThread() ~= true then
        return nil, "Hooks.Schema must run on the Unreal game thread"
    end
    local functionObject, _, targetError = __hookTarget(target)
    if targetError ~= nil then return nil, targetError end
    if functionObject == nil then return nil, "target is not resolved: " .. tostring(target) end
    local functionAddress = __objectAddress(functionObject)
    if functionAddress == nil then return nil, "function did not provide a valid address" end
    local schema, schemaError = __schemaArgs(functionObject)
    if schema == nil then return nil, schemaError end
    local args = { functionAddress }
    __append(args, schema)
    local flat = table.pack(UE4SSLuaEventBridge_HookSchema(table.unpack(args, 1, #args)))
    if flat.n == 0 then return {} end -- a function without properties
    if flat[1] == nil then return nil, flat[2] or "schema unavailable" end

    local root, byName = {}, {}
    for i = 1, flat.n - 2, 3 do
        local name, kind, meta = flat[i], flat[i + 1], flat[i + 2]
        local numbers = {}
        if type(meta) == "number" then
            numbers[1] = meta
        elseif type(meta) == "string" then
            for piece in meta:gmatch("[^:|,;]+") do numbers[#numbers + 1] = tonumber(piece) end
        end
        local parentName, leaf = name:match("^(.*)%.([^%.]+)$")
        local node = {
            name = leaf or name, kind = kind, offset = numbers[1], size = numbers[2],
            dim = numbers[3], flags = numbers[4], fields = {},
        }
        byName[name] = node
        local siblings = parentName ~= nil and byName[parentName] and byName[parentName].fields or root
        siblings[#siblings + 1] = node
    end
    return root
end

bridge.Hooks = Hooks

-- Dispatcher: -10 event, -5 fault, -4 back to pending, -3 wake-up.
local function __decodeHookValue(tag, value)
    if tag == "b" then return value == true or value == 1 end
    if tag == "i" then
        local number = tonumber(value)
        return number and (math.tointeger(number) or number) or nil
    end
    if tag == "n" then return tonumber(value) end
    if tag == "s" then return value end
    if tag == "e" then
        if type(value) == "number" then return { value = math.tointeger(value) or value } end
        local number, name = tostring(value):match("^(%-?%d+)|?(.*)$")
        if number == nil then return nil end
        return { value = math.tointeger(tonumber(number)), name = name ~= "" and name or nil }
    end
    if tag == "o" then
        local address, index, serial, rest = tostring(value):match("^(%-?%d+):(%-?%d+):(%-?%d+)(.*)$")
        if address == nil then return nil end
        if tonumber(address) == 0 then return nil end -- null object
        local object = {
            address = math.tointeger(tonumber(address)), index = math.tointeger(tonumber(index)),
            serial = math.tointeger(tonumber(serial)),
        }
        if rest:sub(1, 1) == ":" and #rest > 1 then object.path = rest:sub(2) end
        return object
    end
    return nil
end

-- "S.Arr[2]" -> root.S.Arr[3]: segments may carry a 0-based [i] element suffix.
local function __setNested(root, dotted, value)
    local node = root
    local segments = {}
    for segment in dotted:gmatch("[^.]+") do segments[#segments + 1] = segment end
    for i, segment in ipairs(segments) do
        local base, element = segment:match("^(.-)%[(%d+)%]$")
        local key, slot = base or segment, element and (tonumber(element) + 1) or nil
        local last = i == #segments
        local container = node
        if slot ~= nil then
            if type(node[key]) ~= "table" then node[key] = {} end
            container, key = node[key], slot
        end
        if last then
            container[key] = value
        else
            if type(container[key]) ~= "table" then container[key] = {} end
            node = container[key]
        end
    end
end

local function __deliverHookEvent(
    nativeId, sequence, phase, gameThread, objAddress, objIndex, objSerial, objPath,
    paramsAvailable, count, ...)
    local record = __hookByNative[nativeId]
    if record == nil or record.state ~= "active" then return end
    local postPhase = phase == 2 or phase == "post"
    local callback = postPhase and record.post or record.pre
    if callback == nil then return end

    local available = paramsAvailable == true or paramsAvailable == 1
    local call = {
        hook = record.handle, phase = postPhase and "post" or "pre", sequence = sequence,
        game_thread = gameThread == true or gameThread == 1, function_path = record.path,
        params_available = available,
    }
    if type(objAddress) == "number" and objAddress ~= 0 then
        call.object = { address = objAddress, index = objIndex, serial = objSerial, path = objPath }
    end
    if available then call.params = {} end
    if postPhase then call.out = {} end

    local values = table.pack(...)
    for i = 1, (count or 0) do
        local base = (i - 1) * 3
        local name, value = values[base + 1], __decodeHookValue(values[base + 2], values[base + 3])
        if type(name) == "string" then
            if name == "@frame" then
                call.frame = value
            elseif name == "@age" then
                if type(value) == "number" then call.clock = os.clock() - value end
            elseif name == "ret" then
                if postPhase then call.result = value end
            elseif name:sub(1, 4) == "out:" then
                if postPhase and value ~= nil then __setNested(call.out, name:sub(5), value) end
            elseif available and value ~= nil then
                __setNested(call.params, name, value)
            end
        end
    end

    local ok, callbackError = xpcall(callback, __traceback, call)
    if ok then
        record.strikes = 0
        return
    end
    record.strikes = record.strikes + 1
    __hookLog("hook callback failed (" .. tostring(record.path) .. "): " .. tostring(callbackError))
    if record.strikes >= __maxHookStrikes then
        __faultRecord(record, "callback failed " .. __maxHookStrikes .. " times in a row: "
            .. tostring(callbackError))
    end
end

__hookDispatch = function(code, hookId, ...)
    if code == -10 then
        return __deliverHookEvent(hookId, ...)
    elseif code == -3 then
        return __scheduleRetry()
    elseif code == -5 then
        local record = __hookByNative[hookId]
        if record ~= nil then __faultRecord(record, tostring((...) or "hook fault")) end
    elseif code == -4 then
        local record = __hookByNative[hookId]
        if record == nil or record.state ~= "active" then return end
        __snapshotCounters(record)
        pcall(UE4SSLuaEventBridge_Unhook, __session, hookId)
        __hookByNative[hookId] = nil
        record.nativeId = nil
        if record.path == nil or record.className == nil then
            record.state, record.reason = "failed", "function was deleted and has no path to retry"
            return
        end
        __makePending(record, tostring((...) or "function was deleted"))
    end
end

-- Called by the native Lua-stop path: unhooks everything the session owns and
-- raises on failure, like __UE4SSLuaEventBridge_StopHelperScopes.
__UE4SSLuaEventBridge_StopHooks = function()
    __hooksStopped = true
    local called, count, completed, stopError = pcall(Hooks.UnhookAll)
    if not called then stopError = count end
    if not called or not completed then
        local message = "[UE4SSLuaEventBridge] Lua-stop hook cleanup incomplete: "
            .. tostring(stopError or "unknown cleanup failure")
        pcall(print, message)
        error(message, 0)
    end
    return true
end

UE4SSLuaEventBridge = bridge

return __dispatch
