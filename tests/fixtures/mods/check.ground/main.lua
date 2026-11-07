-- Selected explicitly: change only this mod's own Chappy on a disposable save.
local check = require("check")
local planned = {
    "ready", "made", "quiet", "body", "first", "standing",
    "launched", "pending", "rising", "apex", "falling", "airborne", "landed",
    "held", "lifted", "suspended", "hover", "resumed", "resume_pending",
    "falling_again", "landed_again", "removed", "gone", "stale"
}
check.plan(table.unpack(planned))

local done = {}
local source_class, made, body, brain, brain_before, stepping
local phase, steps, since, controlled = "ready", 0, 0, nil
local elapsed, stable_control, pause_logs, waiting_updates = 0, 0, 0, false
local quiet = {movement = true}
local stopped = {physics = true, collision = true}
local quiet_held, stopped_held = false, false
local floor_z, lifted_z, previous_vz, previous_ground, previous_height
local rise, apex, fall, fall_again = false, false, false, false
local floor_touch, restored_floor_touch = false, false
local airborne_samples, false_support, stopped_problem, drop_support = 0, nil, nil, nil

local function record(name, fn)
    done[name] = true
    return check.run(name, fn) == "pass"
end

local function change(next_phase)
    phase, since = next_phase, steps
    mod.log("ground phase " .. phase .. " step=" .. steps)
end

local function sample(label, at, velocity, grounded, why)
    mod.log(string.format("ground %s z=%.3f vz=%.3f grounded=%s result=%s",
        label, at.location.z, velocity.z, tostring(grounded), tostring(why or "ok")))
end

local function stop()
    if source_class then asset.release(source_class) source_class = nil end
    stepping:cancel()
end

local function restore_brain()
    if brain_before == nil or not brain or actor.alive(made) == false then return true end
    if component.owner(brain) ~= made then return false end
    reflection.call(brain, "SetComponentTickEnabled", {bEnabled = brain_before})
    local restored = reflection.call(brain, "IsComponentTickEnabled") == brain_before
    mod.log("ground own-brain-restored=" .. tostring(restored))
    return restored
end

local function remove(reason)
    if reason then
        for _, name in ipairs(planned) do
            if not done[name] and name ~= "removed" and name ~= "gone" and name ~= "stale" then
                record(name, function() check.expect(false, reason) end)
            end
        end
    end
    if not made then
        for _, name in ipairs({"removed", "gone", "stale"}) do
            record(name, function() check.expect(false, "no owned actor was made") end)
        end
        return stop()
    end
    local holds_released = true
    for _, parts in ipairs({stopped_held and stopped or false, quiet_held and quiet or false}) do
        if parts then
            local called, released = pcall(entity.release_suspended, made, parts)
            holds_released = holds_released and called and released == true
        end
    end
    stopped_held, quiet_held = false, false
    local restored_call, brain_restored = pcall(restore_brain)
    -- Try removal even when restoration failed. Assertions never gate the request.
    local removed_call, removed, problem = pcall(entity.destroy, made)
    record("removed", function()
        check.expect(holds_released, "its owned suspension holds were released")
        check.expect(restored_call and brain_restored, "the owned brain's original component tick state was restored")
        check.expect(removed_call, "the owned actor's removal request did not throw: " .. tostring(removed))
        if removed_call then check.result("ok", "quietly removing only its owned actor", removed, problem) end
    end)
    change("gone")
end

local function expired(limit, message)
    if steps - since < limit then return false end
    remove(message)
    return true
end

local function read_motion()
    local at, velocity = entity.pose(made), entity.velocity(made)
    if at and velocity then return at, velocity end
end

local function finish_flight(at, velocity)
    record("rising", function() check.expect(rise, "a false contact sample while rising above 20cm") end)
    record("apex", function() check.expect(apex, "false contact on both sides of the vertical-speed sign change above 20cm") end)
    record("falling", function() check.expect(fall, "a false contact sample while falling above 20cm") end)
    record("airborne", function()
        check.expect(airborne_samples >= 3, "at least three completed airborne contact samples")
        check.expect(false_support == nil, false_support or "no supporting contact was reported in free flight")
    end)
    record("landed", function()
        check.near(at.location.z, floor_z, 8, "support returned near the original floor height")
        check.near(velocity.z, 0, 10, "the supported body's vertical speed")
    end)
    sample("landed", at, velocity, true)
    change("hold")
end

local function advance()
    elapsed = elapsed + 1
    if phase == "ready" and elapsed >= 9000 then
        return remove("stable unpaused player control was not available within 9000 world steps")
    end
    local control = game.has_control() == true
    local paused = control and game.paused() or nil
    if not control or type(paused) ~= "table" or paused.actors == true then
        stable_control = 0
        if phase == "ready" then
            check.run("ready", function() check.wait("loaded play with unpaused actor updates on a disposable save") end)
        elseif not waiting_updates and pause_logs < 8 then
            mod.log("ground waiting for player control and unpaused actor updates; phase budget held")
            pause_logs = pause_logs + 1
        end
        waiting_updates = true
        return
    end
    if waiting_updates and phase ~= "ready" and pause_logs < 8 then
        mod.log("ground actor updates resumed; phase=" .. phase)
        pause_logs = pause_logs + 1
    end
    waiting_updates = false
    steps = steps + 1
    if phase == "ready" then
        stable_control = stable_control + 1
        if stable_control < 30 then
            return check.run("ready", function() check.wait("30 consecutive controlled steps with actor updates unpaused") end)
        end
        controlled = controlled or steps
        local captain = player.captain(0)
        local at = captain and entity.pose(captain)
        if not source_class then
            source_class = asset.load("/Game/Carrot4/Placeables/Teki/GChappy.GChappy_C")
        end
        if not at or not source_class then
            if steps - controlled >= 180 then return remove("captain or stock Chappy class unavailable after 180 controlled steps") end
            return check.run("ready", function() check.wait("the first captain's pose and the stock Chappy class") end)
        end
        record("ready", function() check.equal(actor.alive(captain), true, "the first captain is in play") end)
        record("made", function()
            made = check.result("ok", "making an owned stock Chappy near the captain",
                entity.spawn(source_class, {x = at.location.x + 100, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false))
        end)
        if not made then return remove("the owned Chappy could not be made") end
        local held, why = entity.hold_suspended(made, quiet)
        quiet_held = held == true
        if not quiet_held then
            record("quiet", function() check.result("ok", "holding only its movement", held, why) end)
            return remove("the owned actor's movement could not be held")
        end
        change("quiet")
        return
    end
    if phase == "quiet" then
        local candidate = reflection.get(made, "CarrotAIComponent")
        local ticking = candidate and component.owner(candidate) == made
            and reflection.call(candidate, "IsComponentTickEnabled")
        if not candidate or component.owner(candidate) ~= made or type(ticking) ~= "boolean" then
            if expired(180, "the owned actor's AI component or tick state was unavailable within 180 steps") then return end
            return check.run("quiet", function() check.wait("the owned CarrotAIComponent and its component tick state") end)
        end
        brain, brain_before = candidate, ticking
        if not record("quiet", function()
            check.equal(component.owner(brain), made, "only its owned brain is changed")
            reflection.call(brain, "SetComponentTickEnabled", {bEnabled = false})
            check.equal(reflection.call(brain, "IsComponentTickEnabled"), false, "its brain component stopped while the actor's update remains enabled")
            local held = check.result("ok", "the owned actor's suspension flags", entity.suspended(made))
            check.expect(held and held.movement == true and held.logic ~= true, "only movement is held; actor logic ticking stays available")
        end) then return remove("the owned actor's brain component could not be paused independently") end
        change("body")
        return
    end
    if phase == "body" then
        for _, part in ipairs(component.list(made) or {}) do
            if reflection.call(part, "IsSimulatingPhysics", {BoneName = ""}) == true then body = part break end
        end
        if not body then
            if expired(180, "the owned actor had no simulating body within 180 steps") then return end
            return check.run("body", function() check.wait("the owned actor's independent simulating body") end)
        end
        record("body", function() check.equal(component.owner(body), made, "the body belongs to its owned actor") end)
        record("first", function()
            local grounded, why = entity.grounded(made)
            mod.log("ground first-query grounded=" .. tostring(grounded) .. " result=" .. tostring(why or "ok"))
            check.result("not_ready", "the first query has no completed observed contact update", grounded, why)
        end)
        change("standing")
        return
    end
    if phase == "standing" then
        local grounded, why = entity.grounded(made)
        local at, velocity = read_motion()
        local age = steps - since
        if age == 1 or age == 30 or age == 90 or age == 180 then
            if at and velocity then sample("standing-wait-" .. age, at, velocity, grounded, why)
            else mod.log("ground standing-wait-" .. age .. " grounded=" .. tostring(grounded) .. " result=" .. tostring(why or "ok") .. " motion=not_ready") end
        end
        -- Native contact acceptance can precede the impact velocity's update.
        -- Establish the floor baseline only after the supported body settles.
        if grounded == true and at and velocity and math.abs(velocity.z) <= 10 then
            floor_z = at.location.z
            record("standing", function()
                check.equal(grounded, true, "a completed native update accepted support")
                check.near(velocity.z, 0, 10, "the standing body's vertical speed")
            end)
            sample("standing", at, velocity, grounded, why)
            record("launched", function()
                check.result("ok", "one upward launch, with gravity left to the game", entity.launch(made, {x = 0, y = 0, z = 600}))
            end)
            record("pending", function()
                check.result("not_ready", "a pending launch invalidates the standing sample immediately", entity.grounded(made))
            end)
            change("flight")
            return
        end
        expired(180, "no fresh supported standing sample within 180 steps")
        return
    end
    if phase == "flight" then
        local grounded, why = entity.grounded(made)
        local at, velocity = read_motion()
        if at and velocity then
            local height = at.location.z - floor_z
            if height > 20 then
                if grounded == false then airborne_samples = airborne_samples + 1 end
                if grounded == true and not false_support then
                    false_support = string.format("support was reported %.3fcm above the original floor at vz=%.3f", height, velocity.z)
                    sample("unexpected-support", at, velocity, grounded, why)
                end
                if not rise and velocity.z > 30 and grounded == false then
                    rise = true
                    sample("rising", at, velocity, grounded, why)
                end
                if not apex and previous_vz and previous_vz > 0 and velocity.z <= 0
                    and previous_height > 20 and previous_ground == false and grounded == false then
                    apex = true
                    sample("apex-crossing", at, velocity, grounded, why)
                end
                if not fall and velocity.z < -30 and grounded == false then
                    fall = true
                    sample("falling", at, velocity, grounded, why)
                end
            end
            previous_vz, previous_ground, previous_height = velocity.z, grounded, height
            if grounded == true and math.abs(height) <= 8 then
                if not floor_touch then
                    floor_touch = true
                    sample("first-floor-support", at, velocity, grounded, why)
                end
                if steps - since >= 3 and math.abs(velocity.z) <= 10 then
                    return finish_flight(at, velocity)
                end
            end
        end
        expired(320, "the upward launch did not complete every phase and return to the original floor within 320 steps")
        return
    end
    if phase == "hold" then
        local held_ok = record("held", function()
            local held, why = entity.hold_suspended(made, stopped)
            stopped_held = held == true
            check.result("ok", "suspending its own physics and collision", held, why)
            check.result("not_ready", "stopping the body invalidates its landing sample", entity.grounded(made))
        end)
        if not held_ok then return remove("the owned body could not be suspended") end
        local at = entity.pose(made)
        if not at then return remove("the owned actor's pose disappeared before lifting") end
        lifted_z = at.location.z + 150
        if not record("lifted", function()
            check.result("ok", "lifting only the stopped owned actor by 150cm", entity.set_pose(made,
                {location = {x = at.location.x, y = at.location.y, z = lifted_z}, rotation = at.rotation}))
            check.result("not_ready", "a displaced stopped body cannot retain the old support sample", entity.grounded(made))
        end) then return remove("the stopped owned body could not be lifted") end
        change("hover")
        return
    end
    if phase == "hover" then
        local grounded, why = entity.grounded(made)
        if (grounded ~= nil or why ~= "not_ready") and not stopped_problem then
            stopped_problem = "stopped lifted body returned " .. tostring(grounded) .. ", " .. tostring(why)
        end
        if steps - since < 45 then return end
        record("suspended", function() check.expect(stopped_problem == nil, stopped_problem or "all 45 stopped-body queries refused stale contact") end)
        record("hover", function()
            local at = check.result("ok", "the stopped actor's pose", entity.pose(made))
            check.expect(at ~= nil, "the stopped actor still has a pose")
            if at then check.near(at.location.z, lifted_z, 5, "it stayed lifted for 45 updates") end
        end)
        if not record("resumed", function()
            local released, why = entity.release_suspended(made, stopped)
            if released == true then stopped_held = false end
            check.result("ok", "restoring its physics and collision", released, why)
            check.result("ok", "a controlled zero-speed launch keeps the restored body updating", entity.launch(made, {x = 0, y = 0, z = 0}))
        end) then return remove("the owned body's restored falling route could not start") end
        record("resume_pending", function()
            check.result("not_ready", "the restored body's pending launch has no fresh contact sample", entity.grounded(made))
        end)
        change("drop")
        return
    end
    if phase == "drop" then
        local grounded, why = entity.grounded(made)
        local at, velocity = read_motion()
        if at and velocity then
            if grounded == true and at.location.z - floor_z > 20 and not drop_support then
                drop_support = string.format("restored body reported support %.3fcm above the floor at vz=%.3f", at.location.z - floor_z, velocity.z)
                sample("unexpected-restored-support", at, velocity, grounded, why)
            end
            if not fall_again and grounded == false and velocity.z < -30
                and lifted_z - at.location.z > 20 and at.location.z - floor_z > 20 then
                fall_again = true
                sample("restored-falling", at, velocity, grounded, why)
            end
            if grounded == true and math.abs(at.location.z - floor_z) <= 8 then
                if not restored_floor_touch then
                    restored_floor_touch = true
                    sample("first-restored-floor-support", at, velocity, grounded, why)
                end
            end
            if grounded == true and math.abs(at.location.z - floor_z) <= 8 and math.abs(velocity.z) <= 10 then
                record("falling_again", function()
                    check.expect(fall_again, "fresh false contact while falling after physics restoration")
                    check.expect(drop_support == nil, drop_support or "no support was reported before the restored body landed")
                end)
                record("landed_again", function()
                    check.near(at.location.z, floor_z, 8, "restored support is near the original floor")
                    check.near(velocity.z, 0, 10, "the supported restored body's vertical speed")
                end)
                sample("restored-landed", at, velocity, grounded, why)
                return remove()
            end
        end
        expired(320, "the restored body did not fall and return to the original floor within 320 steps")
        return
    end
    if phase == "gone" then
        if actor.alive(made) == true and steps - since < 90 then return end
        record("gone", function() check.equal(actor.alive(made), false, "the owned actor left play within 90 updates") end)
        record("stale", function() check.result("not_found", "a retired actor handle has no contact result", entity.grounded(made)) end)
        stop()
    end
end

stepping = time.on_step(function()
    local ok, why = pcall(advance)
    if not ok then
        if phase == "gone" then
            for _, name in ipairs({"gone", "stale"}) do
                if not done[name] then record(name, function() check.expect(false, "cleanup error: " .. tostring(why)) end) end
            end
            return stop()
        end
        remove("fixture error: " .. tostring(why))
    end
end)
