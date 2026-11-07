-- Observe game-owned generators. Native controls perform combat and saving.
local ids = require("ids")
local steps, lines, held, known = 0, {}, {}, {}
local function log(text)
    if #lines == 1200 then table.remove(lines, 1) end
    lines[#lines + 1] = tostring(steps) .. " " .. text
    print(text)
end
local function write_observations()
    storage.write("observations", table.concat(lines, "\n") .. "\n")
end
local function identity(one)
    return known[tostring(one)] or entity.placement(one)
end
combat.on_defeated(function(event)
    local id = identity(event.actor)
    if id == ids.larva then
        log("larva defeated event previous " .. tostring(event.previous) .. " life " .. tostring(event.life))
    end
end)
game.on_world_unloaded(function(event)
    log("world unloaded " .. tostring(event.world))
end)
game.on_world_loaded(function(event)
    log("world loaded " .. tostring(event.world))
end)
game.on_stage_changed(function(event)
    log("stage changed previous " .. tostring(event.previous) .. " stage " .. tostring(event.stage))
    write_observations()
end)
log("observer started")
time.on_step(function()
    steps = steps + 1
    if steps % 60 ~= 0 then return end
    local ok, why = pcall(function()
        log("state world " .. tostring(game.world()) .. " mode " .. tostring(game.mode()) ..
            " stage " .. tostring(game.stage()) .. " control " .. tostring(game.has_control()) ..
            " placement mod " .. tostring(mod.version_of("release.t3.night")))
        local captain = player.captain(0)
        local at = captain and entity.pose(captain)
        if at then log(string.format("captain %.3f %.3f %.3f", at.location.x, at.location.y, at.location.z)) end
        local present = {}
        for _, one in ipairs(entity.list() or {}) do
            local id = entity.placement(one)
            if id == ids.larva or id == ids.pot then
                known[tostring(one)] = id
                held[tostring(one)] = one
                present[id] = true
                local place, life = entity.pose(one), combat.life(one)
                local class = reflection.class_of(one)
                local path = class and reflection.path(class)
                log("placed " .. tostring(id) .. " name " .. tostring(reflection.name(one)) ..
                    " class " .. tostring(path) .. " defeated " .. tostring(creature.defeated(one)) ..
                    " life " .. tostring(life and life.life))
                if place then log(string.format("position %s %.3f %.3f %.3f", tostring(id),
                    place.location.x, place.location.y, place.location.z)) end
            end
        end
        log("present larva " .. tostring(present[ids.larva] == true) .. " pot " .. tostring(present[ids.pot] == true))
        for key, one in pairs(held) do
            local kind, error = entity.kind(one)
            if not kind then
                log("held handle ended " .. tostring(known[key]) .. " error " .. tostring(error))
                held[key] = nil
            end
        end
    end)
    if not ok then log("observation error " .. tostring(why)) end
    write_observations()
end)
