local function run(scenario)
    local state={play=false,character="dog",marker=false,active=true,logs={},leaders={one="dog",two="dog"},switches=0,joins=0,tries=0}
    state.saved="finish\n"
    state.storage_busy=scenario=="storage_busy"
    file={read=function(name) if name=="targets.txt" then return "30\n20\n20\n" end assert(name=="finish.txt") return state.marker and "finish\n" or nil end}
    storage={write=function(name,value)
        assert(state.play and name=="finish.txt" and value=="waitxx\n")
        if state.storage_busy then return nil,"busy" end
        state.saved=value state.writes=(state.writes or 0)+1 return true
    end,read=function(name,offset,most) assert(state.play and name=="finish.txt" and offset==0 and most==16) return state.marker and "finish\n" or state.saved end}
    mod={log=function(text) state.logs[#state.logs+1]=text end}
    time={on_step=function(fn) state.step=fn return {cancel=function() state.active=false end} end}
    game={has_control=function() return state.play end}
    entity={kind=function(actor) return actor=="captain" and "AVATAR" or "DOG" end}
    player={captain=function(player) assert(player==0 and state.play) return "captain" end,
        dog=function(player) assert(player==0 and state.play) return "dog" end,
        character=function(player) assert(player==0 and state.play) return state.character end,
        set_character=function(player,actor)
            assert(player==0 and (actor=="captain" or actor=="dog"))
            state.tries=state.tries+1
            if scenario=="locked" then return nil,"not_ready" end
            if state.tries==1 then return nil,"busy" end
            if state.tries==2 then return nil,"not_ready" end
            if not state.pending then state.pending,state.delay=actor,2 state.switches=state.switches+1 end
            return true
        end}
    captain={cursor=function(actor) assert(actor=="captain") if state.character~="captain" then return nil,"not_ready" end return {x=1,y=2,z=3} end,
        whistle=function(actor) assert(actor=="captain") return {active=false} end}
    squad={members=function(actor) local members={} for _,name in ipairs({"one","two"}) do if state.leaders[name]==actor then members[#members+1]=name end end return members end,
        leader=function(actor) return state.leaders[actor] end,
        join=function(actor,leader)
            assert(state.leaders[actor] and (leader=="dog" or leader=="captain"))
            if scenario=="join_refused" and actor=="two" and leader=="captain" then return nil,"not_ready" end
            state.leaders[actor]=leader state.joins=state.joins+1 return true
        end}
    dofile("tests/routes/dayplay/main.lua")
    local function steps(count)
        for i=1,count do
            if state.pending then state.delay=state.delay-1 if state.delay==0 then state.character,state.pending=state.pending,nil end end
            if state.active then state.step() end
        end
    end
    steps(3)
    assert(state.switches==0 and state.joins==0 and state.active)
    state.play=true
    if scenario=="storage_busy" then
        steps(1)
        assert(state.switches==0 and state.joins==0 and state.active)
        state.storage_busy=false
    end
    if scenario=="locked" then
        steps(60)
        assert(not state.active and state.character=="dog" and state.joins==0)
    elseif scenario=="join_refused" then
        steps(30)
        assert(not state.active and state.character=="dog" and state.leaders.one=="dog" and state.leaders.two=="dog")
    else
        steps(9)
        assert(state.active and state.character=="captain" and state.leaders.one=="captain" and state.leaders.two=="captain")
        assert(state.switches==1 and state.joins==2)
        state.marker=true
        steps(8)
        assert(not state.active and state.character=="dog" and state.leaders.one=="dog" and state.leaders.two=="dog")
        assert(state.switches==2 and state.joins==4)
    end
    assert(state.writes==1 and state.saved=="waitxx\n")
    local log=table.concat(state.logs,"\n")
    assert(log:find("route dayplay refused switch not_ready",1,true))
    if scenario=="locked" then assert(log:find("readiness timed out",1,true))
    elseif scenario=="join_refused" then assert(log:find("joining stock member not_ready",1,true))
    else assert(log:find("route dayplay ready captain cursor whistle squad 2",1,true)) end
    assert(log:find("route dayplay complete original leaders and character restored",1,true))
end
run("ready")
run("locked")
run("join_refused")
run("storage_busy")
return "day captain setup and exact restoration pass"
