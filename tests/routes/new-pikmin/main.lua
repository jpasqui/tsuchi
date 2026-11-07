local check = require("check")
local entity_api, actor_api, captain_api = entity, actor, captain
local phase = (file.read("phase.txt",0,32) or ""):match("%S+")
local names = {[19]="route.new-pikmin:amber", [21]="route.new-pikmin:azure"}
local stepping, captain, initial, added, victim, death_listener
local state, age, since, throwing, highest, throw_start = 0, 0, 0, 1, 0, 0
local deaths = {}
local prekill_gate, prekill_kind, prekill_status
local prekill_nonce = phase == "spawn" and (file.read("prekill-nonce.txt",0,64) or ""):match("%S+") or nil
local function native_color(a)
    return reflection.get(a,"PikminColor")
end
local function grouped()
    local groups = {[19]={}, [21]={}, stock={}}
    for _, a in ipairs(pikmin.list() or {}) do
        local color = native_color(a)
        if color == 19 or color == 21 then groups[color][#groups[color]+1]=a
        else groups.stock[#groups.stock+1]=a end
    end
    return groups
end
local function verify(a,color,leaf,leader)
    local info=check.result("ok","Pikmin information",pikmin.info(a))
    check.equal(native_color(a),color,"distinct native color")
    check.equal(info.kind,phase=="missing" and "#"..color or names[color],"public type identity")
    if leaf then check.equal(info.leaf,leaf,"requested leaf stage") end
    check.equal(info.in_army,true,"native army membership")
    local move=check.result("ok","the actor's native move component",reflection.get(a,"CarrotMoveComponent"))
    local expected=color==19 and 117 or 223
    if phase=="missing" then
        local table=check.result("ok","holding the native fallback property table",asset.load("/Game/Carrot4/Core/GActor/DT_PikminProperty.DT_PikminProperty"))
        local field=check.result("ok","the restored native leaf stage",({leaf="LeafSpeed",bud="BudSpeed",flower="FlowerSpeed"})[info.leaf])
        expected=check.result("ok","the retained stock donor speed",datatable.get(table,color==19 and "Red" or "Blue",field))
        check.equal(datatable.get(table,"Tsuchi"..color,"PikminColor"),color,"missing provider retains a distinct fallback color")
        check.near(datatable.get(table,"Tsuchi"..color,field),expected,0.001,"missing provider restores its donor parameter row")
        asset.release(table)
    end
    check.near(reflection.get(move,"CarrotMoveParameter.MaxSpeed"),expected,0.01,"native movement uses this type's cached parameter row")
    if leader then check.equal(squad.leader(a),leader,"native squad leader") end
end
local function wait(name,why) return check.run(name,function() check.wait(why) end) end
if phase~="spawn" and phase~="reload" and phase~="missing" then error("phase.txt must name spawn, reload or missing") end
if phase=="spawn" then
    check.plan("admitted","spawned","parameters","stock","squad","asked_amber","throw_amber","asked_azure","throw_azure","killed","death","survivors","departure","transition")
else check.plan("restored","stock","survivors") end
stepping=time.on_step(function()
    age=age+1
    if age>22500 then mod.log("new-pikmin timeout phase="..phase.." state="..state) stepping:cancel() return end
    if game.has_control()~=true or game.mode()~="story" then return wait(phase=="spawn" and "admitted" or "restored","ordinary story play") end
    if state==0 then
        captain=player.captain(0)
        if not captain then return wait(phase=="spawn" and "admitted" or "restored","the first captain") end
        local mount, why=captain_api.riding(captain)
        if mount then
            local okay, problem=captain_api.dismount(captain)
            return wait(phase=="spawn" and "admitted" or "restored","native dismount "..tostring(problem or okay))
        end
        if why then return wait(phase=="spawn" and "admitted" or "restored","an unmounted captain "..why) end
        if player.character(0)~=captain then
            local okay, problem=player.set_character(0,captain)
            return wait(phase=="spawn" and "admitted" or "restored","native captain switch "..tostring(problem or okay))
        end
        local groups=grouped()
        if phase~="spawn" then
            if #groups[19]~=1 or #groups[21]~=2 then
                if age%90==0 then mod.log("new-pikmin restored counts19="..#groups[19].."21="..#groups[21].." stock="..#groups.stock) end
                return wait("restored","the exact saved survivors: one color19 and two color21")
            end
            check.run("restored",function()
                for _,color in ipairs({19,21}) do for _,a in ipairs(groups[color]) do verify(a,color,nil,captain) end end
            end)
            check.run("stock",function() check.expect(#groups.stock>0,"stock and added Pikmin coexist after native reload") end)
            check.run("survivors",function() check.equal(#groups[19],1,"the counted death stays absent") check.equal(#groups[21],2,"both other added Pikmin survive") end)
            mod.log("new-pikmin ready phase="..phase.." native19=1 native21=2")
            stepping:cancel() return
        end
        if #groups[19]~=0 or #groups[21]~=0 then error("spawn phase requires an isolated save with no previous added army") end
        initial={}
        for _,a in ipairs(pikmin.list() or {}) do initial[a]={color=native_color(a),kind=pikmin.info(a).kind,leader=squad.leader(a)} end
        local first, why=pikmin.add(captain,names[19],"flower",2)
        if not first and why=="not_ready" then return wait("admitted","native addedtype presentation") end
        check.run("admitted",function()
            check.result("ok","adding nativecolor19",first,why)
            check.result("ok","adding nativecolor21",pikmin.add(captain,names[21],"bud",2))
            check.result("ok","adding a stock red beside both",pikmin.add(captain,"red","flower",1))
        end)
        state,since=1,age return
    end
    if state==5 then
        if age-since<90 then return end
        local okay, why=story.leave_cave()
        if not okay and (why=="busy" or why=="not_ready") then return wait("departure","native cave departure "..why) end
        check.run("departure",function() check.result("ok","native cave departure saves the surviving army",okay,why) end)
        state,since=6,age return
    end
    if state==6 then
        if not (game.world() or ""):match("^Area") then return wait("transition","native surface arrival") end
        local groups=grouped()
        if (#groups[19]~=1 or #groups[21]~=2) and age-since<600 then return wait("transition","native saved-army birth") end
        check.run("transition",function()
            check.equal(#groups[19],1,"native transition births only one first-color survivor")
            check.equal(#groups[21],2,"native transition births both second-color survivors")
            check.expect(#groups.stock>0,"the stock army survives beside added colors")
            for _,color in ipairs({19,21}) do for _,a in ipairs(groups[color]) do verify(a,color,nil,nil) end end
        end)
        mod.log("new-pikmin surface ready native19="..#groups[19].." native21="..#groups[21].."; quit and reload this physical save")
        stepping:cancel() return
    end
    if state==1 then
        local groups=grouped()
        if (#groups[19]~=2 or #groups[21]~=2) and age-since<90 then return end
        check.run("spawned",function()
            check.equal(#groups[19],2,"two new color19 Pikmin") check.equal(#groups[21],2,"two new color21 Pikmin")
            for _,color in ipairs({19,21}) do for _,a in ipairs(groups[color]) do verify(a,color,color==19 and "flower" or "bud",captain) end end
        end)
        if #groups[19]~=2 or #groups[21]~=2 then stepping:cancel() return end
        added={groups[19][1],groups[21][1]}; victim=groups[19][2]
        check.run("parameters",function()
            local table=check.result("ok","holding the native property table",asset.load("/Game/Carrot4/Core/GActor/DT_PikminProperty.DT_PikminProperty"))
            check.equal(datatable.get(table,"Tsuchi19","PikminColor"),19,"first row has its own color")
            check.equal(datatable.get(table,"Tsuchi21","PikminColor"),21,"second row has its own color")
            check.near(datatable.get(table,"Tsuchi19","FlowerSpeed"),117,0.001,"first mapped parameter")
            check.near(datatable.get(table,"Tsuchi21","BudSpeed"),223,0.001,"second mapped parameter")
            check.expect(datatable.get(table,"Red","FlowerSpeed")~=117,"stock red parameter unchanged")
            check.expect(datatable.get(table,"Blue","BudSpeed")~=223,"stock blue parameter unchanged")
            asset.release(table)
        end)
        check.run("stock",function()
            check.expect(#groups.stock>0,"stock and added actors coexist")
            for a,before in pairs(initial) do
                check.equal(actor_api.alive(a),true,"an original stock actor remains")
                check.equal(native_color(a),before.color,"its nativecolor remains")
                check.equal(pikmin.info(a).kind,before.kind,"its SDKtype remains")
            end
        end)
        local leaving=check.run("squad",function()
            local members={} for _,a in ipairs(squad.members(captain) or {}) do members[a]=true end
            for _,color in ipairs({19,21}) do for _,a in ipairs(groups[color]) do check.expect(members[a],"distinct-color member occupies the squad") end end
            check.result("ok","leave first custom member",squad.leave(added[1]))
            check.wait("the first custom member's next update")
        end)
        if leaving=="waiting" then state,since=11,age else stepping:cancel() end
        return
    end
    if state==11 then
        local joined=check.run("squad",function()
            if not check.equal(squad.leader(added[1]),nil,"it leaves on its next update") then return end
            local members={} for _,a in ipairs(squad.members(captain) or {}) do members[a]=true end
            if not check.expect(not members[added[1]],"the departed member leaves the squad list") then return end
            verify(added[1],19,"flower",nil)
            check.result("ok","rejoin first custom member",squad.join(added[1],captain))
            check.equal(squad.leader(added[1]),captain,"it rejoins at once")
            local restored={} for _,a in ipairs(squad.members(captain) or {}) do restored[a]=true end
            check.expect(restored[added[1]],"the rejoined member returns to the squad list")
        end)
        if joined=="pass" then state,since=2,age else stepping:cancel() end
        return
    end
    if state==2 then
        local a=added[throwing]
        local at=entity_api.pose(captain)
        local okay,why=captain_api.hold_cursor(captain,{x=at.location.x+100,y=at.location.y,z=at.location.z})
        if not okay then return wait("throw_"..(throwing==1 and "amber" or "azure"),"cursor readiness "..tostring(why)) end
        local start=entity_api.pose(a)
        okay,why=captain_api.throw_pikmin(captain,a)
        if not okay and (why=="busy" or why=="not_ready") then return end
        check.run("asked_"..(throwing==1 and "amber" or "azure"),function() check.result("ok","native captain throws addedtype",okay,why) end)
        highest=start.location.z; throw_start=start.location.z; state,since=3,age return
    end
    if state==3 then
        local a=added[throwing]
        local at=entity_api.pose(a)
        if at then highest=math.max(highest,at.location.z) end
        if age-since<60 then return end
        check.run("throw_"..(throwing==1 and "amber" or "azure"),function()
            check.equal(squad.leader(a),nil,"thrown custom member leaves leader")
            verify(a,throwing==1 and 19 or 21,nil,nil)
            check.expect(at~=nil,"the thrown actor stays alive")
            check.expect(highest-throw_start>20,"native addedtype throw rises over20cm")
            check.result("ok","restoring the thrown survivor",squad.join(a,captain))
        end)
        captain_api.release_cursor(captain)
        if throwing==1 then throwing=2 state,since=2,age return end
        prekill_kind=check.result("ok","captured victim kind",entity_api.kind(victim))
        local why
        prekill_gate,why=require("prekill_gate").new(
            {captain=captain,victim=victim,world=game.world(),kind=prekill_kind,public_kind=names[19],nonce=prekill_nonce,max_polls=18000},
            {world=game.world,has_control=game.has_control,captain=player.captain,controlled=player.character,
             alive=actor_api.alive,kind=entity_api.kind,color=native_color,
             public_kind=function(a) local info,why=pikmin.info(a) return info and info.kind,why end,
             leaf=function(a) local info,why=pikmin.info(a) return info and info.leaf,why end,whistle=captain_api.whistle})
        if not prekill_gate then mod.log("new-pikmin prekill refused "..tostring(why)) stepping:cancel() return end
        state,since=30,age return
    end
    if state==30 then
        local confirmed,why=prekill_gate.poll()
        local status,refusal,baseline,polls=prekill_gate.status()
        if status~=prekill_status then
            local hex=require("prekill_gate").handle_hex
            mod.log("new-pikmin prekill status="..status.." nonce="..tostring(prekill_nonce).." world="..game.world()
                .." captain="..hex(captain).." victim="..hex(victim).." kind="..prekill_kind
                .." color=19 leaf=2 baseline="..tostring(baseline).." polls="..polls
                .." readiness_only=true parent_baseline_and_png_required=true")
            prekill_status=status
        end
        if confirmed==nil then mod.log("new-pikmin prekill refused "..tostring(refusal or why)) stepping:cancel() return end
        if not confirmed then return end
        local fresh,reason=prekill_gate.revalidate()
        if fresh~=true then mod.log("new-pikmin prekill refused fresh "..tostring(reason)) stepping:cancel() return end
        death_listener=combat.on_pikmin_died(function(event) if event.pikmin==victim then deaths[#deaths+1]=event end end)
        check.run("killed",function() verify(victim,19,"flower",captain) check.result("ok","counted customtype bombdeath",combat.kill_pikmin(victim,"bomb",captain)) end)
        state,since=4,age return
    end
    if (actor_api.alive(victim) or #deaths<1) and age-since<90 then return end
    check.run("death",function()
        check.equal(actor_api.alive(victim),false,"custom victim leaves play")
        check.equal(#deaths,1,"one native counted death")
        local event=deaths[1]
        check.equal(event.pikmin,victim,"captured full victim identity")
        check.equal(event.attacker,captain,"captured captain attribution")
        check.equal(event.kind,prekill_kind,"captured native entity kind")
        check.equal(event.cause,"bomb","counted native bomb cause")
    end)
    death_listener:cancel()
    local groups=grouped()
    check.run("survivors",function()
        check.equal(#groups[19],1,"one survivor of first customtype") check.equal(#groups[21],2,"two survivors of second customtype")
        for _,color in ipairs({19,21}) do for _,a in ipairs(groups[color]) do verify(a,color,nil,captain) end end
    end)
    mod.log("new-pikmin ready phase=spawn native19=1 native21=2; save natively and reload with phase.txt=reload")
    state,since=5,age
end)
