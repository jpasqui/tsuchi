-- Optional cold-save reentry approach. Only the captain is warped as route setup.
-- Native Talk, expedition menus, saving, and same-ID persistence remain independent.
local steps, lines, phase, asked_at, target, cap, doctor, stepping = 0, {}, "wait"
local function log(text)
    if #lines==600 then table.remove(lines,1) end
    lines[#lines+1]=tostring(steps).." night-reentry "..text
    print(text)
    storage.write("reentry-observations",table.concat(lines,"\n").."\n")
end
local function finish(text)
    phase="done"
    log(text)
    if stepping then stepping:cancel() end
end
local function position(one)
    local transform=one and entity.pose(one)
    local at=transform and transform.location
    if not at then return end
    for _,key in ipairs({"x","y","z"}) do
        local value=at[key]
        if type(value)~="number" or value~=value or math.abs(value)>=10000000 then return end
    end
    return at
end
log("observer started; ordinary night UI and reload proof remain manual")
stepping=time.on_step(function()
    steps=steps+1
    if phase=="done" then return end
    if steps>=6000 then return finish("BLOCKED bounded camp approach ended; no reload claim") end
    if steps<2400 or steps%30~=0 then return end
    if game.world()~="Area500_P" or game.has_control()~=true then return end
    if phase=="readback" then
        local at=position(cap)
        if at then
            log(string.format("captain readback %.3f %.3f %.3f",at.x,at.y,at.z))
            if math.abs(at.x-target.x)<=40 and math.abs(at.y-target.y)<=40 and math.abs(at.z-target.z)<=100 then
                return finish("READY captain approach read back; native Talk/A and night menus remain manual; no persistence claim")
            end
        end
        if steps-asked_at>=180 then return finish("BLOCKED captain approach readback timed out") end
        return
    end
    cap=player.captain(0)
    if not cap or player.character(0)~=cap or captain.riding(cap) then return finish("BLOCKED no unmounted controlled captain") end
    local found={}
    for _,one in ipairs(entity.list() or {}) do
        local ai=reflection.get(one,"NpcAI")
        if ai then
            local key=reflection.get(ai,"NpcInfoKey")
            local role=reflection.get(ai,"AIParameter.NpcRoleType")
            local ai_class=reflection.class_of(ai)
            -- Captured ENpcRoleType::Doctor is 9; both key and role must agree.
            if key=="SV005" and role==9 and ai_class and reflection.name(ai_class)=="NpcAIComponent" then
                found[#found+1]=one
            end
        end
    end
    if #found~=1 then return finish("BLOCKED exact SV005/Doctor9 NPC count "..tostring(#found)) end
    doctor=found[1]
    local at=position(doctor)
    local class=reflection.class_of(doctor)
    local path=class and reflection.path(class)
    if not at or not path or path=="" or not position(cap) then return finish("BLOCKED Doctor/captain position or Doctor class path unreadable") end
    log("Doctor actor "..tostring(doctor).." name "..tostring(reflection.name(doctor))..
        " class "..path.." NpcInfoKey SV005 NpcRoleType 9 control true")
    log(string.format("Doctor position %.3f %.3f %.3f",at.x,at.y,at.z))
    target={x=at.x+150,y=at.y,z=at.z+50}
    phase,asked_at="readback",steps
    local okay,why=entity.warp(cap,target,{pitch=0,yaw=180,roll=0})
    log("controlled captain-only approach request "..tostring(okay).." "..tostring(why))
    if okay~=true then return finish("BLOCKED captain approach refused; no second request") end
end)
