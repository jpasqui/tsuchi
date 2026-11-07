-- Runs on an isolated save. Each transform change is restored on the same step.
local check = require("check")
local names = {
    "world_list", "scenery_owner", "scenery_root", "scenery_set_restore",
    "light_owner", "light_root", "light_set_restore", "entity_refuses_scenery",
    "entity_refuses_light", "character_divergence", "character_entity_setter",
    "character_actor_setter"
}
check.plan(table.unpack(names))
local finished, classes, refused = {}, {}, {}
local steps = 0
local timeout = 18000
-- A missing map or unsupported donor ends in a measured failure after ten minutes of steps.
local clock = time.on_step(function() steps = steps + 1 end)

local function class(name)
    if not classes[name] then classes[name] = reflection.find_class(name) end
    return classes[name]
end
local function member(list, wanted)
    for _, value in ipairs(list or {}) do if value == wanted then return true end end
    return false
end
local function difference(a, b)
    return (a - b + 180) % 360 - 180
end
local function vector(value)
    if type(value) ~= "table" then return nil end
    if type(value.X) ~= "number" or type(value.Y) ~= "number" or type(value.Z) ~= "number" then return nil end
    return {x=value.X, y=value.Y, z=value.Z}
end
local function rotation(value)
    if type(value) ~= "table" then return nil end
    if type(value.Pitch) ~= "number" or type(value.Yaw) ~= "number" or type(value.Roll) ~= "number" then return nil end
    return {pitch=value.Pitch, yaw=value.Yaw, roll=value.Roll}
end
local function engine_pose(owner)
    local location = vector(reflection.call(owner, "K2_GetActorLocation"))
    local angles = rotation(reflection.call(owner, "K2_GetActorRotation"))
    local scale = vector(reflection.call(owner, "GetActorScale3D"))
    if location and angles and scale then return {location=location, rotation=angles}, scale end
end
local function same_vector(got, wanted, slack, label)
    check.expect(type(got) == "table", label .. " is a vector")
    if not got then return end
    for _, field in ipairs({"x", "y", "z"}) do
        check.near(got[field], wanted[field], slack, label .. " " .. field)
    end
end
local function same_rotation(got, wanted, label)
    check.expect(type(got) == "table", label .. " is a rotation")
    if not got then return end
    for _, field in ipairs({"pitch", "yaw", "roll"}) do
        check.near(difference(got[field], wanted[field]), 0, 0.1, label .. " " .. field)
    end
end
local function same_pose(got, wanted, label)
    check.expect(type(got) == "table", label .. " is a pose")
    if not got then return end
    same_vector(got.location, wanted.location, 0.1, label .. " location")
    same_rotation(got.rotation, wanted.rotation, label .. " rotation")
end
local function changed(before, turn)
    return {location={x=before.location.x+2, y=before.location.y-3, z=before.location.z+4},
            rotation={pitch=before.rotation.pitch, yaw=before.rotation.yaw+turn, roll=before.rotation.roll}}
end
local function tag(owner)
    local kind = reflection.class_of(owner)
    return "world=" .. tostring(game.world()) .. " class=" .. tostring(kind and reflection.name(kind))
        .. " actor=" .. tostring(owner) .. " name=" .. tostring(reflection.name(owner))
end
local function log_pose(label, owner, pose, scale)
    if not pose or not scale then return end
    mod.log(string.format("%s %s location=%.5f,%.5f,%.5f rotation=%.5f,%.5f,%.5f scale=%.5f,%.5f,%.5f",
        label, tag(owner), pose.location.x, pose.location.y, pose.location.z,
        pose.rotation.pitch, pose.rotation.yaw, pose.rotation.roll, scale.x, scale.y, scale.z))
end
local function wait_for(what)
    if steps >= timeout then check.expect(false, "timed out after " .. steps .. " steps: " .. what)
    else check.wait(what) end
end
local function run(name, body)
    if finished[name] then return end
    local state = check.run(name, body)
    if state ~= "waiting" then finished[name] = state end
end
local function ordinary(owner, family)
    if reflection.is_a(owner, class("Actor")) ~= true or reflection.is_a(owner, class("GActor")) ~= false then return nil end
    local wanted = family == "light" and class("LightComponent") or class("MeshComponent")
    if not wanted then return nil end
    for _, part in ipairs(component.list(owner) or {}) do
        if reflection.is_a(part, wanted) == true and component.owner(part) == owner then return part end
    end
end
local function donors(list, family)
    local found = {}
    for _, owner in ipairs(list) do
        local part = ordinary(owner, family)
        if part then found[#found+1] = {owner=owner, part=part} end
    end
    -- Keep the captured native classes first while allowing other supported ordinary owners.
    table.sort(found, function(a, b)
        local function preferred(entry)
            local name = reflection.name(reflection.class_of(entry.owner)) or ""
            return name:find("CarrotSkySphere",1,true) or name:find("BP_PointLight",1,true)
                or name:find("CMA_T03_BG_Fan01",1,true) or name:find("CMA_T03_BG_CableRack",1,true)
        end
        return preferred(a) ~= nil and preferred(b) == nil
    end)
    return found
end
local function first_ready(list)
    for _, candidate in ipairs(list) do
        local pose, scale = engine_pose(candidate.owner)
        if pose then return candidate, pose, scale end
    end
end
local function owner_check(candidate, all, label)
    local owner, part = candidate.owner, candidate.part
    check.equal(check.result("ok", "the engine actor handle", actor.alive(owner)), true, "the owner is alive")
    check.equal(check.result("ok", "the part's owner", component.owner(part)), owner, "the same central Actor handle")
    check.expect(member(all, owner), "the owner is in the world list")
    local kind = check.result("ok", "the owner's class", reflection.class_of(owner))
    local filtered = check.result("ok", "the class-filtered world list", actor.list(kind))
    check.expect(member(filtered, owner), "the class filter includes the owner")
    for _, item in ipairs(filtered or {}) do
        check.equal(reflection.is_a(item, kind), true, "a filtered actor belongs to the class")
        check.expect(member(all, item), "a filtered actor belongs to the world list")
    end
    mod.log(label .. " " .. tag(owner) .. " component=" .. tostring(part))
end
local function reject_entity(owner, pose)
    check.result("invalid", "ordinary kind", entity.kind(owner))
    check.result("invalid", "ordinary team", entity.team(owner))
    check.result("invalid", "ordinary placement", entity.placement(owner))
    check.result("invalid", "ordinary gameplay transform", entity.pose(owner))
    check.result("invalid", "ordinary velocity", entity.velocity(owner))
    check.result("invalid", "ordinary suspension query", entity.suspended(owner))
    check.result("invalid", "ordinary gameplay setter", entity.set_pose(owner, pose))
    check.result("invalid", "ordinary warp", entity.warp(owner, pose.location, pose.rotation))
    check.result("invalid", "ordinary launch", entity.launch(owner, {x=0,y=0,z=1}))
    check.result("invalid", "ordinary suspension", entity.hold_suspended(owner, {logic=true}))
    check.result("invalid", "ordinary suspension release", entity.release_suspended(owner, {logic=true}))
    check.result("invalid", "ordinary gameplay removal", entity.destroy(owner))
    check.result("invalid", "ordinary gameplay spawn class",
        entity.spawn(reflection.class_of(owner), pose.location, pose.rotation, -1, false))
    check.expect(not member(entity.list(), owner), "the gameplay list excludes the ordinary owner")
end
local function restore(owner, original, scale, body)
    check.result("ok", "restore the root", actor.set_transform(owner, original))
    if body then check.result("ok", "restore the gameplay body", entity.set_pose(owner, body)) end
    local restored, restored_scale = engine_pose(owner)
    same_pose(restored, original, "the restored native root")
    same_vector(restored_scale, scale, 0.001, "the restored native scale")
    same_pose(actor.transform(owner), original, "the restored Engine actor pose")
    if body then same_pose(entity.pose(owner), body, "the restored gameplay body") end
    log_pose("restored", owner, restored, restored_scale)
end
local function ordinary_set(list, family)
    for _, candidate in ipairs(list) do
        local owner = candidate.owner
        local before, scale = engine_pose(owner)
        if before then
            local target = changed(before, 17)
            -- A refused native move may still have changed state. Always try to restore it.
            local ok, result, reason = pcall(actor.set_transform, owner, target)
            if not ok then
                actor.set_transform(owner, before)
                error(result)
            end
            if result then
                local ran, problem = pcall(function()
                    local after, after_scale = engine_pose(owner)
                    same_pose(after, target, "the moved native " .. family .. " root")
                    same_pose(actor.transform(owner), target, "the moved Engine actor root")
                    same_vector(after_scale, scale, 0.001, "the native scale is unchanged")
                    check.expect(math.abs(target.location.x-before.location.x) >= 2,
                        "the setter changed the requested location")
                    check.expect(math.abs(difference(target.rotation.yaw,before.rotation.yaw)) >= 17,
                        "the setter changed the requested rotation")
                    check.result("invalid", "the moved owner is ordinary", entity.kind(owner))
                    log_pose("before-" .. family, owner, before, scale)
                    log_pose("moved-" .. family, owner, after, after_scale)
                end)
                restore(owner, before, scale)
                if not ran then error(problem) end
                return tag(owner)
            end
            local key = tostring(owner)
            if not refused[key] then
                refused[key] = true
                mod.log("setter-refused " .. tag(owner) .. " result=" .. tostring(reason))
            end
            -- Static roots can refuse a move. This is a donor refusal, not passing setter evidence.
            actor.set_transform(owner, before)
            local again, again_scale = engine_pose(owner)
            if not again then check.expect(false, "a refused setter lost its native root") return end
            same_pose(again, before, "the refused setter left the root unchanged")
            same_vector(again_scale, scale, 0.001, "the refused setter left scale unchanged")
        end
    end
    wait_for("an ordinary " .. family .. " owner that accepts a location and rotation change")
end
local function character(list)
    local candidates = {}
    for index=0,(player.count() or 0)-1 do
        local owner = player.captain(index)
        if owner then candidates[#candidates+1] = owner end
    end
    local base = class("GPikminBase_C")
    if base then
        for _, owner in ipairs(list) do
            if reflection.is_a(owner,base) == true then candidates[#candidates+1] = owner end
        end
    end
    for _, owner in ipairs(candidates) do
        local root, scale = engine_pose(owner)
        local body = entity.pose(owner)
        if root and body and math.abs(root.rotation.pitch) < 70
            and math.abs(difference(root.rotation.yaw,body.rotation.yaw)) > 2 then
            return owner, root, scale, body
        end
    end
end

check.retry(function()
    if game.has_control() ~= true then
        for _, name in ipairs(names) do run(name, function() wait_for("a loaded game with player control") end) end
    else
        local all = actor.list()
        if not all or #all == 0 then
            for _, name in ipairs(names) do run(name, function() wait_for("the Engine actor world list") end) end
        else
            run("world_list", function()
                local every = check.result("ok", "the Actor class filter", actor.list(class("Actor")))
                check.equal(#every, #all, "the Actor filter covers the world list")
                for _, owner in ipairs(all) do
                    check.equal(actor.alive(owner), true, "a listed Actor is alive")
                    check.expect(member(every,owner), "the class list contains each world Actor")
                end
                local entities = check.result("ok", "the preserved gameplay list", entity.list())
                check.expect(entities and #entities > 0, "the world has gameplay actors")
                for _, owner in ipairs(entities or {}) do
                    check.expect(member(all,owner), "a gameplay Actor has the same handle in the Engine list")
                end
                check.result("invalid", "an Actor passed as a class", actor.list(all[1]))
                check.result("invalid", "a component class", actor.list(class("SceneComponent")))
                return "world=" .. tostring(game.world()) .. " engine=" .. #all .. " entity=" .. #entities
            end)
            for _, family in ipairs({"scenery","light"}) do
                local selected = {}
                if not (finished[family .. "_owner"] and finished[family .. "_root"]
                    and finished[family .. "_set_restore"] and finished["entity_refuses_" .. family]) then
                    selected = donors(all, family)
                end
                local candidate, pose, scale = first_ready(selected)
                run(family .. "_owner", function()
                    if not candidate then wait_for("an ordinary " .. family .. " Actor with its own component") return end
                    owner_check(candidate,all,family .. "-owner")
                    return tag(candidate.owner)
                end)
                run(family .. "_root", function()
                    if not candidate then wait_for("an ordinary " .. family .. " native root pose") return end
                    same_pose(check.result("ok", "the Engine actor root pose", actor.transform(candidate.owner)),pose,
                        "the independent native root")
                    log_pose(family .. "-root",candidate.owner,pose,scale)
                    return tag(candidate.owner)
                end)
                run("entity_refuses_" .. family, function()
                    if not candidate then wait_for("an ordinary " .. family .. " owner for gameplay refusal") return end
                    reject_entity(candidate.owner,pose)
                    same_pose(engine_pose(candidate.owner),pose,"the root after gameplay refusals")
                    return tag(candidate.owner)
                end)
                run(family .. "_set_restore",function() return ordinary_set(selected,family) end)
            end
            local owner, root, scale, body
            if not (finished.character_divergence and finished.character_entity_setter
                and finished.character_actor_setter) then
                owner, root, scale, body = character(entity.list() or {})
            end
            run("character_divergence",function()
                if not owner then wait_for("a captain or Pikmin with distinct root and body yaw") return end
                same_pose(actor.transform(owner),root,"the character's independent native root")
                check.expect(math.abs(difference(root.rotation.yaw,body.rotation.yaw)) > 2,
                    "the root and gameplay body have distinct yaw")
                log_pose("character-root",owner,root,scale)
                log_pose("character-body",owner,body,scale)
                return tag(owner)
            end)
            run("character_entity_setter",function()
                if not owner then wait_for("a character with separate body facing") return end
                local target = changed(body,37)
                local ran, problem = pcall(function()
                    check.result("ok","set the gameplay body's pose",entity.set_pose(owner,target))
                    local after, after_scale = engine_pose(owner)
                    same_vector(after and after.location,target.location,0.1,"the native root location follows the gameplay setter")
                    -- This candidate has a separate facing component. The preserved setter
                    -- retains the root quaternion before moving, then turns that body component.
                    same_rotation(after and after.rotation,root.rotation,"the native root keeps its own rotation")
                    same_pose(entity.pose(owner),target,"the gameplay body follows its setter")
                    same_vector(after_scale,scale,0.001,"the native scale stays unchanged")
                    log_pose("entity-setter-root",owner,after,after_scale)
                    log_pose("entity-setter-body",owner,entity.pose(owner),after_scale)
                end)
                restore(owner,root,scale,body)
                if not ran then error(problem) end
                return tag(owner)
            end)
            run("character_actor_setter",function()
                if not owner then wait_for("a character with separate body facing") return end
                -- Pick a root yaw that also differs from the body's preexisting yaw.
                local turn = math.abs(difference(root.rotation.yaw+19,body.rotation.yaw)) > 2 and 19 or -19
                local target = changed(root,turn)
                local ran, problem = pcall(function()
                    check.result("ok","set the character's Engine root pose",actor.set_transform(owner,target))
                    local after, after_scale = engine_pose(owner)
                    same_pose(after,target,"the independent native root follows the Engine setter")
                    same_pose(actor.transform(owner),target,"the Engine pose follows its setter")
                    same_vector(after_scale,scale,0.001,"the native scale stays unchanged")
                    local after_body = entity.pose(owner)
                    check.expect(after_body ~= nil,"the gameplay body is readable after the root setter")
                    log_pose("actor-setter-root",owner,after,after_scale)
                    log_pose("actor-setter-body",owner,after_body,after_scale)
                end)
                restore(owner,root,scale,body)
                if not ran then error(problem) end
                return tag(owner)
            end)
        end
    end
    for _, name in ipairs(names) do if not finished[name] then return "waiting" end end
    if clock then clock:cancel() end
    return "pass"
end,30)
