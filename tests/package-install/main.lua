-- A read-only smoke check, staged separately from the release archive.
mod.log("package-install start framework=" .. tostring(mod.framework()) .. " id=" .. mod.id())

local observed_world, steps = nil, 0
local last_state, changes = nil, 0
local finished = false
local stepping

local function state(name)
    if name ~= last_state then
        last_state = name
        if changes < 8 then
            changes = changes + 1
            mod.log("package-install state=" .. name)
        end
    end
end

stepping = time.on_step(function()
    if finished then return end
    if game.has_control() ~= true then
        observed_world, steps = nil, 0
        state("waiting-control")
        return
    end
    local world = game.world()
    if not world then
        steps = 0
        state("waiting-world")
        return
    end
    local character = player.character(0)
    if not character then
        steps = 0
        state("waiting-character")
        return
    end
    local transform = entity.pose(character)
    local location = transform and transform.location
    if not location or location.x ~= location.x or location.y ~= location.y or location.z ~= location.z then
        steps = 0
        state("waiting-transform")
        return
    end
    if world ~= observed_world then
        observed_world, steps = world, 0
    end
    state("observing")
    steps = steps + 1
    if steps >= 60 then
        finished = true
        mod.log("package-install pass world=" .. tostring(world) .. " controlled_steps=" .. tostring(steps))
        stepping:cancel()
    end
end)
