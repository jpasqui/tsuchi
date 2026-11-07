-- Plays a named sound on the first player's captain.
local check = require("check")
check.plan("heard", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("heard", function() check.wait("a loaded game with player control") end) end
    local captain = player.captain(0)
    if not captain then return check.run("heard", function() check.wait("the first player's captain") end) end
    local state = check.run("heard", function()
        local played, why = audio.play_on(captain, "Whistle_Charge")
        if why == "not_ready" then check.wait("the game's sound system") end
        check.result("ok", "the captain's whistle charge", played, why)
    end)
    if state == "waiting" then return state end
    check.run("refused", function()
        check.result("not_found", "a sound the captain lacks", audio.play_on(captain, "TsuchiNoSuchSound"))
        check.result("invalid", "an empty sound key", audio.play_on(captain, ""))
    end)
    return state
end)
