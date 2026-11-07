local actor_api = entity
local engine_actor = actor
local component_api = component

-- Throw and warp only this mod's own stock actor, then remove it quietly.
local check = require("check")
check.plan("found", "made", "body", "launched", "rise", "speed", "landed", "warped", "facing", "cancelled", "removed", "gone")
local wanted = (file.read("targets.txt", 0, 1024) or ""):match("[^\r\n]+")
local phase, steps, peak, fastest = 0, 0, 0, 0
local made, original, thrown_at, warped_at, asked, removed_at, stable, stepping
local cancel_launch, cancel_problem, cancel_warp, warp_problem
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 then
        if game.has_control() ~= true then check.run("found", function() check.wait("a loaded game with player control") end) return end
        local class, at
        for _, actor in ipairs(actor_api.list() or {}) do
            if actor_api.kind(actor) == wanted then class, at = reflection.class_of(actor), actor_api.pose(actor) if class and at then break end end
        end
        if not class or not at then check.run("found", function() check.wait("the stock actor kind in targets.txt") end) return end
        check.run("found", function() check.expect(class ~= nil and at ~= nil, "the source's class and place") end)
        check.run("made", function() made = check.result("ok", "an owned copy", actor_api.spawn(class, {x = at.location.x + 350, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false)) end)
        if not made then
            for _, name in ipairs({"body", "launched", "rise", "speed", "landed", "warped", "facing", "cancelled", "removed", "gone"}) do check.run(name, function() check.expect(false, "no actor was made") end) end
            stepping:cancel() return
        end
        phase = 1 return
    end
    if phase == 1 then
        local body
        for _, part in ipairs(component_api.list(made) or {}) do
            if reflection.call(part, "IsSimulatingPhysics", {BoneName = ""}) == true then body = part break end
        end
        if not body then check.run("body", function() check.wait("the owned actor's running physics body") end) return end
        check.run("body", function() check.equal(component_api.owner(body), made, "its own body") end)
        original = actor_api.pose(made)
        if not original then check.run("launched", function() check.wait("the owned body's world place") end) return end
        check.run("launched", function() check.result("ok", "throwing it upward", actor_api.launch(made, {x = 0, y = 0, z = 600})) end)
        thrown_at, stable, phase = steps, 0, 2 return
    end
    if phase == 2 then
        local at, velocity = actor_api.pose(made), actor_api.velocity(made)
        if at and velocity then
            peak, fastest = math.max(peak, at.location.z - original.location.z), math.max(fastest, velocity.z)
            if peak > 20 and fastest > 50 and math.abs(velocity.z) < 5 then stable = stable + 1 else stable = 0 end
        end
        if stable < 3 and steps - thrown_at < 320 then return end
        check.run("rise", function() check.expect(peak > 20, "the throw rose more than 20cm") end)
        check.run("speed", function() check.expect(fastest > 50, "the upward speed exceeded 50cm/s") end)
        check.run("landed", function() check.expect(stable >= 3, "its vertical speed settled for three updates") end)
        asked = {x = original.location.x + 300, y = original.location.y, z = original.location.z + 100}
        check.run("warped", function()
            check.result("ok", "warping after its flight", actor_api.warp(made, asked, {pitch = 0, yaw = 90, roll = 0}))
            local place = check.result("ok", "the grounded warp's place", actor_api.pose(made))
            check.near(place.location.x, asked.x, 5, "the warp's x")
            check.near(place.location.y, asked.y, 5, "the warp's y")
        end)
        check.run("facing", function() check.near(check.result("ok", "the warped facing", actor_api.pose(made)).rotation.yaw, 90, 1, "the requested yaw") end)
        cancel_launch, cancel_problem = actor_api.launch(made, {x = 0, y = 0, z = 600})
        cancel_warp, warp_problem = actor_api.warp(made, asked, {pitch = 0, yaw = 90, roll = 0})
        warped_at, phase = steps, 3 return
    end
    if phase == 3 then
        if steps - warped_at < 3 then return end
        check.run("cancelled", function()
            check.result("ok", "another throw to call off", cancel_launch, cancel_problem)
            check.result("ok", "warping calls that throw off", cancel_warp, warp_problem)
            local velocity = check.result("ok", "the called-off throw's speed", actor_api.velocity(made))
            check.expect(math.abs(velocity.z) < 5, "the called-off throw did not resume")
        end)
        check.run("removed", function() check.result("ok", "quiet removal of its owned actor", actor_api.destroy(made)) end)
        removed_at, phase = steps, 4 return
    end
    if engine_actor.alive(made) and steps - removed_at < 60 then return end
    check.run("gone", function() check.equal(check.result("ok", "the removed actor's life", engine_actor.alive(made)), false, "its owned actor left play") end)
    stepping:cancel()
end)
