local file_api = file
local save_api = save

-- Reads the story clock, then checks explicitly selected original story transitions.
local check = require("check")
check.plan("day", "clock", "refused", "treasure", "travel", "arrived", "portal", "entered", "leave", "left", "released")
local basic_day, basic_clock = false, false
check.retry(function()
    if game.has_control() ~= true then return check.run("day", function() check.wait("a loaded story with player control") end) end
    local state = check.run("day", function()
        local day, why = story.day()
        if why == "not_ready" then check.wait("a loaded story") end
        check.expect(check.result("ok", "the day", day, why) >= 1, "the story counts days from one")
    end)
    basic_day = state == "pass"
    return state
end)
check.retry(function()
    if game.has_control() ~= true then return check.run("clock", function() check.wait("a loaded story with player control") end) end
    local before, why = story.clock()
    if why == "not_ready" then return check.run("clock", function() check.wait("a story level with a clock") end) end
    local state = check.run("clock", function()
        check.result("ok", "the clock", before, why)
        check.expect(before >= 0 and before <= 1, "the clock is a part of a day")
        check.result("ok", "putting the clock back", story.set_clock(before))
        check.near(story.clock(), before, 0.0001, "the clock on the same step")
    end)
    check.run("refused", function()
        check.result("invalid", "a clock below its start", story.set_clock(-0.1))
        check.result("invalid", "a clock above its end", story.set_clock(1.1))
        check.near(story.clock(), before, 0.0001, "the refusals left the clock alone")
    end)
    basic_clock = state == "pass"
    return state
end)


-- This existing group is explicitly selected on an isolated story save.
-- Empty portal targets leave the positive checks visibly waiting until real targets are supplied.
local lines = {}
for line in ((file_api.read("targets.txt", 0, 1024) or "") .. "\n"):gmatch("([^\n]*)\n") do lines[#lines+1] = line:gsub("\r$", "") end
local primary, alternate, portal_class, cave_world, treasure = table.unpack(lines, 1, 5)
local phase, age, destination, day_before, old_world = 0, 0
local portal_actor, save_held, stepping
local function waiting(name, why) check.run(name, function() check.wait(why) end) end
local function release()
    check.run("released", function()
        if save_held then
            check.result("ok", "releasing the save hold", save_api.release())
            check.result("not_found", "releasing it twice", save_api.release())
            save_held = false
        end
    end)
    stepping:cancel()
end
stepping = time.on_step(function()
    age = age + 1
    if phase == 0 then
        if not basic_day or not basic_clock or game.has_control() ~= true then return end
        old_world, day_before = game.world(), story.day()
        destination = old_world == primary and alternate or primary
        if not destination or destination == "" then return waiting("travel", "a different original area named in targets.txt") end
        local state = check.run("treasure", function()
            save_held = check.result("ok", "holding saves for the journey", save_api.hold()) == true
            local before = check.result("ok", "the original treasure count", story.treasure_count(treasure))
            check.result("not_found", "an absent treasure count", story.treasure_count("TsuchiNoSuchTreasure"))
            check.result("ok", "recording one original treasure without announcement", story.give_treasure(treasure, false))
            check.equal(story.treasure_count(treasure), before + 1, "exactly one collection was recorded")
            check.result("not_found", "an absent treasure delivery", story.give_treasure("TsuchiNoSuchTreasure", false))
        end)
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 10, 0
        return
    end
    if phase == 10 then
        local state = check.run("travel", function()
            if not save_held then save_held = check.result("ok", "holding saves for the journey", save_api.hold()) == true end
            check.result("invalid", "an invalid destination", story.travel("no such area"))
            check.result("not_found", "an absent area", story.travel("Area1000_P"))
            check.result("not_ready", "leaving a cave above ground", story.leave_cave())
            local result, why = story.travel(destination)
            if why == "busy" or why == "not_ready" then check.wait("ordinary daytime story travel readiness") end
            check.result("ok", "requesting the original destination", result, why)
            check.result("busy", "requesting travel twice", story.travel(destination))
        end)
        if state == "waiting" then return end
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 1, 0
        return
    end
    if phase == 1 then
        if (game.world() ~= destination or game.has_control() ~= true) and age < 5400 then return end
        local state = check.run("arrived", function()
            check.equal(game.world(), destination, "the requested area actually loaded")
            check.equal(game.has_control(), true, "ordinary play resumed there")
            check.equal(story.day(), day_before, "travel did not advance the day")
        end)
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 2, 0
        return
    end
    if phase == 2 then
        if not portal_class or portal_class == "" or not cave_world or cave_world == "" then
            waiting("portal", "an original portal actor class and its cave world in targets.txt")
            waiting("entered", "the original cave-mouth target")
            waiting("leave", "a successful original cave entry")
            waiting("left", "a successful original cave exit")
            release()
            phase = 6
            return
        end
        local class = reflection.find_class(portal_class)
        local actors_of_class = class and entity.list(class) or {}
        portal_actor = actors_of_class and actors_of_class[1]
        if not portal_actor then return waiting("portal", "the original named cave-mouth actor in the destination") end
        local result, why = story.enter_portal(portal_actor)
        if why == "busy" or why == "not_ready" then return waiting("portal", "ordinary cave-mouth confirmation readiness") end
        local state = check.run("portal", function()
            check.result("ok", "requesting the actual cave mouth", result, why)
            check.result("busy", "requesting the portal twice", story.enter_portal(portal_actor))
        end)
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 3, 0
        return
    end
    if phase == 3 then
        if (game.world() ~= cave_world or game.has_control() ~= true) and age < 5400 then return end
        local state = check.run("entered", function()
            check.equal(game.world(), cave_world, "the original cave floor actually loaded")
            check.equal(game.has_control(), true, "ordinary cave play resumed")
        end)
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 4, 0
        return
    end
    if phase == 4 then
        local result, why = story.leave_cave()
        if why == "busy" or why == "not_ready" then return waiting("leave", "ordinary loaded-cave exit readiness") end
        local state = check.run("leave", function()
            check.result("ok", "asking to leave the actual cave", result, why)
            check.result("busy", "requesting departure twice", story.leave_cave())
        end)
        if state ~= "pass" then release() phase = 6 return end
        phase, age = 5, 0
        return
    end
    if phase == 5 then
        if (game.world() ~= destination or game.has_control() ~= true) and age < 5400 then return end
        check.run("left", function()
            check.equal(game.world(), destination, "the original area actually loaded after leaving")
            check.equal(game.has_control(), true, "ordinary play resumed outside the cave")
        end)
        release()
        phase = 6
    end
end)
