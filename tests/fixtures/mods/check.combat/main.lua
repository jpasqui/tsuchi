local actor_api = entity

-- Reads a living actor's life and writes the same value; no hit or death is made.
local check = require("check")
check.plan("read", "put_back", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("read", function() check.wait("a loaded game with player control") end) end
    local actor, before
    for _, each in ipairs(actor_api.list() or {}) do
        local life = combat.life(each)
        if life and life.life > 0 then actor, before = each, life break end
    end
    if not actor then return check.run("read", function() check.wait("a living actor with positive life") end) end
    check.run("read", function()
        check.expect(actor ~= nil, "an actor has positive life")
        if not actor then return end
        check.expect(before.life >= 0 and before.life <= before.max_life, "life is within its maximum")
        check.expect(before.extra >= 0 and before.extra <= before.max_extra, "extra life is within its maximum")
    end)
    check.run("put_back", function()
        check.result("ok", "putting its life back", combat.set_life(actor, before.life))
        local after = check.result("ok", "reading it back", combat.life(actor))
        for _, field in ipairs({"life", "max_life", "extra", "max_extra"}) do check.near(after[field], before[field], 0.01, "the life field " .. field) end
    end)
    check.run("refused", function()
        check.result("invalid", "life below zero", combat.set_life(actor, -1))
        check.result("invalid", "life above its maximum", combat.set_life(actor, before.max_life + 1))
        check.result("invalid", "a negative hit", combat.damage(actor, -1))
        check.near(combat.life(actor).life, before.life, 0.01, "the refusals left its life alone")
    end)
    return "pass"
end)
