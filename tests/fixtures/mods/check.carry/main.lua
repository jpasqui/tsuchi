local actor_api = entity
local engine_actor = actor

-- Added Pikmin, up to three and no more than the object holds, are ordered to carry the carryable object
-- nearest the first captain. The check compares carry's carriers, carried_by and info with the workers it
-- ordered, then removes its workers. An object still on its plant is knocked down first, after which the
-- workers stand free, so a worker that is not carrying is ordered again every two seconds. A light object
-- needs fewer carriers than it holds, so the answers are read once one worker carries, and must agree with
-- each other. The object may be carried away or delivered, so this runs only on an isolated save.
local check=require("check")
check.plan("found","info","added","ordered","carriers","carried_by","refused","removed")
local captain,kind,before,object,stepping
local wanted=3
local workers={}
local phase,steps,since=0,0,0
local function wait_for(name,why) check.run(name,function() check.wait(why) end) end
local function distance(a,b) local dx,dy,dz=a.x-b.x,a.y-b.y,a.z-b.z return math.sqrt(dx*dx+dy*dy+dz*dz) end
local function remove_owned()
    check.run("removed",function()
        for _,worker in ipairs(workers) do
            if engine_actor.alive(worker) then check.result("ok","quietly removing an added worker",actor_api.destroy(worker)) end
        end
    end)
    stepping:cancel()
end
local function is_worker(actor) for _,worker in ipairs(workers) do if worker==actor then return true end end return false end
local function listed_has(listed,actor) for _,each in ipairs(listed) do if each==actor then return true end end return false end

stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait_for("found","a loaded game with player control") end
        captain=player.captain(0)
        if not captain then return wait_for("found","the first player's captain") end
        local at=actor_api.pose(captain)
        local listed=pikmin.list()
        local info=listed and listed[1] and pikmin.info(listed[1])
        if not at or not info then return wait_for("found","a living Pikmin of a named type and the captain's position") end
        local best,best_distance
        for _,candidate in ipairs(carry.list() or {}) do
            local pose=actor_api.pose(candidate)
            local carried=carry.info(candidate)
            if pose and carried and carried.weight>=1 and carried.weight<=10 and carried.count==0 then
                local d=distance(pose.location,at.location)
                if not best_distance or d<best_distance then best,best_distance=candidate,d end
            end
        end
        if not best then return wait_for("found","an uncarried object of weight 1 to 10") end
        object,kind,before=best,info.kind,{}
        wanted=math.min(3,carry.info(best).maximum)
        for _,actor in ipairs(listed) do before[actor]=true end
        check.run("found",function()
            check.equal(engine_actor.alive(object),true,"the object is in play")
            check.expect(best_distance<3000,"the object is within 30 meters of the captain")
        end)
        check.run("info",function()
            local carried=carry.info(object)
            check.expect(carried~=nil,"the object's carry information is read")
            check.expect(carried.maximum>=carried.weight,"its maximum strength is at least its weight")
            check.equal(carried.count,0,"nobody carries it yet")
            check.equal(#(carry.carriers(object) or {"?"}),0,"its carrier list is empty")
        end)
        check.run("added",function() check.result("ok","adding workers of a living type",pikmin.add(captain,kind,"leaf",wanted)) end)
        phase,since=1,steps
        return
    end
    if phase==1 then
        local fresh={}
        for _,actor in ipairs(pikmin.list() or {}) do if not before[actor] then fresh[#fresh+1]=actor end end
        if #fresh<wanted and steps-since<90 then return end
        -- An added Pikmin takes orders once it follows its captain, as check.work waits for.
        local following=0
        for _,actor in ipairs(fresh) do
            local action=behavior.action(actor)
            if action and action~="" and squad.leader(actor)==captain then following=following+1 end
        end
        if following<#fresh and steps-since<180 then return end
        workers=fresh
        check.run("ordered",function()
            check.equal(#workers,wanted,"as many new Pikmin as were added")
            for _,worker in ipairs(workers) do check.result("ok","a work order on the object",pikmin.work_on(worker,object)) end
        end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        local listed=carry.carriers(object) or {}
        local held=0
        for _,carrier in ipairs(listed) do if is_worker(carrier) then held=held+1 end end
        if held<1 and steps-since<900 then
            if (steps-since)%60==0 then
                for _,worker in ipairs(workers) do
                    if carry.carried_by(worker)~=object then pikmin.work_on(worker,object) end
                end
            end
            return
        end
        check.run("carriers",function()
            local seen={}
            for _,carrier in ipairs(listed) do
                check.expect(not seen[carrier],"each carrier appears once")
                seen[carrier]=true
            end
            check.expect(held>=1,"an ordered worker is listed as a carrier")
            local carried=carry.info(object)
            check.expect(carried and carried.count>=held,"the carrier count includes the listed workers")
        end)
        check.run("carried_by",function()
            local carrying=0
            for _,worker in ipairs(workers) do
                local by=carry.carried_by(worker)
                if by==object then carrying=carrying+1 end
                check.equal(by==object,listed_has(listed,worker),"a worker carries the object exactly when it is listed")
            end
            check.expect(carrying>=1,"a worker carries the object")
            check.equal(carry.carried_by(captain),nil,"the captain carries nothing")
        end)
        check.run("refused",function()
            check.result("unsupported","a captain has no carry component",carry.info(captain))
            check.result("unsupported","nor carriers of its own",carry.carriers(captain))
        end)
        remove_owned()
    end
end)
