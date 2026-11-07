local state, step, callback, stopped, lines = 0, 0, nil, false, {}
local function log(text) lines[#lines+1] = text end
mod = {log=log}
time = {on_step=function(fn) callback=fn return {cancel=function() stopped=true end} end}
game = {mode=function() return state == 0 and "title" or "story" end,
    world=function() if state == 0 then return nil end return "Area011_P" end,
    has_control=function() return state ~= 0 end}
player = {count=function() return state == 0 and 0 or 1 end,
    captain=function() return "captain" end, dog=function() return "dog" end,
    character=function() return state == 1 and "dog" or "captain" end}
entity = {kind=function(actor) return actor == "captain" and "HERO" or "MOSSDOG" end}
reflection = {find_class=function() if state == 0 then return nil, "not_found" end return "AvatarComponent" end,
    class_of=function(actor) return actor == "captain" and "GHero_C" or actor == "dog" and "GMoss_C" or state == 1 and "HeroPart" or "AvatarComponent" end,
    name=function(class) return class end,
    is_a=function(part, class) return state == 2 and part == "part" and class == "AvatarComponent" end}
component = {list=function() return {"part"} end}
captain = {cursor=function() if state == 1 then return nil,"not_found" end return {x=1,y=2,z=3} end,
    whistle=function() if state == 1 then return nil,"not_found" end return {active=false} end,
    riding=function() if state == 1 then return "dog" end return nil end}
squad = {members=function(actor) if actor == "dog" and state == 1 or actor == "captain" and state == 2 then return {"red","blue","yellow"} end return {} end}
dofile("tests/routes/captain/main.lua")
callback()
state=1
for i=2,30 do callback() end
state=2
for i=31,18000 do callback() end
local all=table.concat(lines,"\n")
for _, wanted in ipairs({"route captain mode title", "player0-captain HERO class=GHero_C", "player0-dog MOSSDOG class=GMoss_C", "player0-cursor not_found", "player0-whistle not_found", "player0-avatar-parts 0", "player0-dog-squad ok count=3", "player0-cursor ok", "player0-avatar-parts 1", "player0-captain-squad ok count=3", "diagnostic complete"}) do
    assert(all:find(wanted,1,true), wanted)
end
assert(stopped and #lines < 60)
return "mounted and missing captain data reported read-only"
