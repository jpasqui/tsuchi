-- The check of `settings`: what the mod finds as it starts, another mod's settings, what is refused, changes that
-- show at once, and each change told on the next step.
local check = require("check")
check.plan("found", "public", "refused", "changed", "heard")

-- As the mod finds them, before it changes anything.
local before = {
    runs = settings.integer("runs"), speed = settings.number("speed"), mode = settings.text("mode"),
    greeting = settings.text("greeting"), loud = settings.toggle("loud"), boots = settings.has("extras", "boots"),
}
local found = string.format("%s %s %s %s %s %s", before.runs, before.speed, before.mode, before.greeting, before.loud, before.boots)

-- A save that holds nothing for the mod gives the defaults its manifest declares. A save that was prepared with a
-- file for it gives what the file holds. And a save the mod's last run wrote to gives what that run set.
check.run("found", function()
    if found == "0 1.5 calm hello true false" then return "fresh" end
    if found == "41 0.75 wild hi there false true" then return "prepared" end
    local kept = math.type(before.runs) == "integer" and before.runs >= 1 and before.speed == 2.5 and before.mode == "wild" and before.boots == true
    check.expect(kept, "what it found is not its defaults, nor what a prepared save holds, nor what its last run set: " .. found)
    return "kept"
end)

-- Another mod's: one that is public, and one that is not.
check.run("public", function()
    check.equal(check.result("ok", "another mod's public setting", settings.integer("check.library:volume")), 7, "its value")
    check.result("denied", "another mod's setting that is not public", settings.text("check.library:motto"))
    check.result("not_found", "a setting of a mod that is not running", settings.integer("check.absent:volume"))
end)

-- A value its setting does not allow, a setting that is another mod's, and one nobody declared.
check.run("refused", function()
    check.result("invalid", "a number above the most the setting takes", settings.set_number("speed", 9))
    check.result("invalid", "a word that is none of the choices", settings.set_text("mode", "loud"))
    check.result("denied", "changing another mod's setting", settings.set_integer("check.library:volume", 1))
    check.result("not_found", "a setting the manifest does not declare", settings.integer("undeclared"))
    check.equal(settings.number("speed"), before.speed, "the number after the refusal")
end)

-- Every change is told to whoever listens, the mod's own included, once, on the next step. A value set to what it
-- holds already is no change.
local heard, expected = {}, {}
settings.on_changed(function(event) heard[#heard + 1] = event.mod .. ":" .. event.key end)
local function changes(key, differs)
    if differs then expected[#expected + 1] = "check.settings:" .. key end
end

check.run("changed", function()
    check.result("ok", "counting the run", settings.set_integer("runs", before.runs + 1))
    changes("runs", true)
    check.result("ok", "a number", settings.set_number("speed", 2.5))
    changes("speed", before.speed ~= 2.5)
    check.result("ok", "one of the choices", settings.set_text("mode", "wild"))
    changes("mode", before.mode ~= "wild")
    check.result("ok", "a word added to a set", settings.set_has("extras", "boots", true))
    changes("extras", not before.boots)
    check.equal(settings.integer("runs"), before.runs + 1, "the count read back")
    check.equal(settings.number("speed"), 2.5, "the number read back")
    check.equal(settings.text("mode"), "wild", "the choice read back")
    check.equal(settings.has("extras", "boots"), true, "the word in the set")
    check.equal(#heard, 0, "changes told before the next step")
end)

local tries = 0
check.retry(function()
    return check.run("heard", function()
        tries = tries + 1
        if tries < 2 then check.wait("the step after the changes") end
        check.equal(table.concat(heard, " "), table.concat(expected, " "), "the changes told")
    end)
end, 1)
