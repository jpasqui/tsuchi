local actor_api = entity
local engine_actor = actor

-- All damage and defeat belongs to the creature this mod makes.
local check = require("check")
check.plan("found", "made", "life", "listened", "intercepted", "lowered", "drop", "healed", "quiet_heal", "hit", "credited", "knock", "defeated", "order", "cancelled", "removed", "gone")
local wanted = (file.read("targets.txt", 0, 256) or ""):match("[^\r\n]+")
local made, captain, initial, lower, after_hit, stepping, damaged, defeated, intercepted
local drops, ends, order, caught = {}, {}, {}, {}
local phase, steps, since = 0, 0, 0
local function wait_for(name, why) check.run(name, function() check.wait(why) end) end
local function finish()
    check.run("cancelled", function()
        for _, listener in ipairs({damaged, defeated}) do
            check.result("ok", "ending the damage listener", listener:cancel())
            check.result("not_found", "ending it twice", listener:cancel())
        end
    end)
    stepping:cancel()
end
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 then
        if game.has_control() ~= true then return wait_for("found", "a loaded game with player control") end
        captain = player.captain(0)
        if not captain then return wait_for("found", "the first captain") end
        local source, class, at
        for _, actor in ipairs(actor_api.list() or {}) do
            local life = combat.life(actor)
            if actor_api.kind(actor) == wanted and life and life.life > 30 then
                source, class, at = actor, reflection.class_of(actor), actor_api.pose(actor)
                if class and at then break end
            end
        end
        if not source or not class or not at then return wait_for("found", "a healthy stock creature named in targets.txt") end
        check.run("found", function() check.equal(engine_actor.alive(source), true, "the healthy source is in play") end)
        check.run("made", function()
            made = check.result("ok", "making its own creature", actor_api.spawn(class, {x=at.location.x+1000, y=at.location.y, z=at.location.z+20}, at.rotation))
            check.expect(made ~= source, "the copy belongs to this check")
        end)
        if not made then stepping:cancel() return end
        phase, since = 1, steps
        return
    end
    if phase == 1 then
        initial = combat.life(made)
        if not initial or initial.life <= 30 then return wait_for("life", "the owned creature's initialized life") end
        check.run("life", function() check.expect(initial.life <= initial.max_life, "the owned creature is healthy") end)
        check.run("listened", function()
            damaged = check.result("ok", "listening for damage", combat.on_damaged(function(event)
                if event.actor == made then drops[#drops+1], order[#order+1] = event, "damaged" end
            end))
            defeated = check.result("ok", "listening for defeat", combat.on_defeated(function(event)
                if event.actor == made then ends[#ends+1], order[#order+1] = event, "defeated" end
            end))
            intercepted = check.result("ok", "listening for suspended messages", combat.on_intercepted(function(event)
                if event.actor == made then caught[#caught+1] = event end
            end))
            check.result("ok", "suspending its owned creature's decisions", actor_api.hold_suspended(made, {logic=true}))
            check.result("ok", "sending a hit to the suspended creature", combat.damage(made, 5, "normal", captain))
        end)
        phase, since = 8, steps
        return
    end
    if phase == 8 then
        if #caught == 0 and steps-since < 90 then return end
        check.run("intercepted", function()
            check.equal(#caught, 1, "exactly one message was intercepted")
            if caught[1] then
                check.equal(caught[1].actor, made, "the owned suspended victim")
                check.equal(caught[1].attacker, captain, "the real sender")
                check.equal(caught[1].kind, "hit", "the message kind")
                check.equal(caught[1].attack, "normal", "the original attack kind")
                check.near(caught[1].amount, 5, 0.01, "the unscaled intercepted amount")
            end
            check.near(check.result("ok", "the intercepted victim's life", combat.life(made)).life, initial.life, 0.01, "the suspended creature did not answer the hit")
            check.equal(#drops, 0, "interception emitted no damage")
            check.result("ok", "restoring its decisions", actor_api.release_suspended(made, {logic=true}))
            check.result("ok", "ending the interception listener", intercepted:cancel())
            check.result("not_found", "ending it twice", intercepted:cancel())
        end)
        phase, since = 9, steps
        return
    end
    if phase == 9 then
        if steps-since < 2 then return end
        lower = initial.life - 5
        check.run("lowered", function()
            check.result("ok", "lowering only its life", combat.set_life(made, lower))
            check.near(combat.life(made).life, lower, 0.01, "the requested life")
        end)
        phase, since = 2, steps
        return
    end
    if phase == 2 then
        if #drops == 0 and steps-since < 90 then return end
        check.run("drop", function()
            check.equal(#drops, 1, "one decrease was heard")
            check.equal(#ends, 0, "the creature was not defeated")
            if drops[1] then
                check.near(drops[1].previous, initial.life, 0.01, "the earlier life")
                check.near(drops[1].life, lower, 0.01, "the lower life")
                check.expect(drops[1].attacker == nil, "the setter names no attacker")
            end
        end)
        check.run("healed", function()
            check.result("ok", "healing its own creature with a smooth gauge", combat.set_life(made, initial.life, {smooth_gauge=true}))
            check.near(combat.life(made).life, initial.life, 0.01, "its restored life")
        end)
        phase, since = 3, steps
        return
    end
    if phase == 3 then
        if steps-since < 3 then return end
        check.run("quiet_heal", function() check.equal(#drops, 1, "healing emits no damage") check.equal(#ends, 0, "healing emits no defeat") end)
        check.run("hit", function() check.result("ok", "the captain hits its own creature", combat.damage(made, 10, "normal", captain)) end)
        phase, since = 4, steps
        return
    end
    if phase == 4 then
        after_hit = combat.life(made)
        if (#drops < 2 or not after_hit or after_hit.life >= initial.life) and steps-since < 90 then return end
        check.run("credited", function()
            check.expect(after_hit and after_hit.life > 0 and after_hit.life < initial.life, "the game scaled a nonfatal hit")
            check.equal(#drops, 2, "one hit decrease was heard")
            check.equal(#ends, 0, "the hit was nonfatal")
            if drops[2] then
                check.equal(drops[2].attacker, captain, "the captain gets credit")
                check.near(drops[2].previous, initial.life, 0.01, "life before the hit")
                check.near(drops[2].life, after_hit.life, 0.01, "the game's life after the hit")
            end
        end)
        check.run("knock", function() check.result("ok", "the creature is sent a knock it ignores", combat.knockback(made, {x=0,y=0,z=300}, 0, captain)) end)
        phase, since = 5, steps
        return
    end
    if phase == 5 then
        if steps-since < 3 then return end
        check.run("defeated", function()
            check.near(combat.life(made).life, after_hit.life, 0.01, "the knock took no life")
            check.equal(#drops, 2, "the harmless knock emitted no damage")
            check.result("ok", "defeating only its own creature", combat.set_life(made, 0))
        end)
        phase, since = 6, steps
        return
    end
    if phase == 6 then
        if (#drops < 3 or #ends == 0) and steps-since < 90 then return end
        check.run("order", function()
            check.equal(#drops, 3, "one final decrease") check.equal(#ends, 1, "one defeat")
            check.equal(table.concat(order, ","), "damaged,damaged,damaged,defeated", "damage precedes defeat")
            for _, event in ipairs({drops[3], ends[1]}) do
                check.near(event.previous, after_hit.life, 0.01, "life before defeat")
                check.near(event.life, 0, 0.01, "life at defeat")
                check.expect(event.attacker == nil, "the defeat setter names no attacker")
            end
        end)
        check.run("removed", function() check.result("ok", "quietly removing its owned creature", actor_api.destroy(made)) end)
        phase, since = 7, steps
        return
    end
    if engine_actor.alive(made) and steps-since < 90 then return end
    check.run("gone", function() check.equal(engine_actor.alive(made), false, "the owned creature left play") end)
    finish()
end)
