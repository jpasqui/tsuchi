-- Run in an isolated gameplay fixture. The log states the observed result.
local lines, labels = {}, {}
local phase, steps, began = 0, 0, 0
local captain, made, placed, mounted, natural, looped
local once, loop, visible, parameter_name, parameter_kind
local function log(message)
    if #lines < 2000 then lines[#lines + 1] = message end
    mod.log(message)
end
local function save()
    local ok, result = storage.write("observations.txt", table.concat(lines, "\n") .. "\n")
    if not ok then mod.log("OBSERVATIONS_SAVE " .. tostring(result)) end
end
local function point_text(p)
    return p and string.format("%.2f,%.2f,%.2f", p.x, p.y, p.z) or "none"
end
effects.on_finished(function(event)
    log("FINISHED " .. (labels[event.effect] or "unlabelled") .. " " .. event.reason)
    save()
end)
local function play(kind, parent, point, label)
    local effect, result
    if parent then effect, result = effects.play_on(kind, parent, point)
    else effect, result = effects.play(kind, point) end
    log("PLAY " .. label .. " " .. tostring(kind) .. " " .. tostring(effect ~= nil) .. " " .. tostring(result))
    if effect then labels[effect] = label end
    return effect
end
local function inspect_asset(asset)
    local fields = {
        {"StartSize", "vector"}, {"SizeScale", "vector"}, {"StartColor", "vector"},
        {"ColorOverLife", "vector"}, {"ColorScaleOverLife", "vector"},
        {"StartAlpha", "scalar"}, {"AlphaOverLife", "scalar"}, {"AlphaScaleOverLife", "scalar"},
        {"SpawnRate", "scalar"}, {"SpawnRateScale", "scalar"}, {"LifeTime", "scalar"},
    }
    local found, kind, field, looping
    for _, emitter in ipairs(reflection.get(asset, "Emitters") or {}) do
        for _, lod in ipairs(reflection.get(emitter, "LODLevels") or {}) do
            local required = reflection.get(lod, "RequiredModule")
            if required then
                local count = reflection.get(required, "EmitterLoops")
                if count == 0 then looping = true end
            end
            local modules = reflection.get(lod, "Modules") or {}
            local spawn = reflection.get(lod, "SpawnModule")
            if spawn then modules[#modules + 1] = spawn end
            for _, module in ipairs(modules) do
                for _, item in ipairs(fields) do
                    local distribution = reflection.get(module, item[1] .. ".Distribution")
                    local name = distribution and reflection.get(distribution, "ParameterName")
                    if name and name ~= "" and name ~= "None" then
                        found, kind, field = name, item[2], item[1]
                        if field == "StartSize" or field == "SpawnRate" or field == "StartAlpha" then break end
                    end
                end
            end
        end
    end
    return found, kind, field, looping
end
local function inspect_loaded_effects(kinds)
    local available = {}
    for _, name in ipairs(kinds) do available[name] = true end
    local particle_class = reflection.find_class("ParticleSystemComponent")
    if not particle_class then log("PARAMETER_ASSET_INSPECTION no_particle_component_class") return end
    local seen, examined = {}, 0
    for _, actor in ipairs(actor.list() or {}) do
        for _, part in ipairs(component.list(actor) or {}) do
            if reflection.is_a(part, particle_class) then
                local asset = reflection.get(part, "Template")
                if asset and not seen[asset] then
                    seen[asset] = true
                    examined = examined + 1
                    local name = reflection.name(asset)
                    local parameter, kind, field, looping = inspect_asset(asset)
                    log("ASSET " .. tostring(name) .. " parameter=" .. tostring(parameter) .. " shape=" .. tostring(kind) .. " field=" .. tostring(field) .. " loops=" .. tostring(looping))
                    if available[name] and looping then loop = name end
                    if available[name] and looping and parameter and (field == "StartSize" or field == "SpawnRate" or field == "StartAlpha" or field == "ColorScaleOverLife") then
                        loop, parameter_name, parameter_kind = name, parameter, kind
                        return
                    end
                end
            end
        end
        if examined >= 64 then break end
    end
    log("PARAMETER_ASSET_INSPECTION no_exposed_size_rate_alpha_parameter")
end
local function ground_at(actor, label)
    local pose = entity.pose(actor)
    if not pose then return end
    local ground, result = world.ground(pose.location, {x = 50, y = 50, z = 300})
    log("GROUND " .. label .. " actor=" .. point_text(pose.location) .. " nav=" .. point_text(ground) .. " " .. tostring(result))
end
local stepping
stepping = time.on_step(function()
    steps = steps + 1
    if phase == 0 then
        if not game.has_control() then return end
        captain = player.captain(0)
        local pose = captain and entity.pose(captain)
        local kinds = effects.list()
        if not pose or not kinds then return end
        phase = -1 -- Setup runs once. An error does not spawn again on every step.
        log("CATALOG " .. #kinds)
        for _, kind in ipairs(kinds) do
            if #lines < 40 then log("KIND " .. kind) end
            local lower = kind:lower()
            if not once and (lower:find("hit", 1, true) or lower:find("foot", 1, true)) then once = kind end
            if kind == "PS_eft_Demo_Pikmin_HitPurpleSmoke00" then once = kind end
            if not loop and (lower:find("loop", 1, true) or lower:find("smoke", 1, true) or lower:find("aura", 1, true)) then loop = kind end
        end
        once = once or kinds[1]
        inspect_loaded_effects(kinds)
        log("CHOSEN once=" .. tostring(once) .. " loop=" .. tostring(loop))
        ground_at(captain, "captain")
        local dog = player.dog(0)
        if dog then ground_at(dog, "dog") end
        local creatures = creature.list() or {}
        local source = creatures[1]
        if source then
            ground_at(source, "creature")
            local class = reflection.class_of(source)
            local location = {x = pose.location.x + 200, y = pose.location.y, z = pose.location.z}
            made = class and entity.spawn(class, location, pose.rotation)
            log("SPAWN " .. tostring(made ~= nil))
        end
        local target = {x = pose.location.x + 500, y = pose.location.y + 100, z = pose.location.z}
        local points, result = world.path(pose.location, target)
        log("PATH " .. tostring(points and #points) .. " " .. tostring(result))
        if points then for i, point in ipairs(points) do log("WAYPOINT " .. i .. " " .. point_text(point)) end end
        placed = play(once, nil, {x = pose.location.x + 80, y = pose.location.y, z = pose.location.z + 60}, "placed")
        if made then mounted = play(loop or once, made, {x = 0, y = 0, z = 60}, "parent") end
        if loop then looped = play(loop, captain, {x = 0, y = 0, z = 80}, "loop") end
        if parameter_name and loop then
            visible = play(loop, nil, {x = pose.location.x + 150, y = pose.location.y, z = pose.location.z + 80}, "visible_parameter")
            if visible then
                local ok, result = effects.set_parameter(visible, parameter_name, parameter_kind, {x = 0, y = 0, z = 0, w = 0})
                log("PARAMETER zero " .. parameter_name .. " " .. tostring(ok) .. " " .. tostring(result))
                local read, read_result = effects.parameter(visible, parameter_name, parameter_kind)
                log("PARAMETER_READ " .. tostring(read and read.x) .. " " .. tostring(read_result))
            end
        end
        natural = play(once, captain, {x = 0, y = 0, z = 80}, "natural")
        began, phase = steps, 1
        save()
    elseif phase == 1 and steps - began >= 180 then
        if visible then
            local amount = parameter_kind == "scalar" and 20 or 1
            local ok, result = effects.set_parameter(visible, parameter_name, parameter_kind, {x = amount, y = amount, z = amount, w = 1})
            log("PARAMETER visible " .. parameter_name .. " " .. tostring(ok) .. " " .. tostring(result))
        end
        if placed then local ok, result = effects.stop(placed) log("STOP placed " .. tostring(ok) .. " " .. tostring(result)) end
        if made then local ok, result = entity.destroy(made) log("DESTROY parent " .. tostring(ok) .. " " .. tostring(result)) end
        save()
        phase = 2
    elseif phase == 2 and steps - began >= 600 then
        if visible then local ok, result = effects.stop(visible) log("STOP visible " .. tostring(ok) .. " " .. tostring(result)) end
        if looped then local ok, result = effects.stop(looped) log("STOP loop " .. tostring(ok) .. " " .. tostring(result)) end
        if natural then local ok, result = effects.stop(natural) log("STOP natural remaining " .. tostring(ok) .. " " .. tostring(result)) end
        log("DONE native selfcheck measures creature follow, screenshots determine visible parameter response")
        save()
        stepping:cancel()
        phase = 3
    end
end)
