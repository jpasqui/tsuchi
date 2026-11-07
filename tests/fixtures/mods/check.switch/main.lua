local captain_api = captain

-- Switches the first player to its other real character and restores the original.
local check=require("check")
check.plan("found","listened","asked","changed","held","coalesced","ended","restored","returned","idempotent","quiet","cancelled")
local captain,dog,original,other,listener,stepping
local events={}
local phase,steps,since=0,0,0
local function wait_for(name,why) check.run(name,function() check.wait(why) end) end
stepping=time.on_step(function()
    steps=steps+1
    if phase==0 then
        if game.has_control()~=true then return wait_for("found","a loaded game with player control") end
        captain,dog,original=player.captain(0),player.dog(0),player.character(0)
        if not captain or not dog or not original or not actor.alive(captain) or not actor.alive(dog) then return wait_for("found","a real captain and dog in play") end
        local mount,why=captain_api.riding(captain)
        if mount or why then return wait_for("found","a captain that is not riding") end
        if original~=captain and original~=dog then return wait_for("found","the first player steering its captain or dog") end
        other=original==captain and dog or captain
        check.run("found",function() check.expect(original~=other,"the two real characters differ") end)
        check.run("listened",function()
            listener=check.result("ok","listening to character changes",player.on_character_changed(function(event)
                if event.player==0 then events[#events+1]=event end
            end))
        end)
        if not listener then stepping:cancel() return end
        phase=1
        return
    end
    if phase==1 then
        local accepted,why=player.set_character(0,other)
        if not accepted and (why=="busy" or why=="not_ready") then return wait_for("asked","the game's ordinary character switch: "..why) end
        check.run("asked",function() check.result("ok","switching to the player's other character",accepted,why) end)
        phase,since=2,steps
        return
    end
    if phase==2 then
        if (player.character(0)~=other or #events<1) and steps-since<90 then return end
        check.run("changed",function()
            check.equal(player.character(0),other,"the other character is steered")
            check.equal(#events,1,"one character change was heard")
            if events[1] then check.equal(events[1].previous,original,"the original character") check.equal(events[1].character,other,"the new character") end
        end)
        check.run("held",function() check.result("ok","holding the other character",player.hold_character(0,other)) end)
        check.run("coalesced",function() check.result("ok","holding it again",player.hold_character(0,other)) check.equal(player.character(0),other,"the held character") end)
        check.run("ended",function() check.result("ok","releasing the character",player.release_character(0)) check.result("not_found","releasing it twice",player.release_character(0)) end)
        phase=3
        return
    end
    if phase==3 then
        local accepted,why=player.set_character(0,original)
        if not accepted and (why=="busy" or why=="not_ready") then return wait_for("restored","restoring through the ordinary switch: "..why) end
        check.run("restored",function() check.result("ok","restoring the original character",accepted,why) end)
        phase,since=4,steps
        return
    end
    if phase==4 then
        if (player.character(0)~=original or #events<2) and steps-since<90 then return end
        check.run("returned",function()
            check.equal(player.character(0),original,"the original character is restored")
            check.equal(#events,2,"one event for each real switch")
            if events[2] then check.equal(events[2].previous,other,"the other character before returning") check.equal(events[2].character,original,"the original character after returning") end
        end)
        check.run("idempotent",function() check.result("ok","selecting the character already steered",player.set_character(0,original)) end)
        phase,since=5,steps
        return
    end
    if steps-since<3 then return end
    check.run("quiet",function() check.equal(#events,2,"selecting the current character emits no additional change") check.equal(player.character(0),original,"the original remains steered") end)
    check.run("cancelled",function() check.result("ok","ending the character listener",listener:cancel()) check.result("not_found","ending it twice",listener:cancel()) end)
    stepping:cancel()
end)
