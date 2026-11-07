local actor_api = entity
local component_api = component
local collision_api = collision

-- Briefly switches one recorded collision part off, independently reads its engine mode, and restores it.
local check = require("check")
check.plan("found", "held", "reflected", "restored", "ended")
local chosen, before
check.retry(function()
    if game.has_control() ~= true then return check.run("found", function() check.wait("a loaded game with player control") end) end
    if not chosen then
        local listed = actor_api.list()
        if not listed then return check.run("found", function() check.wait("a loaded world") end) end
        for _, actor in ipairs(listed) do
            for _, part in ipairs(component_api.list(actor) or {}) do
                local mode = reflection.call(part, "GetCollisionEnabled")
                if type(mode) == "number" and mode > 0 and collision_api.off(part) == false then
                    local held = collision_api.hold_off(part)
                    if held then chosen, before = part, mode break end
                end
            end
            if chosen then break end
        end
    end
    if not chosen then return check.run("found", function() check.wait("a part with a collision record the game can change") end) end
    local state = check.run("found", function()
        check.expect(before > 0, "its engine collision was enabled before the hold")
        check.expect(component_api.owner(chosen) ~= nil, "the part has an actor in the world")
    end)
    check.run("held", function()
        check.equal(check.result("ok", "the collision hold", collision_api.off(chosen)), true, "the part is held off")
        check.result("ok", "taking the same hold again", collision_api.hold_off(chosen))
    end)
    check.run("reflected", function()
        check.equal(check.result("ok", "the engine's collision mode", reflection.call(chosen, "GetCollisionEnabled")), 0, "the engine also sees collision disabled")
    end)
    check.run("restored", function()
        check.result("ok", "ending the coalesced hold", collision_api.release_off(chosen))
        check.equal(check.result("ok", "whether it remains held", collision_api.off(chosen)), false, "the hold ended")
        check.equal(check.result("ok", "the restored engine mode", reflection.call(chosen, "GetCollisionEnabled")), before, "the original collision mode")
    end)
    check.run("ended", function()
        check.result("not_found", "ending the same hold twice", collision_api.release_off(chosen))
    end)
    collision_api.release_off(chosen)
    return state
end)
