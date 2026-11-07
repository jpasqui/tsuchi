local actor_api = entity
local captain_api = captain
local player_api = player

-- Read-only optional diagnosis of captain data, including mounted story characters.
local previous, changes, steps, stepping = {}, 0, 0
local function status(value, why) return value ~= nil and "ok" or tostring(why or "none") end
local function tell(key, value)
    value = tostring(value)
    if previous[key] == value or changes >= 200 then return end
    previous[key], changes = value, changes + 1
    mod.log("route captain " .. key .. " " .. value)
end
local function actor_name(actor, why)
    if not actor then return tostring(why or "none") end
    local kind, error = actor_api.kind(actor)
    local class, problem = reflection.class_of(actor)
    local name, trouble = class and reflection.name(class)
    return tostring(kind or error) .. " class=" .. tostring(name or trouble or problem)
end
local function read_squad(key, actor)
    if not actor then return end
    local members, why = squad.members(actor)
    tell(key, members and ("ok count=" .. #members) or tostring(why or "none"))
end
stepping = time.on_step(function()
    steps = steps + 1
    if steps == 1 then mod.log("route captain diagnostic read-only") end
    if steps ~= 1 and steps % 30 ~= 0 then return end
    local mode, mode_why = game.mode()
    local world, world_why = game.world()
    local control, control_why = game.has_control()
    tell("mode", mode or mode_why)
    tell("world", world or world_why)
    tell("control", control ~= nil and tostring(control) or control_why)
    local count, why = player_api.count()
    tell("players", count or why)
    local avatar, avatar_why = reflection.find_class("AvatarComponent")
    tell("avatar-class", status(avatar, avatar_why))
    for player = 0, (count or 0)-1 do
        local prefix = "player" .. player
        local captain, captain_why = player_api.captain(player)
        local dog, dog_why = player_api.dog(player)
        local character, character_why = player_api.character(player)
        tell(prefix .. "-captain", actor_name(captain, captain_why))
        tell(prefix .. "-dog", actor_name(dog, dog_why))
        tell(prefix .. "-character", actor_name(character, character_why))
        if captain then
            local cursor, cursor_why = captain_api.cursor(captain)
            local whistle, whistle_why = captain_api.whistle(captain)
            local mount, mount_why = captain_api.riding(captain)
            tell(prefix .. "-cursor", status(cursor, cursor_why))
            tell(prefix .. "-whistle", whistle and ("ok active=" .. tostring(whistle.active)) or tostring(whistle_why or "none"))
            tell(prefix .. "-mount", actor_name(mount, mount_why))
            read_squad(prefix .. "-captain-squad", captain)
            local parts, parts_why = component.list(captain)
            tell(prefix .. "-components", parts and ("ok count=" .. #parts) or tostring(parts_why))
            local descriptions, avatars = {}, 0
            for _, part in ipairs(parts or {}) do
                local class = reflection.class_of(part)
                descriptions[#descriptions+1] = tostring(class and reflection.name(class) or "unknown")
                if avatar and reflection.is_a(part, avatar) == true then avatars = avatars + 1 end
            end
            table.sort(descriptions)
            tell(prefix .. "-component-classes", table.concat(descriptions, ","))
            tell(prefix .. "-avatar-parts", avatars)
        end
        read_squad(prefix .. "-dog-squad", dog)
    end
    if steps >= 18000 or changes >= 200 then
        mod.log("route captain diagnostic complete changes=" .. changes)
        stepping:cancel()
    end
end)
