-- UE4SSLuaEventBridge load-time verifications. Ships as Scripts/main.lua in the
-- bridge's own mod folder and runs only when the MCS setting "Enable load-time
-- verifications" is on (settings.ini: [Verifications] LoadTime=1); it is off by
-- default. Results go to UE4SS.log as PASS, FAIL, SKIP and NOTE lines.
--
-- Three stages, each with its own SUMMARY line:
--   load    API surface and argument checks, while the mod loads
--   engine  loop start, native handles and object lifetimes, on the game thread
--   player  input components, bindings and helpers, once a player controller
--           with a pawn exists (load a save). Tap or hold NumPad 9 during the
--           listening window to also check delivery; that part is optional.

local TAG = "[UE4SSLuaEventBridge verify] "
local PLAYER_WAIT_SECONDS = 900
local LISTEN_SECONDS = 20
local LISTEN_KEY = "NumPadNine"

local function log(text)
    print(TAG .. text .. "\n")
end

-- Settings ------------------------------------------------------------------------

local function modFolder()
    local source = debug.getinfo(1, "S").source
    local scripts = source:gsub("^@", ""):match("^(.*)[/\\]")
    return scripts and scripts:match("^(.*)[/\\][^/\\]+$")
end

-- Reads [Verifications] LoadTime from the mod's settings.ini. A missing file or
-- an unreadable value means off: configuration never blocks the bridge.
local function verificationsEnabled()
    local folder = modFolder()
    local file = folder and io.open(folder .. "/settings.ini", "r")
    if not file then return false end
    local section, value
    for line in file:lines() do
        line = line:match("^%s*(.-)%s*$")
        local name = line:match("^%[(.-)%]$")
        if name then
            section = name:lower()
        elseif section == "verifications" then
            local key, raw = line:match("^([^=]-)%s*=%s*(.*)$")
            if key and key:lower() == "loadtime" then value = raw end
        end
    end
    file:close()
    return tonumber(value) == 1
end

if not verificationsEnabled() then return end

-- Harness -------------------------------------------------------------------------

local counts = { passed = 0, failed = 0, skipped = 0 }

local function pass(name)
    counts.passed = counts.passed + 1
    log("PASS " .. name)
end

local function fail(name, detail)
    counts.failed = counts.failed + 1
    log("FAIL " .. name .. (detail ~= nil and (": " .. tostring(detail)) or ""))
end

local function skip(name, why)
    counts.skipped = counts.skipped + 1
    log("SKIP " .. name .. ": " .. why)
end

local function check(name, condition, detail)
    if condition then pass(name) else fail(name, detail) end
    return condition
end

local function group(name, fn)
    local ok, err = xpcall(fn, debug.traceback)
    if not ok then fail(name .. " (aborted)", err) end
end

-- A call that must fail with nil or false and an error message.
local function rejects(name, value, message)
    if (value == nil or value == false) and type(message) == "string" then
        pass(name .. " rejected")
    else
        fail(name, "expected nil/false and a message; got " .. tostring(value) .. ", " .. tostring(message))
    end
end

local function raises(name, fn, ...)
    local done, err = pcall(fn, ...)
    check(name .. " raises", not done, "no error")
    return err
end

local function summary(stage)
    log(string.format("SUMMARY %s passed=%d failed=%d skipped=%d",
        stage, counts.passed, counts.failed, counts.skipped))
end

local function isValid(object)
    if object == nil then return false end
    local ok, valid = pcall(function() return object:IsValid() end)
    return ok and valid == true
end

local function fullName(object)
    local ok, name = pcall(function() return object:GetFullName() end)
    return ok and type(name) == "string" and name or nil
end

-- "Class /Path/To.Object" -> "/Path/To.Object"
local function objectPath(object)
    local name = fullName(object)
    return name and name:match("^%S+ (.+)$")
end

local function address(object)
    local ok, value = pcall(function() return object:GetAddress() end)
    return ok and value or nil
end

-- Load stage ------------------------------------------------------------------------

-- Everything below runs inside one protected call: a failure the groups
-- don't catch is reported, never raised out of main.lua.
local function main()
    local Bridge = UE4SSLuaEventBridge
    if type(Bridge) ~= "table" then
        fail("UE4SSLuaEventBridge global", "missing; the native bridge did not start for this Lua state")
        summary("load")
        return
    end
    log("load-time verifications enabled; bridge " .. tostring(Bridge.GetVersion and Bridge.GetVersion()))

    local caps = {}

    group("version and capabilities", function()
        local version = Bridge.GetVersion()
        check("GetVersion is x.y.z", type(version) == "string" and version:match("^%d+%.%d+%.%d+$") ~= nil, version)
        check("API_VERSION is at least 6", type(Bridge.API_VERSION) == "number" and Bridge.API_VERSION >= 6,
            Bridge.API_VERSION)
        caps = Bridge.GetCapabilities()
        check("GetCapabilities returns a table", type(caps) == "table")
        check("capabilities api matches API_VERSION", caps.api == Bridge.API_VERSION, caps.api)
        for _, key in ipairs({ "enhanced_input", "explicit_target", "helpers", "dynamic_input", "trigger_tap",
            "trigger_hold", "detailed_errors", "debug_tracing", "binding_snapshot", "target_delivery_faults",
            "loop_start" }) do
            check("capability " .. key, caps[key] == true, caps[key])
        end
        check("target_ue4ss_commit", caps.target_ue4ss_commit == "97b7e501", caps.target_ue4ss_commit)
        check("weak_handles and loss_opt_in follow object_lifetimes",
            caps.weak_handles == caps.object_lifetimes and caps.loss_opt_in == caps.object_lifetimes)
        if caps.object_lifetimes ~= true then
            log("NOTE object_lifetimes is off at load: " .. tostring(caps.object_lifetimes_reason)
                .. " (the probe runs at Unreal initialization; rechecked in the engine stage)")
        end

        local stats = Bridge.GetDispatchStats()
        check("GetDispatchStats fields", type(stats) == "table" and type(stats.queued) == "number"
            and type(stats.queue_high_water) == "number" and type(stats.rejected_events) == "number"
            and type(stats.rejected_traces) == "number")
        if stats and (stats.rejected_events or 0) > 0 then
            log("NOTE rejected_events=" .. stats.rejected_events .. ": input was lost earlier in this process")
        end
    end)

    group("aliases and helpers", function()
        check("Unsubscribe aliases Unbind", Bridge.Unsubscribe == Bridge.Unbind)
        check("UnsubscribeAll aliases UnbindAll", Bridge.UnsubscribeAll == Bridge.UnbindAll)
        check("SubscribeEnhancedInput exists", type(Bridge.SubscribeEnhancedInput) == "function")
        local Helpers = Bridge.Helpers
        check("Helpers table", type(Helpers) == "table" and type(Helpers.OpenInput) == "function")
        local Trigger = Helpers.Trigger
        check("Trigger.Tap and Trigger.Hold are distinct", Trigger.Tap ~= nil and Trigger.Hold ~= nil
            and Trigger.Tap ~= Trigger.Hold)
        raises("assigning Helpers.Trigger", function() Trigger.Press = {} end)
        local listed = 0
        for _ in pairs(Trigger) do listed = listed + 1 end
        check("pairs(Trigger) lists two triggers", listed == 2, listed)
        raises("Helpers.OpenInput without options", Helpers.OpenInput)
    end)

    group("argument validation", function()
        rejects("OpenInputComponent(number)", Bridge.OpenInputComponent(5))
        rejects("OpenInputComponent(empty)", Bridge.OpenInputComponent(""))
        rejects("OpenInputComponent(513 bytes)", Bridge.OpenInputComponent(string.rep("a", 513)))
        rejects("BindAction(target 0)", Bridge.BindAction(0, "/Game/IA.IA", "Triggered", function() end))
        rejects("BindAction(bad event)", Bridge.BindAction(1, "/Game/IA.IA", "Pressed", function() end))
        rejects("BindAction(no callback)", Bridge.BindAction(1, "/Game/IA.IA", "Triggered", nil))
        rejects("BindAction(empty action)", Bridge.BindAction(1, "", "Triggered", function() end))
        rejects("SubscribeEnhancedInput(non-table spec)", Bridge.SubscribeEnhancedInput(5, function() end))
        rejects("Unbind(string)", Bridge.Unbind("1"))
        rejects("CloseInputComponent(-1)", Bridge.CloseInputComponent(-1))
        rejects("InspectInputComponent(0)", Bridge.InspectInputComponent(0))
        rejects("IsTargetDeliveryValid(1.5)", Bridge.IsTargetDeliveryValid(1.5))
        rejects("SetTargetDeliveryFaultHandler(no callback)", Bridge.SetTargetDeliveryFaultHandler(1, nil))
        rejects("lifetimes.captureAddress(0)", Bridge.lifetimes.captureAddress(0))
        rejects("lifetimes.captureObject(nil)", Bridge.lifetimes.captureObject(nil))
        check("lifetimes.valid with a bad token is false", Bridge.lifetimes.valid(1, "abc") == false)
        check("lifetimes.release with a bad token is false", Bridge.lifetimes.release("0") == false)
        raises("onLoopStart(non-function)", Bridge.onLoopStart, 5)
    end)

    -- Engine stage ---------------------------------------------------------------------

    local playerStage

    local function engineStage()
        group("handles nobody owns", function()
            rejects("Unbind(unknown handle)", Bridge.Unbind(999999))
            rejects("CloseInputComponent(unknown target)", Bridge.CloseInputComponent(999999))
            rejects("InspectInputComponent(unknown target)", Bridge.InspectInputComponent(999999))
            rejects("SetTargetDeliveryFaultHandler(unknown target)",
                Bridge.SetTargetDeliveryFaultHandler(999999, function() end))
            check("IsTargetDeliveryValid(unknown target) is false", Bridge.IsTargetDeliveryValid(999999) == false)
            rejects("OpenInputComponent(missing object)", Bridge.OpenInputComponent("/Script/Nowhere.Nothing:Missing"))
            rejects("Helpers.OpenInput(missing objects)", Bridge.Helpers.OpenInput({
                component_path = "/Script/Nowhere.Nothing:Missing",
                subsystem_path = "/Script/Nowhere.Nothing:Missing",
            }))
            local count, completed = Bridge.UnbindAll()
            check("UnbindAll with nothing bound", count == 0 and completed == true,
                tostring(count) .. " " .. tostring(completed))
        end)

        group("object lifetimes", function()
            caps = Bridge.GetCapabilities()
            if caps.object_lifetimes ~= true then
                skip("object lifetimes", "object_lifetimes is off: " .. tostring(caps.object_lifetimes_reason))
                return
            end
            local L = Bridge.lifetimes
            check("reportLosses opts in", L.reportLosses() == true)
            -- A class default object lives for the whole process.
            local object = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
            if not isValid(object) then
                skip("object lifetimes", "KismetSystemLibrary default object not found")
                return
            end
            local where = address(object)
            local token, why = L.captureObject(object)
            if not check("captureObject returns a decimal token", type(token) == "string"
                and token:match("^[1-9]%d*$") ~= nil, why) then return end
            check("capturing again returns the same token", L.captureObject(object) == token)
            check("captureAddress returns the same token", L.captureAddress(where) == token)
            check("valid(address, token)", L.valid(where, token) == true)
            check("valid with another address is false", L.valid(where + 8, token) == false)
            check("release (1 of 3)", L.release(token) == true)
            check("release (2 of 3)", L.release(token) == true)
            check("still valid while captured", L.valid(where, token) == true)
            check("release (3 of 3)", L.release(token) == true)
            check("the last release ends the observation", L.valid(where, token) == false)
            local lost = L.takeLost()
            check("a release is not a loss", lost == nil, lost)

            local handle, weakError = L.weak(object)
            if check("weak returns a handle", handle ~= nil, weakError) then
                local live = handle:get()
                check("weak:get returns the live object", live ~= nil and address(live) == where)
                local weakToken, weakAddress = handle:identity()
                check("weak:identity", type(weakToken) == "string" and weakAddress == where)
                handle:release()
                check("weak:get after release is nil", handle:get() == nil)
                check("weak:identity survives release", handle:identity() == weakToken)
            end
        end)

        group("coroutine call", function()
            -- UE4SS refuses native calls from a Lua thread it didn't create. The
            -- bridge must turn that into an error, not a crash.
            log("NOTE calling a native from a mod coroutine; a crash right after this line is a finding")
            local outcome = { coroutine.wrap(function()
                return pcall(Bridge.IsTargetDeliveryValid, 999999)
            end)() }
            pass("native call from a mod coroutine returned (" .. tostring(outcome[1]) .. ", "
                .. tostring(outcome[2]) .. ")")
        end)

        summary("engine")
        playerStage()
    end

    local loopStarted = false
    local cancelled = false
    group("onLoopStart", function()
        local cancel = Bridge.onLoopStart(function() cancelled = true end)
        check("cancel before delivery", cancel() == true)
        check("cancel twice", cancel() == false)
        Bridge.onLoopStart(function()
            loopStarted = true
            check("onLoopStart callback ran", true)
            check("a cancelled callback did not run", cancelled == false)
            ExecuteInGameThread(function() group("engine stage", engineStage) end)
        end)
    end)

    summary("load")

    -- Player stage ----------------------------------------------------------------------

    -- Finds the local player's input component and Enhanced Input subsystem.
    local function findPlayerInput()
        for _, controller in ipairs(FindAllOf("PlayerController") or {}) do
            local name = fullName(controller) or ""
            if isValid(controller) and not name:find("Default__", 1, true) then
                local pawn = controller.Pawn
                local component = isValid(pawn) and pawn.InputComponent or nil
                local player = controller.Player
                if isValid(component) and isValid(player) then
                    local playerPath = objectPath(player)
                    local fallback, count = nil, 0
                    for _, subsystem in ipairs(FindAllOf("EnhancedInputLocalPlayerSubsystem") or {}) do
                        local subsystemName = fullName(subsystem) or ""
                        if isValid(subsystem) and not subsystemName:find("Default__", 1, true) then
                            count = count + 1
                            fallback = subsystem
                            if playerPath and subsystemName:find(playerPath .. ".", 1, true) then
                                return component, subsystem
                            end
                        end
                    end
                    if count == 1 then return component, fallback end
                end
            end
        end
    end

    local function findInputAction()
        for _, action in ipairs(FindAllOf("InputAction") or {}) do
            local name = fullName(action) or ""
            if isValid(action) and not name:find("Default__", 1, true) and not name:find("/Engine/Transient", 1, true) then
                return action
            end
        end
    end

    local listening = { tap = 0, hold = 0 }

    local function playerChecks(component, subsystem)
        local componentPath, subsystemPath = objectPath(component), objectPath(subsystem)
        log("NOTE player found: " .. tostring(componentPath))

        group("primitive target", function()
            local target, why = Bridge.OpenInputComponent(componentPath)
            if not check("OpenInputComponent", type(target) == "number" and target > 0, why) then return end
            check("IsTargetDeliveryValid before opting in is false", Bridge.IsTargetDeliveryValid(target) == false)
            check("SetTargetDeliveryFaultHandler", Bridge.SetTargetDeliveryFaultHandler(target, function(fault)
                log("NOTE delivery fault " .. tostring(fault.reason))
            end) == true)
            rejects("SetTargetDeliveryFaultHandler twice",
                Bridge.SetTargetDeliveryFaultHandler(target, function() end))
            check("IsTargetDeliveryValid after opting in", Bridge.IsTargetDeliveryValid(target) == true)

            local action = findInputAction()
            if action then
                local actionPath = objectPath(action)
                local handle, bindError = Bridge.BindAction(target, actionPath, "Triggered", function() end)
                if check("BindAction on " .. tostring(actionPath), type(handle) == "number", bindError) then
                    local snapshot = Bridge.InspectInputComponent(target)
                    check("InspectInputComponent lists the binding", type(snapshot) == "string"
                        and snapshot:find("snapshot_end", 1, true) ~= nil, snapshot and #snapshot)
                    check("Unbind", Bridge.Unbind(handle) == true)
                    rejects("Unbind twice", Bridge.Unbind(handle))
                end
                local spec = Bridge.SubscribeEnhancedInput({ target = target, action = actionPath, event = "Started" },
                    function() end)
                if check("SubscribeEnhancedInput", type(spec) == "number") then
                    check("Unsubscribe", Bridge.Unsubscribe(spec) == true)
                end
            else
                skip("BindAction", "no InputAction asset is loaded")
            end
            check("CloseInputComponent", Bridge.CloseInputComponent(target) == true)
            rejects("CloseInputComponent twice", Bridge.CloseInputComponent(target))
            check("a closed target is not valid", Bridge.IsTargetDeliveryValid(target) == false)
        end)

        group("component weak handle", function()
            if Bridge.GetCapabilities().object_lifetimes ~= true then
                skip("component weak handle", "object_lifetimes is off")
                return
            end
            local handle = Bridge.lifetimes.weak(component)
            check("weak handle on the input component", handle ~= nil and handle:get() ~= nil)
            if handle then handle:release() end
        end)

        local input
        group("helper scope", function()
            local Trigger = Bridge.Helpers.Trigger
            local why
            input, why = Bridge.Helpers.OpenInput({
                component_path = componentPath,
                subsystem_path = subsystemPath,
                debug_label = "LEB verify",
            })
            if not check("Helpers.OpenInput", input ~= nil, why) then return end
            raises("Bind with an unsupported option", input.Bind, input, LISTEN_KEY, Trigger.Tap, function() end,
                { bogus = true })
            raises("Tap with one_shot", input.Bind, input, LISTEN_KEY, Trigger.Tap, function() end, { one_shot = true })
            raises("Bind with an unknown trigger", input.Bind, input, LISTEN_KEY, {}, function() end)
            raises("assigning a scope field", function() input.extra = true end)

            local tap, tapError = input:Bind(LISTEN_KEY, Trigger.Tap, function(event)
                listening.tap = listening.tap + 1
                log("NOTE tap delivered: key=" .. tostring(event.key) .. " phase=" .. tostring(event.phase))
            end, { threshold_seconds = 0.2 })
            check("Bind Tap", tap ~= nil, tapError)
            local hold, holdError = input:Bind(LISTEN_KEY, Trigger.Hold, function(event)
                listening.hold = listening.hold + 1
                log(string.format("NOTE hold delivered after %.0f ms", (event.elapsed_processed or 0) * 1000))
            end, { threshold_seconds = 0.5, one_shot = true })
            check("Bind Hold", hold ~= nil, holdError)
            local extra = input:Bind(LISTEN_KEY, Trigger.Hold, function() end, { threshold_seconds = 2 })
            if check("Bind a second Hold", extra ~= nil) then
                check("Unbind a helper binding", input:Unbind(extra) == true)
                rejects("Unbind it twice", input:Unbind(extra))
            end
            local snapshot = input:InspectBindings()
            check("InspectBindings", type(snapshot) == "string" and snapshot:find("snapshot_end", 1, true) ~= nil)
            check("Refresh", input:Refresh() == true)
        end)

        if input == nil then
            summary("player")
            return
        end
        log("NOTE listening for " .. LISTEN_SECONDS .. " s: tap and hold NumPad 9 to check delivery (optional)")
        local waited = 0
        LoopAsync(1000, function()
            waited = waited + 1
            if waited < LISTEN_SECONDS then return false end
            ExecuteInGameThread(function()
                group("helper delivery and close", function()
                    if listening.tap > 0 then pass("Tap delivered") else skip("Tap delivery", "no tap observed") end
                    if listening.hold > 0 then pass("Hold delivered") else skip("Hold delivery", "no hold observed") end
                    check("Close", input:Close() == true)
                    check("Close twice", input:Close() == true)
                    local after, why = input:Bind(LISTEN_KEY, Bridge.Helpers.Trigger.Tap, function() end)
                    check("Bind after Close", after == nil and why == "input scope is closed", why)
                    local count, completed = Bridge.UnbindAll()
                    check("UnbindAll leaves nothing behind", count == 0 and completed == true,
                        tostring(count) .. " " .. tostring(completed))
                    local stats = Bridge.GetDispatchStats()
                    check("no rejected events", stats.rejected_events == 0, stats.rejected_events)
                end)
                summary("player")
            end)
            return true
        end)
    end

    playerStage = function()
        log("NOTE waiting up to " .. PLAYER_WAIT_SECONDS .. " s for a player with a pawn (load a save)")
        local waited, searching, found = 0, false, false
        LoopAsync(1000, function()
            if found then return true end
            waited = waited + 1
            if waited > PLAYER_WAIT_SECONDS then
                skip("player stage", "no player with a pawn within " .. PLAYER_WAIT_SECONDS .. " s")
                summary("player")
                return true
            end
            if not searching then
                searching = true
                ExecuteInGameThread(function()
                    local component, subsystem = findPlayerInput()
                    searching = false
                    if component and not found then
                        found = true
                        group("player stage", function() playerChecks(component, subsystem) end)
                    end
                end)
            end
            return false
        end)
    end

    -- If the loop never starts, say so instead of staying silent.
    LoopAsync(30000, function()
        if not loopStarted then
            fail("onLoopStart callback", "not delivered within 30 s")
            summary("engine")
        end
        return true
    end)

end

local finished, err = xpcall(main, debug.traceback)
if not finished then
    fail("verifications (aborted)", err)
    summary("load")
end
