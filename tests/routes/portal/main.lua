-- Explicit isolated-save route for the game's real cave entrance and departure.
local actor_api, player_api, mod_api = entity, player, mod
local steps, phase, phase_at, origin, requested, portal = 0, "ready", 0, nil, nil, nil
local lines, dirty, previous, selection = {}, false, {}, nil
local area_world, cave_world = "Area001_P", "Cave001_F00_P"
local arrival_since, ready_since, ready_world, setup_since
local stepping
local function log(text)
    if #lines >= 240 then return end
    lines[#lines + 1] = string.format("%6d %s", steps, text)
    dirty = true
    mod_api.log("route portal " .. text)
end
local function flush()
    if dirty and storage.write("log", table.concat(lines, "\n") .. "\n") == true then dirty = false end
end
local function finish(result)
    log("RESULT " .. result)
    phase = "done"
end
local function change(to)
    phase, phase_at, requested = to, steps, nil
    log("phase " .. to)
end
local function request(label, fn, ...)
    local done, why = fn(...)
    local text = tostring(done) .. " " .. tostring(why)
    if requested ~= text then log("request " .. label .. " " .. text) requested = text end
    if done == true then return true end
    if why ~= "busy" and why ~= "not_ready" then finish("FAIL " .. label .. " " .. text) end
    return false
end
local function select_portal()
    if not selection then
        local class, why = reflection.find_class("PortalTriggerComponent")
        if not class then
            if previous.trigger ~= why then log("waiting trigger class " .. tostring(why)) previous.trigger = why end
            return nil
        end
        selection = {class = class, list = actor_api.list() or {}, at = 1}
        if previous.search_count ~= #selection.list then
            log("entrance search actors=" .. #selection.list)
            previous.search_count = #selection.list
        end
    end
    for _ = 1, 24 do
        local subject = selection.list[selection.at]
        if not subject then selection = nil return nil end
        selection.at = selection.at + 1
        local part = reflection.call(subject, "GetComponentByClass", {ComponentClass = selection.class})
        if part then
            local destination = reflection.get(part, "ToLevelName")
            if destination == "Cave001_F00" or destination == cave_world then
                log("selected " .. tostring(reflection.name(subject)) .. " destination=" .. destination)
                return subject
            end
        end
    end
    return nil
end
log("started destination=" .. cave_world)
stepping = time.on_step(function()
    steps = steps + 1
    if steps % 15 ~= 0 then return end
    local world, controlled = game.world(), game.has_control()
    if world ~= previous.world or controlled ~= previous.control then
        log("observed world=" .. tostring(world) .. " control=" .. tostring(controlled))
        previous.world, previous.control = world, controlled
    end
    if phase == "done" then flush() if not dirty then stepping:cancel() end return end
    if steps > 18000 or steps - phase_at > 6000 then finish("FAIL timeout phase=" .. phase) flush() return end
    if phase == "ready" then
        if controlled ~= true or steps - phase_at < 2400 then ready_since = nil flush() return end
        if ready_world ~= world then ready_world, ready_since = world, nil end
        if not ready_since then ready_since = steps end
        if steps - ready_since < 120 then flush() return end
        if world == area_world then
            origin = area_world
            change("find_entrance")
        elseif type(world) == "string" and world:sub(1, 4) == "Area" then
            log("setup initial area=" .. world .. " target=" .. area_world)
            change("setup_travel")
        else finish("FAIL unexpected initial world=" .. tostring(world)) end
    elseif phase == "setup_travel" then
        if controlled ~= true or world ~= ready_world then setup_since = nil flush() return end
        if not setup_since then setup_since = steps end
        if steps - setup_since >= 120 and request("travel setup", story.travel, area_world, 0) then change("setup_arrival") end
    elseif phase == "setup_arrival" then
        if world == area_world and controlled == true then
            origin = area_world
            log("observed setup arrival world=" .. world .. " control=true")
            change("find_entrance")
        end
    elseif phase == "find_entrance" then
        if world ~= origin or controlled ~= true or steps - phase_at < 300 then flush() return end
        portal = select_portal()
        if portal then
            local at = actor_api.pose(portal)
            local leader, dog = player_api.captain(0), player_api.dog(0)
            if not at or not leader then finish("FAIL selected entrance has no approach")
            else
                captain.dismount(leader)
                local face = {pitch = 0, yaw = 0, roll = 0}
                local location = {x = at.location.x - 200, y = at.location.y, z = at.location.z + 30}
                if dog then log("approach dog " .. tostring(actor_api.warp(dog, location, face))) end
                log("approach captain " .. tostring(actor_api.warp(leader, location, face)))
                change("entry_request")
            end
        end
    elseif phase == "entry_request" then
        if controlled == true and steps - phase_at >= 60 and request("enter_portal", story.enter_portal, portal) then
            log("awaiting native confirmation destination=" .. cave_world)
            change("entering")
        end
    elseif phase == "entering" then
        if world == cave_world and controlled == true then
            if not arrival_since then arrival_since = steps end
            if steps - arrival_since < 120 then flush() return end
            log("observed portal arrival world=" .. world .. " control=true")
            change("departure_request")
        else arrival_since = nil end
    elseif phase == "departure_request" then
        if world == cave_world and controlled == true and steps - phase_at >= 600
            and request("leave_cave", story.leave_cave) then change("leaving") end
    elseif phase == "leaving" then
        if world == origin and controlled == true then
            log("observed cave departure world=" .. world .. " control=true")
            finish("PASS portal entry and cave departure")
        end
    end
    flush()
end)
