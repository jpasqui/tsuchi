---
title: Add a map
description: Add an area with its own progress and travel destination.
---

:::caution[WIP]
New maps haven't been tested in the game yet.
:::

Before you start, [create your mod](/develop/first-mod/), then make your area's files with
Kadan or your own tools. You'll need a scene asset, first base, ground, placement lists,
and navigation in the game's formats. Build lighting and navigation for your area before
exporting it, and include the rebuilt data in your mod.

In Kadan, add the area's maps, assets, or folders to your mod's selection on the **Mods**
page, then review what ships and the assets it needs. Kadan shows which area slots are
taken while you choose your builds. Use Unreal's actor tools for scenery and gameplay
objects. When you export, Kadan writes the saved gameplay objects as placement lists and
AI files. Each list starts with an empty *mark row*, which lets Tsuchi add the list to a
save only once. The export includes your own assets and your map, table, and content
changes, leaves game assets out, and waits until the area's lighting is built. Your cooked
files go in your mod's [container](/develop/publish/#container), next to `mod.json`.

## Manifest

Add your area under `new_maps`, using a short name of your own. Your area gets its own
progress and travel destination. For example:

```json
"new_maps": {
  "course": {
    "name": "Terrain Course",
    "builds": [{
      "world": "/Game/Mods/terrain_course/Area005_P",
      "scene": "/Game/Mods/terrain_course/SD_Area005.SD_Area005"
    }],
    "preview": "/Game/Mods/terrain_course/UI/T_Course.T_Course",
    "radar": {
      "texture": "/Game/Mods/terrain_course/UI/T_CourseRadar.T_CourseRadar",
      "size": 12000
    },
    "pikmin": ["red", "ice"],
    "totals": {"treasures": 3, "castaways": 0, "flarlic": 1}
  }
}
```

Combine your mod's ID and the short name to get the destination's ID. For example, if
your mod's ID is `terrain.course`, give `terrain.course:course` to
[`story.travel`](/reference/story/#travel). The player should arrive at your level's
first base, and the area list and travel screens should show your area's name. Set the
first base in your level, as the manifest has no spawn point.

## Builds

Make one to four *builds*, versions of your area that use the game's free area numbers:
5, 7, 8, and 9. Make sure the world and scene names use the same number in each build,
such as `Area005_P` and `SD_Area005`. Use that number in the sublevels, placements, and
scene references too. Don't rename a cooked file to change its number.

When mods start, Tsuchi assigns your area's first available build and saves the
assignment in the player's save. If you supply all four builds, your area can run
alongside three other added areas. If none of your builds has an available number,
Tsuchi blocks your mod and names the occupied numbers in the log.

## Pictures

Give object paths for `preview` and the radar's `texture`. Set the radar's `size` to the
world's width in centimeters, or omit it to use `12000`. You can also set `offset` as
`[x, y]`, `angle` in degrees, `scale` for the initial zoom, and `limits` as
`[left, top, right, bottom]`. If you omit them, you'll get `[0, 0]`, `-90`, `3.75`, and
half the size in each direction.

Use `pikmin` to choose the area's icons: `red`, `blue`, `yellow`, `rock`, `wing`,
`purple`, `white`, or `ice`. Set `totals` to the area's treasure, castaway, and Flarlic
counts, each from 0 to 10000. If you include `totals`, any count you omit becomes 0.

Make sure your scene's `ActorPlacedNumAsset` counts match its placements, as the game
uses that asset for placement totals and completion. The manifest's `totals` can fill a
missing total shown as `???` in the area list, but won't change placement counts,
collected records, or completion percentages.

## Removal

Once Tsuchi reserves an area's number for the story, the number stays with your map even
if you remove your mod. Tsuchi can reserve it while preparing the area list, before the
player visits. Removing an unvisited area won't necessarily free its number. Keep a
build for the assigned number in later releases so players can reinstall your mod and
return to their progress.

End the day before removing a new-map mod. If a player loads a save made inside your
area without the mod, the game returns to the title screen and leaves the save
untouched. Reinstalling the mod lets the player load that destination again. A save
made at the base after ending the day loads without the map mod.

When your mod stops, its area disappears from the travel list. A loaded world stays
until the player leaves it, and its radar keeps the picture it began with. Tsuchi
keeps the map's saved number and progress, so reinstalling the mod restores them.

## Check

Travel to your area from the game's list, save, reload, leave, and return. You should see
your preview, radar, and first base, and keep your progress after the reload. Check the
lighting and Pikmin paths around your ground, then compare the object counts with your
placement lists and try your mod alongside another added
area. Check the log if Tsuchi can't assign an area number.
