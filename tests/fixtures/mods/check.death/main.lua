local actor_api = entity
local engine_actor = actor

-- Only two owned Pikmin copies are removed, one quietly and one through a counted death.
local check=require("check")
check.plan("found","made","info","listened","quiet_removed","quiet_gone","no_death","killed","reported","credited","location","gone","refused","cancelled","originals")
local quiet,victim,captain,listener,stepping,at,kind,originals,source_kind
local events={}
local phase,steps,since=0,0,0
local function wait_for(name,why) check.run(name,function() check.wait(why) end) end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait_for("found","a loaded game with player control") end
        captain=player.captain(0)
        local listed=pikmin.list()
        local source=listed and listed[1]
        local class=source and reflection.class_of(source)
        local source_at=source and actor_api.pose(source)
        if not captain or not class or not source_at then return wait_for("found","a real captain and living Pikmin class") end
        local source_info=pikmin.info(source)
        if not source_info then return wait_for("found","the living source Pikmin's initialized information") end
        source_kind=source_info.kind
        originals=listed
        check.run("found",function() check.equal(engine_actor.alive(source),true,"the real source Pikmin is in play") end)
        check.run("made",function()
            quiet=check.result("ok","its own quiet copy",actor_api.spawn(class,{x=source_at.location.x+100,y=source_at.location.y,z=source_at.location.z+20},source_at.rotation))
            victim=check.result("ok","its own counted copy",actor_api.spawn(class,{x=source_at.location.x+150,y=source_at.location.y,z=source_at.location.z+20},source_at.rotation))
            check.expect(quiet~=source and victim~=source and quiet~=victim,"two distinct owned copies")
        end)
        if not quiet or not victim then
            if quiet then actor_api.destroy(quiet) end
            if victim then actor_api.destroy(victim) end
            stepping:cancel() return
        end
        phase,since=1,steps
        return
    end
    if phase==1 then
        local quiet_info,victim_info=pikmin.info(quiet),pikmin.info(victim)
        if not quiet_info or not victim_info then return wait_for("info","both owned copies have their initialized Pikmin parts") end
        local same_kind=quiet_info.kind==source_kind and victim_info.kind==source_kind
        check.run("info",function()
            check.equal(quiet_info.kind,source_kind,"the quiet copy's initialized living type")
            check.equal(victim_info.kind,source_kind,"the counted copy's initialized living type")
        end)
        if not same_kind then
            -- Incomplete spawn data is never passed into the game's death accounting.
            actor_api.destroy(quiet) actor_api.destroy(victim)
            stepping:cancel() return
        end
        check.run("listened",function()
            listener=check.result("ok","listening for their counted death",combat.on_pikmin_died(function(event)
                if event.pikmin==quiet or event.pikmin==victim then events[#events+1]=event end
            end))
        end)
        check.run("quiet_removed",function() check.result("ok","quietly removing its first copy",actor_api.destroy(quiet)) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        if (engine_actor.alive(quiet) or steps-since<3) and steps-since<90 then return end
        check.run("quiet_gone",function() check.equal(engine_actor.alive(quiet),false,"the quiet copy left play") end)
        check.run("no_death",function() check.equal(#events,0,"quiet removal counts no Pikmin death") end)
        at=actor_api.pose(victim)
        kind=actor_api.kind(victim)
        if not at or not kind then return wait_for("killed","the counted copy's last place and kind") end
        check.run("killed",function() check.result("ok","a counted bomb death for its owned copy",combat.kill_pikmin(victim,"bomb",captain)) end)
        phase,since=3,steps
        return
    end
    if (#events<1 or engine_actor.alive(victim)) and steps-since<90 then return end
    check.run("reported",function() check.equal(#events,1,"one counted death") if events[1] then check.equal(events[1].pikmin,victim,"the owned victim") check.equal(events[1].kind,kind,"its actual kind") check.equal(events[1].pikmin_type,source_kind,"its Pikmin type") check.equal(events[1].cause,"bomb","the requested counted cause") end end)
    check.run("credited",function() check.expect(events[1]~=nil,"the counted report exists") if events[1] then check.equal(events[1].attacker,captain,"the actual captain credited") end end)
    check.run("location",function() check.expect(events[1]~=nil,"the counted report exists") if events[1] then for _,field in ipairs({"x","y","z"}) do check.near(events[1].location[field],at.location[field],5,"the last place's "..field) end end end)
    check.run("gone",function() check.equal(engine_actor.alive(victim),false,"the counted copy left play") end)
    check.run("refused",function() check.result("not_found","the gone Pikmin cannot be counted again",combat.kill_pikmin(victim,"bomb",captain)) check.result("not_found","the gone copy cannot be quietly removed twice",actor_api.destroy(victim)) end)
    check.run("cancelled",function() check.result("ok","ending the death listener",listener:cancel()) check.result("not_found","ending it twice",listener:cancel()) end)
    check.run("originals",function() for _,actor in ipairs(originals) do check.equal(engine_actor.alive(actor),true,"each original Pikmin remains in play") end end)
    stepping:cancel()
end)
