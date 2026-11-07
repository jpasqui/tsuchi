# The real cave portal route

This explicit test mod uses an isolated story save. It is not part of ordinary
SDK staging. Copy `mod.json` and `main.lua` into `romfs/tsuchi/mods/route.portal`.
Use a daytime area save. After frame 2400, the route waits for the same world and
player control to persist for 120 frames. If the world is another area, it calls
`story.travel("Area001_P", 0)` and waits for Area001 and player control. This
travel step sets up the route and does not count as portal entry or departure.

The route selects the real entrance whose `PortalTriggerComponent.ToLevelName`
is `Cave001_F00` or `Cave001_F00_P`. It approaches the entrance, calls
`story.enter_portal`, and
waits for the normal confirmation. Confirm with the game's ordinary input. The
route waits for Cave001_F00_P and player control to persist for 120 frames before
recording arrival. After another 600 frames, it calls `story.leave_cave` while the
cave and player control remain available, then waits for Area001 and control.
The captain and dog warps approach the selected native portal and provide route
setup. Do not send confirmation until a screenshot shows the native cave menu.

The bounded receipt is the mod's saved `log` entry. `RESULT PASS` requires both
observed transitions. Accepted requests alone are insufficient. Keep screenshots
of confirmation and both destinations, the installed production module hash, and
the run's independent world observations beside the game receipt. The host test
checks exact portal selection, setup travel, asynchronous waits, retries, and
refusal of a cave checkpoint or an unrelated world.
It does not establish a result in the game.
