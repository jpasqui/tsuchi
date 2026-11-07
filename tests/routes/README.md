# Optional setup routes

These mods set up isolated game runs through the public SDK. Ordinary check
staging does not install them, and they declare no check results.

## A stock Chappy for the animation check

The original actor class is
`/Game/Carrot4/Placeables/Teki/GChappy.GChappy_C`. The clip used by `check.anim` is the compatible stock AS_Chappy_Wait object.

When an isolated night or kept-save route lacks a Chappy, stage the requested
checks first, then run `python tools/stage_route.py chappy`. This explicitly
adds `route.chappy` to `build/test-mods/romfs/tsuchi/mods`. Its class asset and
spawned actor belong to it. It waits for player control and the first captain,
loads and verifies the original class path, spawns its own Chappy and holds
its decisions and movement quiet. It logs `route chappy ready owned CHAPPY`
with the exact class path. It never changes a level actor.

Keep the setup actor until `check.anim` has ended all twelve checks in the
SDK log. Then write `finish\n` to
`tsuchi/route.chappy/data/finish.txt` in the active isolated save. The next
world step releases the setup holds and requests quiet removal. A later step
logs `route chappy complete owned actor gone` after the public alive read says
false, and releases the class asset. The marker coordinates cleanup explicitly;
another mod's spawn or destruction cannot remove the setup actor early.

The setup waits at most 9000 updates for play/class readiness, and 3000
updates after making the actor for the runner's finish marker. A timeout is
logged as such and cleans up the setup; it does not count as an animation pass.
Stopping the setup mod also ends its owned actor and holds through the framework.

## Captain readiness

After staging the requested checks, `python tools/stage_route.py captain` adds
the optional read-only `route.captain` diagnostic. It logs mode, world, control,
each player's captain, dog and steered character kinds and classes, captain
cursor and whistle result names, mount identity, captain and dog squad sizes,
component classes and AvatarComponent membership. It reports changes every
30 updates with at most 200 log lines, then ends after 18000 updates.

This distinguishes an empty mounted captain squad from the dog's squad, and
missing captain components from a cursor that is not ready. It never changes
characters, squads or mounting, and does not declare acceptance checks.

## A day route that steers the dog

`python tools/stage_route.py dayplay` explicitly adds `route.dayplay` to an
isolated staged run. It records the actual steered character and every member
and leader of the stock dog's squad, requests the normal public switch to the
actual first captain, waits for character, cursor and whistle readback, and
joins the recorded squad members to that captain. It logs `route dayplay ready`
after the captain's squad is nonempty. Locked, pending or absent data keeps its
actual result name in a `refused` log line; readiness has a timeout.

Keep this setup until the SDK log reports all captain, squad and
animation checks complete. The Chappy route can provide the separate stock
animation prerequisite. Write `finish\n` to the active staged
`tsuchi/route.dayplay/data/finish.txt` in the active isolated save. It restores each changed member to
its recorded leader and requests the original steered character, then logs
`route dayplay complete original leaders and character restored` only after
both readbacks agree. No save progress is written. Targets bound readiness,
retention and cleanup; a timeout or restoration refusal is a failed setup,
and does not pass missing checks.

`motiontrace` is a read-only observer for an isolated motion run. It logs the captain and CHAPPY positions, public velocity, the engine location readback, and reflected body simulation and collision values. It observes at most four actors for 360 updates and writes at most 600 lines; it creates no actor and changes no game state. Stage it after `motion` and, when needed, `chappy`. Its log is diagnostic evidence, not a check result.

Before changing actors or squads, `chappy` and `dayplay` write and read back the inert seven bytes `waitxx\n` through their public `storage` service. This resets an old completion marker in their own save data. Busy or unavailable storage keeps the setup waiting without a world mutation. Each logs `marker-ready` after readback. After the selected SDK checks have ended, the runner verifies the active task, module and isolated save, then replaces the existing save-side file with the seven bytes `finish\n`. The route opens and closes that file through `storage.read` and logs `finish-requested` when it observes the replacement. Retain that observation separately from timeout cleanup; successful restoration alone does not prove a finish marker was observed. Mounted ROMFS files are not a writable cleanup channel.
