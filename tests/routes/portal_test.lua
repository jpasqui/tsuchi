local callback, cancelled, saved, logs
local world, controlled, travel_calls, leave_calls, entry_calls, warps, destination
local function reset(initial)
    callback, cancelled, saved, logs = nil, false, "", {}
    world, controlled, travel_calls, leave_calls, entry_calls, warps = initial, false, 0, 0, 0, 0
    destination = "Cave001_F00"
    time = {on_step = function(fn) callback = fn return {cancel = function() cancelled = true end} end}
    mod = {log = function(text) logs[#logs + 1] = text end}
    local storage_calls = 0
    storage = {write = function(name, data)
        assert(name == "log") storage_calls = storage_calls + 1
        if storage_calls == 1 then return nil, "busy" end
        saved = data return true
    end}
    game = {world = function() return world end, has_control = function() return controlled end}
    entity = {list = function() return {"tutorial", "wrong_floor", "entrance"} end,
        pose = function(subject) assert(subject == "entrance") return {location = {x = 1000, y = 2000, z = 300}} end,
        warp = function(subject, location, rotation)
            assert(subject == "leader" or subject == "dog")
            assert(location.x == 800 and location.y == 2000 and location.z == 330 and rotation.yaw == 0)
            warps = warps + 1 return true
        end}
    player = {captain = function() return "leader" end, dog = function() return "dog" end}
    captain = {dismount = function(subject) assert(subject == "leader") return true end}
    reflection = {find_class = function(name) assert(name == "PortalTriggerComponent") return "trigger" end,
        call = function(subject, name, args) assert(name == "GetComponentByClass" and args.ComponentClass == "trigger") return subject end,
        get = function(subject, field)
            assert(field == "ToLevelName")
            if subject == "tutorial" then return "Cave000_F00" end
            if subject == "wrong_floor" then return "Cave001_F01" end
            return destination
        end, name = function(subject) return subject end}
    story = {travel = function(area, base)
        assert(controlled and area == "Area001_P" and base == 0)
        travel_calls = travel_calls + 1
        if travel_calls == 1 then return nil, "busy" end
        return true
    end, leave_cave = function()
        assert(world == "Cave001_F00_P" and controlled)
        leave_calls = leave_calls + 1
        if leave_calls == 1 then return nil, "busy" end
        return true
    end, enter_portal = function(subject)
        assert(subject == "entrance" and world == "Area001_P" and controlled)
        entry_calls = entry_calls + 1
        if entry_calls == 1 then return nil, "busy" end
        return true
    end}
    dofile("tests/routes/portal/main.lua")
end
local function frames(count) for _ = 1, count do if not cancelled then callback() end end end
local function until_log(text, limit)
    for _ = 1, (limit or 1200) / 15 do
        frames(15)
        if saved:find(text, 1, true) then return end
    end
    error("missing receipt: " .. text .. "\n" .. saved)
end
-- Day-area setup cannot count as cave entry or departure.
reset("Area011_P") frames(2500)
assert(travel_calls == 0 and entry_calls == 0 and leave_calls == 0)
controlled = true frames(90) assert(travel_calls == 0)
controlled = false frames(30) assert(travel_calls == 0)
controlled = true until_log("phase setup_arrival")
assert(travel_calls == 2 and entry_calls == 0)
frames(180) assert(entry_calls == 0 and leave_calls == 0)
world, controlled = "Area001_P", false frames(180) assert(entry_calls == 0)
controlled = true until_log("phase entering")
assert(entry_calls == 2 and warps == 2 and leave_calls == 0)
assert(saved:find("selected entrance destination=Cave001_F00", 1, true))
frames(300) assert(leave_calls == 0 and not saved:find("RESULT PASS", 1, true))
-- Accepted entry, a different cave, and a momentary control frame cannot prove arrival.
world, controlled = "Cave000_F00_P", true frames(300) assert(leave_calls == 0)
world, controlled = "Cave001_F00_P", false frames(300) assert(leave_calls == 0)
controlled = true frames(90) assert(leave_calls == 0)
controlled = false frames(30)
controlled = true until_log("phase departure_request") assert(leave_calls == 0)
controlled = false frames(660) assert(leave_calls == 0)
controlled = true until_log("phase leaving") assert(leave_calls == 2)
frames(120) assert(not saved:find("RESULT PASS", 1, true))
world, controlled = "Area001_P", false frames(120) assert(not cancelled)
controlled = true until_log("RESULT PASS portal entry and cave departure") frames(30)
assert(cancelled and #logs < 45)
for _, text in ipairs({"request travel setup true nil", "observed setup arrival world=Area001_P control=true",
    "awaiting native confirmation destination=Cave001_F00_P", "observed portal arrival world=Cave001_F00_P control=true",
    "request leave_cave true nil", "observed cave departure world=Area001_P control=true"}) do
    assert(saved:find(text, 1, true), text)
end
-- Starting directly outside skips setup. No matching real portal means no entry request.
reset("Area001_P") controlled = true destination = "Cave099_F00"
frames(9000) assert(cancelled and travel_calls == 0 and entry_calls == 0 and leave_calls == 0)
assert(saved:find("FAIL timeout phase=find_entrance", 1, true))
-- No accepted setup request alone can trigger entry while control is absent.
reset("Area007_P") controlled = true until_log("phase setup_arrival", 3000)
world, controlled = "Area001_P", false frames(6200)
assert(cancelled and entry_calls == 0 and leave_calls == 0 and not saved:find("RESULT PASS", 1, true))
-- An unrelated world or an initial cave is refused before any route API request.
for _, initial in ipairs({"Challenge_P", "Cave003_F00_P"}) do
    reset(initial) controlled = true frames(2595)
    assert(cancelled and saved:find("RESULT FAIL unexpected initial world=" .. initial, 1, true))
    assert(travel_calls == 0 and entry_calls == 0 and leave_calls == 0)
end
return "real portal selection and asynchronous departure observations pass"
