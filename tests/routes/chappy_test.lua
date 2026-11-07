-- The route's public callbacks on a title screen, in play and through delayed removal.
local path = "/Game/Carrot4/Placeables/Teki/GChappy.GChappy_C"
local function run(scenario)
    local state = {play = false, marker = false, calls = {}, logs = {}, active = true, released = 0, spawned = 0, removed = 0}
    state.saved="finish\n" -- stale completion from a previous run must be reset
    state.storage_busy=scenario=="storage_busy"
    local class, captain, made = {}, {}, nil
    local function call(name) state.calls[#state.calls + 1] = name end
    file = {read = function(name)
        if name == "targets.txt" then return path .. "\n20\n10\n" end
        assert(name == "finish.txt")
        return state.marker and "finish\n" or nil, state.marker and nil or "not_found"
    end}
    storage = {
        write=function(name,value)
            assert(state.play and name=="finish.txt" and value=="waitxx\n")
            if state.storage_busy then return nil,"busy" end
            state.saved=value state.writes=(state.writes or 0)+1 return true
        end,
        read=function(name,offset,most)
            assert(state.play and name=="finish.txt" and offset==0 and most==16)
            return state.marker and "finish\n" or state.saved
        end
    }
    mod = {log = function(line) state.logs[#state.logs + 1] = line end}
    game = {has_control = function() return state.play end}
    time = {on_step = function(callback)
        state.callback = callback
        return {cancel = function() state.active = false return true end}
    end}
    player = {captain = function(number) assert(number == 0 and state.play) call("captain") return captain end}
    asset = {
        load = function(wanted) assert(wanted == path and state.play) call("class") return class end,
        release = function(value) assert(value == class) state.released = state.released + 1 return true end
    }
    reflection = {
        class_of = function(value) assert(value == made) return class end,
        path = function(value) assert(value == class) return scenario == "wrong_class" and "/Wrong.Class" or path end
    }
    entity = {
        pose = function(value) assert(value == captain) return {location = {x = 10, y = 20, z = 30}, rotation = {pitch = 0, yaw = 45, roll = 0}} end,
        spawn = function(value, place, facing, team, keep)
            assert(value == class and place.x == 610 and place.y == 20 and place.z == 50 and facing.yaw == 45 and team == -1 and keep == false)
            state.spawned = state.spawned + 1
            made = {alive = true}
            return made
        end,
        kind = function(value) assert(value == made) return "CHAPPY" end,
        hold_suspended = function(value, parts) assert(value == made and parts.logic and parts.movement and not parts.physics and not parts.collision) state.held = true return true end,
        release_suspended = function(value, parts) assert(value == made and parts.logic and parts.movement) state.held = false return true end,
        destroy = function(value)
            assert(value == made and state.held == false)
            state.removed = state.removed + 1
            made.queued = true
            return true
        end
    }
    actor = {alive = function(value) assert(value == made) return made.alive end}
    dofile("tests/routes/chappy/main.lua")
    local function step(number)
        for _ = 1, number do if state.active then state.callback() end end
    end
    step(3)
    assert(#state.calls == 0 and state.spawned == 0 and state.active) -- no world action at Title
    if scenario == "no_play" then
        step(20)
        assert(not state.active and state.spawned == 0 and state.released == 0)
    else
        state.play = true
        step(1)
        if scenario=="storage_busy" then
            assert(state.spawned==0 and state.released==0 and state.active)
            state.storage_busy=false
            step(1)
        end
        assert(state.saved=="waitxx\n" and state.writes==1)
        if scenario == "wrong_class" then
            assert(not state.active and state.spawned == 0 and state.released == 1)
        else
            assert(state.spawned == 1 and state.held and state.released == 0 and state.removed == 0)
            step(4) -- the animation copy's receipt has not been read yet
            assert(state.removed == 0 and state.held and state.released == 0)
            if scenario == "finished" or scenario=="storage_busy" then state.marker = true else step(10) end
            step(1)
            assert(state.removed == 1 and not state.held and state.released == 0 and made.alive and state.active)
            step(2)
            assert(state.released == 0 and state.removed == 1) -- queued removal is not yet gone
            made.alive = false
            step(1)
            assert(not state.active and state.released == 1 and state.removed == 1)
        end
    end
    local log = table.concat(state.logs, "\n")
    if scenario == "finished" or scenario=="storage_busy" then
        assert(log:find("route chappy finish-requested runner read the animation receipt", 1, true))
        assert(log:find("route chappy complete owned actor gone", 1, true))
        assert(not log:find("failed", 1, true) and not log:find("timed-out", 1, true))
    elseif scenario == "timeout" then
        assert(log:find("route chappy timed-out runner did not request cleanup", 1, true))
    else assert(log:find("route chappy failed", 1, true)) end
end
for _, scenario in ipairs({"finished", "timeout", "no_play", "wrong_class", "storage_busy"}) do run(scenario) end
return "optional Chappy route: Title, class identity, coordinated cleanup and timeouts pass\n"
