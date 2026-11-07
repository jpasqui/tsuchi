local function run(scenario)
    local s={frame=0,logs={},actors={},active=true,cursor_calls=0,join_calls=0,join_attempts=0}
    local chief={color=-1}
    local function add(color,leaf,count)
        for i=1,count do s.actors[#s.actors+1]={color=color,leaf=leaf,leader=chief} end
    end
    add(0,"flower",1)
    mod={log=function(text) s.logs[#s.logs+1]=text end}
    local check=dofile("sdk/check/check.lua")
    require=function(name) assert(name=="check") return check end
    file={read=function(name) if name=="phase.txt" then return "spawn\n" end return string.rep("a",32) end}
    time={on_step=function(fn) s.step=fn return {cancel=function() s.active=false end} end}
    game={has_control=function() return true end,mode=function() return "story" end,world=function() return "Cave000_F00_P" end}
    player={captain=function() return chief end,character=function() return chief end}
    actor={alive=function() return true end}
    captain={riding=function() return nil end,hold_cursor=function()
        s.cursor_calls=s.cursor_calls+1 return nil,"not_ready"
    end}
    entity={pose=function() return {location={x=0,y=0,z=0}} end}
    pikmin={list=function() return s.actors end,
        add=function(who,kind,leaf,count)
            assert(who==chief)
            add(kind=="route.new-pikmin:amber" and 19 or kind=="route.new-pikmin:azure" and 21 or 0,leaf,count)
            return true
        end,
        info=function(a) return {kind=a.color==19 and "route.new-pikmin:amber" or a.color==21 and "route.new-pikmin:azure" or "red",leaf=a.leaf,in_army=true} end}
    reflection={get=function(a,key)
        if key=="PikminColor" then return a.color end
        if key=="CarrotMoveComponent" then return a end
        assert(key=="CarrotMoveParameter.MaxSpeed") return a.color==19 and 117 or 223
    end}
    asset={load=function() return "parameters" end,release=function() end}
    datatable={get=function(_,row,field)
        if field=="PikminColor" then return row=="Tsuchi19" and 19 or 21 end
        return row=="Tsuchi19" and 117 or row=="Tsuchi21" and 223 or 90
    end}
    squad={members=function(who)
        local out={} for _,a in ipairs(s.actors) do if a.leader==who then out[#out+1]=a end end return out
    end,leader=function(a) return a.leader end,
        leave=function(a)
            s.leave_frame=s.frame
            if scenario=="refused" then return nil,"not_ready" end
            s.pending=a
            assert(a.leader==chief) -- The native request has not applied yet.
            return true
        end,
        join=function(a,who)
            s.join_attempts=s.join_attempts+1
            assert(a.leader==nil and s.frame==s.leave_frame+1)
            a.leader=who s.join_frame=s.frame s.join_calls=s.join_calls+1 return true
        end}
    dofile("tests/routes/new-pikmin/main.lua")
    for i=1,5 do
        s.frame=i
        if s.pending and scenario~="stuck" then s.pending.leader=nil s.pending=nil end
        if s.active then s.step() end
    end
    local log=table.concat(s.logs,"\n")
    if scenario=="ready" then
        assert(s.join_attempts==1 and s.join_calls==1 and s.join_frame==s.leave_frame+1 and s.cursor_calls>0)
        assert(log:find("check squad waiting the first custom member's next update",1,true))
        assert(log:find("check squad pass",1,true) and not log:find("check squad fail",1,true))
    else
        assert(s.join_attempts==0 and s.join_calls==0 and s.cursor_calls==0 and not s.active)
        assert(log:find("check squad fail",1,true) and not log:find("check squad pass",1,true))
        if scenario=="refused" then assert(log:find("leave first custom member: got not_ready",1,true))
        else assert(log:find("it leaves on its next update",1,true)) end
    end
end
run("ready")
run("refused")
run("stuck")
return "added Pikmin next-update departure and immediate rejoin pass"
