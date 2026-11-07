-- Checks a script mod makes in the game, written to its log.
--
-- A check is one thing a mod finds out about the running game and reports as passed or failed.
-- It is lines in the mod's log, one for each thing that happens to it:
--
--     check <name> planned
--     check <name> waiting <for what>
--     check <name> pass [<note>]
--     check <name> fail <why>
--
-- Put this file in the mod's folder. A mod plans its checks as it starts, and runs each as a
-- function that states what it expects:
--
--     local check = require("check")
--     check.plan("refused")
--     check.run("refused", function()
--         check.result("invalid", "a name that leads out", storage.write("../escape", "x"))
--     end)
--
-- That writes `check refused pass`, or `check refused fail a name that leads out: got ok, wanted
-- invalid`. The first expectation that fails is the one told. An error in the function fails the
-- check with the error's text. A check that cannot run yet calls `check.wait` with what it waits
-- for, and `check.retry` tries it again as steps go by.
local check = {}

local failed, why    -- of the check that is running: whether an expectation has failed, and the first that did
local waits = {}     -- by a check's name: what it last said it waits for
local WAIT = {}      -- what check.wait raises

local function shown(value)
    if type(value) == "string" then return string.format("%q", value) end
    return tostring(value)
end

-- Says that the mod has checks of these names. Call it as the mod starts, with every check, so
-- that whoever reads the log knows of a check that never ran.
function check.plan(...)
    for _, name in ipairs({...}) do mod.log("check " .. name .. " planned") end
end

-- Expects something to be true. `what` says what, for the line a failure writes. Every
-- expectation returns whether it held.
function check.expect(held, what)
    if not held and not failed then failed, why = true, what end
    return held and true or false
end

-- Expects a value to be equal to another.
function check.equal(got, wanted, what)
    return check.expect(got == wanted, what .. ": got " .. shown(got) .. ", wanted " .. shown(wanted))
end

-- Expects a number to within `slack` either way.
function check.near(got, wanted, slack, what)
    local held = type(got) == "number" and got >= wanted - slack and got <= wanted + slack
    return check.expect(held, what .. ": got " .. shown(got) .. ", wanted " .. shown(wanted))
end

-- Expects a call to have answered as it should. The call goes last, so that all it returns is
-- seen: check.result("ok", "the piece is kept", storage.write("a", "b")). `wanted` is a result's
-- name. A call that fails returns nil and the name; anything else is `ok`. Returns what the
-- call returned first.
function check.result(wanted, what, value, name)
    local got = value == nil and name or "ok"
    check.expect(got == wanted, what .. ": got " .. tostring(got) .. ", wanted " .. wanted)
    return value
end

-- Says that the check that is running cannot run yet, or cannot finish, and what it waits for.
-- It ends the function there. The check has not ended: run it again later.
function check.wait(for_what)
    error({WAIT, for_what}, 0)
end

-- Runs one check: `body` states what it expects. Writes `pass`, with what `body` returns as a
-- note when it returns text, or `fail` with the first expectation that did not hold. Returns
-- "pass", "fail", or "waiting" when `body` called check.wait.
function check.run(name, body)
    failed, why = false, nil
    local ran, note = pcall(body)
    if not ran and not failed and type(note) == "table" and note[1] == WAIT then
        if waits[name] ~= note[2] then
            waits[name] = note[2]
            mod.log("check " .. name .. " waiting " .. tostring(note[2]))
        end
        return "waiting"
    end
    if not ran then check.expect(false, "error: " .. (type(note) == "string" and note or "a value that is no text")) end
    if failed then
        mod.log("check " .. name .. " fail " .. tostring(why))
        return "fail"
    end
    mod.log("check " .. name .. (type(note) == "string" and " pass " .. note or " pass"))
    return "pass"
end

-- Runs a check now, and while it waits again every `steps` steps of the world: 30, a second,
-- when none is given. `again` is a function that runs the check and returns what check.run
-- returned.
function check.retry(again, steps)
    if again() ~= "waiting" then return end
    local count, listening = 0, nil
    listening = time.on_step(function()
        count = count + 1
        if count % (steps or 30) == 0 and again() ~= "waiting" then listening:cancel() end
    end)
end

return check
