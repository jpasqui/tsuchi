local actor_api = entity
local component_api = component

-- Lists an actor's parts, holds one hidden and restores it, then adds and removes a part of its own.
local check = require("check")
check.plan("listed", "owned", "found", "hidden", "restored", "added", "removed")
local owner, listed, scene, chosen
check.retry(function()
    if game.has_control() ~= true then return check.run("listed", function() check.wait("a loaded game with player control") end) end
    if not owner then
        scene = reflection.find_class("SceneComponent")
        for _, actor in ipairs(actor_api.list()) do
            local parts = component_api.list(actor)
            if parts and #parts > 0 then
                for _, part in ipairs(parts) do
                    if reflection.is_a(part, scene) and reflection.get(part, "bVisible") == true then
                        local held = component_api.hold_hidden(part, false)
                        if held then
                            component_api.release_hidden(part)
                            owner, listed, chosen = actor, parts, part
                            break
                        end
                    end
                end
            end
            if owner then break end
        end
    end
    if not owner then return check.run("listed", function() check.wait("an actor with parts in the world") end) end
    local state = check.run("listed", function()
        local again = check.result("ok", "listing its parts again", component_api.list(owner))
        check.equal(#again, #listed, "the count")
        for i, part in ipairs(listed) do check.equal(again[i], part, "the part at " .. i) end
    end)
    check.run("owned", function()
        for _, part in ipairs(listed) do check.equal(check.result("ok", "the part's owner", component_api.owner(part)), owner, "the actor it belongs to") end
    end)
    local part = chosen
    check.run("found", function()
        local name = check.result("ok", "its name", reflection.name(part))
        check.equal(check.result("ok", "finding the part by name", component_api.find(owner, name)), part, "the same part")
        check.equal(check.result("ok", "finding it without letter case", component_api.find(owner, name:lower())), part, "the same part")
        check.result("not_found", "a part the actor lacks", component_api.find(owner, "TsuchiMissingPart"))
    end)
    local before = reflection.get(part, "bVisible")
    local held = false
    check.run("hidden", function()
        check.equal(component_api.hidden(part), false, "no hold at the start")
        held = check.result("ok", "holding the part hidden", component_api.hold_hidden(part, false))
        check.equal(check.result("ok", "the hold", component_api.hidden(part)), true, "it is held hidden")
        check.equal(check.result("ok", "the engine's visibility", reflection.get(part, "bVisible")), false, "it is not drawn")
    end)
    check.run("restored", function()
        if not held then check.expect(false, "the hide did not succeed") return end
        check.result("ok", "ending the hold", component_api.release_hidden(part))
        check.equal(component_api.hidden(part), false, "the hold has ended")
        check.equal(reflection.get(part, "bVisible"), before, "the engine's visibility is restored")
        check.result("not_found", "ending it twice", component_api.release_hidden(part))
    end)
    local added
    check.run("added", function()
        added = check.result("ok", "adding a scene part", component_api.add(owner, scene, part))
        check.equal(component_api.owner(added), owner, "the added part's owner")
        check.equal(#component_api.list(owner), #listed + 1, "one part was added")
    end)
    check.run("removed", function()
        check.result("ok", "removing its added part", component_api.remove(added))
        check.equal(#component_api.list(owner), #listed, "the count is restored")
        check.result("not_found", "removing it twice", component_api.remove(added))
        check.result("not_found", "removing a part no mod added", component_api.remove(part))
    end)
    return state
end)
