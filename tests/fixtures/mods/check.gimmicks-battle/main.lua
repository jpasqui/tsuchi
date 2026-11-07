local check = require("check")
local listed = false
local battle_seen = false
local changed = {}
local acted = {}
local tick = 0
local replay = nil
local battle_tick = 0
local rounds = 0
check.plan("gimmicks-listed", "battle-read")
gimmicks.on_changed(function(event)
    changed[event.actor] = true
    mod.log("gimmick event " .. event.kind .. " done=" .. tostring(event.done)
        .. " progress=" .. tostring(event.progress) .. " on=" .. tostring(event.on))
end)
battle.on_phase_changed(function(event)
    mod.log("battle phase " .. event.phase)
    if event.phase == "running" then
        rounds = rounds + 1
        battle_tick = tick
    end
end)
battle.on_score_changed(function(event)
    mod.log("battle score " .. event.player .. " " .. event.score)
end)
battle.on_popped(function(event)
    mod.log("battle pop " .. event.group .. "/" .. event.place .. " " .. event.kind
        .. " class=" .. event.of_class .. " alternate=" .. tostring(event.alternate))
    if acted.battle and not acted.pop and event.actor then
        acted.pop = true
        local held, why = battle.hold_outcome("pop")
        mod.log("battle hold pop " .. tostring(held) .. " " .. tostring(why))
        if held then
            replay = {group=event.group, place=event.place, kind=event.kind,
                of_class=event.of_class, alternate=event.alternate, attempts=0}
            local removed, reason = entity.destroy(event.actor)
            mod.log("battle clear pop " .. tostring(removed) .. " " .. tostring(reason))
        end
    end
end)
time.on_step(function()
    tick = tick + 1
    if tick % 30 ~= 0 then return end
    local actors, why = gimmicks.list()
    if actors and not listed then
        listed = true
        check.run("gimmicks-listed", function() return "count=" .. #actors end)
        for _, actor in ipairs(actors) do
            local info = gimmicks.describe(actor)
            if info then mod.log("gimmick " .. info.kind .. " done=" .. tostring(info.done)) end
        end
    end
    if tick > 90 and actors then
        for _, actor in ipairs(actors) do
            if not acted[actor] then
                local info = gimmicks.describe(actor)
                if info and not info.done then
                    local kind = info.kind:upper()
                    local action
                    if kind:find("CRACKPOT", 1, true) then action = function() return gimmicks.finish(actor) end
                    elseif kind:find("GATE", 1, true) then action = function() return gimmicks.break_gate(actor, false) end
                    elseif kind:find("SWITCH", 1, true) then action = function() return gimmicks.press(actor) end
                    elseif kind:find("EXCAVATION", 1, true) then action = function() return gimmicks.finish(actor) end
                    end
                    if action then
                        acted[actor] = true
                        local ok, result = action()
                        mod.log("gimmick action " .. kind .. " " .. tostring(ok) .. " " .. tostring(result))
                    end
                end
            end
        end
    end
    local phase = battle.phase()
    if phase and not battle_seen then
        battle_seen = true
        check.run("battle-read", function() return phase end)
    end
    if phase == "running" and not acted.battle then
        acted.battle = true
        battle_tick = tick
        mod.log("battle add score " .. tostring(battle.add_score(0, 50)))
    end
    if replay and phase == "running" then
        replay.attempts = replay.attempts + 1
        local ok, why = battle.replay_pop(replay.group, replay.place, replay.kind,
            replay.of_class, replay.alternate)
        mod.log("battle replay pop " .. tostring(ok) .. " " .. tostring(why))
        if ok or why ~= "busy" or replay.attempts >= 20 then
            battle.release_outcome("pop")
            replay = nil
        end
    end
    if acted.battle and phase == "running" and rounds == 1 and tick - battle_tick > 1800 and not acted.ended then
        acted.ended = true
        mod.log("battle end " .. tostring(battle.end_match()))
    end
    if acted.battle and phase == "running" and rounds > 1 and tick - battle_tick > 300 and not acted.aborted then
        acted.aborted = true
        mod.log("battle abort " .. tostring(battle.abort()))
    end
    local flow = battle.flow()
    if flow and flow.stage == "results" and flow.step == 3 and not acted.again then
        acted.again = true
        mod.log("battle Play Again " .. tostring(battle.play_again()))
    end
end)
