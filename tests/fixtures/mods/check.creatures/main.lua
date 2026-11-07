local actor_api = entity
local engine_actor = actor

-- Creature state is read without defeating a level's creature or reviving one.
local check = require("check")
check.plan("listed", "state", "alive", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("listed", function() check.wait("a loaded game with player control") end) end
    local listed, healthy = creature.list(), nil
    if listed then
        for _, actor in ipairs(listed) do
            local life = combat.life(actor)
            if creature.defeated(actor) == false and life and life.life > 0 then healthy = actor break end
        end
    end
    if not listed or #listed == 0 or not healthy then return check.run("listed", function() check.wait("a living undefeated creature in the world") end) end
    local state = check.run("listed", function()
        local again = check.result("ok", "creatures again", creature.list())
        check.equal(#again, #listed, "the same count on this step")
        local seen, global = {}, {}
        for _, actor in ipairs(check.result("ok", "all actors", actor_api.list())) do global[actor] = true end
        for i, actor in ipairs(listed) do
            check.expect(not seen[actor], "each creature appears once")
            seen[actor] = true
            check.equal(again[i], actor, "the same ordered creature")
            check.expect(global[actor] == true, "the creature is in the public actor list")
        end
    end)
    check.run("state", function()
        for _, actor in ipairs(listed) do
            local defeated = check.result("ok", "whether it is defeated", creature.defeated(actor))
            local carcass = check.result("ok", "whether it is a carcass", creature.carcass(actor))
            check.expect(type(defeated) == "boolean" and type(carcass) == "boolean", "both creature states are booleans")
            check.expect(not carcass or defeated, "a carcass is defeated")
        end
    end)
    check.run("alive", function()
        check.equal(check.result("ok", "the healthy actor's life", engine_actor.alive(healthy)), true, "it is in play")
        check.equal(check.result("ok", "the healthy creature's defeat", creature.defeated(healthy)), false, "it is undefeated")
        check.equal(check.result("ok", "the healthy creature's carcass", creature.carcass(healthy)), false, "it is not a carcass")
    end)
    check.run("refused", function()
        local captain = check.result("ok", "the first captain", player.captain(0))
        check.result("not_found", "a captain passed as a creature", creature.defeated(captain))
        local before = check.result("ok", "the healthy creature's life", combat.life(healthy)).life
        check.result("not_ready", "reviving an undefeated creature", creature.revive(healthy))
        check.equal(check.result("ok", "its life after the refused request", combat.life(healthy)).life, before, "the request left its life unchanged")
        check.equal(creature.defeated(healthy), false, "the request left it undefeated")
    end)
    return state
end)
