-- Checks of the reflect service from a script. The host gives `red` (a creature,
-- which is an actor), and `body` and `spare` (two components).

local function check(condition, what)
    if not condition then error(what or "check failed", 2) end
end
local function fails(with, value, why)
    return value == nil and why == with
end

-- Classes and names.
local Creature = reflection.find_class("Creature")
local Actor = reflection.find_class("Actor")
check(Creature and Actor and Creature ~= Actor)
check(reflection.class_of(red) == Creature, "the same class is the same value")
check(reflection.is_a(red, Actor) == true and reflection.is_a(body, Actor) == false)
check(reflection.name(red) == "Red" and reflection.name(Creature) == "Creature")
check(fails("not_found", reflection.find_class("Onion")))

-- Values come back as what they are.
check(reflection.get(red, "Health") == 12.5 and math.type(reflection.get(red, "Health")) == "float")
check(reflection.get(red, "Lives") == -3 and math.type(reflection.get(red, "Lives")) == "integer")
check(reflection.get(red, "bCanBeDamaged") == true and reflection.get(red, "bHidden") == false)
check(reflection.get(red, "Tag") == "Leader")
check(reflection.get(red, "Label") == "Olimar é🌱")
check(reflection.get(red, "Root") == body, "a reference is a handle, and the same one")
check(reflection.get(red, "Root.Scale") == 2.0)

-- A struct is a table of its fields, an array is a list.
local location = reflection.get(red, "Location")
check(location.X == 0 and location.Y == 7 and location.Z == 0)
local scores = reflection.get(red, "Scores")
check(#scores == 3 and scores[1] == 10 and scores[3] == 30)
local points = reflection.get(red, "Points")
check(#points == 2 and points[2].Z == 9 and points[1].Z == 0)
local tags = reflection.get(red, "Tags")
check(#tags == 2 and tags[1] == "Big" and tags[2] == "Night_2")
check(reflection.get(red, "Corners")[2].Y == 5)

-- What cannot be read says why.
check(fails("not_found", reflection.get(red, "Nope")))
check(fails("invalid", reflection.get(red, "Tags[")))
check(fails("unsupported", reflection.get(red, "Lookup")))
check(fails("unsupported", reflection.get(red, "Odd")), "a list with holes in it would lie about its length")
check(fails("not_found", reflection.get(red, string.rep("a", 600))))
check(fails("invalid", reflection.get(red, string.rep("a", 5000))))

-- Writing plain values.
check(reflection.set(red, "Health", 3) == true and reflection.get(red, "Health") == 3.0)
check(reflection.set(red, "Health", 40.25) and reflection.get(red, "Health") == 40.25)
check(fails("invalid", reflection.set(red, "Lives", 2.5)) and reflection.get(red, "Lives") == -3)
check(fails("invalid", reflection.set(red, "Health", "a lot")))
check(reflection.set(red, "bHidden", true) and reflection.get(red, "bHidden") == true)
check(reflection.set(red, "Tag", "Chief") and reflection.get(red, "Tag") == "Chief")
check(reflection.set(red, "Label", "Louie é") and reflection.get(red, "Label") == "Louie é")
check(fails("invalid", reflection.set(red, "Label", "bad\xFF")) and reflection.get(red, "Label") == "Louie é")
check(fails("invalid", reflection.set(red, "Label", "cut\0short")))

-- References: a handle of the right class, or nil for none.
check(reflection.set(red, "Root", spare) and reflection.get(red, "Root") == spare)
check(fails("invalid", reflection.set(red, "Root", red)), "an actor is not a component")
check(fails("invalid", reflection.set(red, "Root", io.stdout)), "some other userdata is not a handle")
check(reflection.set(red, "Root", nil))
local root, why = reflection.get(red, "Root")
check(root == nil and why == nil, "an empty reference is nil, and not a failure")
check(reflection.set(red, "Root", body))
-- A weak reference is a handle too, or nil.
check(reflection.get(red, "Target") == nil and reflection.set(red, "Target", spare) and reflection.get(red, "Target") == spare)
check(reflection.get(red, "Target.Scale") == reflection.get(spare, "Scale") and reflection.set(red, "Target", nil) and reflection.get(red, "Target") == nil)

-- A table writes the fields it names and leaves the others. Names ignore case.
check(reflection.set(red, "Location", {X = 1, z = 3}))
location = reflection.get(red, "Location")
check(location.X == 1 and location.Y == 7 and location.Z == 3)
check(fails("not_found", reflection.set(red, "Location", {W = 1})))
check(fails("invalid", reflection.set(red, "Location", {[1] = 1})))
check(fails("unsupported", reflection.set(red, "Location", 5)))
check(fails("invalid", reflection.set(red, "Health", {X = 1})))

-- A list becomes the array's whole contents.
check(reflection.set(red, "Tags", {"A", "B", "C"}))
tags = reflection.get(red, "Tags")
check(#tags == 3 and tags[1] == "A" and tags[3] == "C")
check(reflection.set(red, "Tags", {}) and #reflection.get(red, "Tags") == 0)
check(reflection.set(red, "Points", {{X = 1}, {Y = 2}, {Z = 3}}))
points = reflection.get(red, "Points")
check(#points == 3 and points[1].X == 1 and points[2].Y == 2 and points[3].Z == 3 and points[3].X == 0)
check(reflection.set(red, "Notes", {"one", "two"}) and reflection.get(red, "Notes")[2] == "two")
check(reflection.set(red, "Notes", {"only"}) and #reflection.get(red, "Notes") == 1)
-- A fixed number of values takes a list of that length and no other.
check(reflection.set(red, "Scores", {1, 2, 3}) and reflection.get(red, "Scores")[2] == 2)
check(fails("invalid", reflection.set(red, "Scores", {1, 2})))
check(fails("unsupported", reflection.set(red, "Odd", {1})))

-- A late failure leaves every earlier element and the original length in place.
check(fails("invalid", reflection.set(red, "Scores", {99, 2147483648, 77})))
scores = reflection.get(red, "Scores")
check(#scores == 3 and scores[1] == 1 and scores[2] == 2 and scores[3] == 3,
      "a late fixed-array overflow leaves the whole array")
check(fails("invalid", reflection.set(red, "Notes", {"changed", "bad\xFF", "third"})))
local notes = reflection.get(red, "Notes")
check(#notes == 1 and notes[1] == "only", "a late text failure leaves contents and length")
check(fails("invalid", reflection.set(red, "Points", {{X = 99}, {Y = 88}, {Z = "bad"}, {X = 77}})))
points = reflection.get(red, "Points")
check(#points == 3 and points[1].X == 1 and points[2].Y == 2 and points[3].Z == 3,
      "a late struct-array failure leaves all elements and length")
check(fails("invalid", reflection.set(red, "Notes", {"changed", "cut\0short"})))
check(#reflection.get(red, "Notes") == 1 and reflection.get(red, "Notes")[1] == "only")
local cyclic = {X = 99}; cyclic.Y = cyclic
check(fails("unsupported", reflection.set(red, "Location", cyclic)))
check(reflection.get(red, "Location.X") == 1, "a cycle is rejected before any fields change")

check(fails("capacity", reflection.set(red, "Notes", {[1000001] = "large"})))
check(#reflection.get(red, "Notes") == 1 and reflection.get(red, "Notes")[1] == "only")
check(fails("invalid", reflection.set(red, "Location", {X = 99, x = 88})))
check(reflection.get(red, "Location.X") == 1, "aliases cannot write one field twice")

-- Calling functions.
check(reflection.set(red, "Health", 10))
local result, written = reflection.call(red, "Heal", {Amount = 2.5})
check(result == 12.5 and type(written) == "table" and next(written) == nil)
check(reflection.get(red, "Health") == 12.5)
check(reflection.call(red, "Heal", {bFull = true}) == 100)
check(reflection.call(red, "Describe") == "Red Bulborb")

local where
where, written = reflection.call(red, "Where")
check(where.X == 1 and where.Y == 2 and where.Z == 3)
check(written.Found == red, "an actor comes back as the actor the host gave")
check(written.ReturnValue == nil)

-- A struct with a parent comes back with the fields of both.
local box = reflection.call(red, "Measure")
check(box.Width == 3 and box.Height == 2)

-- A function of a class, on the class or on one of its objects.
check(reflection.call(Creature, "Spawn", {Count = 21}) == 42)
check(reflection.call(red, "Spawn", {Count = 4}) == 8)

-- No result, no arguments.
local anger = reflection.get(red, "Anger")
result, written = reflection.call(red, "Jump")
check(result == nil and type(written) == "table" and reflection.get(red, "Anger") == anger + 1)

-- Text and lists as arguments.
check(reflection.call(red, "Sum", {Values = {1, 2, 3}}) == 6)
check(reflection.call(red, "Sum", {Values = {}}) == 0)
result, written = reflection.call(red, "Rename", {NewName = "Skipper", Title = "Captain"})
check(result == nil and next(written) == nil, "an argument the function only reads is not given back")
check(reflection.get(red, "Tag") == "Skipper")

-- Names far longer than usual: a field, a struct's part with another after it, and a function's results.
local long = string.rep("L", 300)
check(reflection.get(wordy, long) == 5)
local wide = reflection.get(wordy, "Shape")
check(wide[long] == 7 and wide.After == 9, "the part after a long name is still read")
check(reflection.set(wordy, "Shape", {[long] = 8}) and reflection.get(wordy, "Shape." .. long) == 8)
result, written = reflection.call(wordy, long)
check(result == nil and written[long] == 41 and written.Tail == 42)

-- A call that cannot be made says why, and one that is given nonsense raises.
check(fails("not_found", reflection.call(red, "Fly")))
check(fails("not_found", reflection.call(red, "Heal", {Speed = 1})))
check(fails("invalid", reflection.call(red, "Heal", {Amount = "plenty"})))
check(fails("invalid", reflection.call(red, "Heal", {[1] = 2})))
check(fails("not_found", reflection.call(body, "Heal")), "a component has no such function")
check(not pcall(reflection.call, red, "Heal", 5))
check(not pcall(reflection.call, red))
check(not pcall(reflection.get, 5, "Health"))
check(not pcall(reflection.get, red))
check(not pcall(reflection.set, red, "Health"))

-- The typed services take the same handle.
local original_health = reflection.get(red, "Health")
local held = assert(reflection.hold_field(red, "Health", 0.1, ""))
check(reflection.get(red, "Health") > 0.09)
check(reflection.update_field(held, 7) and reflection.get(red, "Health") == 7)
check(reflection.release_field(held) and reflection.get(red, "Health") == original_health)
check(fails("invalid", reflection.hold_field(red, "Health", {}, "")))
check(fails("invalid", reflection.hold_field(red, "Health", 0/0, "")))
check(fails("invalid", reflection.hold_field(red, "Health\0ignored", 1, "")))
check(fails("not_found", reflection.update_field(held, 1)))
check(reflection.get(red, "Health") == original_health)
check(combat ~= nil and not pcall(combat.life, body), "a component is not an actor")
