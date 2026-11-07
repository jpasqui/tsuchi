local actor_api = entity
local engine_actor = actor
local captain_api = captain

-- One request-identified worker is thrown by the real captain, then quietly removed.
local check=require("check")
check.plan("found","added","identified","aimed","asked","left","rise","speed","engine","refused","ended","removed","gone","originals")
local captain,kind,before,worker,start,stepping
local phase,steps,since,rise,speed,engine=0,0,0,0,0,true
local function wait(name,why) check.run(name,function() check.wait(why) end) end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait("found","loaded play with player control") end
        captain=player.captain(0)
        if not captain or player.character(0)~=captain then return wait("found","the player steering its first captain") end
        local mount,problem=captain_api.riding(captain)
        if mount or problem then return wait("found","a captain that is not riding") end
        local members=squad.members(captain)
        local listed=pikmin.list()
        local info=listed and listed[1] and pikmin.info(listed[1])
        local at=actor_api.pose(captain)
        if not members or not info or not at or not captain_api.cursor(captain) then return wait("found","a squad, named living Pikmin, captain position and cursor") end
        kind,before=info.kind,{}
        for _,actor in ipairs(listed) do before[actor]={leader=squad.leader(actor)} end
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the actual captain is in play") end)
        check.run("added",function() check.result("ok","one flower worker of the actual living type",pikmin.add(captain,kind,"flower",1)) end)
        phase,since=1,steps
        return
    end
    if phase==1 then
        local fresh={}
        for _,actor in ipairs(pikmin.list() or {}) do if not before[actor] then fresh[#fresh+1]=actor end end
        if #fresh<1 and steps-since<90 then return end
        local candidate=fresh[1]
        local info=candidate and pikmin.info(candidate)
        local identified=#fresh==1 and info and info.kind==kind and info.leaf=="flower" and info.in_army==true and info.team==actor_api.team(captain) and squad.leader(candidate)==captain
        check.run("identified",function() check.expect(identified,"exactly one new request-matching worker follows this captain") end)
        if not identified then stepping:cancel() return end -- ambiguous additions are never touched
        worker=candidate
        start=actor_api.pose(worker)
        if not start then return end
        local at=actor_api.pose(captain)
        check.run("aimed",function() check.result("ok","a nearby throw point",captain_api.hold_cursor(captain,{x=at.location.x+100,y=at.location.y,z=at.location.z})) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        local okay,why=captain_api.throw_pikmin(captain,worker)
        if not okay and (why=="busy" or why=="not_ready") then return wait("asked","throw readiness " .. why) end
        check.run("asked",function() check.result("ok","the actual captain throws only its identified worker",okay,why) end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        local at=actor_api.pose(worker)
        local velocity=actor_api.velocity(worker)
        if at then
            rise=math.max(rise,at.location.z-start.location.z)
            local value=reflection.call(worker,"K2_GetActorLocation",{})
            for _,axis in ipairs({"x","y","z"}) do
                local coordinate=value and (value[axis:upper()] or value[axis])
                engine=engine and type(coordinate)=="number" and math.abs(coordinate-at.location[axis])<5
            end
        end
        if velocity then speed=math.max(speed,math.sqrt(velocity.x^2+velocity.y^2+velocity.z^2)) end
        if (squad.leader(worker)~=nil or rise<=20 or speed<=50) and steps-since<90 then return end
        check.run("left",function() check.equal(squad.leader(worker),nil,"the thrown worker left its leader") end)
        check.run("rise",function() check.expect(rise>20,"the owned throw rises more than 20 cm") end)
        check.run("speed",function() check.expect(speed>50,"the owned throw moves more than 50 cm per second") end)
        check.run("engine",function() check.expect(engine,"independent engine positions agree with the SDK through the throw") end)
        check.run("refused",function() check.result("invalid","a worker that no longer follows cannot be thrown again",captain_api.throw_pikmin(captain,worker)) check.result("not_found","a captain is not a Pikmin",captain_api.throw_pikmin(captain,captain)) end)
        check.run("ended",function() check.result("ok","ending its cursor hold",captain_api.release_cursor(captain)) check.result("not_found","ending its hold twice",captain_api.release_cursor(captain)) end)
        check.run("removed",function() check.result("ok","quietly removing only its identified worker",actor_api.destroy(worker)) end)
        phase,since=4,steps
        return
    end
    if engine_actor.alive(worker) and steps-since<90 then return end
    check.run("gone",function() check.equal(engine_actor.alive(worker),false,"the owned worker left play") end)
    check.run("originals",function()
        local after={}
        for _,actor in ipairs(pikmin.list()) do after[actor]=true end
        for actor,entry in pairs(before) do check.expect(after[actor],"each original Pikmin remains") check.equal(squad.leader(actor),entry.leader,"each original leader remains") after[actor]=nil end
        check.equal(next(after),nil,"no added worker remains")
        check.equal(player.character(0),captain,"the player still steers its original captain")
    end)
    stepping:cancel()
end)
