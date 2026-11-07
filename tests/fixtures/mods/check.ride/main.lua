local actor_api = entity
local engine_actor = actor
local captain_api = captain

-- Ordinary mounting on an isolated save, with original squad leaders restored afterward.
local check=require("check")
check.plan("found","asked","pending","mounted","off","restored","refused","quiet","originals")
local captain,dog,stepping
local phase,steps,since=0,0,0
local original,participants={},{}
local function wait(name,why) check.run(name,function() check.wait(why) end) end
local function restore_leaders()
    for actor,entry in pairs(participants) do
        if squad.leader(actor)~=entry.leader then
            if entry.leader then squad.join(actor,entry.leader) else squad.leave(actor) end
        end
    end
end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait("found","loaded play with player control") end
        captain,dog=player.captain(0),player.dog(0)
        if not captain or not dog or player.character(0)~=captain then return wait("found","the real first captain steered beside its real dog") end
        local mount,why=captain_api.riding(captain)
        if mount or why then return wait("found","an unmounted captain with known mount data") end
        local cap_members,dog_members=squad.members(captain),squad.members(dog)
        local listed=pikmin.list()
        if not cap_members or not dog_members or not listed then return wait("found","both squads and the Pikmin list") end
        for _,actor in ipairs(listed) do original[actor]=true end
        for _,members in ipairs({cap_members,dog_members}) do for _,actor in ipairs(members) do participants[actor]={leader=squad.leader(actor)} end end
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the actual captain is alive") check.equal(engine_actor.alive(dog),true,"its actual dog is alive") end)
        phase=1
    end
    if phase==1 then
        local asked,why=captain_api.ride(captain)
        if not asked and (why=="busy" or why=="not_ready") then return wait("asked","ordinary ride readiness " .. why) end
        check.run("asked",function() check.result("ok","the captain's ordinary ride request",asked,why) end)
        check.run("pending",function() check.result("busy","another request while the first is pending",captain_api.ride(captain)) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        local mount=captain_api.riding(captain)
        if mount~=dog and steps-since<300 then return end
        check.run("mounted",function() check.equal(mount,dog,"the original dog is the completed mount") check.result("busy","another ride while mounted",captain_api.ride(captain)) end)
        check.run("off",function() check.result("ok","the ordinary dismount request",captain_api.dismount(captain)) end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        local mount,why=captain_api.riding(captain)
        if (mount or why) and steps-since<300 then return end
        restore_leaders()
        local restored=mount==nil and why==nil
        for actor,entry in pairs(participants) do restored=restored and squad.leader(actor)==entry.leader end
        if not restored and steps-since<300 then return end
        check.run("restored",function()
            check.equal(mount,nil,"the captain is unmounted again") check.equal(why,nil,"the mount read succeeded")
            for actor,entry in pairs(participants) do check.equal(squad.leader(actor),entry.leader,"each participating original leader is restored") end
        end)
        check.run("refused",function() check.result("not_ready","dismounting while unmounted",captain_api.dismount(captain)) check.result("not_found","a dog is not a captain",captain_api.ride(dog)) end)
        phase,since=4,steps
        return
    end
    if steps-since<3 then return end
    check.run("quiet",function() check.equal(captain_api.riding(captain),nil,"a settled request does not mount again") end)
    check.run("originals",function()
        local after={}
        for _,actor in ipairs(pikmin.list()) do after[actor]=true end
        for actor in pairs(original) do check.expect(after[actor],"each original Pikmin remains") after[actor]=nil end
        check.equal(next(after),nil,"mounting added no Pikmin")
        for actor,entry in pairs(participants) do check.equal(squad.leader(actor),entry.leader,"each original leader remains restored") end
        check.equal(player.character(0),captain,"the original captain remains steered")
        check.equal(engine_actor.alive(dog),true,"the original dog remains alive")
    end)
    stepping:cancel()
end)
