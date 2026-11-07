-- The in-game check of mods written in Lua runs this and reads what it prints, one thing to a line.
local helper = require("lib.helper")
-- The framework says which version it is, as a whole number, and it is no older than this mod's manifest asks for.
assert(math.type(mod.framework()) == "integer" and mod.framework() >= 0)
print("started " .. mod.id() .. " " .. mod.version() .. " with " .. mod.version_of("check.library"))
print("helper " .. helper.double(21))

-- The whole language, as tests/lua/language.lua uses it: the same text here as where the checks were built.
-- Both files are put beside this one when the mods are laid out for the game.
local expected = file.read("language.txt", 0, file.size("language.txt"))
local ran, told = pcall(load(file.read("language.lua", 0, file.size("language.lua")), "=script")) -- its name there
print(ran and told == expected and "language same" or "language differs")
if not ran then
    print("differs in that it fails: " .. tostring(told))
elseif told ~= expected then -- the first line that is not the same, as it is here
    local theirs = expected:gmatch("[^\n]*")
    local number = 0
    for mine in told:gmatch("[^\n]*") do
        number = number + 1
        if mine ~= theirs() then
            print("differs at line " .. number .. ": " .. mine)
            break
        end
    end
end
print("caught " .. tostring(select(2, pcall(error, "this"))))

local frames, told_actors = 0, false
time.on_step(function()
    frames = frames + 1
    if frames == 30 then print("frames 30") end
    if not told_actors and frames % 30 == 0 then
        local actors = entity.list()
        if actors and #actors >= 100 then
            told_actors = true
            print("actors " .. #actors .. " in " .. game.world())
        end
    end
end)
