-- hook.engine_function against the made-up engine (tests/core/engine_hooks_test.cpp). Heal(Amount, bFull) gives the
-- new health; Where() gives a vector and writes Found; Rename(NewName, Title) takes a name and a string; OnPoke
-- and OnOther are events that run as entry points 42 and 7 of their class's event graph.

-- Refused before anything goes on.
local none, why = hook.engine_function('/Script/Test.Actor:Nope', {before = function() end})
assert(none == nil and why == 'not_found', why)
none, why = hook.engine_function('/Script/Test.Actor:Heal', {})
assert(none == nil and why == 'invalid', why)
assert(not pcall(hook.engine_function, '/Script/Test.Actor:Heal', {before = 3}))
-- The engine's functions that take a value of any type are refused, by library or by name, whatever the case.
none, why = hook.engine_function('/Script/Engine.KismetArrayLibrary:Array_Add', {before = function() end})
assert(none == nil and why == 'unsupported', why)
none, why = hook.engine_function('/script/engine.kismetsystemlibrary:setstructurepropertybyname', {after = function() end})
assert(none == nil and why == 'unsupported', why)
local plain = assert(hook.engine_function('/Script/Engine.KismetSystemLibrary:PrintString', {before = function() end}))
assert(hook.remove(plain))

-- `before` changes an argument, `after` replaces the result.
local seen = {}
local heal_hook
heal_hook = assert(hook.engine_function('/Script/Test.Actor:Heal', {
    before = function(call)
        seen.object, seen.amount, seen.full = call.object, call.args.Amount, call.args.bFull
        seen.result_before = call.result
        seen.remove = {hook.remove(heal_hook)} -- every other call fails inside a hook
        call.args.Amount = call.args.Amount * 2
    end,
    after = function(call)
        seen.result, seen.after_amount = call.result, call.args.Amount
        return call.result + 1
    end,
}))
assert(heal(5, false) == 23.5)
assert(health() == 22.5)
assert(seen.object == red and seen.amount == 5 and seen.full == false and seen.result_before == nil)
assert(seen.remove[1] == nil and seen.remove[2] == 'invalid')
assert(seen.result == 22.5 and seen.after_amount == 10)

-- A second hook, with a path in other case: `before` runs newest first, `after` oldest first.
local order = {}
local second = assert(hook.engine_function('/script/test.actor:HEAL', {
    before = function() order[#order + 1] = 'second before' end,
    after = function(call) order[#order + 1] = 'second after ' .. call.result end,
}))
assert(heal(1, false) == 25.5) -- 22.5 + 2, and the first hook's + 1
assert(#order == 2 and order[1] == 'second before' and order[2] == 'second after 25.5', table.concat(order, ', '))
assert(hook.remove(second))

-- An error drops that handler's changes, and the call goes on. It's told at the next frame.
local failing = assert(hook.engine_function('/Script/Test.Actor:Heal', {
    before = function(call) call.args.Amount = 1000 error('boom') end,
}))
local errors_before = errors()
assert(heal(1, false) == 27.5) -- the first hook's doubling stands; the failing one's 1000 doesn't
frame()
local count, message = errors()
assert(count == errors_before + 1 and message:find('boom'), message)
assert(hook.remove(failing))

-- A handler that runs away is stopped at its own allowance, switched off, and told.
local runaway = assert(hook.engine_function('/Script/Test.Actor:Heal', {before = function() while true do end end}))
assert(heal(1, false) == 29.5)
frame()
count, message = errors()
assert(count == errors_before + 2 and message:find('budget'), message)
assert(heal(1, false) == 31.5) -- switched off, the others still run
assert(hook.remove(runaway))
assert(hook.remove(heal_hook))
assert(not hook.remove(heal_hook))
assert(heal(1, false) == 31.5)

-- Calls that skip the handlers: one on another thread, and a Blueprint's call that hands over script code.
local skipped = 0
local counting = assert(hook.engine_function('/Script/Test.Actor:Heal', {before = function() skipped = skipped + 1 end}))
local stats = stats()
threaded_heal()
scripted_heal()
assert(skipped == 0)
assert(stats.off_thread + 1 == _G.stats().off_thread and stats.script_arguments + 1 == _G.stats().script_arguments)
heal(1, false)
assert(skipped == 1)
assert(hook.remove(counting))

-- A struct result and an argument the function writes: both read in `after`, and both replaced.
local where_hook = assert(hook.engine_function('/Script/Test.Actor:Where', {
    after = function(call)
        seen.where = call.result
        seen.found = call.args.Found
        return {X = 7, Y = 8, Z = 9}
    end,
}))
local x, y, z, found_red = where()
assert(x == 7 and y == 8 and z == 9 and found_red)
assert(seen.where.X == 1 and seen.where.Y == 2 and seen.where.Z == 3 and seen.found == red)
assert(hook.remove(where_hook))
-- A change that doesn't fit is refused whole: the object is the wrong class, so the result stays too.
where_hook = assert(hook.engine_function('/Script/Test.Actor:Where', {
    after = function(call)
        call.args.Found = body
        return {X = 70}
    end,
}))
x, y, z, found_red = where()
assert(x == 1 and y == 2 and z == 3 and found_red)
frame()
count, message = errors()
assert(count == errors_before + 3 and message:find('invalid'), message)
assert(hook.remove(where_hook))

-- Text: a name argument changed in `before`, and a string the function only reads.
local rename_hook = assert(hook.engine_function('/Script/Test.Actor:Rename', {
    before = function(call)
        seen.title = call.args.Title
        call.args.NewName = call.args.NewName .. ' Changed'
    end,
}))
local tag, heard = rename('Captain', 'Sir')
assert(tag == 'Captain Changed' and heard == 'Sir' and seen.title == 'Sir', tag .. '|' .. heard)
assert(hook.remove(rename_hook))

-- An event without arguments: its hook runs for its own entry point of the event graph, and only for that.
local pokes = 0
assert(hook.engine_function('/Script/Test.Creature:OnPoke', {
    before = function(call) assert(next(call.args) == nil and call.object == red) pokes = pokes + 1 end,
}))
local runs, entry = event(true)
assert(pokes == 1 and entry == 42)
local runs_after, other_entry = event(false)
assert(pokes == 1 and other_entry == 7 and runs_after == runs + 1)

-- A Blueprint's calls: the arguments are script code, which the hook runs for the handlers before the function.
local script_calls = _G.stats().script_calls
local bp = {}
local bp_heal = assert(hook.engine_function('/Script/Test.Actor:Heal', {
    before = function(call) bp.amount = call.args.Amount call.args.Amount = call.args.Amount * 2 end,
    after = function(call) bp.result = call.result return call.result + 1 end,
}))
local before_health = health()
assert(blueprint_heal(5, false) == before_health + 11)
assert(health() == before_health + 10 and bp.amount == 5 and bp.result == before_health + 10)
assert(blueprint_heal(1, true) == 101) -- bFull from the script code
assert(hook.remove(bp_heal))
assert(blueprint_heal(1, false) == health()) -- unhooked, the function runs its script code itself
-- An argument passed by reference is the caller's own variable.
local bp_where = assert(hook.engine_function('/Script/Test.Actor:Where', {
    after = function(call)
        bp.found = call.args.Found
        call.args.Found = nil
        return {X = 4, Y = 5, Z = 6}
    end,
}))
local bx, by, bz, bp_found_red = blueprint_where()
assert(bx == 4 and by == 5 and bz == 6 and not bp_found_red and bp.found == red)
assert(hook.remove(bp_where))
bx, by, bz, bp_found_red = blueprint_where()
assert(bx == 1 and bp_found_red)
local bp_rename = assert(hook.engine_function('/Script/Test.Actor:Rename', {
    before = function(call) bp.title = call.args.Title call.args.NewName = 'Louie' end,
}))
local bp_tag, bp_heard = blueprint_rename('Olimar', 'Captain')
assert(bp_tag == 'Louie' and bp_heard == 'Captain' and bp.title == 'Captain', bp_tag .. '|' .. bp_heard)
assert(hook.remove(bp_rename))
assert(_G.stats().script_calls == script_calls + 4 and _G.stats().bad_opcodes == 0)

-- The last hooks stay on, for the mod's stop to end.
assert(hook.engine_function('/Script/Test.Actor:Heal', {after = function() end}))
