-- The mod that check.restart restarts. While check.restart runs, each revision sets the first captain's
-- CustomTimeDilation to its own value at every world step. Revision A is the one installed. The others are in
-- tests/fixtures/restart, and tools/drive_restart_check.py writes them into the development folder on the SD card.
-- Revision D compiles and fails as it starts: the restart blocks the mod.
print("restart target revision D starts")
error("revision D stops as it starts")
