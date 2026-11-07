local actor_api = entity
local engine_actor = actor

-- The game's list and each Pikmin's description agree with public actor reads.
local check = require("check")
check.plan("listed", "info", "team", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("listed", function() check.wait("a loaded game with player control") end) end
    local listed = pikmin.list()
    if not listed or #listed == 0 then return check.run("listed", function() check.wait("living Pikmin in the world") end) end
    local state = check.run("listed", function()
        local again = check.result("ok", "listing Pikmin again", pikmin.list())
        check.equal(#again, #listed, "the same count on this step")
        local seen = {}
        for i, actor in ipairs(listed) do
            check.expect(not seen[actor], "each Pikmin appears once")
            seen[actor] = true
            check.equal(again[i], actor, "the same ordered handle")
            check.equal(engine_actor.alive(actor), true, "the listed Pikmin is in play")
        end
    end)
    check.run("info", function()
        for _, actor in ipairs(listed) do
            local info = check.result("ok", "its Pikmin information", pikmin.info(actor))
            check.expect(type(info.kind) == "string" and #info.kind > 0, "the Pikmin type is named")
            check.expect(info.leaf == "leaf" or info.leaf == "bud" or info.leaf == "flower", "a leaf, bud or flower")
            check.expect(type(info.in_army) == "boolean", "army membership is a boolean")
        end
    end)
    check.run("team", function()
        for _, actor in ipairs(listed) do
            check.equal(pikmin.info(actor).team, check.result("ok", "the actor's team", actor_api.team(actor)), "the two services agree on its team")
        end
    end)
    check.run("refused", function()
        local captain = check.result("ok", "the first captain", player.captain(0))
        check.result("not_found", "a captain passed as a Pikmin", pikmin.info(captain))
        check.result("invalid", "asking for zero new Pikmin", pikmin.add(captain, "red", "leaf", 0))
    end)
    return state
end)
