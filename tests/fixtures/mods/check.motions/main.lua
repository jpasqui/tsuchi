local actor_api = entity
local engine_actor = actor
local component_api = component
local file_api = file

-- Read the motions of the first player's captain and dog, then hold a motion, a move motion and a speed on this mod's
-- own suspended copy of a creature, hear the changes, and release each.
local check = require("check")
check.plan("found", "captain", "dog", "made", "mesh", "held", "moved", "speed", "changed", "released", "refused", "removed", "gone")
local targets = {}
for line in (file_api.read("targets.txt", 0, 1024) or ""):gmatch("[^\r\n]+") do targets[#targets + 1] = line end
local function names(line)
    local list = {}
    for name in (line or ""):gmatch("[^,]+") do list[#list + 1] = name end
    return list
end
local captain_machines, dog_machines, kinds, machine = names(targets[1]), names(targets[2]), names(targets[3]), targets[4]
local captain, dog, source, class, at, made, mesh, loaded
local before, held, before_move, held_move, before_speed, wanted_speed
local heard, heard_move, listening, listening_move
local phase, steps, made_at, held_at, removed_at = 0, 0, 0, 0, 0
local stepping

-- The skeletal mesh of an actor whose blueprint has every one of these state machines, with the names it has.
local function body_of(actor, wanted)
    for _, part in ipairs(component_api.list(actor) or {}) do
        local machines = animation.state_machines(part)
        if machines then
            local has = {}
            for _, name in ipairs(machines) do has[name] = true end
            local all = true
            for _, name in ipairs(wanted) do all = all and has[name] == true end
            if all then return part, machines end
        end
    end
end

-- Every state machine's motion, the move motion and the speed of a character's body, read without an error.
local function reads(character, wanted, who)
    check.expect(character ~= nil, "the first player's " .. who)
    local part, machines = body_of(character, wanted)
    check.expect(part ~= nil, who .. "'s mesh with the state machines " .. table.concat(wanted, ","))
    if not part then return end
    local told = {}
    for _, name in ipairs(machines) do
        local motion = check.result("ok", who .. "'s " .. name .. " motion", animation.motion(part, name))
        told[#told + 1] = name .. "=" .. tostring(motion)
    end
    local move = check.result("ok", who .. "'s move motion", animation.move_motion(part))
    local speed = check.result("ok", who .. "'s speed", animation.speed(part))
    check.expect(type(speed) == "number" and speed == speed, who .. "'s speed is a number")
    return table.concat(told, " ") .. " move=" .. tostring(move) .. " speed=" .. tostring(speed)
end

local function finish()
    if listening then listening:cancel() end
    if listening_move then listening_move:cancel() end
    if loaded then asset.release(loaded) end
    stepping:cancel()
end
local function failed_without(what, names_left)
    for _, name in ipairs(names_left) do check.run(name, function() check.expect(false, what) end) end
    finish()
end

stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 then
        if game.has_control() ~= true then check.run("found", function() check.wait("a loaded game with player control") end) return end
        captain, dog = player.captain(0), player.dog(0)
        -- The first creature in play of the kinds in targets.txt, in their order, or else any creature in play.
        local rank = {}
        for i, name in ipairs(kinds) do rank[name] = i end
        local best = math.huge
        for _, actor in ipairs(creature.list() or {}) do
            local found_class, found_at = reflection.class_of(actor), actor_api.pose(actor)
            local order = rank[actor_api.kind(actor)] or #kinds + 1
            if found_class and found_at and order < best then source, class, at, best = actor, found_class, found_at, order end
        end
        if not source and targets[9] then
            -- None in play: a creature of the class that targets.txt names, beside the captain.
            local why
            if not loaded then loaded, why = asset.load(targets[9]) end
            if not loaded then check.run("found", function() check.wait("a creature in play, or its class: " .. tostring(why)) end) return end
            source, class, at = captain, loaded, captain and actor_api.pose(captain)
        end
        if not source or not at then check.run("found", function() check.wait("a creature in play") end) return end
        check.run("found", function()
            check.expect(captain ~= nil and dog ~= nil and class ~= nil, "the first player's captain and dog, and the creature's class")
            return source == captain and "loaded " .. targets[9] or tostring(actor_api.kind(source))
        end)
        check.run("captain", function() return reads(captain, captain_machines, "captain") end)
        check.run("dog", function() return reads(dog, dog_machines, "dog") end)
        check.run("made", function()
            made = check.result("ok", "an owned creature", actor_api.spawn(class, {x = at.location.x + 200, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false))
            check.result("ok", "its decisions and movement suspended", actor_api.hold_suspended(made, {logic = true, movement = true}))
            -- Listening starts from what each mesh plays when it is first looked at, so it starts some steps before the holds.
            listening = check.result("ok", "listening for motions", animation.on_motion_changed(function(event)
                if held and event.component == mesh and event.state_machine == machine and event.motion == held and not heard then heard = event end
            end))
            listening_move = check.result("ok", "listening for move motions", animation.on_move_motion_changed(function(event)
                if held_move and event.component == mesh and event.motion == held_move and not heard_move then heard_move = event end
            end))
        end)
        if not made then failed_without("the creature was not made", {"mesh", "held", "moved", "speed", "changed", "released", "refused", "removed", "gone"}) return end
        made_at, phase = steps, 1
        return
    end
    if phase == 1 then
        mesh = body_of(made, {machine})
        if steps - made_at < 5 then return end
        if not mesh and steps - made_at < 60 then check.run("mesh", function() check.wait("the owned creature's mesh and blueprint") end) return end
        check.run("mesh", function()
            check.expect(mesh ~= nil, "the owned creature's mesh has the state machine " .. tostring(machine))
            if mesh then check.equal(component_api.owner(mesh), made, "the mesh belongs to its own creature") end
        end)
        if not mesh then failed_without("the mesh was not found", {"held", "moved", "speed", "changed", "released", "refused", "removed", "gone"}) return end
        check.run("held", function()
            before = check.result("ok", "the motion before", animation.motion(mesh, machine))
            held = before == targets[5] and targets[6] or targets[5]
            check.result("ok", "holding another motion", animation.hold_motion(mesh, machine, held))
            check.equal(animation.motion(mesh, machine), held, "the held motion read back")
            check.result("ok", "holding it again", animation.hold_motion(mesh, machine, held))
            before_speed = check.result("ok", "the speed before", animation.speed(mesh))
            wanted_speed = (before_speed or 0) + 50
            check.result("ok", "holding a speed", animation.hold_speed(mesh, wanted_speed))
            return machine .. ": " .. tostring(before) .. " -> " .. tostring(held)
        end)
        check.run("moved", function()
            before_move = check.result("ok", "the move motion before", animation.move_motion(mesh))
            held_move = before_move == targets[7] and targets[8] or targets[7]
            check.result("ok", "holding another move motion", animation.hold_move_motion(mesh, held_move))
            check.equal(animation.move_motion(mesh), held_move, "the held move motion read back")
            return tostring(before_move) .. " -> " .. tostring(held_move)
        end)
        held_at, phase = steps, 2
        return
    end
    if phase == 2 then
        if steps - held_at < 20 then return end
        check.run("speed", function()
            -- Some blueprints ease toward a speed over several steps.
            local now = check.result("ok", "the held speed", animation.speed(mesh))
            check.expect(now == wanted_speed or math.abs(now - wanted_speed) < math.abs(before_speed - wanted_speed), "the speed went toward the held one: " .. tostring(before_speed) .. " -> " .. tostring(now))
            check.result("ok", "releasing the speed", animation.release_speed(mesh))
            check.result("not_found", "releasing it twice", animation.release_speed(mesh))
            return tostring(before_speed) .. " -> " .. tostring(now) .. " toward " .. tostring(wanted_speed)
        end)
        check.run("changed", function()
            check.expect(heard ~= nil, "the held motion was heard")
            if heard then
                check.equal(heard.motion, held, "the heard motion")
                check.equal(heard.previous, before, "the motion before it")
            end
            check.expect(heard_move ~= nil, "the held move motion was heard")
            if heard_move then
                check.equal(heard_move.motion, held_move, "the heard move motion")
                check.equal(heard_move.previous, before_move, "the move motion before it")
            end
        end)
        check.run("released", function()
            check.equal(animation.motion(mesh, machine), held, "the motion is still held")
            check.result("ok", "releasing the motion", animation.release_motion(mesh, machine))
            check.equal(animation.motion(mesh, machine), before, "the motion the game last set")
            check.result("ok", "releasing the move motion", animation.release_move_motion(mesh))
            check.equal(animation.move_motion(mesh), before_move, "the move motion the game last set")
            check.result("not_found", "releasing the motion twice", animation.release_motion(mesh, machine))
            check.result("not_found", "releasing the move motion twice", animation.release_move_motion(mesh))
        end)
        check.run("refused", function()
            check.result("not_found", "a state machine the blueprint lacks", animation.motion(mesh, "TsuchiNoSuchMachine"))
            check.result("not_found", "holding a state machine the blueprint lacks", animation.hold_motion(mesh, "TsuchiNoSuchMachine", held))
            check.result("invalid", "a name too long to be one", animation.hold_motion(mesh, machine, string.rep("x", 2000)))
            local root = actor_api.pose(made) and component_api.list(made)
            local plain
            for _, part in ipairs(root or {}) do
                local machines, why = animation.state_machines(part)
                if not machines and why == "unsupported" then plain = part break end
            end
            check.expect(plain ~= nil, "a part of the creature without a blueprint of the game's own")
            if plain then check.result("unsupported", "the motions of that part", animation.move_motion(plain)) end
        end)
        if listening then listening:cancel() listening = nil end
        if listening_move then listening_move:cancel() listening_move = nil end
        check.run("removed", function() check.result("ok", "removing its owned creature", actor_api.destroy(made)) end)
        removed_at, phase = steps, 3
        return
    end
    if engine_actor.alive(made) and steps - removed_at < 60 then return end
    check.run("gone", function()
        check.equal(check.result("ok", "the removed creature's life", engine_actor.alive(made)), false, "the creature left play")
        check.result("not_found", "the old creature's place", actor_api.pose(made))
    end)
    finish()
end)
