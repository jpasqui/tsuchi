-- Run explicitly. Edit both selectors, request reset, then edit them again.
local check = require("check")
check.plan("calls", "code-input", "data-input")
local options = '[{"id":"calm","label":"Calm"},{"id":"wild","label":"Wild"}]'
local code = check.result("ok", "create", ui.create("UI check"))
check.result("ok", "choice", ui.add_choice(code, "mode", "Mode", options, "calm"))
check.result("ok", "integer", ui.add_integer(code, "count", "Count", 0, 10, 1, 1))
check.result("ok", "text", ui.add_text(code, "note", "Note", "Code and data use stock widgets"))
for i = 0, 2 do check.result("ok", "extra", ui.add_text(code, "row-" .. i, "Extra row " .. (i + 1), "Read-only rows do not take focus")) end
check.result("ok", "button", ui.add_button(code, "done", "Finish this screen"))
local data = check.result("ok", "load", ui.load("screen.json"))
check.run("calls", function()
    check.equal(check.result("ok", "choice read", ui.choice(code, "mode")), "calm", "UI value")
    check.result("ok", "choice write", ui.set_choice(code, "mode", "wild"))
    check.equal(check.result("ok", "choice reread", ui.choice(code, "mode")), "wild", "UI value")
    check.result("ok", "choice restore", ui.set_choice(code, "mode", "calm"))
    check.equal(check.result("ok", "integer read", ui.integer(code, "count")), 1, "UI value")
    check.result("ok", "integer write", ui.set_integer(code, "count", 3))
    check.equal(check.result("ok", "integer reread", ui.integer(code, "count")), 3, "UI value")
    check.result("ok", "integer restore", ui.set_integer(code, "count", 1))
    check.result("ok", "text write", ui.set_text(code, "note", "Temporary"))
    check.equal(check.result("ok", "text read", ui.text(code, "note")), "Temporary", "UI value")
    check.result("ok", "text restore", ui.set_text(code, "note", "Code and data use stock widgets"))
    check.equal(check.result("ok", "open read", ui.open(code)), false, "UI value")
    check.equal(check.result("ok", "data choice", ui.choice(data, "mode")), "calm", "UI value")
    check.equal(check.result("ok", "data integer", ui.integer(data, "count")), 1, "UI value")
    check.equal(check.result("ok", "data text", ui.text(data, "note")), ui.text(code, "note"), "UI value")
    return "all16 calls finish with physical holds and destroy"
end)
local current, phase, opened = code, "code-input", false
local choice, integer, reset = false, false, false
ui.on_choice_changed(function(event)
    if event.screen == current then choice = event.choice ~= event.previous end
end)
ui.on_integer_changed(function(event)
    if event.screen == current then integer = event.integer ~= event.previous end
end)
ui.on_reset_requested(function(event)
    if event.screen ~= current then return end
    check.expect(choice and integer, "edit both selectors before requesting reset")
    check.result("ok", "reset choice", ui.set_choice(current, "mode", "calm"))
    check.result("ok", "reset integer", ui.set_integer(current, "count", 1))
    choice, integer, reset = false, false, true
    mod.log("UI proof reset handled; edit both selectors again before Finish")
end)
ui.on_activated(function(event)
    if event.screen ~= current or event.control ~= "done" then return end
    check.run(phase, function()
        check.expect(reset and choice and integer, "request reset, then edit choice and integer with physical input before Done")
        check.result("ok", "release", ui.release_open(current))
        check.result("ok", "destroy", ui.destroy(current))
        return "physical stock widgets, scrolling, passive text, and scoped reset"
    end)
    if current == code then current, phase, opened = data, "data-input", false; choice, integer, reset = false, false, false
    else current = nil end
end)
time.on_presented(function()
    if current and not opened then
        local result = ui.hold_open(current)
        if result then opened = true; mod.log("UI proof " .. phase .. " open") end
    end
end)
