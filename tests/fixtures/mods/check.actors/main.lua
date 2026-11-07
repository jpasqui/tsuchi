local actor_api = entity
local engine_actor = actor

-- The public actors service, including the game's own answer for each actor's place.
local check = require("check")
check.plan("listed", "described", "transform", "class", "placed", "compared", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("listed", function() check.wait("a loaded game with player control") end) end
    local listed = actor_api.list()
    local placed = false
    for _, actor in ipairs(listed or {}) do
        local placement = actor_api.placement(actor)
        if placement and placement ~= 0 then placed = true break end
    end
    if not listed or #listed == 0 or not placed then return check.run("listed", function() check.wait("a loaded world with level-placed actors") end) end
    check.run("listed", function()
        check.expect(type(listed) == "table" and #listed > 0, "a loaded world has actors")
        local again = check.result("ok", "listing again", actor_api.list())
        check.equal(#again, #listed, "the count on the same step")
        for i, actor in ipairs(listed) do check.equal(again[i], actor, "the actor at " .. i) end
    end)
    local first = listed and listed[1]
    check.run("described", function()
        check.expect(type(check.result("ok", "its kind", actor_api.kind(first))) == "string", "its kind is text")
        check.equal(check.result("ok", "whether it is alive", engine_actor.alive(first)), true, "it is in play")
        check.expect(math.type(check.result("ok", "its team", actor_api.team(first))) == "integer", "its team is an integer")
        check.expect(math.type(check.result("ok", "its placement", actor_api.placement(first))) == "integer", "its placement is an integer")
    end)
    check.run("transform", function()
        local actor, before
        for _, each in ipairs(listed) do
            local at = actor_api.pose(each)
            if at then actor, before = each, at break end
        end
        check.expect(actor ~= nil, "an actor has a place in the world")
        if not actor then return end
        check.result("ok", "putting it where it already is", actor_api.set_pose(actor, before))
        local after = check.result("ok", "reading its place back", actor_api.pose(actor))
        for _, field in ipairs({"x", "y", "z"}) do check.near(after.location[field], before.location[field], 0.5, "its location " .. field) end
        for _, field in ipairs({"pitch", "yaw", "roll"}) do check.near(after.rotation[field], before.rotation[field], 0.5, "its rotation " .. field) end
    end)
    check.run("class", function()
        local class = check.result("ok", "the game's actor class", reflection.find_class("GActor"))
        local by_class = check.result("ok", "listing its actors", actor_api.list(class))
        check.equal(#by_class, #listed, "every actor belongs to that class")
    end)
    check.run("placed", function()
        local found = 0
        for _, actor in ipairs(listed) do
            local placement = check.result("ok", "its placement", actor_api.placement(actor))
            if placement ~= 0 then
                check.equal(check.result("ok", "finding a placed actor", actor_api.find_placed(placement)), actor, "the same actor")
                found = found + 1
            end
        end
        check.expect(found > 0, "the world has an actor a level placed")
    end)
    check.run("compared", function()
        local compared = 0
        for _, actor in ipairs(listed) do
            local here = actor_api.pose(actor)
            if here then
                local answer = reflection.call(actor, "K2_GetActorLocation")
                if answer then
                    compared = compared + 1
                    for index, field in ipairs({"x", "y", "z"}) do check.near(here.location[field], answer[({"X", "Y", "Z"})[index]], 0.01, "the game's location " .. field) end
                end
            end
        end
        check.expect(compared > 0, "an actor's place was compared with the game's answer")
    end)
    check.run("refused", function()
        check.result("invalid", "an actor passed as a class", actor_api.list(first))
        check.result("invalid", "no placement", actor_api.find_placed(0))
    end)
    return "pass"
end)
