-- The mod that check.restart restarts. While check.restart runs, each revision sets the first captain's
-- CustomTimeDilation to its own value at every world step. Revision A is the one installed. The others are in
-- tests/fixtures/restart, and tools/drive_restart_check.py writes them into the development folder on the SD card.
local REVISION, DILATION = "A", 0.5
print("restart target revision " .. REVISION .. " started")
time.on_step(function()
    if not mod.version_of("check.restart") then return end
    local captain = player.captain(0)
    if captain then reflection.set(captain, "CustomTimeDilation", DILATION) end
end)
