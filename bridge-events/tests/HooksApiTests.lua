-- Offline tests for the Hooks block of bridge_api.lua: the real Lua API against
-- fake natives and fake UE4SS objects. They do not exercise the native hook core.
package.path = "shared/tests/?.lua;" .. package.path
local Kit = require("TestKit")
local expect = Kit.expect
local suite = Kit.suite("HooksApiTests")

local FUNCTION_CLASS = "/Script/CoreUObject.Function"
local CLASS_CLASS = "/Script/CoreUObject.Class"

local function unpackText(args, i)
    local length = args[i]
    local words = (length + 7) // 8
    local bytes = {}
    for n = 1, length do
        local word = args[i + 1 + (n - 1) // 8]
        bytes[n] = string.char((word >> (((n - 1) % 8) * 8)) & 0xFF)
    end
    return table.concat(bytes), i + 1 + words
end

-- Fake UE objects ------------------------------------------------------------
local nextAddress = 0x1000
local function address() nextAddress = nextAddress + 0x10; return nextAddress end

local function fname(text) return { ToString = function() return text end } end

local function property(name, offset, extra)
    local p = { name = name, offset = offset, address = address() }
    for k, v in pairs(extra or {}) do p[k] = v end
    function p:GetFName() return fname(self.name) end
    function p:GetOffset_Internal() return self.offset end
    function p:GetAddress() return self.address end
    if p.struct then function p:GetStruct() return self.struct end end
    if p.enum then function p:GetEnum() return self.enum end end
    return p
end

local function propertyOwner(properties)
    local o = { properties = properties or {} }
    function o:IsValid() return true end
    function o:ForEachProperty(callback)
        for _, p in ipairs(self.properties) do
            if callback(p) then return end
        end
    end
    return o
end

local function enumOf(names)
    local e = { names = names }
    function e:IsValid() return true end
    function e:ForEachName(callback)
        for _, entry in ipairs(self.names) do callback(fname(entry[1]), entry[2]) end
    end
    return e
end

local function classObject(path)
    local c = { path = path, address = address() }
    function c:IsValid() return true end
    function c:IsA(p) return p == CLASS_CLASS end
    function c:GetAddress() return self.address end
    function c:GetFullName() return "Class " .. self.path end
    return c
end

local function functionObject(path, owner, properties)
    local f = propertyOwner(properties)
    f.path, f.owner, f.address = path, owner, address()
    function f:IsA(p) return p == FUNCTION_CLASS end
    function f:GetAddress() return self.address end
    function f:GetOuter() return self.owner end
    function f:GetFullName() return "Function " .. self.path end
    return f
end

-- Fixture ----------------------------------------------------------------------
local function fixture(options)
    options = options or {}
    local w = {
        objects = {}, printed = {}, queue = {}, timers = {}, natives = {}, hooks = {},
        pendingNames = {}, forgotten = {}, unhooked = {}, gameThread = true,
        nextHook = 0, refuse = nil, unhookFails = false, enabled = true,
        capabilities = options.capabilities or { 6, true, true, true, true, true, true, true, true,
            "97b7e501", true, true, true, true, nil, true, nil, true, true, false, true },
    }
    local env = setmetatable({ __UE4SSLuaEventBridge_SessionId = 5 }, { __index = _G })
    env.print = function(text) w.printed[#w.printed + 1] = text end
    env.FName = function(s) return s end
    env.StaticFindObject = function(path) return w.objects[path] end
    env.ExecuteInGameThread = function(fn) w.queue[#w.queue + 1] = fn end
    env.ExecuteInGameThreadWithDelay = function(ms, fn) w.timers[#w.timers + 1] = { ms = ms, fn = fn } end
    env.UE4SSLuaEventBridge_GetVersion = function() return "test" end
    env.UE4SSLuaEventBridge_IsInGameThread = function() return w.gameThread end
    env.UE4SSLuaEventBridge_GetCapabilities = function()
        return table.unpack(w.capabilities, 1, 21)
    end
    if not options.oldNative then
        env.UE4SSLuaEventBridge_HooksEnabled = function()
            if w.enabled then return true end
            return false, "disabled by Enabled=0"
        end
        env.UE4SSLuaEventBridge_HookFunction = function(...)
            local args = table.pack(...)
            expect(args[1] == 5, "wrong session")
            if w.refuse then return nil, w.refuse end
            local optionText, i = unpackText(args, 5)
            local count = args[i]; i = i + 1
            local properties = {}
            for n = 1, count do
                local p = { parent = args[i], address = args[i + 1], offset = args[i + 2] }
                p.name, i = unpackText(args, i + 3)
                local enumCount = args[i]; i = i + 1
                p.enum = {}
                for e = 1, enumCount do p.enum[e], i = unpackText(args, i) end
                properties[n] = p
            end
            expect(i == args.n + 1, "trailing arguments")
            w.nextHook = w.nextHook + 1
            w.hooks[w.nextHook] = { fn = args[2], class = args[3], mask = args[4],
                options = optionText, properties = properties }
            return w.nextHook
        end
        env.UE4SSLuaEventBridge_Unhook = function(session, id)
            expect(session == 5, "wrong session")
            if w.unhookFails then return false, "injected unhook failure" end
            if w.hooks[id] == nil then return false, "unknown hook" end
            w.hooks[id] = nil
            w.unhooked[#w.unhooked + 1] = id
            return true
        end
        env.UE4SSLuaEventBridge_HookStatus = function(_, id)
            if w.hooks[id] == nil then return nil end
            return "active", nil, 10, 8, 6, 2, 1
        end
        env.UE4SSLuaEventBridge_HookPendingName = function(_, token, name)
            w.pendingNames[token] = name
            return true
        end
        env.UE4SSLuaEventBridge_HookPendingForget = function(_, token)
            w.pendingNames[token] = nil
            w.forgotten[#w.forgotten + 1] = token
        end
        env.UE4SSLuaEventBridge_GetHookStats = function() return 3, 9, 1, true, 100, 0, 2, 0xFE, 0xFD end
        env.UE4SSLuaEventBridge_HookSchema = function(fnAddress, ...)
            w.schemaFor = fnAddress
            if w.schemaEmpty then return end
            if w.schemaFails then return nil, "no layout" end
            return "Count", "int32", "0:4:1:128", "Data", "struct", "8:8:1:128",
                "Data.Dir", "int32", "0:4:1:0"
        end
    end
    local dispatch = assert(loadfile("bridge-events/lua/bridge_api.lua", "t", env))()
    w.dispatch = dispatch
    w.api = env.UE4SSLuaEventBridge
    w.Hooks = w.api.Hooks
    w.stop = env.__UE4SSLuaEventBridge_StopHooks
    function w:add(path, f) self.objects[path] = f; return f end
    function w:drain()
        local pending = self.queue
        self.queue = {}
        for _, fn in ipairs(pending) do fn() end
    end
    function w:fireTimer()
        local timer = table.remove(self.timers, 1)
        expect(timer ~= nil, "no timer armed")
        timer.fn()
    end
    return w
end

local PATH = "/Game/UI/WBP_Warn.WBP_Warn_C:OnShowWarning"
local function standardFunction(w, path)
    path = path or PATH
    local owner = classObject("/Game/UI/WBP_Warn.WBP_Warn_C")
    local data = propertyOwner({ property("Dir", 0), property("Power", 4) })
    local kind = enumOf({ { "EKind::None", 0 }, { "EKind::Arrow", 1 }, { "EKind::Skull", 3 } })
    return w:add(path, functionObject(path, owner, {
        property("Count", 0),
        property("Data", 8, { struct = data }),
        property("Kind", 24, { enum = kind }),
    }))
end

local function noop() end

-- Tests ---------------------------------------------------------------------------
suite.run("option validation messages", function()
    local w = fixture()
    standardFunction(w)
    local cases = {
        { nil, "options must be a table" },
        { {}, "at least one of pre or post must be a function" },
        { { pre = 1 }, "pre must be a function" },
        { { post = "x" }, "post must be a function" },
        { { pre = noop, bogus = 1 }, "unsupported Hook option: bogus" },
        { { pre = noop, params = {} }, "params must be a non-empty array of strings" },
        { { pre = noop, params = { "a|b" } }, "params entries must be 1..256 byte strings without ; | =" },
        { { pre = noop, object_names = "Reticle" }, "object_names must be a non-empty array of strings" },
        { { pre = noop, object = {} }, "object must be a weak handle from lifetimes.weak" },
        { { pre = noop, outer_class = {} }, "outer_class must be a live UClass object or path" },
        { { pre = noop, outer_class_exact = true }, "outer_class_exact needs outer_class" },
        { { pre = noop, object_paths = "yes" }, "object_paths must be a boolean" },
        { { pre = noop, max_queued = 0 }, "max_queued must be an integer in 1..1000000" },
        { { pre = noop, max_queued = 1.5 }, "max_queued must be an integer in 1..1000000" },
        { { pre = noop, delivery = "sync" }, "sync delivery needs hooks_sync" },
        { { pre = noop, delivery = "later" }, 'delivery must be "queued" or "sync"' },
    }
    for i, case in ipairs(cases) do
        local handle, message = w.Hooks.Hook(PATH, case[1])
        expect(handle == nil and message == case[2],
            "case " .. i .. ": expected " .. case[2] .. ", got " .. tostring(message))
    end
    local handle, message = w.Hooks.Hook(PATH .. "x:y:z", { pre = noop })
    expect(handle == nil and message:find("target path", 1, true), tostring(message))
    handle, message = w.Hooks.Hook(42, { pre = noop })
    expect(handle == nil and message:find("path string or a live UFunction", 1, true), tostring(message))
    expect(next(w.hooks) == nil, "validation failures must not reach the native side")
end)

suite.run("bad targets and Schema(nil) fail with nil, reason", function()
    local w = fixture()
    for _, target in ipairs({ 42, "" }) do
        local h, why = w.Hooks.Hook(target, { pre = noop })
        expect(h == nil and type(why) == "string", tostring(target))
    end
    local h, why = w.Hooks.Hook(nil, { pre = noop })
    expect(h == nil and type(why) == "string")
    h, why = w.Hooks.Hook(PATH, { pre = noop, params = 5 })
    expect(h == nil and type(why) == "string")
    h, why = w.Hooks.Hook(PATH, { pre = noop, max_queued = "9" })
    expect(h == nil and type(why) == "string")
    h, why = w.Hooks.Schema(nil)
    expect(h == nil and type(why) == "string")
end)

suite.run("target must be a function and the call must be on the game thread", function()
    local w = fixture()
    w:add("/Game/X.X_C:NotFn", classObject("x"))
    local handle, message = w.Hooks.Hook("/Game/X.X_C:NotFn", { pre = noop })
    expect(handle == nil and message:find("not a UFunction", 1, true), tostring(message))
    w.gameThread = false
    handle, message = w.Hooks.Hook(PATH, { pre = noop })
    expect(handle == nil and message:find("game thread", 1, true), tostring(message))
end)

suite.run("capabilities and unavailable natives", function()
    local w = fixture()
    local caps = w.api.GetCapabilities()
    expect(caps.hooks == true and caps.hooks_bp == true and caps.hooks_native == true)
    expect(caps.hooks_sync == false and caps.hooks_frame_delivery == true and caps.hooks_reason == nil)
    expect(caps.object_lifetimes == true, "existing fields unchanged")

    local old = fixture({ capabilities = { 6, true, true, true, true, true, true, true, true,
        "97b7e501", true, true, true, true } , oldNative = true })
    caps = old.api.GetCapabilities()
    expect(caps.hooks == false and caps.hooks_bp == false and caps.hooks_native == false)
    expect(caps.hooks_sync == false and caps.hooks_frame_delivery == false and caps.hooks_reason == nil)
    local handle, message = old.Hooks.Hook(PATH, { pre = noop })
    expect(handle == nil and message:find("not available", 1, true), tostring(message))
    expect(old.Hooks.GetStats() == nil)

    local off = fixture({ capabilities = { 6, true, true, true, true, true, true, true, true,
        "97b7e501", true, true, true, true, nil, false, "turned off", false, false, false, false } })
    expect(off.api.GetCapabilities().hooks_reason == "turned off")
    off.enabled = false
    handle, message = off.Hooks.Hook(PATH, { pre = noop })
    expect(handle == nil and message == "disabled by Enabled=0", tostring(message))
end)

suite.run("property list and options encoding", function()
    local w = fixture()
    local f = standardFunction(w)
    local class = classObject("/Script/Engine.Actor")
    local handle = assert(w.Hooks.Hook(PATH, {
        pre = noop, post = noop, params = { "Count", "Data.Dir" },
        object_names = { "Reticle", "TopArrow" }, outer_class = class, outer_class_exact = true,
        object = { identity = function() return "7001", 0x7000 end },
        object_paths = true, max_queued = 64,
    }))
    local hook = w.hooks[1]
    expect(hook.fn == f.address and hook.class == f.owner.address and hook.mask == 3)
    expect(hook.options == "names=Reticle|TopArrow;outer=" .. class.address
        .. ";exact=1;obj=28672:7001;paths=1;maxq=64;params=Count|Data.Dir", hook.options)
    local names = {}
    for i, p in ipairs(hook.properties) do names[i] = p.name end
    expect(table.concat(names, ",") == "Count,Data,Dir,Power,Kind", table.concat(names, ","))
    local byName = {}
    for _, p in ipairs(hook.properties) do byName[p.name] = p end
    expect(byName.Count.parent == -1 and byName.Count.offset == 0 and #byName.Count.enum == 0)
    expect(byName.Data.parent == -1 and byName.Data.offset == 8)
    expect(byName.Dir.parent == 1 and byName.Power.parent == 1 and byName.Power.offset == 4)
    expect(byName.Kind.parent == -1 and byName.Kind.offset == 24)
    expect(table.concat(byName.Kind.enum, ",") == "EKind::None,EKind::Arrow,,EKind::Skull",
        table.concat(byName.Kind.enum, ","))
    expect(byName.Count.address ~= nil and byName.Kind.address ~= nil)
    expect(handle:Status().state == "active")
end)

suite.run("struct recursion stops at depth 3", function()
    local w = fixture()
    local owner = classObject("c")
    local level3 = propertyOwner({ property("Leaf", 0) })
    local level2 = propertyOwner({ property("L3", 0, { struct = level3 }) })
    local level1 = propertyOwner({ property("L2", 0, { struct = level2 }) })
    w:add(PATH, functionObject(PATH, owner, { property("L1", 0, { struct = level1 }) }))
    assert(w.Hooks.Hook(PATH, { pre = noop }))
    local names = {}
    for i, p in ipairs(w.hooks[1].properties) do names[i] = p.name end
    -- L1 (0) -> L2 (1) -> L3 (2) -> Leaf (3); a struct at depth 3 would not expand.
    expect(table.concat(names, ",") == "L1,L2,L3,Leaf", table.concat(names, ","))
end)

suite.run("wrapper target and native refusal", function()
    local w = fixture()
    local f = standardFunction(w)
    local handle = assert(w.Hooks.Hook(f, { post = noop }))
    expect(w.hooks[1].mask == 2 and handle:Status().state == "active")
    w.refuse = "property layout mismatch"
    local refused, why = w.Hooks.Hook(PATH, { pre = noop })
    expect(refused == nil and why == "property layout mismatch", tostring(why))
end)

suite.run("pending becomes active on wake-up", function()
    local w = fixture()
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    local status = handle:Status()
    expect(status.state == "pending" and status.reason:find(PATH, 1, true), tostring(status.reason))
    local token, className = next(w.pendingNames)
    expect(className == "WBP_Warn_C", tostring(className))
    expect(#w.timers == 1 and w.timers[1].ms == 1000, "1 s retry timer")

    w.dispatch(-3, token)
    expect(#w.queue == 1, "retry is scheduled on the game thread")
    w.dispatch(-3, token)
    expect(#w.queue == 1, "wake-ups coalesce")
    w:drain()
    expect(handle:Status().state == "pending", "class not loaded yet")

    standardFunction(w)
    w.dispatch(-3, token)
    w:drain()
    expect(handle:Status().state == "active" and w.hooks[1] ~= nil)
    expect(w.pendingNames[token] == nil, "wake-up name is released once attached")
end)

suite.run("pending retries on the one second timer only while pending", function()
    local w = fixture()
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    w:fireTimer()
    expect(#w.timers == 1, "re-armed while pending")
    standardFunction(w)
    w:fireTimer()
    expect(handle:Status().state == "active")
    expect(#w.timers == 0, "no timer once nothing is pending")
end)

suite.run("a refused retry stays pending with the reason", function()
    local w = fixture()
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    standardFunction(w)
    w.refuse = "script is not serialized"
    w:fireTimer()
    local status = handle:Status()
    expect(status.state == "pending" and status.reason == "script is not serialized", tostring(status.reason))
    w.refuse = nil
    w:fireTimer()
    expect(handle:Status().state == "active")
end)

suite.run("orphan returns to pending and attaches again", function()
    local w = fixture()
    standardFunction(w)
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    w.dispatch(-4, 1, "function deleted")
    local status = handle:Status()
    expect(status.state == "pending" and status.reason == "function deleted", tostring(status.reason))
    expect(w.hooks[1] == nil and w.unhooked[1] == 1, "old native subscription released")
    expect(select(2, next(w.pendingNames)) == "WBP_Warn_C")
    local second = standardFunction(w)
    w.dispatch(-3, 0)
    w:drain()
    expect(handle:Status().state == "active")
    expect(w.hooks[2] ~= nil and w.hooks[2].fn == second.address, "re-attached to the new function")
    -- Events for the old native id are ignored.
    local called = false
    w.dispatch(-10, 1, 1, 1, true, 0, 0, 0, nil, true, 0)
    expect(not called)
end)

suite.run("Unhook on pending and active hooks", function()
    local w = fixture()
    local pending = assert(w.Hooks.Hook(PATH, { pre = noop }))
    local token = next(w.pendingNames)
    expect(pending:Unhook() == true)
    expect(w.pendingNames[token] == nil and pending:Status().state == "unhooked")
    local again, againWhy = pending:Unhook()
    expect(again == false and againWhy == "hook is already unhooked", "second Unhook reports it")
    w:fireTimer()
    expect(#w.timers == 0 and next(w.hooks) == nil, "an unhooked pending hook never attaches")

    standardFunction(w)
    local active = assert(w.Hooks.Hook(PATH, { pre = noop }))
    expect(active:Unhook() == true and w.hooks[1] == nil)
    local status = active:Status()
    expect(status.state == "unhooked" and status.calls == 10 and status.delivered == 6,
        "counters survive unhook")
    local ok, why = w.Hooks.Unhook({})
    expect(ok == false and why:find("returned by Hooks.Hook", 1, true))

    local failing = assert(w.Hooks.Hook(PATH, { pre = noop }))
    w.unhookFails = true
    ok, why = failing:Unhook()
    expect(ok == false and why == "injected unhook failure")
    expect(failing:Status().state == "active", "failed Unhook keeps the hook")
end)

suite.run("Status reports native counters", function()
    local w = fixture()
    standardFunction(w)
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    local s = handle:Status()
    expect(s.state == "active" and s.calls == 10 and s.matched == 8 and s.delivered == 6
        and s.dropped == 2 and s.reentries == 1)
end)

suite.run("dispatch decodes every tag", function()
    local w = fixture()
    standardFunction(w)
    local seen
    local handle = assert(w.Hooks.Hook(PATH, { pre = function(call) seen = call end }))
    w.dispatch(-10, 1, 41, 1, true, 4096, 12, 99, "/Game/Obj.Obj_C_0", true, 9,
        "Flag", "b", true,
        "Count", "i", 7,
        "Scale", "n", 1.5,
        "Label", "s", "hello",
        "Kind", "e", "3|EKind::Skull",
        "Target", "o", "8192:5:6:/Game/Level.Level:PersistentLevel.Foe_0",
        "Bare", "o", "16:1:2",
        "Data.Dir", "i", 4,
        "Data.Deep.Power", "n", 0.25)
    expect(seen ~= nil, "callback ran")
    expect(seen.hook == handle and seen.phase == "pre" and seen.sequence == 41 and seen.game_thread == true)
    expect(seen.function_path == PATH and seen.params_available == true)
    expect(seen.object.address == 4096 and seen.object.index == 12 and seen.object.serial == 99
        and seen.object.path == "/Game/Obj.Obj_C_0")
    local p = seen.params
    expect(p.Flag == true and p.Count == 7 and math.type(p.Count) == "integer")
    expect(p.Scale == 1.5 and p.Label == "hello")
    expect(p.Kind.value == 3 and p.Kind.name == "EKind::Skull")
    expect(p.Target.address == 8192 and p.Target.index == 5 and p.Target.serial == 6
        and p.Target.path == "/Game/Level.Level:PersistentLevel.Foe_0", tostring(p.Target.path))
    expect(p.Bare.address == 16 and p.Bare.path == nil)
    expect(p.Data.Dir == 4 and p.Data.Deep.Power == 0.25)
    expect(seen.out == nil and seen.result == nil, "pre carries no out or result")

    w.dispatch(-10, 1, 42, 1, false, 0, -1, 0, nil, true, 1, "Skip", "x", nil)
    expect(seen.sequence == 42 and seen.object == nil and seen.game_thread == false
        and seen.params.Skip == nil, "unsupported tag delivers nil")
end)

suite.run("capture frame and clock are call fields, not parameters", function()
    local w = fixture()
    standardFunction(w)
    local seen
    assert(w.Hooks.Hook(PATH, { post = function(call) seen = call end }))
    local before = os.clock()
    w.dispatch(-10, 1, 7, 2, true, 0, 0, 0, nil, true, 3,
        "@frame", "i", 6337, "@age", "n", 0.25, "IconType", "e", "2|ERebelIndicatorIconType::Right")
    expect(seen.frame == 6337, "call.frame is the capture frame")
    expect(type(seen.clock) == "number" and seen.clock <= before + 0.01 - 0.24, "call.clock is os.clock() minus the age")
    expect(seen.params["@frame"] == nil and seen.params["@age"] == nil, "reserved names stay out of params")
    expect(seen.params.IconType.value == 2)
end)

suite.run("fixed arrays, empty enum names and null objects", function()
    local w = fixture()
    standardFunction(w)
    local seen
    assert(w.Hooks.Hook(PATH, { post = function(call) seen = call end }))
    w.dispatch(-10, 1, 1, 2, true, 1, 1, 1, nil, true, 9,
        "Arr[0]", "i", 10, "Arr[1]", "i", 11, "Arr[2]", "i", 12,
        "S.Arr[1]", "n", 2.5, "S.Arr[0]", "n", 1.5,
        "Kind", "e", "5|",
        "Target", "o", "0:-1:0",
        "out:Outs[1]", "i", 8,
        "Single", "i", 3)
    local arr = seen.params.Arr
    expect(#arr == 3 and arr[1] == 10 and arr[2] == 11 and arr[3] == 12, "1-based array")
    expect(seen.params.S.Arr[1] == 1.5 and seen.params.S.Arr[2] == 2.5)
    expect(seen.params.Kind.value == 5 and seen.params.Kind.name == nil)
    expect(seen.params.Target == nil, "null object is nil")
    expect(seen.out.Outs[2] == 8 and seen.params.Single == 3)
end)

suite.run("a struct at depth 3 is listed but not expanded", function()
    local w = fixture()
    local inner = propertyOwner({ property("Never", 0) })
    local l3 = propertyOwner({ property("Deep", 0, { struct = inner }) })
    local l2 = propertyOwner({ property("L3", 0, { struct = l3 }) })
    local l1 = propertyOwner({ property("L2", 0, { struct = l2 }) })
    w:add(PATH, functionObject(PATH, classObject("c"), {
        property("L1", 0, { struct = l1 }), property("After", 8) }))
    assert(w.Hooks.Hook(PATH, { pre = noop }))
    local names, parents = {}, {}
    for i, p in ipairs(w.hooks[1].properties) do names[i], parents[i] = p.name, p.parent end
    expect(table.concat(names, ",") == "L1,L2,L3,Deep,After", table.concat(names, ","))
    expect(table.concat(parents, ",") == "-1,0,1,2,-1", table.concat(parents, ","))
end)

suite.run("post delivers out, result and params_available", function()
    local w = fixture()
    standardFunction(w)
    local seen
    assert(w.Hooks.Hook(PATH, { post = function(call) seen = call end }))
    w.dispatch(-10, 1, 1, 2, true, 1, 1, 1, nil, true, 4,
        "Count", "i", 3,
        "out:Hit", "b", true,
        "out:Info.Damage", "n", 12.5,
        "ret", "i", 5)
    expect(seen.phase == "post" and seen.params.Count == 3)
    expect(seen.out.Hit == true and seen.out.Info.Damage == 12.5 and seen.result == 5)

    w.dispatch(-10, 1, 2, "post", true, 1, 1, 1, nil, false, 1, "ret", "n", 2.5)
    expect(seen.params == nil and seen.params_available == false and seen.result == 2.5
        and type(seen.out) == "table", "BP to native path")

    -- A phase the hook did not ask for is ignored.
    seen = nil
    w.dispatch(-10, 1, 3, 1, true, 1, 1, 1, nil, true, 0)
    expect(seen == nil)
end)

suite.run("callback errors are logged and 32 in a row disable the hook", function()
    local w = fixture()
    standardFunction(w)
    local mode, calls = "fail", 0
    local handle = assert(w.Hooks.Hook(PATH, { pre = function()
        calls = calls + 1
        if mode == "fail" then error("boom") end
    end }))
    for i = 1, 31 do w.dispatch(-10, 1, i, 1, true, 1, 1, 1, nil, true, 0) end
    expect(handle:Status().state == "active" and #w.printed == 31 and w.printed[1]:find("boom", 1, true))
    mode = "ok"
    w.dispatch(-10, 1, 32, 1, true, 1, 1, 1, nil, true, 0)
    mode = "fail"
    for i = 1, 31 do w.dispatch(-10, 1, 100 + i, 1, true, 1, 1, 1, nil, true, 0) end
    expect(handle:Status().state == "active", "a success resets the strike count")
    w.dispatch(-10, 1, 200, 1, true, 1, 1, 1, nil, true, 0)
    local status = handle:Status()
    expect(status.state == "failed" and status.reason:find("32 times in a row", 1, true), tostring(status.reason))
    expect(w.hooks[1] == nil, "native subscription released")
    local before = calls
    w.dispatch(-10, 1, 201, 1, true, 1, 1, 1, nil, true, 0)
    expect(calls == before, "a failed hook receives nothing")
    expect(handle:Unhook() == true)
end)

suite.run("native fault disables the hook", function()
    local w = fixture()
    standardFunction(w)
    local handle = assert(w.Hooks.Hook(PATH, { pre = noop }))
    w.dispatch(-5, 1, "queue overflow")
    local status = handle:Status()
    expect(status.state == "failed" and status.reason == "queue overflow", tostring(status.reason))
end)

suite.run("UnhookAll removes active and pending hooks", function()
    local w = fixture()
    standardFunction(w)
    local a = assert(w.Hooks.Hook(PATH, { pre = noop }))
    local b = assert(w.Hooks.Hook("/Game/Other.Other_C:Fn", { pre = noop }))
    local count, ok = w.Hooks.UnhookAll()
    expect(count == 2 and ok == true)
    expect(a:Status().state == "unhooked" and b:Status().state == "unhooked")
    expect(next(w.hooks) == nil and next(w.pendingNames) == nil)
    count, ok = w.Hooks.UnhookAll()
    expect(count == 0 and ok == true)

    local c = assert(w.Hooks.Hook(PATH, { pre = noop }))
    w.unhookFails = true
    local failed, completed, why = w.Hooks.UnhookAll()
    expect(failed == 0 and completed == false and why:find("injected unhook failure", 1, true), tostring(why))
    w.unhookFails = false
    expect(select(2, w.Hooks.UnhookAll()) == true and c:Status().state == "unhooked")
end)

suite.run("session stop unhooks everything and raises on failure", function()
    local w = fixture()
    standardFunction(w)
    local active = assert(w.Hooks.Hook(PATH, { pre = noop }))
    local pending = assert(w.Hooks.Hook("/Game/Other.Other_C:Fn", { pre = noop }))
    expect(w.stop() == true)
    expect(active:Status().state == "unhooked" and pending:Status().state == "unhooked")
    expect(next(w.hooks) == nil and next(w.pendingNames) == nil)
    expect(select(2, w.Hooks.Hook(PATH, { pre = noop })):find("stopped", 1, true))
    w:fireTimer()
    expect(#w.timers == 0, "no retry after stop")

    local f = fixture()
    standardFunction(f)
    assert(f.Hooks.Hook(PATH, { pre = noop }))
    f.unhookFails = true
    Kit.expectRaise("hook cleanup incomplete", f.stop)
    expect(f.printed[#f.printed]:find("injected unhook failure", 1, true))
end)

suite.run("GetStats", function()
    local w = fixture()
    assert(w.Hooks.Hook(PATH, { pre = noop }))
    local s = w.Hooks.GetStats()
    expect(s.queued == 3 and s.high_water == 9 and s.rejected == 1 and s.delivery == "tick"
        and s.ticks == 100 and s.update_calls == 0 and s.entries == 2
        and s.slots.a == 0xFE and s.slots.b == 0xFD and s.pending == 1)
end)

suite.run("Schema builds a tree from flat triplets", function()
    local w = fixture()
    local f = standardFunction(w)
    local tree = assert(w.Hooks.Schema(PATH))
    expect(w.schemaFor == f.address)
    expect(#tree == 2 and tree[1].name == "Count" and tree[1].kind == "int32"
        and tree[1].offset == 0 and tree[1].size == 4 and tree[1].dim == 1 and tree[1].flags == 128)
    expect(tree[2].name == "Data" and #tree[2].fields == 1 and tree[2].fields[1].name == "Dir")
    local none, why = w.Hooks.Schema("/Game/Nope.Nope_C:Fn")
    expect(none == nil and why:find("not resolved", 1, true), tostring(why))
end)

suite.run("Schema of a function without parameters is an empty table", function()
    local w = fixture()
    standardFunction(w)
    w.schemaEmpty = true
    local tree, why = w.Hooks.Schema(PATH)
    expect(type(tree) == "table" and #tree == 0 and why == nil)
    w.schemaEmpty, w.schemaFails = false, true
    tree, why = w.Hooks.Schema(PATH)
    expect(tree == nil and why == "no layout", tostring(why))
end)

suite.finish()
