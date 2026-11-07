local actor_api = entity
local engine_actor = actor
local creature_api = creature

-- Select this on an isolated daytime route: night creatures do not become carcasses.
local check=require("check")
check.plan("found","class","made","life","hit","defeated","carcass","listed","asked","identified","living","removed","gone","released","originals")
local class_path=(file.read("targets.txt",0,256) or ""):match("[^\r\n]+")
local captain,class,victim,back,kind,home,listener,stepping
local original,before_revive,spawned={}, {}, {}
local phase,steps,since=0,0,0
local initialized=false
local function wait(name,why) check.run(name,function() check.wait(why) end) end
local function begin_cleanup()
    check.run("removed",function()
        for _,actor in ipairs(back and back~=victim and {back,victim} or {victim}) do
            if engine_actor.alive(actor) then
                local done,why=actor_api.destroy(actor)
                -- A revival that spawned a new identity has the game kill the carcass itself: it may already be going.
                if actor==victim and back and back~=victim and why=="busy" then
                    check.expect(true,"the game is already removing the carcass it revived from")
                else
                    check.result("ok","quietly removing only a proven owned identity",done,why)
                end
            end
        end
    end)
    phase,since=6,steps
end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait("found","an isolated daytime level with player control") end
        captain=player.captain(0)
        local at=captain and actor_api.pose(captain)
        local listed=creature_api.list()
        if not at or not listed then return wait("found","the actual first captain and creature list") end
        local loaded,why=asset.load(class_path)
        if not loaded and (why=="not_ready" or why=="busy") then return wait("found","the original Dwarf Bulborb class load " .. why) end
        class=loaded
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the actual captain is in play") end)
        check.run("class",function() check.result("ok","loading the original class",loaded,why) check.equal(reflection.path(class),class_path,"the exact original asset path") end)
        if not class then stepping:cancel() return end
        for _,actor in ipairs(listed) do original[actor]=true end
        local listener_why
        listener,listener_why=actor_api.on_spawned(function(event) spawned[event.actor]=event end)
        home={x=at.location.x+400,y=at.location.y,z=at.location.z+50}
        check.run("made",function()
            check.result("ok","listening for native spawn identity",listener,listener_why)
            victim=check.result("ok","making the check's own original Dwarf Bulborb",actor_api.spawn(class,home,at.rotation))
            check.equal(reflection.class_of(victim),class,"the exact loaded class")
            check.expect(not original[victim],"the new actor is owned by this check")
            kind=check.result("ok","the owned creature kind",actor_api.kind(victim))
            check.expect(type(kind)=="string" and #kind>0,"the creature has its native kind")
        end)
        if not victim then asset.release(class) if listener then listener:cancel() end stepping:cancel() return end
        phase,since=1,steps
        return
    end
    if phase==1 then
        local life=combat.life(victim)
        if not life or life.life<=0 then return wait("life","the owned daytime creature's initialized positive life") end
        if not initialized then check.run("life",function()
            check.expect(life.max_life>0 and life.life<=life.max_life,"the creature is healthy")
            check.equal(creature_api.defeated(victim),false,"it is initially undefeated")
            check.equal(creature_api.carcass(victim),false,"it is initially no carcass")
            check.result("not_ready","reviving before defeat",creature_api.revive(victim))
        end)
        initialized=true end
        local asked,why=combat.damage(victim,100000,"normal",captain)
        if not asked and (why=="busy" or why=="not_ready") then return wait("hit","the owned creature to take its fatal native hit " .. why) end
        check.run("hit",function() check.result("ok","the captain hits only its owned creature",asked,why) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        local down=creature_api.defeated(victim)
        if down~=true and steps-since<300 then return end
        check.run("defeated",function() check.equal(down,true,"the original owned victim is defeated by the game") end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        local carcass=creature_api.carcass(victim)
        if carcass~=true and steps-since<900 then return end
        check.run("carcass",function() check.equal(carcass,true,"the daytime victim becomes the native carcass") end)
        local listed=creature_api.list() or {}
        local present=false
        for _,actor in ipairs(listed) do before_revive[actor]=true present=present or actor==victim end
        check.run("listed",function() check.equal(present,true,"the exact carcass remains listed") check.equal(creature_api.defeated(victim),true,"the listed carcass remains defeated") end)
        if carcass~=true or not present then begin_cleanup() return end
        phase,since=4,steps
    end
    if phase==4 then
        local asked,why=creature_api.revive(victim)
        if not asked and (why=="busy" or why=="not_ready") and steps-since<900 then return wait("asked","the uncarried native carcass to take revival " .. why) end
        check.run("asked",function() check.result("ok","the carcass asks the game for its revival",asked,why) end)
        phase,since=5,steps
        return
    end
    if phase==5 then
        local candidates={}
        for _,actor in ipairs(creature_api.list() or {}) do
            if (actor==victim or not before_revive[actor]) and creature_api.defeated(actor)==false and
                reflection.class_of(actor)==class and actor_api.kind(actor)==kind then candidates[#candidates+1]=actor end
        end
        if #candidates==0 and steps-since<900 then return end
        local candidate=#candidates==1 and candidates[1]
        local at=candidate and actor_api.pose(candidate)
        local proved=candidate and at and math.abs(at.location.x-home.x)<=150 and math.abs(at.location.y-home.y)<=150 and
            (candidate==victim or spawned[candidate]~=nil)
        check.run("identified",function()
            check.equal(#candidates,1,"exactly one revival has the original class and kind")
            check.expect(proved,"the original owned identity, or one native-spawned new identity at its home, proves ownership")
        end)
        if not proved then begin_cleanup() return end
        back=candidate
        check.run("living",function()
            local life=check.result("ok","the revived creature's life",combat.life(back))
            check.expect(life.life>0 and life.life<=life.max_life,"the revival has positive healthy life")
            check.equal(creature_api.defeated(back),false,"it is undefeated") check.equal(creature_api.carcass(back),false,"it is no carcass")
            check.result("not_ready","reviving it twice while healthy",creature_api.revive(back))
        end)
        begin_cleanup()
        return
    end
    if phase~=6 then return end
    if (engine_actor.alive(victim) or (back and engine_actor.alive(back))) and steps-since<90 then return end
    check.run("gone",function() check.equal(engine_actor.alive(victim),false,"the owned original victim left play") if back then check.equal(engine_actor.alive(back),false,"the proved revival left play") end end)
    check.run("released",function()
        check.result("ok","releasing the original class hold",asset.release(class)) check.result("not_found","releasing it twice",asset.release(class))
        check.result("ok","ending the native spawn listener",listener:cancel()) check.result("not_found","ending it twice",listener:cancel())
    end)
    check.run("originals",function()
        local after={}
        for _,actor in ipairs(creature_api.list()) do after[actor]=true end
        for actor in pairs(original) do check.expect(after[actor],"each original creature remains listed") end
        check.expect(not after[victim] and (not back or not after[back]),"neither owned identity remains listed")
        check.equal(engine_actor.alive(captain),true,"the actual captain remains alive")
    end)
    stepping:cancel()
end)
