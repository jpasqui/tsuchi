local callback,cancelled,warps,logs,stored,control,world,npcs,key,role,ai_name,readback,mounted,position_ready
local function setup()
    cancelled,warps,logs,stored=false,0,{},""
    control,world,npcs,key,role,ai_name,readback,mounted,position_ready=true,"Area500_P",{"doctor"},"SV005",9,"NpcAIComponent",false,false,true
    time={on_step=function(fn) callback=fn return {cancel=function() cancelled=true end} end}
    game={world=function() return world end,has_control=function() return control end}
    player={captain=function() return "captain" end,character=function() return "captain" end}
    captain={riding=function() return mounted and "dog" or nil end}
    actor=setmetatable({}, {__index=function() error("night gameplay must use entity") end})
    entity={list=function() return npcs end,pose=function(one)
      if not position_ready then return nil end
      return {location={x=one=="captain" and (readback and 423 or 1000) or 273,y=-1204,z=readback and 319 or 269}}
      end,warp=function(one,at)
      assert(one=="captain" and at.x==423 and at.y==-1204 and at.z==319)
      warps=warps+1 return true end}
    reflection={get=function(one,field)
      if field=="NpcAI" then return one=="doctor" and "ai" or nil end
      if field=="NpcInfoKey" then return key end
      if field=="AIParameter.NpcRoleType" then return role end
      error("unexpected reflection field "..field)
      end,class_of=function(one) return one=="ai" and "ai_class" or "doctor_class" end,
      name=function(one) return one=="ai_class" and ai_name or "GNpcDoctor" end}
    reflection.path=function() return "/Game/Carrot4/Placeables/Npc/GNpcDoctor.GNpcDoctor_C" end
    storage={write=function(name,text) assert(name=="reentry-observations") stored=text return true end}
    print=function(text) logs[#logs+1]=text end
    dofile("tests/fixtures/release_night/release.t3.night.reentry/main.lua")
end
local function tick(count) for _=1,count do if not cancelled then callback() end end end
setup() tick(2399) assert(warps==0)
control=false tick(61) assert(warps==0 and not cancelled)
control=true world="Area001_P" tick(60) assert(warps==0 and not cancelled)
world="Area500_P" tick(30) assert(warps==1 and not cancelled)
tick(30) assert(warps==1 and not cancelled)
readback=true tick(30) assert(warps==1 and cancelled)
assert(stored:find("NpcInfoKey SV005 NpcRoleType 9 control true",1,true))
assert(stored:find("READY captain approach read back",1,true) and not stored:find("PASS",1,true))
tick(300) assert(warps==1)
for _,bad in ipairs({"key","role","class","missing","ambiguous","mounted","position"}) do
 setup()
 if bad=="key" then key="SV004" elseif bad=="role" then role=8 elseif bad=="class" then ai_name="OtherAI"
 elseif bad=="missing" then npcs={"other"} elseif bad=="ambiguous" then npcs={"doctor","doctor"}
 elseif bad=="mounted" then mounted=true else position_ready=false end
 tick(2400) assert(warps==0 and cancelled and stored:find("BLOCKED",1,true))
end
setup() tick(2580) assert(warps==1 and cancelled and stored:find("readback timed out",1,true))
return "night reentry uses exact Doctor guards, captain-only setup and readback"
