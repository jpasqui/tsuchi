-- Shows that a press reaches the game, and that a hidden button doesn't. It presses + for the first player
-- while + is hidden, then presses + again with nothing hidden, which should open the game's menu. It leaves the
-- menu open for ten seconds for a screenshot, then presses + to close it.
local check = require("check")
check.plan("hidden-ignored", "shown-opened", "closed")

local PLUS = {plus = true}
local phase, age, listening = "waiting", 0, nil

local function state()
    local paused = game.paused()
    local kinds = {}
    for name in pairs(paused or {}) do kinds[#kinds + 1] = tostring(name) end
    table.sort(kinds)
    return game.has_control() == true, #kinds > 0, table.concat(kinds, ",")
end

local function note(what)
    local control, paused, kinds = state()
    mod.log("proof input " .. what .. " step " .. age .. " control " .. tostring(control) .. " paused " .. tostring(paused) ..
            " kinds " .. (kinds ~= "" and kinds or "none"))
end

listening = time.on_step(function()
    age = age + 1
    if phase == "waiting" then
        -- Ten seconds of control first, so that another mod's checks are over.
        if game.has_control() ~= true or not input.pad(0) then age = 0 end
        if age < 300 then return end
        age = 0
        note("ready")
        check.result("ok", "hiding +", input.hold_hidden(0, PLUS))
        check.result("ok", "pressing + while it is hidden", input.hold_pressed(0, PLUS, 3))
        phase = "hidden"
    elseif phase == "hidden" and age == 45 then
        note("after hidden press")
        local control, paused = state()
        check.run("hidden-ignored", function()
            check.equal(control, true, "the player still has control")
            check.equal(paused, false, "nothing is paused")
        end)
        input.release_hidden(0)
        phase = "shown"
    elseif phase == "shown" and age == 75 then
        note("pressing + with nothing hidden")
        check.result("ok", "pressing +", input.hold_pressed(0, PLUS, 3))
    elseif phase == "shown" and age == 120 then
        note("after shown press")
        local control = state()
        check.run("shown-opened", function()
            check.equal(control, false, "the menu took control from the player")
            return "screenshot now"
        end)
        phase = "open"
    elseif phase == "open" and age == 420 then
        note("pressing + to close")
        input.hold_pressed(0, PLUS, 3)
    elseif phase == "open" and age == 480 then
        note("after closing press")
        local control = state()
        check.run("closed", function() check.equal(control, true, "the player has control again") end)
        listening:cancel()
    end
end)
