# Cave creature at night fixture

Stage a retained cooked placement donor with the typed list writer. The first
row is the list mark, followed by one GBaby generator and one pot generator.
The new package and deterministic IDs isolate this check from old save rows.
The map entry explicitly selects night. The observer is a separate mod.

The recorded surface captain position supplies the initial placement height.
Confirm the creature settles onto actual ground. A spawn at that position
alone does not prove its life cycle or persistence.

Observe the larva's class, placement ID, position, health and natural defeat
event. Use native Pikmin combat. Do not set health, destroy the creature, edit
the save or interpret disappearance alone as defeat. Complete the night's
native save route, then reload the resulting save with the same IDs. Keep
the pot as a surviving generator control and parse the native generator save.

Keep the observer when the placement mod stops or is absent on a later boot.
Accepted generators belong to the game and may remain after the mod stops.
Observe their saved state separately from old handles ending on world unload.
Record the actual module hash, save source and native inputs for each run.

The optional combat mod waits for settled ground, approaches with the captain,
and throws existing Glow Pikmin. After the exact native defeat event and a
successful cursor release, it requires the surviving pot, player control in
Area001_P, and a clock below 1 before calling story.set_clock(1) once. This is
controlled route setup: the original clock posts its night DayEnd request on
the next enabled, unpaused tick. It does not prove a successful expedition.

Observe native night results and the Save flow's completion, then inspect the
game-written generator rows for the same larva, pot, and runtime mark IDs.
Reload that written save and revisit the same night with the same IDs: the
defeated larva must stay absent, and the saved pot must remain. A request log,
a Save stage alone, or handles ending on departure does not prove persistence.
The failed-night Retry/Exit route did not serialize these IDs in attempt 2.

The optional reentry mod is for a cold reload of the game-written camp save.
At frame 2400 or later, with camp control in Area500_P, it requires one NPC
whose NpcAI has NpcInfoKey SV005, Doctor role 9, and NpcAIComponent metadata.
It records the Doctor's identity, class path, and current position, warps only
the unmounted controlled captain about 150 cm away as route setup, and checks
the captain's position. Missing identity or readback emits BLOCKED. It sends
no Talk, travel, or debug request; native Talk/A and expedition menus remain
manual. Choose Sun-Speckled Terrace and the same night base through Yonny.

On that night revisit, observe the saved pot's same placement ID, with no
fresh live larva under the defeated ID. Parse the written B/gen file for the
path-bound runtime mark and pot owning generator identity. Raw byte matches
inside a native body row do not identify an owning larva generator. Reentry
approach and native menu admission alone do not close the persistence row.

Recorded native result on pinned production module 59c3f4e: attempt 3
observed the exact larva's native Glow Pikmin defeat, then requested the
controlled clock end with the pot alive. Native DayEnd and Save completed.
The written normal Area001 generator file owned the same pot and runtime
list mark; the consumed larva's owning row was absent.

Attempt 5 cold-loaded that written save, walked to Yonny and used native
Night Expedition menus to enter Sun-Speckled Terrace, Rugged Scaffold
(zero-based night index 1). Actual Area001 control and the same pot class,
placement ID and position were observed from frame 16560 through 19920,
with no live larva under its defeated ID. Native saved NightAreaStart,
bNightStart and bNightTime, together with the night HUD and Lumiknoll,
qualify the night context; game.mode reported story during that night.
These are recorded results on the pinned API. Repeat affected observations
on the final service split integration before final release acceptance.

Use --pack with --retoc pointing to the retoc executable. The pack step calls
retoc to-zen with UE4_26 and accepts only this fixture's new placement package.
It requires all three nonempty container files before replacing the previous
output. A stock package replacement requires header redirects and is refused.

The split API fixture calls entity for gameplay actors, placement numbers,
body-facing transforms, and captain warps. Actor handles remain shared with
reflection and combat; the defeated event still carries event.actor. The
recorded pinned runs above used the earlier API. The migrated calls and mocks
have host coverage; repeat the affected native observations on the final
integrated runtime before release acceptance.
