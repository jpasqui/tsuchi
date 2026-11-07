local actor_api = entity
local component_api = component

-- Holds a mesh's existing material unchanged, then releases and reads it independently.
local check = require("check")
check.plan("found", "held", "read", "restored", "ended", "refused")
local chosen, original
check.retry(function()
    if game.has_control() ~= true then return check.run("found", function() check.wait("a loaded game with player control") end) end
    if not chosen then
        local listed = actor_api.list()
        if not listed then return check.run("found", function() check.wait("a loaded world") end) end
        local mesh_class = reflection.find_class("MeshComponent")
        if mesh_class then
            for _, actor in ipairs(listed) do
                for _, part in ipairs(component_api.list(actor) or {}) do
                    if reflection.is_a(part, mesh_class) then
                        local material = component_api.material(part, 0)
                        if material then chosen, original = part, material break end
                    end
                end
                if chosen then break end
            end
        end
    end
    if not chosen then return check.run("found", function() check.wait("a mesh in the world with a material in slot zero") end) end
    local state = check.run("found", function()
        check.equal(check.result("ok", "reading the material again", component_api.material(chosen, 0)), original, "the existing material")
        check.expect(reflection.is_a(original, check.result("ok", "the material base class", reflection.find_class("MaterialInterface"))), "the object is a material")
    end)
    local held = false
    check.run("held", function()
        held = check.result("ok", "holding the same material", component_api.hold_material(chosen, 0, original))
    end)
    check.run("read", function()
        check.equal(check.result("ok", "the held slot", component_api.material(chosen, 0)), original, "holding it left its material unchanged")
        check.equal(check.result("ok", "the engine's material", reflection.call(chosen, "GetMaterial", {ElementIndex = 0})), original, "the engine also sees the original material")
    end)
    check.run("restored", function()
        if not held then check.expect(false, "the material hold must have started") return end
        check.result("ok", "ending the material hold", component_api.release_material(chosen, 0))
        check.equal(check.result("ok", "the restored slot", component_api.material(chosen, 0)), original, "the game's original material")
    end)
    check.run("ended", function()
        check.result("not_found", "ending the same hold twice", component_api.release_material(chosen, 0))
    end)
    check.run("refused", function()
        check.result("invalid", "a slot outside the mesh", component_api.material(chosen, 0xFFFFFFFF))
        check.result("invalid", "an object that is not a material", component_api.hold_material(chosen, 0, chosen))
    end)
    if held then component_api.release_material(chosen, 0) end
    return state
end)
