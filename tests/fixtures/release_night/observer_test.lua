local callback, defeated, unloaded, loaded, stage_changed
local logs, stored, present, live = {}, "", {"larva", "pot"}, true
require = function() return {mark=1,larva=8845280987219195551,pot=7633535673922496013} end
time={on_step=function(fn) callback=fn end}
game={world=function() return live and "Area001_P" or "Title_P" end,mode=function() return "story" end,
 stage=function() return live and "Story/StoryInGame" or "Title/MenuTitle" end,has_control=function() return live end,
 on_world_unloaded=function(fn) unloaded=fn end,on_world_loaded=function(fn) loaded=fn end,
 on_stage_changed=function(fn) stage_changed=fn end}
mod={version_of=function() return nil,"not_found" end}
player={captain=function() return live and "captain" or nil end}
actor=setmetatable({}, {__index=function() error("night gameplay must use entity") end})
entity={list=function() return present end,placement=function(one)
 if one=="larva" then return 8845280987219195551 else return 7633535673922496013 end end,
 pose=function() return {location={x=-377,y=1371,z=109}} end,
 kind=function(one) if not live then return nil,"not_found" end return "BABY" end}
combat={life=function() return {life=10} end,on_defeated=function(fn) defeated=fn end}
creature={defeated=function() return false end}
reflection={name=function(one) return one end,class_of=function() return "class" end}
reflection.path=function() return "/Game/Carrot4/Placeables/Teki/GBaby.GBaby_C" end
storage={write=function(_,text) stored=text return true end}
print=function(text) logs[#logs+1]=text end
dofile("tests/fixtures/release_night/release.t3.night.observer/main.lua")
stage_changed({previous="Story/StoryInGame/DayEnd",stage="Story/StoryInGame/DayEnd/Save"})
assert(stored:find("0 stage changed previous Story/StoryInGame/DayEnd stage Story/StoryInGame/DayEnd/Save",1,true))
assert(not stored:find("state world",1,true) and not stored:find("PASS",1,true))
-- Stage logging writes immediately without changing the 60-frame placement sampling.
for _=1,59 do callback() end
assert(not stored:find("state world",1,true))
callback()
assert(stored:find("present larva true pot true",1,true))
assert(not stored:find("larva defeated event",1,true))
-- Disappearance is not a native defeat event.
present={"pot"}
for _=1,60 do callback() end
assert(stored:find("present larva false pot true",1,true))
assert(not stored:find("larva defeated event",1,true))
defeated({actor="pot",previous=10,life=0})
assert(not table.concat(logs,"\n"):find("larva defeated event",1,true))
defeated({actor="larva",previous=10,life=0})
assert(table.concat(logs,"\n"):find("larva defeated event previous 10 life 0",1,true))
live=false;present={}
unloaded({world="Area001_P"});loaded({world="Title_P"})
for _=1,60 do callback() end
assert(stored:find("held handle ended 8845280987219195551 error not_found",1,true))
-- More than 1200 observations keep a bounded retained log.
for _=1,40000 do callback() end
local count=0 for _ in stored:gmatch("\n") do count=count+1 end
assert(count<=1200 and not stored:find("PASS",1,true))
-- A burst of stage events uses the same bound and keeps the final observed fields.
for n=1,1205 do stage_changed({previous="old"..n,stage="new"..n}) end
count=0 for _ in stored:gmatch("\n") do count=count+1 end
assert(count==1200 and stored:find("stage changed previous old1205 stage new1205",1,true))
assert(not stored:find("stage changed previous old1 stage new1\n",1,true) and not stored:find("PASS",1,true))
return "night observer separates absence, defeat and handle expiry"
