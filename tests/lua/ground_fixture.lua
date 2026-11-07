-- Exercise the game check with a completed contact before vertical speed settles.
local callback, velocity, queries, launches = nil, -163, 0, 0
local completed, problems = {}, {}
local checker = {}
function checker.plan() end
function checker.wait() end
function checker.expect(good, why) assert(good, why) end
function checker.equal(got, expected, why) assert(got == expected, why) end
function checker.near(got, expected, slack, why) assert(math.abs(got-expected) <= slack, why) end
function checker.result(expected, label, got, why)
    if expected == "ok" then assert(got ~= nil and why == nil, label)
    else assert(got == nil and why == expected, label) end
    return got
end
function checker.run(name, body)
    local good, why = pcall(body)
    if not good then problems[#problems+1] = name .. ": " .. tostring(why) end
    completed[name] = good
    return good and "pass" or "fail"
end
require = function(name) assert(name == "check"); return checker end
mod = {log=function() end}
game = {has_control=function() return true end, paused=function() return {} end}
player = {captain=function() return "captain" end}
actor = {alive=function() return true end}
asset = {load=function() return "class" end}
local brain_tick = true
entity = {
    pose=function() return {location={x=0,y=0,z=0},rotation={pitch=0,yaw=0,roll=0}} end,
    velocity=function() return {x=0,y=0,z=velocity} end,
    spawn=function() return "owned" end,
    hold_suspended=function() return true end,
    suspended=function() return {movement=true} end,
    grounded=function()
        queries=queries+1
        if queries==1 or launches>0 then return nil,"not_ready" end
        return true
    end,
    launch=function() launches=launches+1;return true end,
}
component = {owner=function() return "owned" end,list=function() return {"body"} end}
reflection = {
    get=function() return "brain" end,
    call=function(_, name, args)
        if name=="IsComponentTickEnabled" then return brain_tick end
        if name=="SetComponentTickEnabled" then brain_tick=args.bEnabled;return end
        if name=="IsSimulatingPhysics" then return true end
        error("unexpected call " .. name)
    end,
}
time = {on_step=function(fn) callback=fn;return {cancel=function() end} end}

-- FIXTURE HERE

for _=1,33 do callback() end
assert(completed.first == true, "the first contact query was exercised")
assert(completed.standing == nil, "a contact during impact is not the settled standing baseline")
assert(launches == 0, "the fixture must wait before launching from the floor baseline")
velocity=0
callback()
assert(completed.standing == true, "the settled supported sample starts the flight")
assert(launches == 1, "the fixture launches once after settling")
assert(#problems == 0, table.concat(problems,"; "))
return "ground fixture waits for a settled supported sample"
