-- The mod that check.restart restarts. While check.restart runs, each revision sets the first captain's
-- CustomTimeDilation to its own value at every world step. Revision A is the one installed. The others are in
-- tests/fixtures/restart, and tools/drive_restart_check.py writes them into the development folder on the SD card.
-- Revision C does not compile: the restart keeps revision B running.
local REVISION, DILATION = "C", 2.0
print("restart target revision " .. REVISION .. " started"
