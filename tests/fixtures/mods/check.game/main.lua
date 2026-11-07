-- Reads the running game and puts its random stream back without advancing it.
local check = require("check")
check.plan("where", "control", "random", "refused", "listened", "title", "events", "cancelled")
local phase, age, asked = 0, 0, false
local before_mode, before_stage, before_world
local modes, stages, loaded, unloaded, listeners = {}, {}, {}, {}, {}
local stepping
stepping = time.on_step(function()
    age = age + 1
    if phase ~= 0 then
        if phase == 1 then
            if age < 2 then return end
            phase, age = 2, 0
        end
        if phase == 2 then
            if not asked then
                local result, why = game.return_to_title()
                if why == "busy" or why == "not_ready" then
                    if age < 1800 then return check.run("title", function() check.wait("ordinary story exit readiness") end) end
                    check.run("title", function() check.result("ok", "requesting the title", nil, why) end)
                    phase = 3
                else
                    asked = check.run("title", function() check.result("ok", "requesting the title", result, why) end) == "pass"
                    phase, age = 3, 0
                end
            end
        elseif phase == 3 then
            if (game.mode() ~= "title" or #modes == 0 or #unloaded == 0 or #loaded == 0) and age < 5400 then return end
            check.run("events", function()
                check.equal(game.mode(), "title", "the requested title mode actually arrived")
                check.expect(#modes > 0 and modes[1].previous == before_mode and modes[#modes].mode == "title", "the mode change names its old and new modes")
                check.expect(#stages > 0 and stages[1].previous == before_stage and stages[#stages].stage == game.stage(), "the stage changes lead to the actual title stage")
                local departed = false
                for _, event in ipairs(unloaded) do if event.world == before_world then departed = true end end
                check.expect(departed, "the original world was reported unloaded")
                check.expect(#loaded > 0 and loaded[#loaded].world == game.world() and loaded[#loaded].world ~= before_world, "the loaded event names the actual new world")
                check.expect(game.has_control() ~= true, "the title gives no gameplay control")
            end)
            check.run("cancelled", function()
                for _, listener in ipairs(listeners) do
                    check.result("ok", "ending a game event listener", listener:cancel())
                    check.result("not_found", "ending it twice", listener:cancel())
                end
            end)
            stepping:cancel()
            phase = 4
        end
        return
    end
    if game.has_control() ~= true then return check.run("where", function() check.wait("a loaded game with player control") end) end
    check.run("where", function()
        local mode = check.result("ok", "the mode", game.mode())
        check.expect(mode ~= "none" and mode ~= "title", "a mode of play is running")
        check.expect(math.type(check.result("ok", "the rule", game.rule())) == "integer", "the rule is an integer")
        check.expect(#check.result("ok", "the stages", game.stage()) > 0, "a stage is running")
        check.expect(#check.result("ok", "the world", game.world()) > 0, "a world is loaded")
    end)
    check.run("control", function()
        check.expect(type(check.result("ok", "the paused kinds", game.paused())) == "table", "pause kinds are a set")
        check.expect(type(check.result("ok", "whether the player has control", game.has_control())) == "boolean", "control is a boolean")
    end)
    check.run("random", function()
        local before = check.result("ok", "the random stream", game.random())
        check.expect((before.a | before.b | before.c | before.d) ~= 0, "its state is not all zero")
        check.result("ok", "putting the state back", game.set_random(before))
        local after = check.result("ok", "reading it back", game.random())
        for _, field in ipairs({"a", "b", "c", "d"}) do check.equal(after[field], before[field], "the state " .. field) end
    end)
    check.run("refused", function()
        check.result("invalid", "an all-zero random state", game.set_random({a = 0, b = 0, c = 0, d = 0}))
        check.result("invalid", "holding no pause kinds", game.hold_pause({}))
    end)
    before_mode, before_stage, before_world = game.mode(), game.stage(), game.world()
    check.run("listened", function()
        listeners[1] = check.result("ok", "listening for modes", game.on_mode_changed(function(event) modes[#modes+1] = event end))
        listeners[2] = check.result("ok", "listening for stages", game.on_stage_changed(function(event) stages[#stages+1] = event end))
        listeners[3] = check.result("ok", "listening for loaded worlds", game.on_world_loaded(function(event) loaded[#loaded+1] = event end))
        listeners[4] = check.result("ok", "listening for unloaded worlds", game.on_world_unloaded(function(event) unloaded[#unloaded+1] = event end))
    end)
    phase, age = 1, 0
end)
