local captain_api = captain

-- Read the first captain and hold its existing cursor point unchanged.
local check = require("check")
check.plan("whistle", "cursor", "held", "ended", "mount")
check.retry(function()
    if game.has_control() ~= true then return check.run("cursor", function() check.wait("a loaded game with player control") end) end
    local captain = player.captain(0)
    local cursor = captain and captain_api.cursor(captain)
    local whistle = captain and captain_api.whistle(captain)
    if not cursor or not whistle then return check.run("cursor", function() check.wait("the first captain's cursor and whistle") end) end
    local state = check.run("whistle", function()
        check.expect(type(whistle.active) == "boolean", "whistling is a boolean")
        check.expect(whistle.radius >= 0 and whistle.radius < math.huge, "the radius is finite and nonnegative")
        check.expect(math.type(whistle.count) == "integer" and whistle.count >= 0, "the whistle count is nonnegative")
        for _, axis in ipairs({"x", "y", "z"}) do check.expect(whistle.center[axis] == whistle.center[axis] and math.abs(whistle.center[axis]) < math.huge, "the center is finite") end
    end)
    check.run("cursor", function()
        for _, axis in ipairs({"x", "y", "z"}) do check.expect(cursor[axis] == cursor[axis] and math.abs(cursor[axis]) < math.huge, "the cursor is finite") end
    end)
    local held = false
    check.run("held", function()
        held = check.result("ok", "holding its existing cursor", captain_api.hold_cursor(captain, cursor))
        check.result("ok", "coalescing the unchanged hold", captain_api.hold_cursor(captain, cursor))
        local after = check.result("ok", "the held cursor", captain_api.cursor(captain))
        for _, axis in ipairs({"x", "y", "z"}) do check.near(after[axis], cursor[axis], 0.01, "the unchanged cursor " .. axis) end
    end)
    check.run("ended", function()
        check.expect(held == true, "the cursor was held")
        check.result("ok", "ending the coalesced hold", captain_api.release_cursor(captain))
        check.result("not_found", "ending it twice", captain_api.release_cursor(captain))
        local after = check.result("ok", "the cursor after the hold", captain_api.cursor(captain))
        for _, axis in ipairs({"x", "y", "z"}) do check.near(after[axis], cursor[axis], 0.01, "the restored cursor " .. axis) end
    end)
    check.run("mount", function()
        local dog, why = captain_api.riding(captain)
        check.expect(dog ~= nil or why == nil, "reading the mount succeeded")
        if dog then check.equal(check.result("ok", "the ridden dog's life", actor.alive(dog)), true, "the dog is in play") end
    end)
    if held then captain_api.release_cursor(captain) end
    return state
end)
