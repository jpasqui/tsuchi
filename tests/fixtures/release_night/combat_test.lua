local callback,defeated,cancelled,warps,holds,releases,throws,logs,present,height
local pot_present,pot_life,clock,clock_sets,controlled,world,cursor_ok
local function setup()
    callback,defeated,cancelled=nil,nil,false
    warps,holds,releases,throws,logs,present,height=0,0,0,{}, {},false,0
    pot_present,pot_life,clock,clock_sets,controlled,world,cursor_ok=true,100,0.25,0,true,"Area001_P",true
    require=function() return {larva=8845280987219195551,pot=7633535673922496013} end
    time={on_step=function(fn) callback=fn return {cancel=function() cancelled=true end} end}
    game={world=function() return world end,has_control=function() return controlled end}
    story={clock=function() return clock end,set_clock=function(value) assert(value==1) clock_sets=clock_sets+1 clock=value return true end}
    player={captain=function() return "captain" end,character=function() return "captain" end}
    actor=setmetatable({}, {__index=function() error("night gameplay must use entity") end})
    entity={list=function() local all={} if present then all[#all+1]="larva" end if pot_present then all[#all+1]="pot" end return all end,
      placement=function(one) return one=="larva" and 8845280987219195551 or one=="pot" and 7633535673922496013 or 0 end,
      pose=function(one) return {location={x=one=="captain" and -257 or -377,y=1371,z=one=="captain" and height+100 or height}} end,
      warp=function(one) assert(one=="captain") warps=warps+1 return true end}
    captain={riding=function() return nil end,hold_cursor_on=function(one,target)
      assert(one=="captain" and target=="larva") holds=holds+1 return true end,
      release_cursor=function(one) assert(one=="captain") releases=releases+1 return cursor_ok end,
      throw_pikmin=function(one,worker) assert(one=="captain" and worker=="glow") throws[#throws+1]=worker return true end}
    combat={on_defeated=function(fn) defeated=fn end,life=function(one) return {life=one=="pot" and pot_life or 10} end}
    creature={defeated=function() return false end}
    reflection={class_of=function() return "class" end}
    reflection.path=function() return "/Game/Carrot4/Placeables/Teki/GBaby.GBaby_C" end
    squad={members=function() return {"red","glow"} end}
    pikmin={info=function(one) return {kind=one=="glow" and "glow" or "red"} end}
    print=function(text) logs[#logs+1]=text end
    dofile("tests/fixtures/release_night/release.t3.night.combat/main.lua")
end
local function tick(count) for _=1,count do if not cancelled then callback() end end end
setup()
tick(2399)
assert(warps==0 and holds==0 and #throws==0)
tick(181)
assert(warps==0 and #throws==0)
present=true
tick(180)
assert(warps==0 and #throws==0)
height=109
tick(150)
assert(warps==1 and #throws==0)
tick(30)
assert(holds==1 and #throws==0)
tick(30)
assert(#throws==1)
tick(29)
assert(#throws==1)
tick(1)
assert(#throws==2 and clock_sets==0)
defeated({actor="other",previous=10,life=0})
assert(releases==0)
defeated({actor="larva",previous=10,life=0})
assert(releases==1)
pot_present=false
tick(1)
assert(not cancelled and clock_sets==0)
pot_present=true pot_life=0
tick(1)
assert(not cancelled and clock_sets==0)
pot_life=100 controlled=false
tick(1)
assert(not cancelled and clock_sets==0)
controlled=true world="Area500_P"
tick(1)
assert(not cancelled and clock_sets==0)
world="Area001_P" clock=1
tick(1)
assert(not cancelled and clock_sets==0)
clock=0/0
tick(1)
assert(not cancelled and clock_sets==0)
clock=0.25
tick(1)
assert(cancelled and #throws==2 and releases==1 and clock_sets==1 and clock==1)
tick(60)
assert(clock_sets==1)
local text=table.concat(logs,"\n")
assert(text:find("OBSERVED native defeat previous 10 life 0",1,true) and not text:find("PASS",1,true))
assert(text:find("request only; native Save and same-ID reload remain independent",1,true))
-- A failed cursor release cannot admit the clock request.
setup() present=true height=109
tick(2580)
cursor_ok=false
defeated({actor="larva",previous=10,life=0})
tick(1)
assert(clock_sets==0 and not cancelled)
cursor_ok=true
tick(1)
assert(clock_sets==1 and cancelled)
return "night native combat is guarded, paced and releases its cursor"
