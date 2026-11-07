-- Restarts check.restart-target while the game runs. The framework restarts a mod whose development folder on the SD
-- card changes, and tools/drive_restart_check.py writes each next revision of the target's main.lua there. Each
-- revision that runs sets the first captain's CustomTimeDilation at every world step; this check reads the field
-- back through reflection, and through the engine's own GetActorTimeDilation.
--
-- First this mod restarts the target itself with mod.restart, with revision A, the one installed, in place. Then it
-- asks the tool for each next revision, "b" to "e", with a `waiting` line that names it, and takes the target's next
-- restart as that revision's. It has no development folder of its own, so nothing the tool writes restarts it.
local check = require("check")
check.plan("original", "refused", "edited", "syntax", "failed-start", "recovered")

local TARGET = "check.restart-target"
local step = 0
local heard = {}
mod.on_restarted(function(event)
    if event.mod == TARGET then heard[#heard + 1] = {outcome = event.outcome, problem = event.problem, at = step} end
end)

local captain, world, ratio, steps_with_control = nil, nil, nil, 0
local stage, asked_at, got, poked = "original", nil, nil, nil

-- The field, and the engine's time dilation for the actor.
local function dilation()
    local field = check.result("ok", "the captain's CustomTimeDilation", reflection.get(captain, "CustomTimeDilation"))
    local engine = check.result("ok", "the engine's GetActorTimeDilation", reflection.call(captain, "GetActorTimeDilation"))
    return field, engine
end

-- The same world as at the start: the world's name, the first captain's handle, and the step count going on.
local function same_world(note)
    check.equal(game.world(), world, "the loaded world")
    check.expect(player.captain(0) == captain, "the first captain is the same actor as at the start")
    mod.log("restart proof " .. note .. " step " .. step .. " world " .. tostring(game.world()))
end

-- The target's next restart and its event: one this mod asks for, or, given a letter, the one the framework makes
-- once the tool has written that revision into the target's development folder.
local function restarted(letter)
    if got then return got end
    if not asked_at then
        heard = {}
        asked_at = step
        if not letter then check.result("ok", "asking for the restart", mod.restart(TARGET)) end
    end
    if #heard == 0 then
        if letter then check.wait("revision " .. letter .. " on the SD card") end
        check.expect(step - asked_at <= 300, "a restarted event within 300 steps")
        check.wait("the restart with revision A")
    end
    if step - heard[1].at < 15 then check.wait("the new script's first steps") end
    check.equal(#heard, 1, "restarted events for the target")
    got = heard[1]
    return got
end

local function run(name, body, next_stage)
    if check.run(name, body) ~= "waiting" then stage, asked_at, got, poked = next_stage, nil, nil, nil end
end

time.on_step(function(event)
    step = event.number
    if game.has_control() ~= true or not player.captain(0) then steps_with_control = 0 return end
    steps_with_control = steps_with_control + 1
    if steps_with_control < 60 or stage == "done" then return end
    if stage == "original" then
        run("original", function()
            if not captain then captain, world = player.captain(0), game.world() end
            local event = restarted(nil)
            check.equal(event.outcome, "restarted", "the outcome of asking for a restart")
            check.equal(mod.state(TARGET), "running", "the target's state")
            check.equal(mod.problem(TARGET), "", "the target's problem")
            local field, engine = dilation()
            check.equal(field, 0.5, "the field that revision A sets")
            check.expect(type(engine) == "number" and engine > 0, "the engine gives a time dilation")
            ratio = engine / field
            same_world("original")
            return "A 0.5"
        end, "refused")
    elseif stage == "refused" then
        run("refused", function()
            check.result("not_found", "a mod that is not installed", mod.restart("check.restart-nobody"))
            check.result("not_found", "the state of a mod that is not installed", mod.state("check.restart-nobody"))
        end, "edited")
    elseif stage == "edited" then
        run("edited", function()
            local event = restarted("b")
            check.equal(event.outcome, "restarted", "the outcome")
            check.equal(mod.state(TARGET), "running", "the target's state")
            local field, engine = dilation()
            check.equal(field, 1.5, "the field that revision B sets")
            check.near(engine, field * ratio, 0.001, "the engine's time dilation")
            same_world("edited")
            return "B 1.5"
        end, "syntax")
    elseif stage == "syntax" then
        run("syntax", function()
            local event = restarted("c")
            check.equal(event.outcome, "kept", "the outcome")
            check.expect(event.problem:find(TARGET .. "/main.lua:", 1, true) ~= nil, "the problem names the file and the line: " .. event.problem)
            check.equal(mod.state(TARGET), "running", "the target's state")
            -- Revision B still runs: a value put in its place is set back at the next step.
            if not poked then
                check.result("ok", "a value put in place of revision B's", reflection.set(captain, "CustomTimeDilation", 0.25))
                poked = step
            end
            if step - poked < 5 then check.wait("revision B's handler") end
            check.equal((dilation()), 1.5, "the field that revision B sets again")
            same_world("syntax")
            return event.problem
        end, "failed-start")
    elseif stage == "failed-start" then
        run("failed-start", function()
            local event = restarted("d")
            check.equal(event.outcome, "blocked", "the outcome")
            check.expect(event.problem:find("revision D stops as it starts", 1, true) ~= nil, "the problem has the script's error: " .. event.problem)
            check.equal(mod.state(TARGET), "blocked", "the target's state")
            check.equal(mod.problem(TARGET), event.problem, "the target's problem")
            same_world("failed-start")
            return event.problem
        end, "recovered")
    elseif stage == "recovered" then
        run("recovered", function()
            local event = restarted("e")
            check.equal(event.outcome, "restarted", "the outcome")
            check.equal(mod.state(TARGET), "running", "the target's state")
            check.equal(mod.problem(TARGET), "", "the target's problem")
            local field, engine = dilation()
            check.equal(field, 1.0, "the field that revision E sets")
            check.near(engine, field * ratio, 0.001, "the engine's time dilation")
            same_world("recovered")
            return "E 1.0"
        end, "done")
    end
end)
