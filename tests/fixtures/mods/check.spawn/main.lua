local actor_api = entity
local engine_actor = actor
local file_api = file

-- An actor made by this mod is briefly suspended, restored and quietly removed.
local check = require("check")
check.plan("found", "listened", "made", "class", "team", "placed", "suspended", "restored", "removed", "gone", "events", "cancelled", "refused")
local wanted = {}
for line in (file_api.read("targets.txt", 0, 1024) or ""):gmatch("[^\r\n]+") do wanted[line] = true end
local source, class, at, made, asked, entered, left
local born, ended, stepping
local phase, steps, removed_at, born_count, left_count = 0, 0, 0, 0, 0
local function expect_flags(flags, logic, movement)
    check.expect(type(flags) == "table", "the suspended parts were read")
    if not flags then return end
    check.equal(flags.logic == true, logic, "the logic hold")
    check.equal(flags.movement == true, movement, "the movement hold")
    check.expect(not flags.physics and not flags.collision, "the actor's body remains its own")
end
local function finish()
    check.run("events", function()
        check.expect(entered ~= nil and left ~= nil, "the actor's entry and departure were both heard")
        check.equal(born_count, 1, "one entry for its own actor")
        check.equal(left_count, 1, "one departure for its own actor")
        if entered and left then
            check.equal(entered.actor, made, "the actor that entered")
            check.equal(left.actor, made, "the actor that left")
            check.equal(entered.kind, left.kind, "the same actor's kind")
            check.equal(entered.team, left.team, "the same actor's team")
        end
    end)
    check.run("cancelled", function()
        check.expect(born ~= nil and ended ~= nil, "both listeners were registered")
        for _, listener in pairs({born, ended}) do
            check.result("ok", "ending the actor listener", listener:cancel())
            check.result("not_found", "ending it twice", listener:cancel())
        end
    end)
    stepping:cancel()
end
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 and game.has_control() ~= true then check.run("found", function() check.wait("a loaded game with player control") end) return end
    if phase == 0 then
        for _, actor in ipairs(actor_api.list() or {}) do
            if wanted[actor_api.kind(actor)] then
                local found_class, found_at = reflection.class_of(actor), actor_api.pose(actor)
                if found_class and found_at then source, class, at = actor, found_class, found_at break end
            end
        end
        if not source then
            check.run("found", function() check.wait("a placed actor of a kind in targets.txt") end)
            return
        end
        check.run("found", function()
            check.equal(engine_actor.alive(source), true, "the source is in play")
            check.expect(class ~= nil and at ~= nil, "its class and place were read")
        end)
        check.run("listened", function()
            born = check.result("ok", "listening for entry", actor_api.on_spawned(function(event) if event.actor == made then entered, born_count = event, born_count + 1 end end))
            ended = check.result("ok", "listening for departure", actor_api.on_destroyed(function(event) if event.actor == made then left, left_count = event, left_count + 1 end end))
        end)
        asked = {x = at.location.x + 150, y = at.location.y, z = at.location.z + 20}
        check.run("made", function()
            made = check.result("ok", "making its own actor", actor_api.spawn(class, asked, at.rotation, -1, false))
            check.expect(made ~= nil and made ~= source, "the new actor is its own")
        end)
        if not made then
            for _, name in ipairs({"class", "team", "placed", "suspended", "restored", "removed", "gone", "refused"}) do
                check.run(name, function() check.expect(false, "the actor was not made") end)
            end
            finish()
            return
        end
        phase = 1
        return
    end
    if phase == 1 then
        check.run("class", function()
            check.equal(check.result("ok", "the new actor's class", reflection.class_of(made)), class, "the class asked for")
            check.equal(check.result("ok", "whether it is alive", engine_actor.alive(made)), true, "the new actor is in play")
        end)
        -- The game copies a request's team onto the actor, so a request laid out wrongly reads back wrong here. Passing no
        -- team must leave the class's own, and a team asked for must be that team.
        check.run("team", function()
            local own = check.result("ok", "the source's team", actor_api.team(source))
            check.equal(check.result("ok", "the new actor's team", actor_api.team(made)), own, "the class's own team, with none asked for")
            local asked_team = own == 1 and 2 or 1
            local other = check.result("ok", "making one on a team", actor_api.spawn(class, {x = asked.x - 300, y = asked.y, z = asked.z}, at.rotation, asked_team, false))
            check.equal(check.result("ok", "that actor's team", actor_api.team(other)), asked_team, "the team asked for")
            check.result("ok", "removing that one", actor_api.destroy(other))
        end)
        check.run("placed", function()
            local here = check.result("ok", "the new actor's place", actor_api.pose(made))
            check.near(here.location.x, asked.x, 60, "the requested x")
            check.near(here.location.y, asked.y, 60, "the requested y")
        end)
        check.run("suspended", function()
            expect_flags(check.result("ok", "before the holds", actor_api.suspended(made)), false, false)
            check.result("ok", "stopping decisions", actor_api.hold_suspended(made, {logic = true}))
            check.result("ok", "stopping movement", actor_api.hold_suspended(made, {movement = true}))
            check.result("ok", "coalescing both holds", actor_api.hold_suspended(made, {logic = true, movement = true}))
            expect_flags(check.result("ok", "both held parts", actor_api.suspended(made)), true, true)
        end)
        check.run("restored", function()
            check.result("ok", "giving decisions back", actor_api.release_suspended(made, {logic = true}))
            expect_flags(check.result("ok", "movement remains held", actor_api.suspended(made)), false, true)
            check.result("ok", "giving movement back", actor_api.release_suspended(made, {movement = true}))
            expect_flags(check.result("ok", "after the holds", actor_api.suspended(made)), false, false)
            check.result("not_found", "ending the hold twice", actor_api.release_suspended(made, {logic = true}))
        end)
        actor_api.release_suspended(made, {logic = true, movement = true})
        check.run("refused", function()
            check.result("invalid", "an empty hold", actor_api.hold_suspended(made, {}))
            check.result("invalid", "an empty release", actor_api.release_suspended(made, {}))
            check.result("invalid", "an actor passed as a class", actor_api.spawn(made, asked, at.rotation))
            check.result("invalid", "a team below the default", actor_api.spawn(class, asked, at.rotation, -2))
        end)
        phase = 2
        return
    end
    if phase == 2 then
        check.run("removed", function()
            check.result("ok", "quietly removing its own actor", actor_api.destroy(made))
            check.result("busy", "removing it again before the next update", actor_api.destroy(made))
        end)
        removed_at, phase = steps, 3
        return
    end
    local alive = engine_actor.alive(made)
    if alive and steps - removed_at < 90 then return end
    check.run("gone", function()
        check.equal(check.result("ok", "the old actor's life", engine_actor.alive(made)), false, "the actor left play")
        check.result("not_found", "the departed actor's transform", actor_api.pose(made))
        check.result("not_found", "removing the departed actor", actor_api.destroy(made))
    end)
    finish()
end)
