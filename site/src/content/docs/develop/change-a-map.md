---
title: Change a map
description: Add ground, move scenery and game objects, and supply navigation for an existing map.
---

:::caution[WIP]
Pikmin walking on your own ground, lighting, and values set on the game's actors haven't been tested in the game yet.
:::

Before you start, [create your mod](/develop/first-mod/), then make the map files
with Kadan or your own tools. Keep your added levels, scenery changes, and navigation
together.

## Kadan

On Kadan's **Mods** page, create your mod with its ID, name, version, and dependencies,
then add the maps, assets, or folders you want to ship to your mod's selection. Kadan
also includes the assets they need and your map, table, and content changes. Before you
export, review what ships, which game assets stay out, and anything another mod in the
project also changes.

Use Unreal's level tools to move or remove the game's scenery, change the values on the
game's actors, and add your own scenery.
Review each asset's edit and export status where you work.
To replace a game texture, mesh with collision, or sound, supply your own asset.
Kadan can export that replacement at the game's path while keeping game data out
of the package. Rebuild lighting and navigation after changing ground. Export waits
for required lighting data, then writes your map changes and own assets for Tsuchi.
Your cooked files, replacements included, go in your mod's
[container](/develop/publish/#container), next to `mod.json`. If another mod replaces the
same asset, the game uses the one from the mod that loads later.

## Manifest

Add a `maps` entry to `mod.json` and name the original world by its package path.
For example, you can add ground to a cave with this entry:

```json
"maps": [{
  "map": "/Game/Carrot4/Maps/Madori/Cave/Cave000/Cave000_F00/Cave000_F00_P",
  "levels": ["/Game/Mods/terrain_course/Cave000_F00_Mine"],
  "changes": "maps/cave.json",
  "navigation": "maps/cave.nav"
}]
```

Write package paths for `map` and `levels`, without an object name after a dot.
For `changes` and `navigation`, write paths relative to your mod's folder. Pack only
your own assets, as Tsuchi loads the game's map and applies your changes.

## Scenery

Keep the original actor and sublevel names in your changes file. In `maps/cave.json`, give positions
in centimeters, rotation as pitch, yaw, and roll in degrees, and scale as three factors.
For example:

```json
{"levels": {"Cave000_F00_GameArea": {
  "move": {"CMA_T02_CP_1x1_3": {
    "location": [120, -40, 0], "rotation": [0, 90, 0], "scale": [1, 1, 1]
  }}
}}}
```

A sublevel entry can also contain `remove`, a list of actor names, and `set`, an object of
actor names and property values. A `set` path follows the same rules as
[`reflection`](/reference/reflection/). Supply numbers, booleans, or text of the
property's type. For an object property, supply an asset's path, or write
`{"asset": "/Game/Mods/terrain_course/SM_Block.SM_Block"}`. Tsuchi loads the asset
and checks that its class suits the property. For an enum, supply its member name,
such as `"QueryAndPhysics"`, or its full name with the enum prefix.

For a struct, supply an object with the fields you want to change. You can also name
those fields by their full reflection paths, so `"Scale": {"X": 2}` and `"Scale.X": 2`
change the same field. For example, a mesh actor can use:

```json
"set": {"CMA_T02_CP_1x1_3": {
  "RootComponent.StaticMesh": "/Game/Mods/terrain_course/SM_Block.SM_Block",
  "RootComponent.BodyInstance.CollisionEnabled": "QueryAndPhysics",
  "RootComponent.RelativeScale3D": {"X": 2, "Y": 1, "Z": 1}
}}
```

Use the component names and property paths from your export. Tsuchi refuses arrays,
null, and other unsupported values with a message in your mod's log. Missing enum
members, missing assets, and assets of the wrong class are refused too.

Before a level registers its components or begins play, Tsuchi applies its remove,
move, and set entries. This covers the persistent level, initial sublevels, and levels
loaded later by volumes or nested streams. Edits do not wait for added ground.
Several mods can edit a level. The last mod's field value wins, and removal wins over
movement or field edits.

When you remove an actor, Tsuchi clears direct references to it in the other actors
of the same level, including references inside structs and arrays. Your mod's log lists
each field that changed. References in components, maps, and sets aren't cleared.

If any named actor is absent, Tsuchi refuses the whole sublevel entry and reports it in your mod's log.

## Game objects

The game makes its pots, gates, creatures, and treasures from its placement lists, not
from the map's levels, so a `move` or `remove` in your changes file doesn't reach them. To
move or remove one of them, change the row of the list that places it with
`placement_changes`:

```json
"maps": [{
  "map": "/Game/Carrot4/Maps/Madori/Cave/Cave000/Cave000_F00/Cave000_F00_P",
  "placement_changes": {
    "/Game/Carrot4/Maps/Madori/Cave/Cave000/Cave000_F00/ActorPlacementInfo/AP_Cave000_F00_P_Objects": {
      "move": {"17179869271": {"location": [121, -49.9, 0], "rotation": [0, 28.6, 0]}},
      "remove": ["17179869268"]
    }
  }
}]
```

Name each list by the package path of its `ActorPlacementInfoAsset`. Name each row by its
placement number, which is the `GenerateInfo.DebugUniqueId` of the object's actor in the
game's level, written as text in decimal digits. A move takes `location`, `rotation`, and
an optional `scale`, as in your changes file.

Tsuchi applies your changes each time the game reads the list, and the game's files stay
as they are. If several mods change one row, the game uses the change from the mod that
loads later, and both mods' logs say so. If a row you name isn't in the list, Tsuchi makes
none of your changes to that list and names the row in your log. Your log also lists each
row that was moved or removed.

The game reads a map's lists only for a save that hasn't been to the map. After that visit,
the save keeps its own record of every object there, so the save keeps a removed object
gone and a moved one in its new place, even without your mod. A save that has already
been to the map loads its objects from that record, and your changes don't reach it. Your
log says so when that happens. A removed treasure or creature still counts toward the
map's totals. Test with a save that hasn't visited the map.

## Cleanup

If you stop your mod in a loaded world, Tsuchi restores fields changed through
`set` to another mod's value, or their original value if no other mod changes
them. Actors you removed or moved stay that way until the player leaves the world.

If you start your mod after a level begins play, Tsuchi applies its changes after added
ground loads and logs the delay once for each entry and level. Changes made then cannot
affect values the game already copied when the level started. Tsuchi also refuses a late
move if it cannot restore every attached component's mobility. Leave the world and return
to apply your edits before the level starts.

## Lighting

After moving or removing scenery, changing a mesh, or adding a level, rebuild lighting
in your editor and include the rebuilt data in your export. Check the shadows around
your changes, then leave and revisit the map to make sure the lighting still matches.

## Navigation

After you change walkable ground or collision, rebuild its navigation in your editor. Kadan writes
the Tsuchi `.nav` file when it exports the map. Include that file, as stock navigation or another
map's file won't match your ground. The file names each map and navmesh and holds
its navigation tiles. Keep the original navigation grid's origin and tile width when
you rebuild it. Tsuchi waits for the added ground before replacing navigation, then
applies the navigation when the map's navmeshes are ready. If several mods supply
navigation, Tsuchi uses the file from the mod that started last and names that mod in
the other mods' logs. If added ground fails, actor edits already made stay in place.

## Modes

Omit `mode` to use the ground in story play, night expeditions, and Olimar's Shipwreck
Tale. Placements, placement changes, and AI in the same entry still apply only to story.

Write `"mode": "story"`, `"night"`, or `"olimar"` to limit the whole entry. If the same
mod also has shared ground, a matching mode entry replaces it when that entry declares any
of `levels`, `changes`, or `navigation`. Include every ground field you need for that
mode, as omitted fields aren't inherited. A mode entry with only placements or AI keeps
the shared ground. An empty `levels` list can select no mod ground in one mode.
If Tsuchi cannot read the mode yet, it retries before applying ground or marking
placement lists as given.

## Check

[Install your mod](/players/install-a-mod/) and visit the map in each mode you made it for. Walk past every actor
you moved, removed, or changed, then leave the map and come back to make sure the changes
are still there. You should see Pikmin walk across your added ground and carry objects
along the paths you meant. If you changed a mesh or its collision, walk into it to make
sure the captain and Pikmin stop where you expect. If part of the map only loads as the
player gets near it, walk up to it and check its changes too. Finally, open your [log](/develop/logs/) and look for a refused level, actor, value, or navigation file. See
[Add placements](/develop/add-placements/) for objects the game records in the save.
