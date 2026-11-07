local actor_api = entity
local captain_api = captain
local file_api = file

-- Explicit isolated setup; every original leader and the steered character are read back after cleanup.
local marker_ready=false
local target={}
for line in (file_api.read("targets.txt",0,128) or ""):gmatch("[^\r\n]+") do target[#target+1]=tonumber(line) end
local waiting_limit,keeping_limit,cleanup_limit=target[1],target[2],target[3]
local phase,steps,born,cleanup_started="waiting",0,0,nil
local captain,dog,original,stepping
local crew,changed,told={},{},{}
local function say(key,note)
    local text="route dayplay " .. key .. (note and " " .. tostring(note) or "")
    if told[text] then return end
    told[text]=true
    mod.log(text)
end
local function complete(note)
    say("complete",note)
    stepping:cancel()
end
local function cleanup(note)
    if note then say("failed",note) end
    phase,cleanup_started="cleanup",steps
end
-- Reset a stale marker once before changing the world, through this mod's saved data.
local function prepare_marker()
    if marker_ready then return true end
    local written,why=storage.write("finish.txt","waitxx\n")
    if not written then say("refused-marker","write " .. tostring(why)) return false end
    local value,problem=storage.read("finish.txt",0,16)
    if value~="waitxx\n" then say("refused-marker","readback " .. tostring(problem or value)) return false end
    marker_ready=true
    say("marker-ready","save data finish.txt 7 bytes waitxx\\n")
    return true
end
if not waiting_limit or not keeping_limit or not cleanup_limit or waiting_limit<1 or keeping_limit<1 or cleanup_limit<1 then
    say("failed","invalid targets")
else
    stepping=time.on_step(function()
        steps=steps+1
        if phase=="waiting" then
            if steps>=waiting_limit then say("failed","public play readiness timed out") complete("no setup made") return end
            if game.has_control()~=true then say("waiting","player control") return end
            if not prepare_marker() then return end
            local found,why=player.captain(0)
            if not found then say("refused","captain " .. tostring(why)) return end
            local partner,problem=player.dog(0)
            if not partner then say("refused","dog " .. tostring(problem or "none")) return end
            local current,error=player.character(0)
            if not current then say("refused","character " .. tostring(error)) return end
            if current~=found and current~=partner then say("failed","the steered actor is neither captain nor dog") complete("no setup made") return end
            local members,missing=squad.members(partner)
            local own,own_missing=squad.members(found)
            if not members or not own then say("refused","squad " .. tostring(missing or own_missing)) return end
            if #members==0 and #own==0 then say("waiting","a stock captain or dog squad") return end
            local snapshot={}
            for _,actor in ipairs(members) do
                local leader,absent=squad.leader(actor)
                if leader~=partner then say("refused","stock dog member leader " .. tostring(absent or "changed")) return end
                snapshot[#snapshot+1]={actor=actor,leader=leader}
            end
            captain,dog,original,crew=found,partner,current,snapshot
            say("recorded","character " .. tostring(actor_api.kind(original)) .. " dog squad " .. #crew)
            phase="switching"
        end
        if phase=="switching" then
            if steps>=waiting_limit then cleanup("captain switch or cursor readiness timed out") return end
            local current,why=player.character(0)
            if current~=captain then
                local switched,problem=player.set_character(0,captain)
                if not switched then say("refused","switch " .. tostring(problem)) else say("switch-requested","captain") end
                return
            end
            say("switched","captain readback")
            local cursor,cursor_why=captain_api.cursor(captain)
            local whistle,whistle_why=captain_api.whistle(captain)
            if not cursor or not whistle then say("refused","captain readiness cursor=" .. tostring(cursor_why or "ok") .. " whistle=" .. tostring(whistle_why or "ok")) return end
            for _,entry in ipairs(crew) do
                local joined,problem=squad.join(entry.actor,captain)
                if not joined then cleanup("joining stock member " .. tostring(problem)) return end
                changed[#changed+1]=entry
                if squad.leader(entry.actor)~=captain then cleanup("joined member leader readback") return end
            end
            local members,problem=squad.members(captain)
            if not members or #members==0 then cleanup("captain squad readback " .. tostring(problem or "empty")) return end
            say("ready","captain cursor whistle squad " .. #members)
            phase,born="keeping",steps
            return
        end
        if phase=="keeping" then
            local finish,marker_error=storage.read("finish.txt",0,16)
            if not told["marker-observed"] then
                say("finish-marker",finish and (#finish .. " bytes " .. finish:gsub("\n","\\n")) or tostring(marker_error or "none"))
                told["marker-observed"]=true
            end
            if finish=="finish\n" then
                say("finish-requested","runner read the SDK receipt")
                cleanup()
            elseif steps-born>=keeping_limit then
                say("timed-out","runner did not request cleanup")
                cleanup()
            end
            return
        end
        if phase=="cleanup" then
            local restored=true
            for _,entry in ipairs(changed) do
                if squad.leader(entry.actor)~=entry.leader then
                    local joined,problem=squad.join(entry.actor,entry.leader)
                    if not joined then say("refused","restore member " .. tostring(problem)) end
                    if squad.leader(entry.actor)~=entry.leader then restored=false end
                end
            end
            local current,why=original and player.character(0)
            if original and current~=original then
                local switched,problem=player.set_character(0,original)
                if not switched then say("refused","restore character " .. tostring(problem)) end
                restored=false
            end
            if restored then complete("original leaders and character restored") return end
            if steps-cleanup_started>=cleanup_limit then
                say("failed","restoration timed out; original leader or character readback missing")
                stepping:cancel()
            end
        end
    end)
end
