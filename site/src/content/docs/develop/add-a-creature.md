---
title: Add a creature
description: Give your creature a class, values, names, and Piklopedia entry.
---

Before you start, [create your mod](/develop/first-mod/), then make your creature's
Blueprint with Kadan or your own tools. Base it on a game creature with the behavior
you need.

In Kadan, add your creature's asset or folder to your mod's selection on the **Mods**
page. When you export, Kadan finds the assets it needs, includes your own assets and
creature data, and leaves game assets out. Drag the Blueprint into a level with Unreal's actor tools,
then edit its placement and AI values there. Exporting the level writes its saved
placement and AI files.

## Manifest

Add a short name under `content.creatures`. For this guide, we'll use `example.gems` as
our mod's ID. For example:

```json
"content": {
  "creatures": {
    "ruby_bulborb": {
      "class": "/Game/Mods/example_gems/BP_RubyBulborb.BP_RubyBulborb_C",
      "from": "KOCHAPPY",
      "life": 450,
      "sparklium": 4,
      "carriers": [3, 6],
      "pikmin": 5,
      "icon": "/Game/Mods/example_gems/T_RubyBulborb.T_RubyBulborb",
      "text": {
        "name": "Ruby Bulborb",
        "scientific_name": "Oculus ruber",
        "family": "Grub-dog family",
        "description": "A red Dwarf Bulborb."
      },
      "night": {"life": 600},
      "catalog": {}
    }
  }
}
```

Set `class`, `from`, and `text.name`. For `class`, give the Blueprint class's full object
path, including `_C`. For `from`, choose a *donor*, an existing creature whose data you
want to copy. Give its row name, such as `KOCHAPPY` for the Dwarf Bulborb, or the ID of a
creature declared earlier in your manifest or added by a mod that starts before yours.
Tsuchi copies the donor's data into new rows.

Your creature's ID combines your mod's ID and its short name, so this example uses
`example.gems:ruby_bulborb`. Make sure your Blueprint and its native parent supply the
behavior you want, as copying a row won't give another class the donor's AI. If another
mod adds your donor, put that mod in `requires` in your
[manifest](/develop/manifest/).

## Values

Set `life` above 0 and `sparklium` to a whole number from 0. Give `carriers` as
`[minimum, maximum]`, using whole numbers from 1, and make sure the maximum is at least
the minimum. You can give one number to use it for both. Set `battle_carriers` for
different Dandori Battle counts, and `pikmin` for the body's Pikmin yield, a whole number
from 0. If you omit `battle_carriers`, Tsuchi uses your declared `carriers`, or the
donor's battle counts if you omitted `carriers` too. Other omitted values and the
icon come from the donor.

Under `night`, you can change `life`, `sparklium`, `carriers`, `battle_carriers`, `pikmin`,
or `values` for nighttime. If you omit `night`, your creature uses its daytime data even
if the donor has night data. To show a different Blueprint on the Piklopedia's night
stage, set `night_class`. Make sure both classes supply the behavior you need.

Under `text`, set `name`, and optionally `scientific_name`, `family`, `description`,
`olimar_notes`, and `louie_notes`. Any text you omit starts empty.

## Piklopedia

Include `catalog`, even an empty object, to add a Piklopedia entry. If you want your
creature to appear in maps without an entry, omit it. Use `values` for other fields in
the creature's row and `catalog.values` for other Piklopedia fields. Follow
[Change a table](/develop/change-tables/) for property paths.

If you stop your mod while a loaded world uses its catalog row, Tsuchi waits until no
loaded world uses the row before removing it. Until then, the row still counts in the
Piklopedia's total.

## Placements

Make generators for your new class, then
[add their placement lists](/develop/add-placements/). For a night placement list, add a
separate entry with `"mode": "night"`. The creature's `night` values don't place it at
night or change the ground. Set the placement's night behavior in its generator so the
game reads the creature's night row.

## Removal

When your mod stops, its content IDs stop resolving, its overrides end, and its asset
holds end. Data rows and loaded-world list entries stay while the game can use them.
Tsuchi removes the Piklopedia row when no loaded world lists it, and new text keys
keep their last text. If your mod starts again in the same run, Tsuchi reuses its
data rows and adds the Piklopedia row again if it was removed.

The player's creature and Piklopedia records stay in the save. Those records don't
stop a save from loading without the mod. Reinstalling the mod restores the entry
with its previous count. Saved placements that still need your creature's class or
ground have separate requirements, and see [Add placements](/develop/add-placements/).

## Check

Visit each mode you chose, watch the creature's behavior, defeat it, and check its body,
carrying counts, and rewards. If you added `catalog`, meet the creature and check its
Piklopedia entry. Save, reload, and revisit. Your progress should stay after the reload.
When you update your mod, keep the class path and content ID the same. If saved
placements still need the creature's class, reinstall its mod before loading that map.
