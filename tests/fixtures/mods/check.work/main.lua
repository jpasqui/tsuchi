local actor_api = entity
local engine_actor = actor

-- One request-identified added Pikmin works on an owned quiet CHAPPY, then both are removed.
local check=require("check")
check.plan("found","added","identified","made","target","baseline","listened","asked","changed","readback","refused","ended","removed","gone")
local captain,kind,before,worker,target,class,at,listener,stepping,baseline
local events,readbacks={},{}
local phase,steps,since=0,0,0
local function wait_for(name,why) check.run(name,function() check.wait(why) end) end
local function remove_owned()
    if target then actor_api.release_suspended(target,{logic=true,movement=true}) end
    check.run("removed",function()
        if worker then check.result("ok","quietly removing only its identified worker",actor_api.destroy(worker)) end
        if target then check.result("ok","quietly removing its owned target",actor_api.destroy(target)) end
    end)
    phase,since=5,steps
end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait_for("found","a loaded game with player control") end
        captain=player.captain(0)
        if not captain or player.character(0)~=captain then return wait_for("found","the first player steering its captain") end
        local listed=pikmin.list()
        local info=listed and listed[1] and pikmin.info(listed[1])
        if not info then return wait_for("found","a living Pikmin of a named type") end
        for _,actor in ipairs(actor_api.list() or {}) do
            if actor_api.kind(actor)=="CHAPPY" then class=reflection.class_of(actor) if class then break end end
        end
        at=actor_api.pose(captain)
        if not class or not at then return wait_for("found","a stock CHAPPY class and captain position") end
        kind,before=info.kind,{}
        for _,actor in ipairs(listed) do before[actor]=true end
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the real captain remains in play") end)
        check.run("added",function() check.result("ok","adding one flower worker of a living type",pikmin.add(captain,kind,"flower",1)) end)
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
        check.run("identified",function() check.expect(identified,"exactly one new request-matching Pikmin follows this captain") end)
        if not identified then stepping:cancel() return end
        worker=candidate
        check.run("made",function() target=check.result("ok","its own quiet stock target",actor_api.spawn(class,{x=at.location.x+250,y=at.location.y,z=at.location.z+20},at.rotation)) end)
        if not target then remove_owned() return end
        check.run("target",function() check.result("ok","keeping its target's decisions and movement quiet",actor_api.hold_suspended(target,{logic=true,movement=true})) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        baseline=behavior.action(worker)
        if not baseline or baseline=="" then return wait_for("baseline","the new worker's initialized following action") end
        check.run("baseline",function() check.equal(squad.leader(worker),captain,"the worker follows its captain before the order") check.expect(#baseline>0,"its prior action is named") end)
        check.run("listened",function()
            listener=check.result("ok","listening to this worker's changed decisions",behavior.on_changed(function(event)
                if event.actor==worker then events[#events+1]=event readbacks[#readbacks+1]=behavior.action(worker) end
            end))
        end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        -- The observer first records what the worker is doing on its next update.
        -- Order work only after that baseline exists, then record changes from this request.
        baseline=behavior.action(worker)
        if not baseline or baseline=="" then return end
        events,readbacks={},{}
        check.run("asked",function() check.result("ok","the captain's work order for one worker",pikmin.work_on(worker,target)) end)
        phase,since=4,steps
        return
    end
    if phase==4 then
        local action=behavior.action(worker)
        if (#events<1 or not action or action==baseline) and steps-since<180 then return end
        check.run("changed",function()
            check.expect(#events>0,"the owned work order caused a decision change")
            check.expect(action~=nil and action~="" and action~=baseline,"the worker acts beyond its original following action")
            if events[1] then check.equal(events[1].previous,baseline,"the first changed action remembers the prior decision") end
        end)
        check.run("readback",function() for i,event in ipairs(events) do check.equal(event.actor,worker,"only its worker is recorded") check.equal(readbacks[i],event.action,"the event names the actual action when heard") end end)
        check.run("refused",function() check.result("not_found","a captain is not a Pikmin worker",pikmin.work_on(captain,target)) end)
        check.run("ended",function() check.result("ok","ending the decision listener",listener:cancel()) check.result("not_found","ending it twice",listener:cancel()) end)
        remove_owned()
        return
    end
    if (engine_actor.alive(worker) or (target and engine_actor.alive(target))) and steps-since<90 then return end
    check.run("gone",function()
        check.equal(engine_actor.alive(worker),false,"its worker left play")
        if target then check.equal(engine_actor.alive(target),false,"its target left play") end
        for actor in pairs(before) do check.equal(engine_actor.alive(actor),true,"each original Pikmin remains") end
    end)
    stepping:cancel()
end)
