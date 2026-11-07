---
title: Add placements
description: Add saved gameplay objects and AI values to a map.
---

Before you start, [create your mod](/develop/first-mod/), then make a placement list and
any AI files you need with Kadan or your own tools. You'll need files in the game's
formats, as the manifest only tells Tsuchi where to find them.

In Kadan, drag gameplay Blueprints into the level and use Unreal's actor tools to
place them. Edit their placement and AI values there, then add the level to your mod's
selection on Kadan's **Mods** page. When you export, Kadan turns the saved gameplay
actors into the game's placement lists and writes their AI files. Each list starts with
an empty *mark row*, which lets Tsuchi add the list to a save only once. The export
includes the assets your level needs and leaves game assets out of the mod.

## Lists

Add your list to a `maps` entry, using the list's package path without an object name
after a dot. For example:

```json
"maps": [{
  "map": "/Game/Carrot4/Maps/Madori/Cave/Cave000/Cave000_F00/Cave000_F00_P",
  "placements": ["/Game/Mods/terrain_course/Cave_Objects"],
  "ai": "maps/cave.ai"
}]
```

The game keeps its rows in an `ActorPlacementInfoAsset`, and its maps name an
`ActorPlacementInfoListAsset`, which holds one or more of those in `AssetList`. You can
name either one in `placements`. When you name an `ActorPlacementInfoListAsset`, Tsuchi
gives each list in its `AssetList` in turn. Kadan exports both.

Keep the first row of each `ActorPlacementInfoAsset` as the mark row, and leave its actor
class empty. In each later row, put one *generator*, the game's record of a placed object
or group. Each generator holds the class, position, count, and saved state. Give each
`ActorPlacementInfoAsset` a package of its own.

Put creatures, treasures, gates, and other gameplay objects in placement lists so the
game can save their state. Put scenery, such as meshes, lights, and volumes, in your
added levels. Don't bake gameplay objects into those levels or put their classes,
positions, or counts in `mod.json`.

After the map's own generators and your mod's added ground are ready, Tsuchi gives your
list to the game's generator system. The game then saves whether those objects have
been defeated, collected, or broken.

## Saves

When you update your mod, keep each placement list's path the same if you want players
to keep their progress. If a save already has the list's mark row, the game won't read that
list again, so changing its rows won't update generators already in the save.

Tsuchi makes the mark from the path of the `ActorPlacementInfoAsset`'s package, so a save
keeps its progress if you switch between naming that asset and naming the
`ActorPlacementInfoListAsset` that holds it.

To add more objects in an update, make another list at a new path and add it to
`placements` or to your `ActorPlacementInfoListAsset`'s `AssetList`. Test with a save that
visited the previous version, then with one that hasn't visited the modded map.

**Warning:** Removing a placement mod doesn't erase its saved generators. The game
can still make those objects when the map loads, and added ground may be absent. Keep
the mod installed when a save needs its actor classes or ground. Classes that Tsuchi
has already loaded for generators stay held for that game run.

Keep each row's placement number the same across updates too. If the map already has a
generator with that number, Tsuchi skips the row and reports it in the log.

## Modes

If you omit `mode`, your placement lists and AI apply to daytime story play. Add
`"mode": "night"` or `"mode": "olimar"` for the other modes. If a mode entry only adds
placements or AI, it uses the shared ground. See
[Change a map](/develop/change-a-map/) for shared ground and ground overrides.

## AI

Put your binary AI file in your mod's folder, then give its relative path in `ai`.
Make sure its entries match the placements, as each entry names a generator by its
placement number and actor class. The file holds groups of AI values as bytes written
by the game's serializer. Tsuchi applies those values each time the creature's AI
starts, after it reads the map's or save's values and before it builds its behavior.
This also applies when the game reconstructs a creature from a save. Don't replace
the binary file with JSON or reuse it for another map.

If several mods change different AI groups for the same placement, their groups
compose. If they change the same group, the mod that started last supplies that
group, and the other mod's log names it. When your mod stops, it supplies no more AI
values. A creature that already started keeps its values until the game makes it again.

## Check

Visit the mode you chose and check your objects and their behavior. Defeat a creature
and collect a treasure, then save, reload, and revisit the map. You should see that the
creature stays defeated and the treasure stays collected. Check the log if Tsuchi
refuses a list, row, or AI file. If an added class comes from another mod, declare that
mod in `requires` in your [manifest](/develop/manifest/).
