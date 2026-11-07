local actor_api = entity
local engine_actor = actor
local file_api = file

-- Optional isolated setup. The runner writes finish.txt only after reading the animation receipt.
local marker_ready=false
local target = {}
for line in (file_api.read("targets.txt", 0, 1024) or ""):gmatch("[^\r\n]+") do target[#target + 1] = line end
local path, waiting_limit, keeping_limit = target[1], tonumber(target[2]), tonumber(target[3])
local class, made, stepping, phase, steps, born, told = nil, nil, nil, "waiting", 0, 0, {}
local cleanup_started
local function say(state, note)
    if not told[state] then mod.log("route chappy " .. state .. (note and " " .. tostring(note) or "")) told[state] = true end
end
local function release_class()
    if class then
        local okay, why = asset.release(class)
        say("class-released", okay and "ok" or why)
        class = nil
    end
end
local function finished(note)
    release_class()
    say("complete", note)
    if stepping then stepping:cancel() end
end
local function fail(note)
    say("failed", note)
    if not made then finished("no actor made") return end
    phase = "cleanup"
end
-- Reset a stale marker once before changing the world, through this mod's saved data.
local function prepare_marker()
    if marker_ready then return true end
    local written,why=storage.write("finish.txt","waitxx\n")
    if not written then say("refused-marker","write " .. tostring(why)) return false end
    local value,problem=storage.read("finish.txt",0,16)
    if value~="waitxx\n" then say("refused-marker","readback " .. tostring(problem or value)) return false end
    marker_ready=true
    say("marker-ready","save data finish.txt 7 bytes waitxx\\n")
    return true
end
if not path or not waiting_limit or not keeping_limit or waiting_limit < 1 or keeping_limit < 1 then
    say("failed", "invalid targets")
else
    stepping = time.on_step(function()
        steps = steps + 1
        if phase == "waiting" then
            if steps >= waiting_limit then fail("play or class readiness timed out") return end
            if game.has_control() ~= true then say("waiting", "player control") return end
            if not prepare_marker() then return end
            local captain = player.captain(0)
            local at = captain and actor_api.pose(captain)
            if not at then say("waiting-player", "the first captain's place") return end
            if not class then
                local loaded, why = asset.load(path)
                if not loaded then
                    if why == "not_ready" then say("waiting-class", path) return end
                    fail("class load " .. tostring(why)) return
                end
                class = loaded
                local actual, problem = reflection.path(class)
                if actual ~= path then fail("class identity " .. tostring(actual or problem)) return end
                say("class", actual)
            end
            local actor, why = actor_api.spawn(class, {x = at.location.x + 600, y = at.location.y, z = at.location.z + 20}, at.rotation, -1, false)
            if not actor then
                if why == "not_ready" then say("waiting-spawn", "world spawner") return end
                fail("spawn " .. tostring(why)) return
            end
            made, born = actor, steps
            if actor_api.kind(made) ~= "CHAPPY" or reflection.class_of(made) ~= class then fail("spawned actor identity") return end
            local held, problem = actor_api.hold_suspended(made, {logic = true, movement = true})
            if not held then fail("quiet setup " .. tostring(problem)) return end
            say("ready", "owned CHAPPY " .. path)
            phase = "keeping"
            return
        end
        if phase == "keeping" then
            if not engine_actor.alive(made) then finished("setup actor already left") return end
            local finish, marker_error = storage.read("finish.txt", 0, 16)
            if not told["finish-marker"] then say("finish-marker", finish and (#finish .. " bytes " .. finish:gsub("\n", "\\n")) or tostring(marker_error or "none")) end
            if finish == "finish\n" then
                say("finish-requested", "runner read the animation receipt")
                phase = "cleanup"
            elseif steps - born >= keeping_limit then
                say("timed-out", "runner did not request cleanup")
                phase = "cleanup"
            else return end
        end
        if phase == "cleanup" or phase == "removing" then
            cleanup_started = cleanup_started or steps
            if steps - cleanup_started >= 90 then
                say("failed", "cleanup timed out; the actor remains owned until the mod stops")
                release_class()
                stepping:cancel()
                return
            end
        end
        if phase == "cleanup" then
            if not engine_actor.alive(made) then finished("owned actor gone") return end
            local released, why = actor_api.release_suspended(made, {logic = true, movement = true})
            say("holds-released", released and "ok" or why)
            local removed, problem = actor_api.destroy(made)
            if removed or problem == "busy" then
                say("remove-requested", "owned actor only")
                phase = "removing"
            else fail("remove " .. tostring(problem)) end
            return
        end
        if not engine_actor.alive(made) then finished("owned actor gone") end
    end)
end
