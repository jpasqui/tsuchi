-- Asks the game's announcement banner to show the check's text.
local check = require("check")
check.plan("shown", "refused")
check.run("refused", function()
    check.result("invalid", "zero seconds", hud.banner("Tsuchi SDK banner check", 0))
    check.result("invalid", "negative seconds", hud.banner("Tsuchi SDK banner check", -1))
end)
check.retry(function()
    if game.has_control() ~= true then return check.run("shown", function() check.wait("a loaded game with player control") end) end
    return check.run("shown", function()
        local shown, why = hud.banner("Tsuchi SDK banner check", 4)
        if why == "not_ready" then check.wait("the game's announcement banner") end
        check.result("ok", "showing the banner", shown, why)
    end)
end)
