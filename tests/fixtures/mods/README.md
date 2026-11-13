# Mods for the framework's own checks

Each folder here is a mod as the framework sees one: a folder named by its ID, with a
`mod.json` in it, or without one where the check is that such a folder is reported and left
alone.

`tools/stage_test_mods.py` lays them out the way the game's file system shows them, under
`build/test-mods/romfs/tsuchi/mods/`. A folder with a `source` folder is a compiled mod: its
sources are built into its `mod.nro` with `sdk/build_mod.py`, and are not copied.

## Check mods

A folder whose manifest has `"meta": {"check": "<name>"}` is a *check mod*. It makes its checks
through the API, as any mod would, and writes what it finds to the log with the SDK's helper:
`sdk/check/tsuchi_check.h` for a compiled mod, and `sdk/check/check.lua` for a script, which
the layout puts into the mod's folder.

| Mod | Checks | Needs |
| --- | --- | --- |
| `check.ai-order` | Five territory values and the patrol value copied immediately before native BabyAI start, owner and current-component identity, and native behavior-tree construction afterward. | Select `ai-order` explicitly on a fresh game boot and load Cave000 with the authored larva placement list named by the private test setup. The framework's values wrapper is taken first (kHookFirst), so the observer's hook runs inside it whichever was installed first; since maps begin before the world loads, the wrapper is in place before the observer starts. `helped.ai-order` declares the replacement values. The stager authors the file and target offsets from the game table. |
| `check.storage` | `storage`: a piece kept, read, listed, replaced, refused and forgotten, and one left for the next run. | Nothing. It runs as it starts. On its second run on one save, `left` passes with `kept`. |
| `check.settings` | `settings`: what the mod finds as it starts, another mod's public and private settings, what is refused, changes that show at once, and each change told on the next step. | `check.library`, and one step. `found` passes with `fresh` on a save that holds nothing for it, with `kept` on its second run on one save, and with `prepared` on a save that holds the file below. |
| `check.time` | Consecutive step numbers, pictures with a fraction from zero through one and finite nonnegative elapsed seconds, and cancellation of both listeners. | Three world steps and a picture afterward. |
| `check.files` | Its own files read whole and in parts, listed and found; absent files and paths outside its folder. | Nothing. It runs as it starts. |
| `check.text` | A named text read, replaced twice and restored; a new key given text that stays after the replacement ends. | The game's text. `targets.txt` names the namespace, key and new key. |
| `check.tables` | Two tables held by their paths, a number, text and an asset reference changed and restored, fields listed, a row copied, invalid names refused, and assets released. | The named assets. `targets.txt` has eight lines: table path, row, number field, text field, references table path, reference row, reference field, replacement asset path. |
| `check.actors` | Actors listed, described, found by class and placement; a transform put back and compared with the engine's own location function; invalid arguments refused. | A loaded world with placed actors. |
| `check.game` | The mode, rule, stage, world, control and pauses read; the random stream restored; a return to Title with actual mode, stage, loaded and unloaded events and listener cancellation. | Select `game` explicitly on an isolated save in stable play. It ends at Title. |
| `check.players` | The players counted, their characters and controllers read, their current character and controller held and released, invalid arguments refused. | A loaded world with players. |
| `check.assets` | A named asset loaded repeatedly with one hold, its path read and used, its hold released once, invalid and missing paths refused; a part's nested path and another class loaded and found. | `targets.txt` names the asset, the other class's path, and its name. |
| `check.components` | Parts listed, their owner and name read; a part hidden and restored; a scene part added and removed. | An actor with a scene part. |
| `check.behavior` | Action names read for the actors in play; a change listener registered and cancelled. | An actor with decisions. |
| `check.combat` | Life read, put back and read again; invalid life and hit amounts refused. | An actor with positive life. |
| `check.story` | Day and clock read; the clock restored; one original treasure recorded; area travel, actual portal entry and cave departure checked through their loaded worlds. | Select `story` explicitly on an isolated daytime save. `targets.txt` names two original areas, an original portal actor class, its cave world and an original treasure row. Empty portal targets leave entry and exit waiting after area travel and release the save hold. Treasure collection changes only the isolated save; it does not prove Sparklium gain. |
| `check.hooks` | The engine's table entry wrapped by name and address and its patched function wrapped; each heard and removed; silence after removal, and absent names refused. | The matching patch and six world steps. The stager writes `targets.txt` from the entry and stock offsets in `game/table.yaml`. All wrappers use the engine update's own pointer, float, and bool arguments. |
| `check.hud` | A banner requested with its own text, and nonpositive durations refused. | The game's first-player announcement banner. |
| `check.audio` | A captain's named sound played, and absent or empty keys refused. | The first player's captain and the game's sound system. |
| `check.saves` | The active slot and its files read, a coalesced hold released, unused stand-in bytes prepared and forgotten, and invalid paths refused. | A loaded slot containing files. The prepared stand-in is never put to use. |
| `check.reflect` | Class membership and names, inherited fields, a boolean read and written unchanged, a scoped engine call with independently compared results, and expired or invalid requests refused. | The first player's captain and a scene part in the world. |
| `check.pause` | Two overlapping pause holds, their next-update flags, pause and control events, continued actor pause after one hold ends, and restoration after both end. | Stable play and seven world updates. The game keeps any pause kinds it already had. |
| `check.mods` | Own metadata and running version, missing IDs refused, and a public log line. | A running mod. |
| `check.material` | A mesh's original material held unchanged, read independently and restored, and invalid requests refused. | A mesh with an occupied material slot zero. |
| `check.collision` | One collision part held off and read through its engine call, a coalesced hold ended, and the original collision mode restored. | A part with a collision record the game can change. |

| `check.spawn` | One owned actor made from a placed actor's class, its entry and departure heard, logic and movement holds coalesced and restored, and quiet removal verified on later updates. | A kind named in `targets.txt`, a loaded world and later world updates. It only removes the actor it made. |

| `check.pikmin` | Ordered living Pikmin, valid information, independently read teams, and invalid requests refused. | Living Pikmin and the first captain. |
| `check.squad` | Ordered squad members, their individual leaders, membership in the game's Pikmin list, and invalid actor kinds refused. | A nonempty first captain's squad. |
| `check.creatures` | Ordered creatures and independent actor membership, defeat and carcass states, and revival of an undefeated creature refused without changing its life. | A living undefeated creature and the first captain. |
| `check.carry` | Carry's list, information, carriers and carried-by answers compared with each other once one of up to three added Pikmin it orders to carry one object is carrying it, a captain refused, and its workers removed. | Select `carry` explicitly on an isolated save with player control, a living Pikmin type and an uncarried object of weight 1 to 10 within 30 meters of the first captain. The object can be carried away. |
| `check.captains` | The first captain's whistle, cursor and riding; its cursor held unchanged, coalesced and released. | The first captain's cursor and whistle. |
| `check.input` | The first player's controller read, and a press taken between two steps held and pressed at the next step. The button hidden while the mod still sees it, the press ended after its two steps and by a release, and invalid players and holds refused. Keyboard keys read as booleans, and usage 256 refused. | Select `input` explicitly on a save with player control. It presses the right stick in for a few steps. In Eden the keyboard reads as up unless keyboard emulation is on. |

| `check.anim` | A stock clip on an owned matching actor, stationary pose and rate changes, direct engine comparisons, restored blueprint animation and later quiet removal. | `targets.txt` names the actor kind and compatible clip object path; the actor must be in play. |

| `check.motions` | The first player's captain's and dog's state machines, motions, move motion and speed read; on an owned suspended creature a motion, a move motion and a speed held, read back, heard as changes and released to the game's values; missing machines, long names and parts without a game blueprint refused; quiet removal. | `targets.txt` has eight lines: the captain's and the dog's state machines, comma-separated; the creature kinds to copy, in order of preference, any creature in play serving otherwise, and the class path loaded when none is in play (line nine); the state machine to hold; two motions and two move motions, of which the one that differs from the current is held. The captain, the dog and a creature must be in play. |

| `check.dialogs` | Three queued dialogs, ordered answers, accepted/cancelled results, listener cancellation and malformed text refused. | Select `dialog` explicitly on an isolated route: accept its first question, cancel its second, then close its message. It logs the three request numbers as `dialogs queued`. |

| `check.restart` | `check.restart-target` restarted five times in one game: once asked for with `mod.restart` (its installed revision again), then by the framework each time its development folder on the SD card changes, with an edited script, a script that doesn't compile (kept running), one that fails as it starts (blocked), and a fixed one (running again). The captain's CustomTimeDilation, which each revision sets, read back through reflection and the engine's `GetActorTimeDilation`, in the same world with the same captain. A missing mod refused. | Select `restart` explicitly, with `check.restart-target` installed, and run `tools/drive_restart_check.py --sd <the emulator's SD folder>` (with `--build <key>` for another build's title) from before the game starts until the check ends. It puts each revision from `tests/fixtures/restart` in the target's development folder in one rename when the check names it, and takes the folder away afterward; check.restart has no development folder, so nothing written restarts it. Player control in a level. |

| `check.body` | An owned stock actor's physics and every blocking part suspended, direct engine reads, 45-update hovering, restored simulation and collision, a fall after release and quiet removal. | A stock CHAPPY in play and later physics updates. It only moves or removes its owned copy. |

| `check.motion` | An owned actor thrown upward, observed rise and speed, settled vertical movement, grounded warp and requested facing, a new throw called off by warp, and quiet removal. | A stock CHAPPY in play and later physics updates. It only moves or removes its owned copy. |

| `check.fight` | An owned creature intercepts a hit while its decisions are suspended without losing life, then loses life, heals, takes an attributed hit, reports damage before defeat, and is quietly removed. | Select `fight` explicitly on an isolated save with a healthy stock CHAPPY and the first captain. Only its own copy takes damage or is defeated; the game can still count that defeat. |

The time, files, text and tables mods have passed against the made-up game and in the game.
Their changes to existing values are restored before each mod's check ends. Native checks
remain for success paths that have not yet passed through the SDK in the game.

The native body, death and spawn assertions now retain only their uncovered work.
Retired status bits stay zero. The public SDK log reports their replacements.

| Native assertions retired | SDK observation | Native assertions retained |
| --- | --- | --- |
| Body bits 0 through 6: held flags, stopped simulation and collision, refused launch, hovering, restoration and falling. | `check.body`, 13 checks on its owned actor with direct engine reads. | Bits 7 and 8: framework allocation count and suspension of the actor with the most colliders. |
| Caught bits 4 and 5: counted bomb death and the exact victim, kind and blamed actor. | `check.death`, 15 checks on initialized owned Pikmin, including quiet removal and one attributed counted death. | The separate squad death observer remains. The former caught phase advances without reporting any pass bits. |
| Spawn bits 0, 1, 2, 4, 5, 6 and 8: source, copies, entry, placement, removal and departure. | `check.spawn`, 12 checks including exact owned actor entry and departure and repeated removal refusals. | Bits 3, 7 and 9: list and framework allocation counts. Bits 10 through 12: pooled object lifetime and handle generation. |

`check.cleanup` observes automatic cleanup after ordinary startup refusal on a
full creature. It covers requested actor, AI and movement tick state, physics,
collision, subtree visibility, a distinct material override, two added parts,
owned actor removal, callback cancellation and reuse of the released holds.
The native subtree persistence, repeated material enforcement, root and bone
attachment, socket position and added-part cleanup on actor departure remain.
Public component counts do not replace framework allocation counts.

To add one: make the folder, name its check group under `meta.check`, list its check names under
`meta.checks`, plan every check as the mod starts,
and end each with `pass` or `fail`. The checks against the made-up game install every check mod
and start it as the game would, three times, and fail when a check of it does not pass. A check that
acts on things of the game's by name reads the names from a `targets.txt` in its folder, and
`tests/game/check_mods/<id>/targets.txt` holds the made-up game's names for the same things.

`check.settings` counts its runs in a setting. Started on a save that already holds settings
for it, `{"loud": false, "runs": 41, "speed": 0.75, "mode": "wild", "greeting": "hi there",
"extras": ["boots"]}` in `tsuchi/check.settings/settings.json`, it has to find those.

## The other folders

These are what the checks still made by the check build find and read. `check.compiled-rejected`
is given a `mod.nro` that is no module the system will load.

World checks wait for public player control and their required actors before reading or holding game state. The host fixture starts all three save runs at Title, verifies the world checks have not passed or failed there, then enters play. Animation targets name the stock kind and asset, followed by the Blueprint and single-clip engine enum values; the host overlay supplies its own values.

A check with `meta.check_explicit` set to `true` requires its group to be named
when staging. Default staging leaves these routes out. For example,
`python tools/stage_test_mods.py fight` stages only the
seventeen `check.fight` results, plus the legacy fixtures without check results.
Read that run against its selected checks. The host suite still tests every
declared SDK check; game route selection does not remove native checks.

| `check.aim` | A cursor held on an owned actor follows its move, changes to a point, a coalesced whistle stays sounding and ends, the cursor is restored and the owned target is quietly removed. | Select `aim` explicitly on an isolated save with the first captain's cursor and inactive whistle and a stock CHAPPY. Whistling can call nearby Pikmin. |

| `check.crew` | Three flower Pikmin added beside the captain, independently described and listed, one leaves and rejoins, the squad is dismissed and restored, only added Pikmin are quietly removed, and every original Pikmin and leader remains. | Select `crew` explicitly on an isolated save with a steered, unmounted captain and living Pikmin. Squad dismissal briefly changes the original formation before restoring leaders. It waits up to 300 updates for an empty squad and repeats once at 150; logs at 45, 150 and 300 report the count and first member action. An unfinished dismissal still fails the same empty-squad assertion. |

| `check.switch` | The first player switches to its real other character and back, listeners report both original/new identities, a character hold coalesces and ends, idempotent selection stays quiet, and the original character is restored. | Select `switch` explicitly on an isolated save with the first captain and dog unmounted and ordinary switching unlocked. Busy or locked story states remain named waiting prerequisites. |

| `check.death` | Two owned copies of a living Pikmin class: quiet removal emits no death, counted bomb removal emits one report with exact victim/kind/cause/captain/location, gone handles refuse repeat removal, and every original Pikmin remains. | Select `death` explicitly on an isolated save with a captain and living Pikmin. The owned victim's counted death can change the isolated save's death statistics. |

| `check.work` | One request-identified flower worker receives a public work order for an owned quiet CHAPPY, changes from its prior action, and reports changed events that match immediate readback; both additions are quietly removed and originals remain. | Select `work` explicitly on an isolated save with a steered first captain, living Pikmin and stock CHAPPY. Its worker can attack the owned target. |

| `check.throw` | One request-identified flower worker is thrown by the real captain at a held nearby cursor; leader departure, rise, speed and independent engine positions are checked, then its hold and worker are removed and original leaders remain. | Select `throw` explicitly on an isolated save with a steered unmounted captain, a working cursor and living Pikmin. Busy throw readiness remains named waiting. |

| `check.ride` | The real captain rides its real dog through an ordinary request, rejects duplicate pending and mounted requests, dismounts, restores every participating original squad leader, and stays unmounted with originals intact. | Select `ride` explicitly on an isolated save with a steered unmounted captain and its nearby dog, with ordinary mounting unlocked. Missing, distant or locked readiness remains named waiting; it uses no warp setup. |

| `check.revive` | An owned original Dwarf Bulborb takes a native fatal hit, becomes a listed carcass, and revives as exactly one healthy original or actor spawned by the game at its home; both owned identities and the class hold are removed, and original creatures remain. | Select `revive` explicitly only on an isolated daytime route. It loads the original `GKochappy.GKochappy_C` class, uses the native 300-update defeat and 900-update carcass/revival limits, and requires the live healthy state and the correct actor identity. Night creatures do not become carcasses. |

| `check.cleanup` | Failed compiled startup returns full actor AI, movement, physics, collision, subtree visibility and a distinct material, removes two added components and an owned actor, and ends callbacks. The observer uses public events and independent engine/native reads. | Select `cleanup` explicitly. The title world must support synchronous creation of the full creature in `game/table.yaml`. A missing full actor prerequisite fails the check. The auxiliary producer logs its completed setup and refuses with `not_ready`. Join the producer-owned handle to the observer birth and removal handles. The observer keeps birth identities even when startup cleanup already removed the actor. A leaked producer callback changes the donor requested byte once, so the observer detects it even when the stopped mod cannot log. Its forbidden callback log must also never appear. |
