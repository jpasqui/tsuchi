local actor_api = entity

-- The actions of actors in play and the public subscription's lifetime.
local check = require("check")
check.plan("read", "listener")
check.retry(function()
    if game.has_control() ~= true then return check.run("read", function() check.wait("a loaded game with player control") end) end
    local decided = false
    for _, actor in ipairs(actor_api.list() or {}) do
        if behavior.action(actor) then decided = true break end
    end
    if not decided then return check.run("read", function() check.wait("an actor with decisions in play") end) end
    check.run("read", function()
        local read = 0
        for _, actor in ipairs(actor_api.list()) do
            local action, why = behavior.action(actor)
            if action then
                read = read + 1
                check.expect(type(action) == "string", "the action is text")
                check.expect(not action:match("^/") and not action:match("/$") and not action:match("//"), "the action path has no empty parts")
            else check.equal(why, "unsupported", "an actor with no decisions") end
        end
        check.expect(read > 0, "an actor's decisions were read")
    end)
    check.run("listener", function()
        local heard = check.result("ok", "listening for changed decisions", behavior.on_changed(function() end))
        check.result("ok", "ending the listener", heard:cancel())
        check.result("not_found", "ending it twice", heard:cancel())
    end)
    return "pass"
end)
