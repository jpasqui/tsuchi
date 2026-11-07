local actor_api = entity
local engine_actor = actor
local captain_api = captain

-- An explicitly selected captain route: restore its cursor and end the whistle it begins.
local check=require("check")
check.plan("found","made","aimed","followed","point","whistle","coalesced","sounding","hushed","cursor_restored","ended","removed","gone")
local wanted=(file.read("targets.txt",0,256) or ""):match("[^\r\n]+")
local captain, cursor, whistle, made, moved, stepping
local phase, steps, since=0,0,0
local function same(point, wanted, label)
    for _,axis in ipairs({"x","y","z"}) do check.near(point[axis],wanted[axis],0.05,label .. " " .. axis) end
end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return check.run("found",function() check.wait("a loaded game with player control") end) end
        captain=player.captain(0)
        cursor=captain and captain_api.cursor(captain)
        whistle=captain and captain_api.whistle(captain)
        if not cursor or not whistle or whistle.active then return check.run("found",function() check.wait("the first captain's cursor and inactive whistle") end) end
        local class, at
        for _,actor in ipairs(actor_api.list() or {}) do
            if actor_api.kind(actor)==wanted then class,at=reflection.class_of(actor),actor_api.pose(actor) if class and at then break end end
        end
        if not class or not at then return check.run("found",function() check.wait("a stock target kind in play") end) end
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the captain is in play") end)
        check.run("made",function()
            made=check.result("ok","making its own aim target",actor_api.spawn(class,{x=at.location.x+500,y=at.location.y,z=at.location.z+20},at.rotation))
        end)
        if not made then stepping:cancel() return end
        phase=1
        return
    end
    if phase==1 then
        check.run("aimed",function()
            check.result("ok","quietly holding its own target",actor_api.hold_suspended(made,{logic=true,movement=true,physics=true}))
            check.result("ok","aiming at its own target",captain_api.hold_cursor_on(captain,made))
            same(captain_api.cursor(captain),actor_api.pose(made).location,"the target's place")
            local at=actor_api.pose(made)
            moved={x=at.location.x+150,y=at.location.y+50,z=at.location.z}
            check.result("ok","moving only the owned target",actor_api.warp(made,moved,at.rotation,false))
        end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        if steps-since<2 then return end
        check.run("followed",function() same(captain_api.cursor(captain),actor_api.pose(made).location,"the cursor followed its moved target") end)
        local point={x=cursor.x+25,y=cursor.y+25,z=cursor.z}
        check.run("point",function()
            check.result("ok","moving its cursor hold onto a point",captain_api.hold_cursor(captain,point))
            same(captain_api.cursor(captain),point,"the chosen point")
        end)
        check.run("whistle",function()
            check.result("ok","beginning its whistle",captain_api.hold_whistle(captain))
            check.equal(captain_api.whistle(captain).active,true,"the whistle began")
        end)
        check.run("coalesced",function() check.result("ok","coalescing its whistle hold",captain_api.hold_whistle(captain)) end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        if steps-since<3 then return end
        check.run("sounding",function()
            local now=check.result("ok","the held whistle",captain_api.whistle(captain))
            check.equal(now.active,true,"the held whistle is sounding")
            check.expect(now.count>whistle.count,"the game counted a new whistle")
            check.expect(now.radius>=0 and now.radius<math.huge,"its radius is finite")
        end)
        check.run("hushed",function()
            check.result("ok","ending its coalesced whistle",captain_api.release_whistle(captain))
            check.result("not_found","ending it twice",captain_api.release_whistle(captain))
            check.equal(captain_api.whistle(captain).active,false,"the whistle ended")
        end)
        check.run("cursor_restored",function()
            check.result("ok","putting its original cursor point back",captain_api.hold_cursor(captain,cursor))
            same(captain_api.cursor(captain),cursor,"the original point")
        end)
        check.run("ended",function()
            check.result("ok","ending its cursor hold",captain_api.release_cursor(captain))
            check.result("not_found","ending it twice",captain_api.release_cursor(captain))
            same(captain_api.cursor(captain),cursor,"the point after release")
            check.result("ok","giving its own target its parts back",actor_api.release_suspended(made,{logic=true,movement=true,physics=true}))
        end)
        check.run("removed",function() check.result("ok","quietly removing its owned target",actor_api.destroy(made)) end)
        phase,since=4,steps
        return
    end
    if engine_actor.alive(made) and steps-since<90 then return end
    check.run("gone",function() check.equal(engine_actor.alive(made),false,"the owned target left play") end)
    stepping:cancel()
end)
