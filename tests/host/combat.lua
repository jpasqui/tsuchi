-- Runs against tests/host/fake_host.cpp: actors 1 and 2 are captains with 100 life,
-- actor 3 is a creature with 500.
local olimar, louie, bulborb = test.actor(1), test.actor(2), test.actor(3)

-- World targets use the same empty handle the native Look contract accepts.
local point = {x=1,y=2,z=3}
assert(camera.hold_look(0,{point=point}))
assert(camera.hold_look(0,{actor=nil,point=point}))
assert(camera.hold_look(0,{actor=olimar,point=point}))
assert(not pcall(camera.hold_look,0,{actor=1,point=point}))

-- Handles: the same actor is the same value, so it works as a table key.
assert(olimar == test.actor(1) and olimar ~= louie)
local seen = {[olimar] = true}
assert(seen[test.actor(1)] and not seen[louie])
assert(test.actor(0) == nil)
assert(tostring(olimar):find('^tsuchi.actor: 1$'))

-- A returned struct is a table of its fields.
local life = assert(combat.life(louie))
assert(life.life == 100 and life.max_life == 100 and life.extra == 20 and life.max_extra == 20)

-- A failed call returns nil and the result's name.
local value, why = combat.life(test.actor(99))
assert(value == nil and why == 'not_found')
value, why = combat.set_life(olimar, -1)
assert(value == nil and why == 'invalid')
value, why = combat.knockback(bulborb, {x = 0, y = 0, z = 100})
assert(value == nil and why == 'unsupported')

-- A wrong argument is the script's mistake and raises.
assert(not pcall(combat.life, 1))
assert(not pcall(combat.life, nil))
assert(not pcall(combat.damage, olimar, nil, 10, 'lava'))
assert(not pcall(combat.set_life, olimar, 50, {gradual = true}))
assert(not pcall(combat.knockback, olimar, nil, {1, 2, 3}))

-- Flags, defaults, enums and vectors reach the service as written.
assert(combat.set_life(olimar, 80, {smooth_gauge = true}) == true)
assert(test.last().flags == 1 and combat.life(olimar).life == 80)
assert(combat.set_life(olimar, 100) and test.last().flags == 0)
assert(combat.knockback(olimar, {x = 1, y = 2, z = 300}, 5, bulborb))
local velocity = test.last().velocity
assert(velocity.x == 1 and velocity.y == 2 and velocity.z == 300)

-- A number of the game's that has no name is told as "#" and the number, a string like any name. A bit of a
-- flag set that has no name is a key of the same kind, beside the bits that have names.
local unnamed, bits = test.told(12)
assert(unnamed == '#12' and test.told(0) == '#0' and test.told(4294967295) == '#4294967295')
assert(bits.both == true and bits['#8'] == true and bits.smooth_gauge == nil and next(bits, next(bits, next(bits))) == nil)
unnamed, bits = test.told(1)
assert(bits.smooth_gauge == true and next(bits, 'smooth_gauge') == nil)

-- Events arrive with their fields; an optional handle is nil when there is none.
local hits, downs = {}, {}
local on_hit = assert(combat.on_damaged(function(event) hits[#hits + 1] = event end))
local on_down = assert(combat.on_defeated(function(event) downs[#downs + 1] = event end))
assert(test.subscriptions() == 2)
assert(combat.damage(olimar, 30, 'fire', bulborb))
assert(test.last().kind == 3)
assert(#hits == 1 and #downs == 0)
assert(hits[1].actor == olimar and hits[1].attacker == bulborb and hits[1].previous == 100 and hits[1].life == 70)
assert(combat.damage(olimar, 70))
assert(test.last().kind == 0)
assert(#hits == 2 and hits[2].attacker == nil)
assert(#downs == 1 and downs[1].actor == olimar and downs[1].life == 0)

-- Canceling ends delivery, and a second cancel says so.
assert(on_hit:cancel() == true)
assert(test.subscriptions() == 1)
value, why = on_hit:cancel()
assert(value == nil and why == 'not_found')
assert(combat.damage(louie, 1))
assert(#hits == 2)

-- An error in a handler is reported and does not stop the game or later events,
-- whatever kind of value it raises.
on_down:cancel()
local calls = 0
combat.on_damaged(function() calls = calls + 1; error({}) end)
combat.on_damaged(function() calls = calls + 1; error('plain') end)
assert(combat.damage(louie, 1))
assert(calls == 2 and test.errors() == 2)

-- A handler can cancel its own subscription while it runs.
local once
once = combat.on_damaged(function() once:cancel() end)
local before = test.subscriptions()
assert(combat.damage(louie, 1))
assert(test.subscriptions() == before - 1)

-- A value that is not a number is refused before the game sees it.
value, why = combat.set_life(louie, 0 / 0)
assert(value == nil and why == 'invalid')
value, why = combat.knockback(louie, {x = 0, y = 1 / 0, z = 0})
assert(value == nil and why == 'invalid')

-- A call the framework has deprecated still works. Its mod is told once, in its log, what to use in its place.
assert(test.logged() == 0)
assert(test.deprecated('combat.old_call', 3, 'combat.new_call') == 3)
local lines, line = test.logged()
assert(lines == 1 and line == 'combat.old_call is deprecated since version 3 of the framework: use combat.new_call')
test.deprecated('combat.old_call', 3, 'combat.new_call')
assert(test.logged() == 1)
test.deprecated('combat.on_old_event', 4, 'combat.new_event')
lines, line = test.logged()
assert(lines == 2 and line == 'combat.on_old_event is deprecated since version 4 of the framework: use combat.new_event')

-- Left subscribed on purpose. The host stops the mod and checks that this ends.
handler_calls = 0
combat.on_damaged(function() handler_calls = handler_calls + 1 end)
assert(combat.damage(louie, 1) and handler_calls == 1)

-- Steps and pictures. The host runs three frames after this script returns, and checks these. A step tells
-- its number and no time; the picture that follows it tells the time, and is all the way at the step.
frames_seen, steps_at_last_frame, last_frame_number, pictures_seen = 0, 0, 0, 0
time.on_step(function(step)
  assert(pictures_seen == frames_seen and step.seconds == nil)
  frames_seen = frames_seen + 1
  steps_at_last_frame = test.steps()
  last_frame_number = step.number
end)
time.on_presented(function(picture)
  pictures_seen = pictures_seen + 1
  assert(pictures_seen == frames_seen and picture.seconds == 0.5 and picture.fraction == 1)
end)
