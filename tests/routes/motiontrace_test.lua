local callback,logs,cancelled=nil,{},false
time={on_step=function(fn) callback=fn return {cancel=function() cancelled=true end} end}
mod={log=function(text) logs[#logs+1]=text end}
local controlled=false
game={has_control=function() return controlled end,world=function() return "Area011_P" end,mode=function() return "story" end,stage=function() return "story_in_game" end}
player={captain=function() return "captain" end}
entity={list=function() return {"source","made"} end,kind=function() return "CHAPPY" end,placement=function(a) return a=="source" and 123 or 0 end,
    pose=function() return {location={x=10,y=20,z=30}} end,velocity=function() return {x=0,y=0,z=600} end}
component={list=function() return {"body"} end,name=function() return "PhysicsBody" end,owner=function() return "made" end}
reflection={name=function(object) return object=="body" and "PhysicsBody" or object end,class_of=function() return "class" end,call=function(object,name)
    if name=="IsSimulatingPhysics" then return true,{} end
    if name=="GetCollisionEnabled" then return 3,{} end
    if name=="K2_GetActorLocation" then return {X=10,Y=20,Z=30},{} end
end}
reflection.path=function() return "/Game/Stock.Stock_C" end
dofile("tests/routes/motiontrace/main.lua")
callback() assert(#logs==1 and logs[1]:find("waiting player control",1,true))
controlled=true
for _=1,500 do if not cancelled then callback() end end
assert(cancelled and #logs<600)
local joined=table.concat(logs,"\n")
for _,text in ipairs({"Area011_P","captain_position=10.000,20.000,30.000","name=source placement=123","name=made placement=0","simulating=true collision=3","velocity=0.000,0.000,600.000","engine=10.000,20.000,30.000","errors=nil,nil,ok","ended bounded observation"}) do assert(joined:find(text,1,true),text) end
return "motion trace is bounded and read-only"
