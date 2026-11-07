local actor_api = entity
local engine_actor = actor
local component_api = component
local file_api = file

-- Play a stock clip on this mod's own copy of its matching actor; restore and remove it afterward.
local check = require("check")
check.plan("found", "asset", "made", "mesh", "held", "seek", "rate", "engine", "restored", "ended", "removed", "gone")
local targets = {}
for line in (file_api.read("targets.txt", 0, 1024) or ""):gmatch("[^\r\n]+") do targets[#targets + 1] = line end
local source, at, class, clip, made, mesh, before_mode
local phase, steps, made_at, removed_at = 0, 0, 0, 0
local stepping
local function failed_without_actor()
    for _, name in ipairs({"mesh", "held", "seek", "rate", "engine", "restored", "ended", "removed", "gone"}) do
        check.run(name, function() check.expect(false, "the actor was not made") end)
    end
    if clip then asset.release(clip) end
    stepping:cancel()
end
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 and game.has_control() ~= true then check.run("found", function() check.wait("a loaded game with player control") end) return end
    if phase == 0 then
        for _, actor in ipairs(actor_api.list() or {}) do
            if actor_api.kind(actor) == targets[1] then
                local found_class, found_at = reflection.class_of(actor), actor_api.pose(actor)
                if found_class and found_at then source, class, at = actor, found_class, found_at break end
            end
        end
        if not source then check.run("found", function() check.wait("an actor of the clip's kind in targets.txt") end) return end
        local loaded, why = asset.load(targets[2])
        if not loaded and why == "not_ready" then check.run("asset", function() check.wait("the stock clip asset to be available") end) return end
        check.run("found", function() check.expect(source ~= nil and class ~= nil and at ~= nil, "the matching stock actor's class and place") end)
        check.run("asset", function() clip = check.result("ok", "the stock animation asset", loaded, why) end)
        check.run("made", function()
            made = check.result("ok", "an owned matching actor", actor_api.spawn(class, {x = at.location.x + 200, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false))
            check.expect(made ~= nil and made ~= source, "its own copy")
        end)
        if not made then failed_without_actor() return end
        made_at, phase = steps, 1
        return
    end
    if phase == 1 then
        local skeletal = reflection.find_class("SkeletalMeshComponent")
        if skeletal then
            for _, part in ipairs(component_api.list(made) or {}) do
                if reflection.is_a(part, skeletal) then
                    local mode = reflection.get(part, "AnimationMode")
                    if mode == tonumber(targets[3]) then mesh, before_mode = part, mode break end
                end
            end
        end
        if not mesh and steps - made_at < 60 then check.run("mesh", function() check.wait("the owned actor's skeletal mesh and animation blueprint") end) return end
        check.run("mesh", function()
            check.expect(mesh ~= nil, "the owned actor has a skeletal mesh animated by its blueprint")
            if mesh then check.equal(component_api.owner(mesh), made, "the mesh belongs to its own actor") end
        end)
        check.run("held", function()
            check.expect(mesh ~= nil and clip ~= nil, "the mesh and clip were found")
            check.result("ok", "holding a looping clip still", animation.hold_clip(mesh, clip, true, 0))
            local held = check.result("ok", "the held clip", animation.clip(mesh))
            check.equal(held.clip, clip, "the requested clip")
            check.equal(held.looping, true, "the requested loop")
            check.near(held.rate, 0, 0.001, "the requested stationary rate")
            check.equal(reflection.get(mesh, "AnimationMode"), tonumber(targets[4]), "the engine runs a single clip")
        end)
        check.run("seek", function()
            check.result("ok", "seeking the stationary clip", animation.seek_clip(mesh, 0.25))
            check.near(check.result("ok", "the position after seeking", animation.clip(mesh)).position, 0.25, 0.01, "the sought pose")
            check.result("invalid", "seeking before its beginning", animation.seek_clip(mesh, -1))
        end)
        check.run("rate", function()
            check.result("ok", "changing its rate", animation.set_clip_rate(mesh, 0.5))
            check.near(check.result("ok", "the changed rate", animation.clip(mesh)).rate, 0.5, 0.001, "half speed")
            check.result("ok", "holding it still again", animation.set_clip_rate(mesh, 0))
        end)
        check.run("engine", function()
            local held = check.result("ok", "the public clip state", animation.clip(mesh))
            check.near(check.result("ok", "the engine's pose", reflection.call(mesh, "GetPosition")), held.position, 0.01, "the engine and service agree on position")
            check.near(check.result("ok", "the engine's rate", reflection.call(mesh, "GetPlayRate")), held.rate, 0.001, "the engine and service agree on rate")
            check.equal(check.result("ok", "the engine's playback", reflection.call(mesh, "IsPlaying")), held.playing, "the engine and service agree on playback")
        end)
        check.run("restored", function()
            check.result("ok", "ending the held clip", animation.release_clip(mesh))
            local held, why = animation.clip(mesh)
            check.expect(held == nil and why == nil, "the mesh has no held clip")
            check.equal(reflection.get(mesh, "AnimationMode"), before_mode, "its animation blueprint runs again")
        end)
        check.run("ended", function()
            check.result("not_found", "ending the clip twice", animation.release_clip(mesh))
            check.result("not_found", "changing an ended clip", animation.set_clip_rate(mesh, 1))
            check.result("not_found", "seeking an ended clip", animation.seek_clip(mesh, 0))
            check.result("ok", "ending the loaded clip's asset hold", asset.release(clip))
        end)
        if mesh then animation.release_clip(mesh) end
        if clip then asset.release(clip) end
        check.run("removed", function() check.result("ok", "removing its owned actor", actor_api.destroy(made)) end)
        removed_at, phase = steps, 2
        return
    end
    if engine_actor.alive(made) and steps - removed_at < 60 then return end
    check.run("gone", function()
        check.equal(check.result("ok", "the removed actor's life", engine_actor.alive(made)), false, "the actor left play")
        check.result("not_found", "the old actor's place", actor_api.pose(made))
    end)
    stepping:cancel()
end)
