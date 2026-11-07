---
title: Add a treasure
description: Give your treasure a class, value, names, and catalog entry.
---

Before you start, [create your mod](/develop/first-mod/), then make your treasure's
Blueprint, mesh, materials, and icon with Kadan or your own tools. Put these assets in
your mod's content.

In Kadan, add your treasure's asset or folder to your mod's selection on the **Mods**
page. When you export, Kadan finds the assets it needs, includes your own assets and
treasure data, and leaves game assets out. To place it, drag the Blueprint into a level with Unreal's
actor tools, then export that level to write the saved object as a placement-list row.

## Manifest

Add a short name under `content.treasures`. For this guide, we'll use `example.gems` as
our mod's ID. For example:

```json
"content": {
  "treasure_series": {"gems": {"name": "Gem Collection"}},
  "treasures": {
    "ruby": {
      "class": "/Game/Mods/example_gems/BP_Ruby.BP_Ruby_C",
      "from": "OTAECLAIR",
      "sparklium": 120,
      "carriers": [4, 8],
      "icon": "/Game/Mods/example_gems/T_Ruby.T_Ruby",
      "text": {"name": "Ruby", "description": "A red stone."},
      "catalog": {"series": "gems"}
    }
  }
}
```

Set `class`, `from`, and `text.name`. For `class`, give the Blueprint class's full object
path, including `_C`. For `from`, choose a *donor*, an existing treasure whose data you
want to copy. Give its row name, or the ID of a treasure declared earlier in your
manifest or added by a mod that starts before yours. Tsuchi copies the donor's data into
new rows and leaves its values unchanged.

Your treasure's ID combines your mod's ID and its short name, so the ruby in this example
is `example.gems:ruby`. If another mod adds your donor, put that mod in `requires` in
your [manifest](/develop/manifest/).

## Values

Set `sparklium` to a whole number from 0. Give `carriers` as `[minimum, maximum]`, using
whole numbers from 1, and make sure the maximum is at least the minimum. You can give
one number to use it for both. If you want different carrying counts in Dandori Battle,
set `battle_carriers` too. If you omit `battle_carriers`, Tsuchi uses your declared
`carriers`, or the donor's battle counts if you omitted `carriers` too. Other omitted
values and the icon come from the donor.

Under `text`, set `name`, and optionally `catalog_name`, `result_name`, `description`,
`olimar_notes`, and `louie_notes`. If you omit the catalog or result name, Tsuchi uses
`name`. Any other text you omit starts empty.

## Catalog

Add your series under `content.treasure_series`, give it a `name`, and put its short name
in `catalog.series`. You can only add treasures to your mod's own series. Tsuchi lists
them in manifest order and assigns their catalog numbers. If you want the treasure to
be collected and paid for without a catalog entry, omit `catalog`.

When you add a catalog entry, the Piklopedia's treasure total goes up. The player still
needs the same treasures to complete the game's existing series.

To change other fields, use the row's `values`, or `catalog.values` for catalog fields.
Follow [Change a table](/develop/change-tables/) for typed property paths. Declare your
rows in the manifest instead of replacing the catalog table. Tsuchi writes `SeriesID`,
`ZukanNo`, and `ActorName` after applying `catalog.values`, so those fields keep the
assigned series, order, and treasure identity.

## Placements

Make a generator for your new class, then
[add its placement list](/develop/add-placements/) to the map where you want the treasure
to appear. Declaring your treasure adds its data, but doesn't place it in a map.

## Removal

When your mod stops, its content IDs stop resolving, its overrides end, and its asset
holds end. Table rows and loaded-world list entries stay while the game can use them.
The catalog row leaves the series and its count. New text keys keep their last text.
If your mod starts again in the same run, Tsuchi reuses the rows it left behind.

Removing your mod keeps its content numbers and the player's collected records.
Those records don't stop a save from loading, and reinstalling the mod restores the
same treasure's progress. Saved placements that still need your classes or ground
have separate requirements, and see [Add placements](/develop/add-placements/).

## Check

Visit the map, carry the treasure to the base, and check its name, icon, carrying counts,
Sparklium, and any catalog entry you added. Save, reload, and revisit. The treasure
should stay collected. When you update your mod, keep the class path and content ID the
same so existing saves can find it.
