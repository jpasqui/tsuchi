local actor_api = entity
local component_api = component

-- Read-only: observe actual CHAPPY bodies through public SDK and engine reflection.
local steps,born,lines=0,nil,0
local stepping
local known={}
local function vector(at)
    return at and string.format("%.3f,%.3f,%.3f",at.x or at.X,at.y or at.Y,at.z or at.Z) or "none"
end
local function say(text)
    if lines<600 then mod.log("motion trace "..text) lines=lines+1 end
end
stepping=time.on_step(function()
    steps=steps+1
    if not born then
        if game.has_control()~=true then
            if steps==1 then say("waiting player control") end
            if steps>=9000 then say("ended control readiness timeout") stepping:cancel() end
            return
        end
        born=steps
        local captain=player.captain(0)
        local at=captain and actor_api.pose(captain)
        say("begin world="..tostring(game.world()).." mode="..tostring(game.mode()).." stage="..tostring(game.stage()).." captain="..tostring(captain).." captain_position="..vector(at and at.location))
    end
    local age=steps-born
    if age>360 or lines>=600 then say("ended bounded observation") stepping:cancel() return end
    if age>=30 and age%5~=0 then return end
    local count=0
    for _,actor in ipairs(actor_api.list() or {}) do
        if actor_api.kind(actor)=="CHAPPY" then
            count=count+1
            if count<=4 then
                local at,at_error=actor_api.pose(actor)
                local velocity,velocity_error=actor_api.velocity(actor)
                if not known[actor] then
                    known[actor]=true
                    local class=reflection.class_of(actor)
                    say("actor age="..age.." handle="..tostring(actor).." name="..tostring(reflection.name(actor)).." placement="..tostring(actor_api.placement(actor)).." class="..tostring(class and reflection.path(class)))
                    for _,part in ipairs(component_api.list(actor) or {}) do
                        local sim,why=reflection.call(part,"IsSimulatingPhysics",{BoneName=""})
                        if sim~=nil and type(why)~="string" then
                            local collision,collision_error=reflection.call(part,"GetCollisionEnabled")
                            say("part handle="..tostring(actor).." name="..tostring(reflection.name(part)).." owner="..tostring(component_api.owner(part)).." simulating="..tostring(sim).." collision="..tostring(collision or collision_error))
                        end
                    end
                end
                local engine,engine_error=reflection.call(actor,"K2_GetActorLocation")
                local engine_problem=type(engine_error)=="string" and engine_error or "ok"
                say("sample age="..age.." handle="..tostring(actor).." position="..vector(at and at.location).." velocity="..vector(velocity).." engine="..vector(engine).." errors="..tostring(at_error)..","..tostring(velocity_error)..","..engine_problem)
            end
        end
    end
end)
