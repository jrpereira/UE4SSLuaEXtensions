-- TestKit: assertions and a case runner for the extensions' offline Lua suites
-- (lua5.4). tests/run.sh puts shared/tests on LUA_PATH, so a suite does
--   local Kit = require("TestKit")
--   local expect, expectFailure, expectRaise = Kit.expect, Kit.expectFailure, Kit.expectRaise
--   local suite = Kit.suite("My suite")
--   suite.run("does a thing", function() expect(...) end)
--   suite.finish()   -- prints the count and exits non-zero on any failure

local Kit = {}

function Kit.expect(condition, message)
    if not condition then error(message or "expectation failed", 2) end
    return condition
end

-- Expects an operation's failure: nil, `code`, message. Returns the message.
function Kit.expectFailure(code, ...)
    local value, actual, message = ...
    if value ~= nil or actual ~= code or type(message) ~= "string" then
        error(string.format("expected nil, %q, message; got %s, %s, %s",
            code, tostring(value), tostring(actual), tostring(message)), 2)
    end
    return message
end

-- Expects fn(...) to raise an error whose message contains `fragment`.
function Kit.expectRaise(fragment, fn, ...)
    local ok, message = pcall(fn, ...)
    if ok then error("expected an error containing " .. fragment, 2) end
    message = tostring(message)
    if not message:find(fragment, 1, true) then
        error("expected an error containing " .. fragment .. ", got " .. message, 2)
    end
    return message
end

-- Runs cases, reporting each failure with its traceback, and counts them.
function Kit.suite(label)
    local suite = { passed = 0, total = 0 }

    function suite.run(name, fn, ...)
        suite.total = suite.total + 1
        local ok, failure = xpcall(fn, debug.traceback, ...)
        if ok then
            suite.passed = suite.passed + 1
        else
            print("FAIL " .. name .. "\n" .. tostring(failure))
        end
        return ok
    end

    function suite.finish()
        print(string.format("%s: %d of %d passed", label, suite.passed, suite.total))
        if suite.passed ~= suite.total then os.exit(1) end
    end

    return suite
end

return Kit
