local actor_api = entity
local engine_actor = actor
local captain_api = captain

-- On an isolated route: track new Pikmin, restore the original squad, and remove only those added here.
local check=require("check")
check.plan("found","added","made","info","following","left","rejoined","dismissed","restored","removed","gone","originals")
local captain,kind,before,originals,new,stepping
local phase,steps,since=0,0,0
local function member_set()
    local found={}
    for _,actor in ipairs(squad.members(captain) or {}) do found[actor]=true end
    return found
end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return check.run("found",function() check.wait("a loaded game with player control") end) end
        captain=player.captain(0)
        if not captain or player.character(0)~=captain then return check.run("found",function() check.wait("the player steering its first captain") end) end
        local mount,why=captain_api.riding(captain)
        if mount or why then return check.run("found",function() check.wait("a captain that is not riding") end) end
        local listed=pikmin.list()
        local members=squad.members(captain)
        if not listed or #listed==0 or not members then return check.run("found",function() check.wait("living Pikmin and the captain's squad data") end) end
        local source_info=pikmin.info(listed[1])
        if not source_info then return check.run("found",function() check.wait("information for a living Pikmin") end) end
        kind=source_info.kind
        before,originals={},{}
        for _,actor in ipairs(listed) do before[actor]=true end
        for _,actor in ipairs(members) do originals[#originals+1]={actor=actor,leader=squad.leader(actor)} end
        check.run("found",function() check.equal(engine_actor.alive(captain),true,"the steered captain is in play") end)
        check.run("added",function() check.result("ok","adding three flower Pikmin of a living type",pikmin.add(captain,kind,"flower",3)) end)
        phase,since=1,steps
        return
    end
    if phase==1 then
        new={}
        for _,actor in ipairs(pikmin.list() or {}) do if not before[actor] then new[#new+1]=actor end end
        if #new<3 and steps-since<90 then return end
        check.run("made",function() check.equal(#new,3,"exactly three Pikmin were added") end)
        if #new~=3 then
            -- No returned handles identify the additions: an ambiguous set is never removed.
            stepping:cancel()
            return
        end
        local identified=true
        for _,actor in ipairs(new) do
            local info=pikmin.info(actor)
            identified=identified and info~=nil and info.kind==kind and info.leaf=="flower" and
                info.team==actor_api.team(captain) and info.in_army==true and squad.leader(actor)==captain and engine_actor.alive(actor)==true
        end
        if not identified then
            check.run("info",function() check.expect(false,"the exact three new Pikmin must match the request and follow this captain") end)
            stepping:cancel()
            return
        end
        check.run("info",function()
            for _,actor in ipairs(new) do
                local info=check.result("ok","the new Pikmin's information",pikmin.info(actor))
                check.equal(info.kind,kind,"the requested living type")
                check.equal(info.leaf,"flower","the requested flower")
                check.equal(info.team,actor_api.team(captain),"the captain's team")
                check.equal(info.in_army,true,"the added Pikmin counts in the army")
            end
        end)
        check.run("following",function()
            local members=member_set()
            for _,actor in ipairs(new) do check.equal(squad.leader(actor),captain,"the new Pikmin follows the captain") check.expect(members[actor],"the member is listed") end
        end)
        check.run("left",function() check.result("ok","one new Pikmin leaves",squad.leave(new[1])) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        if squad.leader(new[1])~=nil and steps-since<90 then return end
        check.run("rejoined",function()
            check.equal(squad.leader(new[1]),nil,"the Pikmin stopped following")
            check.expect(not member_set()[new[1]],"the departed member is not listed")
            check.equal(pikmin.info(new[1]).in_army,true,"leaving does not change army membership")
            check.result("ok","the new Pikmin joins again",squad.join(new[1],captain))
            check.equal(squad.leader(new[1]),captain,"the new leader at once")
            check.expect(member_set()[new[1]],"the rejoined member is listed")
        end)
        check.run("dismissed",function() check.result("ok","dismissing this isolated captain's squad",squad.dismiss(captain)) end)
        phase,since=3,steps
        return
    end
    if phase==3 then
        -- Match the native herd check: settle for up to 300 updates, with one
        -- repeat request at 150. The empty-squad assertion stays unchanged.
        local elapsed=steps-since
        local members,why=squad.members(captain)
        if elapsed==45 or elapsed==150 or elapsed==300 then
            local actor=members and members[1]
            local action,action_why
            if actor then action,action_why=behavior.action(actor) end
            mod.log("crew dismiss age=" .. elapsed .. " members=" .. tostring(members and #members) ..
                " read=" .. tostring(why or "ok") .. " first=" .. tostring(actor) ..
                " action=" .. tostring(action) .. " action_read=" .. tostring(action_why or "ok"))
        end
        if members and #members>0 and elapsed==150 then
            local asked,why=squad.dismiss(captain)
            mod.log("crew dismiss repeated age=150 result=" .. tostring(asked and "ok" or why))
        end
        if (not members or #members~=0) and elapsed<300 then return end
        check.run("restored",function()
            check.equal(#squad.members(captain),0,"the dismissed squad is empty")
            for _,actor in ipairs(new) do
                check.equal(squad.leader(actor),nil,"the new Pikmin was dismissed")
                check.equal(pikmin.info(actor).in_army,true,"dismissing leaves army membership alone")
            end
            for _,entry in ipairs(originals) do check.result("ok","restoring an original member",squad.join(entry.actor,entry.leader)) end
            for _,actor in ipairs(new) do check.result("ok","restoring a new member",squad.join(actor,captain)) end
            for _,entry in ipairs(originals) do check.equal(squad.leader(entry.actor),entry.leader,"the original member's leader") end
            local members=member_set()
            for _,actor in ipairs(new) do check.expect(members[actor],"the new member was restored") end
        end)
        -- Always attempt restoration before removing the newly added Pikmin, even if a read assertion failed.
        for _,entry in ipairs(originals) do squad.join(entry.actor,entry.leader) end
        check.run("removed",function() for _,actor in ipairs(new) do check.result("ok","quietly removing only an added Pikmin",actor_api.destroy(actor)) end end)
        phase,since=4,steps
        return
    end
    local alive=false
    for _,actor in ipairs(new) do alive=engine_actor.alive(actor) or alive end
    if alive and steps-since<90 then return end
    check.run("gone",function() for _,actor in ipairs(new) do check.equal(engine_actor.alive(actor),false,"the added Pikmin left play") end end)
    check.run("originals",function()
        local after={}
        for _,actor in ipairs(pikmin.list()) do after[actor]=true end
        for actor in pairs(before) do check.expect(after[actor],"each original Pikmin remains in play") after[actor]=nil end
        check.equal(next(after),nil,"no added Pikmin remains in the list")
        for _,entry in ipairs(originals) do check.equal(squad.leader(entry.actor),entry.leader,"each original leader is restored") end
    end)
    stepping:cancel()
end)
