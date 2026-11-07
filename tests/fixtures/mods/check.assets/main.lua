local actor_api = entity
local asset_api = asset
local file_api = file

-- An asset loaded twice has one hold; its path can load it again.
local check = require("check")
check.plan("held", "path", "released", "refused", "nested", "fresh")
local targets = {}
for line in (file_api.read("targets.txt", 0, 1024)):gmatch("[^\r\n]+") do targets[#targets + 1] = line end
local path = targets[1]
local asset
check.retry(function()
    if game.has_control() ~= true then return check.run("held", function() check.wait("a loaded game with player control") end) end
    local loaded, why = asset_api.load(path)
    if why == "not_ready" then return check.run("held", function() check.wait("the game to load assets") end) end
    local state = check.run("held", function()
        asset = check.result("ok", "loading the named asset", loaded, why)
        check.equal(check.result("ok", "loading it twice", asset_api.load(path)), asset, "the same asset")
    end)
    check.run("path", function()
        local found = check.result("ok", "the asset's path", reflection.path(asset))
        check.equal(found, path, "the path used to load it")
        check.equal(check.result("ok", "loading its own path", asset_api.load(found)), asset, "the same asset")
    end)
    check.run("released", function()
        check.result("ok", "ending its one hold", asset_api.release(asset))
        check.result("not_found", "the second load added no hold", asset_api.release(asset))
    end)
    check.run("refused", function()
        check.result("not_found", "an asset that does not exist", asset_api.load("/No/Such.Asset"))
        check.result("invalid", "a name that is no path", asset_api.load("Nothing"))
    end)
    check.run("nested", function()
        local nested = 0
        for _, actor in ipairs(actor_api.list()) do
            local parts = component.list(actor)
            if parts then
                for _, part in ipairs(parts) do
                    local name = reflection.path(part)
                    if name and name:sub(1, 1) == "/" and name:find(":", 1, true) then nested = nested + 1 end
                end
            end
        end
        check.expect(nested > 0, "a part inside a level has a nested path")
    end)
    check.run("fresh", function()
        local fresh = check.result("ok", "loading the other class by its path", asset_api.load(targets[2]))
        check.equal(check.result("ok", "finding the class after the load", reflection.find_class(targets[3])), fresh, "the loaded class")
        check.equal(reflection.path(fresh), targets[2], "its path")
        check.result("ok", "releasing the other class", asset_api.release(fresh))
    end)
    return state
end)
