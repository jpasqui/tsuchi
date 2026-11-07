local actor_api = entity
local engine_actor = actor

-- Squad members are living Pikmin, and their individual leader agrees with the squad list.
local check = require("check")
check.plan("members", "leaders", "listed", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("members", function() check.wait("a loaded game with player control") end) end
    local captain = player.captain(0)
    local members = captain and squad.members(captain)
    if not members or #members == 0 then return check.run("members", function() check.wait("the first captain's nonempty squad") end) end
    local state = check.run("members", function()
        local again = check.result("ok", "the squad again", squad.members(captain))
        check.equal(#again, #members, "the same count on this step")
        local seen = {}
        for i, actor in ipairs(members) do
            check.expect(not seen[actor], "each member appears once")
            seen[actor] = true
            check.equal(again[i], actor, "the same ordered member")
        end
    end)
    check.run("leaders", function()
        for _, actor in ipairs(members) do
            check.equal(check.result("ok", "the member's leader", squad.leader(actor)), captain, "the listed member follows this captain")
            check.equal(engine_actor.alive(actor), true, "the member is in play")
        end
    end)
    check.run("listed", function()
        local global = {}
        for _, actor in ipairs(check.result("ok", "every living Pikmin", pikmin.list())) do global[actor] = true end
        for _, actor in ipairs(members) do
            check.expect(global[actor] == true, "the member is in the game's Pikmin list")
            check.expect(pikmin.info(actor) ~= nil, "the member has Pikmin information")
        end
    end)
    check.run("refused", function()
        check.result("not_found", "a captain passed as a Pikmin", squad.leader(captain))
        check.result("not_found", "a Pikmin passed as a leader", squad.members(members[1]))
    end)
    return state
end)
