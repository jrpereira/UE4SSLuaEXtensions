-- Offline integration: real bridge_api.lua, simulated UE objects/native boundary.
-- No claim of C++ shutdown, real concurrency, Unreal GC, or ABI validation.
package.path = "shared/tests/?.lua;" .. package.path
local Kit = require("TestKit")
local check = Kit.expect
local function count(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end

local function fixture()
    local f = {
        gameThread = true, contexts = {}, bindings = {}, targets = {},
        nextId = 0, nextSession = 0, removeFailures = 0, unbindFailures = 0,
        removals = 0, mutations = 0, componentAlive = true, dead = {}, constructed = {},
        unserialized = {}, serialized = 0,
    }
    function f:id() self.nextId = self.nextId + 1; return self.nextId end
    function f:mutate()
        check(self.gameThread, "reflected mutation attempted off game thread")
        self.mutations = self.mutations + 1
    end
    local function object(kind)
        local o = { valid = true, path = "/Engine/Transient.Lifecycle_" .. f:id() }
        function o:IsValid() return self.valid end
        function o:IsA(name)
            return name == "/Script/EnhancedInput." .. kind
                or (kind == "Class" and name == "/Script/CoreUObject.Class")
        end
        function o:GetFullName() return kind .. " " .. self.path end
        return o
    end
    f.subsystem = object("EnhancedInputLocalPlayerSubsystem")
    function f.subsystem:AddMappingContext(context)
        f:mutate()
        f.contexts[context] = true
    end
    function f.subsystem:RemoveMappingContext(context)
        check(self.valid, "called destroyed subsystem")
        f:mutate()
        if f.removeFailures > 0 then
            f.removeFailures = f.removeFailures - 1
            error("injected removal failure")
        end
        check(f.contexts[context], "context removed twice")
        f.contexts[context] = nil
        f.removals = f.removals + 1
    end
    function f:loadSession()
        self.nextSession = self.nextSession + 1
        local session = { id = self.nextSession, lost = {}, references = {} }
        local env = setmetatable({ __UE4SSLuaEventBridge_SessionId = session.id }, { __index = _G })
        env.FName = function(s) return s end
        env.StaticFindObject = function(path)
            if path == "Subsystem" then return self.subsystem end
            if path == "/Script/Engine.Default__KismetSystemLibrary" then
                if self.kismetMissing then return nil end
                return { IsValid = function() return true end,
                    -- A soft reference gives a live object its weak serial.
                    Conv_ObjectToSoftObjectReference = function(_, target)
                        self:mutate()
                        self.serialized = self.serialized + 1
                        if type(target) == "table" and type(target.GetAddress) == "function" then
                            self.unserialized[target:GetAddress()] = nil
                        end
                    end }
            end
            local c = object("Class")
            c.class = path:match("%.([^%.]+)$")
            return c
        end
        env.StaticConstructObject = function(class, _outer, _name, flags)
            self:mutate()
            local o = object(class.class)
            o.flags = flags
            self.constructed[class.class] = (self.constructed[class.class] or 0) + 1
            function o:MapKey(action, key) self.action = action; self.key = key end
            function o:UnmapAll() self.action, self.key = nil, nil end
            return o
        end
        local function own(id) check(id == session.id, "cross-session native call") end
        env.UE4SSLuaEventBridge_GetVersion = function() return "test-fixture" end
        env.UE4SSLuaEventBridge_GetCapabilities = function()
            local ready = self.lifetimesReason == nil
            return 6, true, true, true, true, true, true, true, true, "fixture", true, true, true, ready,
                self.lifetimesReason
        end
        env.UE4SSLuaEventBridge_RegisterLoopStart = function(id, token)
            own(id)
            session.loopToken = token
            return true
        end
        env.UE4SSLuaEventBridge_CancelLoopStart = function(id, token)
            own(id)
            if session.loopToken ~= token then return false end
            session.loopToken = nil
            return true
        end
        env.UE4SSLuaEventBridge_LifetimeCapture = function(id, address)
            own(id)
            -- Native capture rejects objects without a serial, as it does dead ones.
            if self.unserialized[address] or self.dead[address] then
                return nil, "address is not a live UObject"
            end
            local token = tostring(address + 1000)
            session.references[token] = (session.references[token] or 0) + 1
            return token
        end
        env.UE4SSLuaEventBridge_LifetimeRelease = function(id, token)
            own(id)
            local count = session.references[token]
            if not count then return false end
            session.references[token] = count > 1 and count - 1 or nil
            return true
        end
        env.UE4SSLuaEventBridge_LifetimeValid = function(id, address, token)
            own(id)
            if not self.gameThread or session.faulted then return false end
            return not self.dead[address] and token == tostring(address + 1000)
        end
        env.UE4SSLuaEventBridge_LifetimeTakeLost = function(id)
            own(id)
            if session.faulted then return nil, "lifetime notification overflow; reload" end
            session.reporting = true
            local token = session.lostToken or table.remove(session.lost, 1)
            session.lostToken = nil
            return token
        end
        env.UE4SSLuaEventBridge_LifetimeReportLosses = function(id)
            own(id)
            session.reporting = true
            return true
        end
        -- Native losses are queued only for sessions that opted in.
        function session:lose(token)
            if self.reporting then self.lost[#self.lost + 1] = token end
        end
        env.UE4SSLuaEventBridge_IsInGameThread = function() return self.gameThread end
        env.UE4SSLuaEventBridge_TraceScope = function() end
        env.UE4SSLuaEventBridge_OpenInputComponent = function(id)
            own(id)
            local target = self:id()
            self.targets[target] = id
            return target
        end
        env.UE4SSLuaEventBridge_CloseInputComponent = function(id, target)
            own(id)
            if self.targets[target] ~= id then return false, "unknown target" end
            self.targets[target] = nil
            return true
        end
        env.UE4SSLuaEventBridge_BindAction = function(...)
            local args = {...}
            own(args[1])
            check(self.componentAlive, "bind after component destruction")
            local handle = self:id()
            self.bindings[handle] = { session = session, token = args[69], handle = handle }
            return handle
        end
        env.UE4SSLuaEventBridge_Unbind = function(id, handle)
            own(id)
            if handle == self.unbindThrowHandle then error("injected scope exception") end
            if self.unbindFailures > 0 then
                self.unbindFailures = self.unbindFailures - 1
                return false, "injected native removal failure"
            end
            local binding = self.bindings[handle]
            check(binding and binding.session == session, "invalid native unbind")
            -- Backend contract: dead component does not prevent logical unsubscribe.
            self.bindings[handle] = nil
            return true
        end
        local function bulkUnbind(id, preserveTargets)
            own(id)
            if self.bulkFailure then return 0, false, "injected bulk failure" end
            local removed = 0
            for handle, binding in pairs(self.bindings) do
                if binding.session == session then
                    self.bindings[handle] = nil
                    removed = removed + 1
                end
            end
            if not preserveTargets then
                for target, owner in pairs(self.targets) do
                    if owner == id then self.targets[target] = nil end
                end
            end
            return removed, true
        end
        env.UE4SSLuaEventBridge_UnbindAll = function(id) return bulkUnbind(id, false) end
        env.UE4SSLuaEventBridge_UnbindAllPreserveTargets = function(id) return bulkUnbind(id, true) end
        session.dispatch = check(loadfile("bridge-events/lua/bridge_api.lua", "t", env))()
        session.api = env.UE4SSLuaEventBridge
        session.cleanup = env.__UE4SSLuaEventBridge_CloseHelperScopes
        session.stop = env.__UE4SSLuaEventBridge_StopHelperScopes
        function session:open(debugEnabled)
            return check(self.api.Helpers.OpenInput({
                component_path = "Component", subsystem_path = "Subsystem",
                debug = debugEnabled or false,
            }))
        end
        return session
    end
    function f:queued(handle)
        local b = check(self.bindings[handle])
        -- Capture before teardown, as an already-drained native event would.
        return function()
            b.session.dispatch(b.token, b.handle, "Action", "Triggered", 0, 0, 1, 0, 0, 0, 1)
        end
    end
    function f:empty()
        check(count(self.contexts) == 0, "mapping contexts leaked")
        check(count(self.bindings) == 0, "native subscriptions leaked")
        check(count(self.targets) == 0, "component targets leaked")
    end
    return f
end

local cases = {}
cases["loop start is one-shot and cancellation releases callbacks"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local unsubscribe = s.api.onLoopStart(function() calls = calls + 1 end)
    local token = check(s.loopToken)
    s.dispatch(-2, token)
    s.dispatch(-2, token)
    check(calls == 1 and not unsubscribe(), "delivered loop callback remained active")
    local cancel = s.api.onLoopStart(function() calls = calls + 10 end)
    local canceled = check(s.loopToken)
    check(cancel(), "pending loop callback was not canceled")
    s.dispatch(-2, canceled)
    check(calls == 1, "canceled loop callback ran")
end

cases["lifetime wrapper preserves decimal tokens and drains losses"] = function()
    local f = fixture()
    local s = f:loadSession()
    local token = check(s.api.lifetimes.captureAddress(4096))
    local object = {
        IsValid = function() return true end,
        GetAddress = function() return 4096 end,
    }
    check(s.api.lifetimes.captureObject(object) == token)
    local missing, why = s.api.lifetimes.captureObject({IsValid = function() return false end})
    check(missing == nil and why:find("live UE4SS UObject wrapper", 1, true))
    check(token == "5096" and s.api.lifetimes.valid(4096, token))
    check(not s.api.lifetimes.valid(4096, "05096"), "noncanonical token accepted")
    s.lostToken = token
    check(s.api.lifetimes.takeLost() == token and s.api.lifetimes.takeLost() == nil)
end

-- A wrapper kept past garbage collection (a save loaded from a running game)
-- must never be dereferenced; the weak handle's lifetime check rejects it first.
local function trackedObject(address)
    local o = { address = address, freed = false, reads = 0 }
    function o:IsValid()
        if self.freed then self.reads = self.reads + 1; error("freed object memory read") end
        return true
    end
    function o:GetAddress()
        if self.freed then self.reads = self.reads + 1; error("freed object memory read") end
        return self.address
    end
    return o
end

cases["weak handle returns its wrapper only while the object lives"] = function()
    local f = fixture()
    local s = f:loadSession()
    local o = trackedObject(700)
    local handle = check(s.api.lifetimes.weak(o))
    check(handle:get() == o, "live object not returned")
    local token, address = handle:identity()
    check(token == "1700" and address == 700, "identity does not expose the captured lifetime")
    f.dead[700], o.freed = true, true
    check(handle:get() == nil and o.reads == 0, "freed object was read")
    f.dead[700] = nil
    check(handle:get() == nil, "a lost handle must stay lost")
    check(handle:identity() == "1700", "identity survives loss")
    check(getmetatable(handle) == false, "handle internals must be private")
end

cases["weak handle keeps its wrapper when read off the game thread"] = function()
    local f = fixture()
    local s = f:loadSession()
    local o = trackedObject(701)
    f.gameThread = false
    local none, why = s.api.lifetimes.weak(o)
    check(none == nil and why:find("game thread", 1, true), "off-thread creation must fail")
    f.gameThread = true
    local handle = check(s.api.lifetimes.weak(o))
    f.gameThread = false
    local value, readWhy = handle:get()
    check(value == nil and readWhy:find("game thread", 1, true), "off-thread read must fail")
    f.gameThread = true
    check(handle:get() == o, "an off-thread read must not forget a live object")
    handle:release()
    check(handle:get() == nil, "release forgets the wrapper")
end

cases["weak handles return their observation when released or collected"] = function()
    local f = fixture()
    local s = f:loadSession()
    local o = trackedObject(705)
    local first = check(s.api.lifetimes.weak(o))
    local second = check(s.api.lifetimes.weak(o))
    check(s.references["1705"] == 2, "handles for one object share an observation")
    first:release()
    first:release()
    check(s.references["1705"] == 1, "release returns one share, once")
    check(second:get() == o, "other handles keep the observation")
    second = nil
    collectgarbage(); collectgarbage()
    check(s.references["1705"] == nil, "a collected handle returns its share")
    check(s.api.lifetimes.release("bad") == false)
    local token = check(s.api.lifetimes.captureObject(o))
    check(s.api.lifetimes.release(token) and not s.api.lifetimes.release(token))
end

-- Consumers test against tests/support/lifetimes.lua; it must answer as bridge_api.lua does.
-- lifetimes: a lifetimes table; make(address) a live object; kill(object) ends its life;
-- thread(on) moves on or off the game thread.
local function weakHandleContract(lifetimes, make, kill, thread)
    local none, why = lifetimes.weak(nil)
    check(none == nil and why:find("live UE4SS UObject wrapper", 1, true), "nil accepted")
    local o = make(720)
    local handle = check(lifetimes.weak(o))
    check(handle:get() == o and handle:get() == o, "get must return the same live wrapper")
    local token, address = handle:identity()
    check(type(token) == "string" and address == 720, "identity is the token and address")
    check(lifetimes.captureObject(o) == token, "capture and handle share the token")
    check(getmetatable(handle) == false, "handle internals must be private")
    thread(false)
    local value, readWhy = handle:get()
    check(value == nil and readWhy:find("game thread", 1, true), "off-thread read must fail")
    none, why = lifetimes.weak(o)
    check(none == nil and why:find("game thread", 1, true), "off-thread creation must fail")
    thread(true)
    check(handle:get() == o, "an off-thread read must not forget a live object")
    local second = check(lifetimes.weak(o))
    second:release(); second:release()
    check(second:get() == nil and handle:get() == o, "release forgets only its own handle")
    kill(o)
    check(handle:get() == nil and handle:identity() == token, "a dead object is not returned")
    none = lifetimes.weak(o)
    check(none == nil and lifetimes.captureObject(o) == nil, "a dead object is not captured")
end

cases["the vendored test double answers as the weak-handle API does"] = function()
    local f = fixture()
    local s = f:loadSession()
    weakHandleContract(s.api.lifetimes, trackedObject,
        function(o) f.dead[o.address], o.freed = true, true end,
        function(on) f.gameThread = on end)
    local dead = {}
    local double = dofile("tests/support/lifetimes.lua").service(function(o) return not dead[o] end)
    weakHandleContract(double,
        function(address) return { GetAddress = function() return address end } end,
        function(o) dead[o] = true end,
        function(on) double.gameThread = on end)
    check(double.held == 1, "held counts unreleased handles")
end

cases["weak handle creation rejects dead wrappers"] = function()
    local f = fixture()
    local s = f:loadSession()
    local none, why = s.api.lifetimes.weak({ IsValid = function() return false end })
    check(none == nil and why:find("live UE4SS UObject wrapper", 1, true))
    none, why = s.api.lifetimes.weak(nil)
    check(none == nil and why:find("live UE4SS UObject wrapper", 1, true))
end

cases["capture initializes the serial of a never weakly referenced object"] = function()
    local f = fixture()
    local s = f:loadSession()
    local o = trackedObject(706)
    f.unserialized[706] = true
    local handle = check(s.api.lifetimes.weak(o))
    check(f.serialized == 1 and handle:get() == o, "serial-less live object not captured")
    check(handle:identity() == "1706", "retry keeps the index and serial identity")
    check(s.api.lifetimes.captureObject(o) == "1706" and f.serialized == 1,
        "a serialized object is captured without another initialization")
    f.unserialized[707] = true
    check(s.api.lifetimes.captureObject(trackedObject(707)) == "1707" and f.serialized == 2)
end

cases["capture retries once and keeps the native reason for dead objects"] = function()
    local f = fixture()
    local s = f:loadSession()
    f.dead[708] = true
    local none, why = s.api.lifetimes.weak(trackedObject(708))
    check(none == nil and why == "address is not a live UObject", "dead object reason changed")
    check(f.serialized == 1, "a rejected capture initializes the serial once")
    f.kismetMissing, f.unserialized[709] = true, true
    none, why = s.api.lifetimes.captureObject(trackedObject(709))
    check(none == nil and why:find("failed to initialize its weak reference", 1, true),
        "initialization failure not reported")
end

cases["losses are reported only after the session opts in"] = function()
    local f = fixture()
    local s = f:loadSession()
    local handle = check(s.api.lifetimes.weak(trackedObject(702)))
    s:lose("11")
    check(s.api.lifetimes.takeLost() == nil, "losses before opting in must not be queued")
    s:lose("12")
    check(s.api.lifetimes.takeLost() == "12", "takeLost opts the session in")
    local other = f:loadSession()
    check(other.api.lifetimes.reportLosses() == true)
    other:lose("13")
    check(other.api.lifetimes.takeLost() == "13", "reportLosses opts the session in")
    check(handle:get() ~= nil, "loss reporting does not affect live handles")
end

cases["capabilities explain unavailable lifetimes"] = function()
    local f = fixture()
    local s = f:loadSession()
    local caps = s.api.GetCapabilities()
    check(caps.api == 6 and caps.object_lifetimes and caps.weak_handles and caps.loss_opt_in)
    check(caps.object_lifetimes_reason == nil, "an available service has no reason")
    f.lifetimesReason = "the UObject layout probe verified 0 of 40 objects (2 required)"
    caps = s.api.GetCapabilities()
    check(not caps.object_lifetimes and not caps.weak_handles and not caps.loss_opt_in)
    check(caps.object_lifetimes_reason == f.lifetimesReason, "reason not exposed")
end

cases["native lifetime fault invalidates weak handles and is reported"] = function()
    local f = fixture()
    local s = f:loadSession()
    local handle = check(s.api.lifetimes.weak(trackedObject(704)))
    s.faulted = true
    check(handle:get() == nil, "a faulted service must not vouch for objects")
    local none, why = s.api.lifetimes.takeLost()
    check(none == nil and why:find("overflow", 1, true))
end

cases["helper contexts are rooted, re-applied and reused"] = function()
    local f = fixture()
    local s = f:loadSession()
    local scope = s:open()
    local first = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    local context = next(f.contexts)
    check(context.flags == 0xC0, "private contexts must be rooted at construction")
    f.contexts = {} -- The game clears its mappings.
    check(scope:Refresh())
    check(f.contexts[context], "Refresh re-applies the cleared context")
    check(scope:Refresh() and f.contexts[context], "Refresh is idempotent")
    check(scope:Unbind(first))
    check(context.action == nil, "an unbound context is emptied")
    check(scope:Bind("B", s.api.Helpers.Trigger.Tap, function() end))
    check(f.contexts[context] and f.constructed.InputMappingContext == 1,
        "a recycled context is reused instead of rooting another")
    check(s.cleanup())
end

cases["a freed subsystem is never dereferenced"] = function()
    local f = fixture()
    local reads = 0
    f.subsystem.address = 900
    function f.subsystem:GetAddress() return self.address end
    local s = f:loadSession()
    local scope = s:open()
    check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    -- The subsystem is freed without a valid=false phase.
    f.dead[900] = true
    for _, method in ipairs({ "IsValid", "GetAddress", "AddMappingContext", "RemoveMappingContext" }) do
        f.subsystem[method] = function() reads = reads + 1; error("freed subsystem read") end
    end
    f.contexts = {}
    local refreshed, why = scope:Refresh()
    check(not refreshed and why:find("no longer live", 1, true))
    check(s.cleanup())
    check(reads == 0, "the freed subsystem was read")
    f:empty()
end

cases["off-thread helper shutdown refuses mutation and permits retry"] = function()
    local f = fixture()
    local s = f:loadSession()
    local scope = s:open()
    check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    local before = f.mutations
    f.gameThread = false
    local ok, why = s.cleanup()
    check(not ok and why:find("game thread", 1, true), "off-thread cleanup must report failure")
    check(f.mutations == before and count(f.contexts) == 1 and count(f.bindings) == 1)
    f.gameThread = true
    check(s.cleanup())
    f:empty()
end

cases["100 fresh Lua sessions clean up and reject old queued callbacks"] = function()
    local f, total, stale = fixture(), 0, {}
    for _ = 1, 100 do
        local s = f:loadSession()
        local scope = s:open(true)
        local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() total = total + 1 end))
        check(count(f.bindings) == 4, "debug observers missing")
        local event = f:queued(handle)
        event()
        stale[#stale + 1] = event
        check(s.cleanup())
        check(s.cleanup(), "cleanup must be idempotent")
        f:empty()
        for _, oldEvent in ipairs(stale) do oldEvent() end
    end
    check(total == 100, "old session callbacks ran after cleanup")
end

cases["failed context removal is retained, reported, and retried"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local scope = s:open(true)
    local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Hold, function() calls = calls + 1 end))
    local event = f:queued(handle)
    f.removeFailures = 2
    for _ = 1, 2 do
        local ok, why = s.cleanup()
        check(not ok and why:find("injected removal failure", 1, true))
        check(count(f.contexts) == 1 and count(f.targets) == 1 and count(f.bindings) == 0)
        event()
        check(calls == 0, "callback survived failed context cleanup")
    end
    check(s.cleanup())
    check(f.removals == 1)
    f:empty()
end

cases["partial native removal failure retains retry ownership"] = function()
    local f = fixture()
    local s = f:loadSession()
    local scope = s:open(true)
    check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    f.unbindFailures = 1
    local ok, why = s.cleanup()
    check(not ok and why:find("injected native removal failure", 1, true))
    check(count(f.bindings) == 1 and count(f.contexts) == 0 and count(f.targets) == 1)
    check(s.cleanup())
    check(f.removals == 1, "retry removed context twice")
    f:empty()
end

cases["component destruction allows helper teardown and suppresses late callback"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local scope = s:open()
    local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() calls = calls + 1 end))
    local event = f:queued(handle)
    event()
    f.componentAlive = false
    check(s.cleanup())
    event()
    check(calls == 1)
    f:empty()
end

cases["late callbacks after explicit close do not reach a new scope"] = function()
    local f, first, second = fixture(), 0, 0
    local s = f:loadSession()
    local a = s:open()
    local h = check(a:Bind("A", s.api.Helpers.Trigger.Tap, function() first = first + 1 end))
    local event = f:queued(h)
    check(a:Close())
    local b = s:open()
    local h2 = check(b:Bind("A", s.api.Helpers.Trigger.Tap, function() second = second + 1 end))
    event()
    f:queued(h2)()
    check(first == 0 and second == 1, "stale callback misrouted")
    check(s.cleanup())
    f:empty()
end


cases["subsystem destruction avoids reflected calls into invalid objects"] = function()
    local f = fixture()
    local s = f:loadSession()
    local scope = s:open()
    check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    -- Engine destruction removes its contexts; Lua wrappers remain but are invalid.
    f.subsystem.valid = false
    f.contexts = {}
    local before = f.mutations
    check(s.cleanup())
    check(f.mutations == before, "cleanup touched destroyed subsystem")
    f:empty()
end

cases["one failed scope does not prevent cleanup of other scopes"] = function()
    local f = fixture()
    local s = f:loadSession()
    for _, key in ipairs({"A", "B", "C"}) do
        local scope = s:open()
        check(scope:Bind(key, s.api.Helpers.Trigger.Tap, function() end))
    end
    f.removeFailures = 1
    local ok, why = s.cleanup()
    check(not ok and why:find("injected removal failure", 1, true))
    check(count(f.bindings) == 0 and count(f.contexts) == 1 and count(f.targets) == 1,
        "cleanup abandoned healthy scopes")
    check(s.cleanup())
    check(f.removals == 3)
    f:empty()
end

cases["native stop helper propagates cleanup failure instead of losing return values"] = function()
    local f = fixture()
    local s = f:loadSession()
    local scope = s:open()
    check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    f.removeFailures = 1
    local ok, why = pcall(s.stop)
    check(not ok and why:find("Lua-stop helper cleanup incomplete", 1, true))
    check(why:find("injected removal failure", 1, true))
    check(count(f.contexts) == 1, "failure falsely reported removal")
    check(s.stop())
    f:empty()
end

cases["thrown scope cleanup error does not abandon healthy scopes"] = function()
    local f = fixture()
    local s = f:loadSession()
    local broken = s:open()
    f.unbindThrowHandle = check(broken:Bind("A", s.api.Helpers.Trigger.Tap, function() end))
    local healthy = s:open()
    check(healthy:Bind("B", s.api.Helpers.Trigger.Tap, function() end))
    local ok, why = s.cleanup()
    check(not ok and why:find("injected scope exception", 1, true))
    check(count(f.contexts) == 1 and f.removals == 1, "healthy scope was abandoned")
    f.unbindThrowHandle = nil
    check(s.cleanup())
    f:empty()
end

cases["public UnbindAll retains failed contexts and supports repeated retries"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local scope = s:open(true)
    local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() calls = calls + 1 end))
    local late = f:queued(handle)
    f.removeFailures = 2
    for _ = 1, 2 do
        local _, ok, why = s.api.UnbindAll()
        check(not ok and why:find("injected removal failure", 1, true))
        check(count(f.targets) == 1 and count(f.contexts) == 1 and count(f.bindings) == 0)
        late()
        check(calls == 0)
    end
    check(scope:Close())
    local _, ok = s.api.UnbindAll()
    check(ok)
    f:empty()
end

cases["public UnbindAll reconciles partial subscription removal"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local scope = s:open(true)
    local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() calls = calls + 1 end))
    local late = f:queued(handle)
    f.unbindFailures = 1
    local _, ok = s.api.UnbindAll()
    check(not ok and count(f.targets) == 1 and count(f.bindings) == 0)
    late()
    check(calls == 0)
    local _, retried = s.api.UnbindAll()
    check(retried)
    check(scope:Close())
    f:empty()
end

cases["public UnbindAll bulk failure keeps ownership but suppresses callbacks"] = function()
    local f, calls = fixture(), 0
    local s = f:loadSession()
    local scope = s:open()
    local handle = check(scope:Bind("A", s.api.Helpers.Trigger.Tap, function() calls = calls + 1 end))
    local late = f:queued(handle)
    f.unbindFailures = 1
    f.bulkFailure = true
    local _, ok, why = s.api.UnbindAll()
    check(not ok and why:find("injected bulk failure", 1, true))
    check(count(f.bindings) == 1 and count(f.targets) == 1)
    late()
    check(calls == 0)
    f.bulkFailure = false
    local _, retried = s.api.UnbindAll()
    check(retried)
    f:empty()
end

local names = {}
for name in pairs(cases) do names[#names + 1] = name end
table.sort(names)
local suite = Kit.suite("Lifecycle helper integration tests (simulated engine/native boundary)")
for _, name in ipairs(names) do suite.run(name, cases[name]) end
suite.finish()
