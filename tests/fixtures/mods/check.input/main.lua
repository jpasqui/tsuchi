-- Reads the first player's controller, presses a button for it between two steps, hides that button from the
-- game, and lets both go. The button is the right stick pressed in, which does little in play.
local check = require("check")
check.plan("read", "pressed", "hidden", "released", "refused", "key")

local BUTTON = "right_stick"
local function has(flags, name) return type(flags) == "table" and flags[name] == true end
local function only(name) return {[name] = true} end

local phase = "waiting"   -- then "press", "pressing", "hiding", "releasing", and "done"
local taken, taken_why    -- what the press between two steps answered
local key_done = false
local steps, pictures

local function finish()
    phase = "done"
    if steps then steps:cancel() end
    if pictures then pictures:cancel() end
end

local function read(pad)
    check.run("read", function()
        check.equal(type(pad.connected), "boolean", "connected")
        for _, name in ipairs({"held", "pressed", "released"}) do
            check.equal(type(pad[name]), "table", name)
        end
        for _, axis in ipairs({"left_x", "left_y", "right_x", "right_y"}) do
            local value = pad[axis]
            check.expect(math.type(value) == "integer" and value >= -32768 and value <= 32767, axis .. " is a stick's axis")
        end
        check.expect(not has(pad.held, BUTTON), "nothing holds the button before the check")
        check.equal(next(check.result("ok", "the presses of mods", input.pressed(0))), nil, "no mod presses a button")
        check.equal(next(check.result("ok", "the hidden buttons", input.hidden(0))), nil, "no mod hides a button")
    end)
end

local function refuse()
    check.run("refused", function()
        check.result("invalid", "a fifth player's controller", input.pad(4))
        check.result("invalid", "a press for a fifth player", input.hold_pressed(4, only(BUTTON), 1))
        check.result("invalid", "a press of no button", input.hold_pressed(0, {}, 1))
        check.result("invalid", "a press for no steps", input.hold_pressed(0, only(BUTTON), 0))
        check.result("invalid", "a press for 601 steps", input.hold_pressed(0, only(BUTTON), 601))
        check.result("invalid", "hiding for a fifth player", input.hold_hidden(4, only(BUTTON)))
        check.result("invalid", "hiding no button", input.hold_hidden(0, {}))
        check.result("invalid", "releasing a fifth player's press", input.release_pressed(4))
        check.result("invalid", "releasing a fifth player's hidden buttons", input.release_hidden(4))
        check.result("invalid", "a fifth player's presses", input.pressed(4))
        check.result("invalid", "a fifth player's hidden buttons", input.hidden(4))
        check.result("not_found", "releasing a press never taken", input.release_pressed(0))
        check.result("not_found", "releasing hidden buttons never taken", input.release_hidden(0))
    end)
end

local function keys()
    if key_done then return end
    key_done = check.run("key", function()
        local down, why = input.key(4)
        if down == nil and why == "not_ready" then check.wait("the system's keyboard") end
        check.equal(type(down), "boolean", "key A")
        check.equal(type(check.result("ok", "the space bar", input.key(44))), "boolean", "the space bar")
        check.equal(type(check.result("ok", "usage 255", input.key(255))), "boolean", "usage 255")
        check.result("invalid", "usage 256", input.key(256))
    end) ~= "waiting"
end

steps = time.on_step(function()
    if phase == "waiting" then
        local pad, why
        local ready = game.has_control() == true
        if ready then pad, why = input.pad(0) end
        if not pad then
            check.run("read", function()
                check.wait(ready and "the first player's controller, " .. tostring(why) or "a loaded game with player control")
            end)
            return
        end
        read(pad)
        refuse()
        keys()
        phase = "press"
    elseif phase == "pressing" then
        keys()
        local pad = input.pad(0)
        check.run("pressed", function()
            check.result("ok", "a press for two steps, taken between steps", taken, taken_why)
            check.equal(has(check.result("ok", "the presses of mods", input.pressed(0)), BUTTON), true, "the button is pressed")
            check.expect(pad ~= nil, "the controller is read")
            check.equal(has(pad and pad.held, BUTTON), true, "the next step holds the button")
            check.equal(has(pad and pad.pressed, BUTTON), true, "the next step has the button go down")
        end)
        check.result("ok", "hiding the button", input.hold_hidden(0, only(BUTTON)))
        phase = "hiding"
    elseif phase == "hiding" then
        keys()
        local pad = input.pad(0)
        check.run("hidden", function()
            check.equal(has(check.result("ok", "the hidden buttons", input.hidden(0)), BUTTON), true, "the button is hidden")
            check.equal(has(check.result("ok", "the presses of mods", input.pressed(0)), BUTTON), true, "the press goes on")
            check.equal(has(pad and pad.held, BUTTON), true, "the mod still sees the hidden button held")
            check.equal(has(pad and pad.pressed, BUTTON), false, "the button went down one step before")
            check.result("ok", "showing the button again", input.release_hidden(0))
            check.equal(next(check.result("ok", "the hidden buttons", input.hidden(0))), nil, "nothing is hidden")
            check.result("not_found", "showing it twice", input.release_hidden(0))
        end)
        phase = "releasing"
    elseif phase == "releasing" then
        keys()
        local pad = input.pad(0)
        check.run("released", function()
            check.equal(next(check.result("ok", "the presses of mods", input.pressed(0))), nil, "the press ended after two steps")
            check.result("not_found", "releasing a press that ended", input.release_pressed(0))
            check.equal(has(pad and pad.held, BUTTON), false, "the button is up")
            check.equal(has(pad and pad.released, BUTTON), true, "the button went up at this step")
            check.result("ok", "a press for five steps", input.hold_pressed(0, only(BUTTON), 5))
            check.equal(has(input.pad(0).held, BUTTON), true, "a press taken in a step holds the button in it")
            check.result("ok", "releasing it at once", input.release_pressed(0))
            check.equal(next(check.result("ok", "the presses of mods", input.pressed(0))), nil, "nothing is pressed")
            check.equal(has(input.pad(0).held, BUTTON), false, "the button is up again")
        end)
        if key_done then finish() else phase = "keys" end
    elseif phase == "keys" then
        keys()
        if key_done then finish() end
    end
end)

-- The press is taken between two steps, so the step after it is the first to see the button go down.
pictures = time.on_presented(function()
    if phase ~= "press" then return end
    taken, taken_why = input.hold_pressed(0, only(BUTTON), 2)
    phase = "pressing"
end)
