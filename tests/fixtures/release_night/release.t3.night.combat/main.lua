-- Optional native Pikmin combat route. Captain warps provide route setup.
-- The clock advances after native defeat as controlled route setup.
-- No actors are spawned, and health values and saves are not edited.
local ids=require("ids")
local steps, phase, held, done, observed = 0, "settle", false, false, false
local victim, cap, last, stable_since, requested_at, throws, stepping
local cursor_released = false
throws=0
local function log(text) print(tostring(steps).." night-combat "..text) end
local function release()
    if held and cap then
        local okay,why=captain.release_cursor(cap)
        log("cursor release "..tostring(okay).." "..tostring(why))
        if okay==true then held=false cursor_released=true end
    end
end
local function finish(why)
    release()
    done=true
    log(why)
    if stepping then stepping:cancel() end
end
combat.on_defeated(function(event)
    local same=victim and tostring(event.actor)==tostring(victim)
    if not same then same=entity.placement(event.actor)==ids.larva end
    if same then
        observed=true
        log("OBSERVED native defeat previous "..tostring(event.previous).." life "..tostring(event.life))
        release()
    end
end)
stepping=time.on_step(function()
    steps=steps+1
    if done then return end
    if steps>=6000 then return finish("bounded route ended; no life-cycle claim") end
    if observed then
        if held then release() end
        if not cursor_released or game.has_control()~=true or game.world()~="Area001_P" then return end
        local pot
        for _,one in ipairs(entity.list() or {}) do
            if entity.placement(one)==ids.pot then pot=one break end
        end
        local life=pot and combat.life(pot)
        if not life or not life.life or life.life<=0 then return end
        local before,why=story.clock()
        if not before or not (before>=0 and before<1) then return end
        local okay,error=story.set_clock(1)
        log("controlled night-end request "..tostring(okay).." "..tostring(error)..
            " clock before "..tostring(before).." readback "..tostring(story.clock()))
        return finish("request only; native Save and same-ID reload remain independent")
    end
    if steps<2400 or steps%30~=0 or game.has_control()~=true or game.world()~="Area001_P" then return end
    cap=player.captain(0)
    if not cap or player.character(0)~=cap or captain.riding(cap) then return log("waiting unmounted controlled captain") end
    victim=nil
    for _,one in ipairs(entity.list() or {}) do
        if entity.placement(one)==ids.larva then victim=one break end
    end
    if not victim then return log("waiting exact placed larva") end
    local cls=reflection.class_of(victim)
    if not cls or reflection.path(cls)~="/Game/Carrot4/Placeables/Teki/GBaby.GBaby_C" then return finish("unexpected donor class; no combat") end
    local place,life=entity.pose(victim),combat.life(victim)
    if not place or not life then return log("waiting position and life") end
    local at=place.location
    log(string.format("target %.3f %.3f %.3f life %.3f phase %s",at.x,at.y,at.z,life.life,phase))
    if life.life<=0 or creature.defeated(victim)==true then return finish("already defeated without route event; save remains independent") end
    if phase=="settle" then
        if at.z<=0 or at.z>=500 then stable_since,last=nil,nil return log("waiting supported surface height") end
        if not last or math.abs(last.z-at.z)>1 then stable_since=steps end
        last={x=at.x,y=at.y,z=at.z}
        if steps-(stable_since or steps)<120 then return end
        log(string.format("OBSERVED settled height %.3f for %d frames",at.z,steps-stable_since))
        local okay,why=entity.warp(cap,{x=at.x+120,y=at.y,z=at.z+100},{pitch=0,yaw=180,roll=0})
        log("captain approach request "..tostring(okay).." "..tostring(why))
        if okay==true then phase,requested_at="approach",steps end
        return
    end
    if phase=="approach" then
        if steps-requested_at>180 then return finish("captain approach timed out") end
        local captain_at=entity.pose(cap)
        if not captain_at then return end
        local c=captain_at.location
        log(string.format("captain readback %.3f %.3f %.3f",c.x,c.y,c.z))
        if math.abs(c.x-at.x)>200 or math.abs(c.y-at.y)>200 or math.abs(c.z-at.z)>200 then return end
        local okay,why=captain.hold_cursor_on(cap,victim)
        log("cursor hold request "..tostring(okay).." "..tostring(why))
        if okay==true then held,phase=true,"throw" end
        return
    end
    if throws>=12 then return finish("throw bound reached without observed defeat") end
    for _,one in ipairs(squad.members(cap) or {}) do
        local info=pikmin.info(one)
        if info and info.kind=="glow" then
            local okay,why=captain.throw_pikmin(cap,one)
            log("native glow throw request "..tostring(okay).." "..tostring(why))
            if okay==true then throws=throws+1 end
            break
        end
    end
end)
