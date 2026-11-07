local actor_api = entity
local engine_actor = actor
local component_api = component
local reflection_api = reflection

-- Suspend, lift, restore and remove this mod's own actor; read its engine body independently.
local check = require("check")
check.plan("found", "made", "body", "held", "simulating", "collision", "refused", "hover", "restored", "fall", "ended", "removed", "gone")
local wanted = (file.read("targets.txt", 0, 1024) or ""):match("[^\r\n]+")
local made, body, original, solid, lifted, held_at, released_at, removed_at
local phase, steps = 0, 0
local parts = {logic = true, movement = true, physics = true, collision = true}
local stepping
local function simulation(part) return reflection_api.call(part, "IsSimulatingPhysics", {BoneName = ""}) end
local function flags(all)
    local got = check.result("ok", "the suspended kinds", actor_api.suspended(made))
    for name in pairs(parts) do check.equal(got[name] == true, all, "the " .. name .. " hold") end
end
local function finish()
    if made then actor_api.release_suspended(made, parts) end
    check.run("removed", function() check.result("ok", "quiet removal of its owned actor", actor_api.destroy(made)) end)
    removed_at, phase = steps, 4
end
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 then
        if game.has_control() ~= true then check.run("found", function() check.wait("a loaded game with player control") end) return end
        local class, at
        for _, actor in ipairs(actor_api.list() or {}) do
            if actor_api.kind(actor) == wanted then class, at = reflection_api.class_of(actor), actor_api.pose(actor) if class and at then break end end
        end
        if not class or not at then check.run("found", function() check.wait("the stock actor kind in targets.txt") end) return end
        check.run("found", function() check.expect(class ~= nil and at ~= nil, "the source's class and place") end)
        check.run("made", function() made = check.result("ok", "an owned copy", actor_api.spawn(class, {x = at.location.x + 250, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false)) end)
        if not made then
            for _, name in ipairs({"body", "held", "simulating", "collision", "refused", "hover", "restored", "fall", "ended", "removed", "gone"}) do check.run(name, function() check.expect(false, "no actor was made") end) end
            stepping:cancel() return
        end
        phase = 1 return
    end
    if phase == 1 then
        solid = {}
        for _, part in ipairs(component_api.list(made) or {}) do
            local mode = reflection_api.call(part, "GetCollisionEnabled")
            if mode == 2 or mode == 3 then solid[#solid + 1] = {part = part, mode = mode} end
            if simulation(part) == true then body = part end
        end
        if not body or #solid == 0 then check.run("body", function() check.wait("the owned actor's simulating and blocking body") end) return end
        check.run("body", function() check.equal(component_api.owner(body), made, "the body belongs to its owned actor") end)
        original = check.result("ok", "its original place", actor_api.pose(made))
        lifted = original.location.z + 150
        check.run("held", function()
            flags(false)
            check.result("ok", "stopping every kind", actor_api.hold_suspended(made, parts))
            check.result("ok", "coalescing the same kinds", actor_api.hold_suspended(made, parts))
            flags(true)
            check.result("ok", "lifting the stopped body", actor_api.set_pose(made, {location = {x = original.location.x, y = original.location.y, z = lifted}, rotation = original.rotation}))
        end)
        check.run("simulating", function() check.equal(check.result("ok", "the engine's stopped physics", simulation(body)), false, "physics is stopped") end)
        check.run("collision", function()
            for _, part in ipairs(solid) do check.equal(check.result("ok", "a formerly blocking part", reflection_api.call(part.part, "GetCollisionEnabled")), part.mode == 3 and 1 or 0, "it no longer blocks") end
        end)
        check.run("refused", function() check.result("unsupported", "throwing stopped physics", actor_api.launch(made, {x = 0, y = 0, z = 600})) end)
        held_at, phase = steps, 2 return
    end
    if phase == 2 then
        if steps - held_at < 45 then return end
        check.run("hover", function() check.near(check.result("ok", "the stopped body's place", actor_api.pose(made)).location.z, lifted, 5, "it stayed lifted for 45 updates") end)
        check.run("restored", function()
            check.result("ok", "giving every kind back", actor_api.release_suspended(made, parts))
            flags(false)
            check.equal(check.result("ok", "the engine's restored physics", simulation(body)), true, "physics is running again")
            for _, part in ipairs(solid) do check.equal(check.result("ok", "the restored collision mode", reflection_api.call(part.part, "GetCollisionEnabled")), part.mode, "the original collision") end
        end)
        check.run("ended", function() check.result("not_found", "ending the coalesced hold twice", actor_api.release_suspended(made, parts)) end)
        released_at, phase = steps, 3 return
    end
    if phase == 3 then
        local at = actor_api.pose(made)
        if at and lifted - at.location.z > 100 then
            check.run("fall", function() check.expect(lifted - at.location.z > 100, "the released body fell more than a meter") end)
            finish() return
        end
        if steps - released_at < 90 then return end
        check.run("fall", function() check.expect(false, "the released body did not fall a meter within 90 updates") end)
        finish() return
    end
    if engine_actor.alive(made) and steps - removed_at < 60 then return end
    check.run("gone", function() check.equal(check.result("ok", "the removed actor's life", engine_actor.alive(made)), false, "its owned actor left play") end)
    stepping:cancel()
end)
