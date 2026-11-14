---
title: Write a manifest
description: Every key you can use in mod.json.
---

Every mod needs a manifest file named `mod.json`. If you only need a minimal example, see
[Write your first mod](/develop/first-mod/).

Make sure to double-check your spelling, as Tsuchi won't load a mod if its manifest has a
key that it doesn't recognize.

```json title="mod.json"
{
  "id": "olimar.tougher",
  "name": "Tougher Bulborbs",
  "version": "1.2.0",
  "author": "Olimar",
  "description": "Bulborbs take more hits.",
  "requires": {"olimar.library": "1.0.0"}
}
```

## Identity

| Key | Required | Description |
| --- | --- | --- |
| `id` | Yes | The mod's ID. It has to match the name of the mod's folder. |
| `name` | Yes | The name that players see. |
| `version` | Yes | The mod's version as three numbers, like `1.2.0`. |
| `author` | No | Your name. |
| `description` | No | A sentence or two about what the mod does. |
| `url` | No | A link where players can learn more about the mod. |
| `meta` | No | Extra data for your own tools. Tsuchi ignores it. |

## Requirements

| Key | Description |
| --- | --- |
| `framework` | The minimum whole-number API version your mod supports, such as `1`. It is a JSON number, not text or a three-part version. |
| `game` | The builds of the game that your mod needs: a build's key or version like `1.1.1`, or a list of them. Leave it out if your mod works on every build. |
| `requires` | Required mods. A numbered requirement matches the same major version with a minor/patch version at least as high, and `*` accepts any version. |
| `optional` | Optional integrations, using the same version rules as `requires`. |
| `conflicts` | Other mods that can't be used together with yours. |

Read the running API number with [`mod.framework()`](/reference/mod/#framework).
Use [Compatibility](/develop/versions/) to choose the minimum number for your mod.

Tsuchi will load a mod's dependencies before it loads the mod. If a dependency is missing
or incompatible, then the mod won't load. For `1.2.0`, `1.3.0` matches and
`2.0.0` doesn't. Among mods whose dependencies are ready, Tsuchi follows the player's
order, or mod ID order before the player chooses one. The mod list reports blocked
requirements, stacked changes, and exclusive claims that need a chosen winner.

```json title="mod.json"
{
  "requires": {"olimar.library": "1.0.0"},
  "optional": {"louie.recipes": "*"},
  "conflicts": ["president.budget"]
}
```

## Restart lifetime

`"restart": "game"` prevents a running mod from being stopped by a development
restart. Use it when native allocations or other session-long effects cannot be
undone safely while the game runs. The default, `"restart": "mod"`, allows the
usual restart behavior.

Tsuchi checks the entire dependent group before releasing anything. If any running
member requires a game restart, all members keep their contexts, hooks and owned
resources, and receive a `mod.restarted` event with outcome `kept` and the reason.
This applies to explicit restart requests and development-folder changes. Ordinary
startup failure still rolls back resources; this option does not make a failed
start permanent, run code earlier, or change how settings take effect. A mod that
reads settings only at startup should explain that a game restart applies changes.

## Data

| Key | Description |
| --- | --- |
| `settings` | Settings your mod reads and changes by key. See [Settings](#settings). |
| `settings_overrides` | Effective values for settings of required mods. See [Compose mods](/develop/compose-mods/). |
| `tables` | Changes to the game's data tables. See [Change a table](/develop/change-tables/). |
| `text` | Replacement text. See [Change text](/develop/change-text/). |
| `maps` | Ground, scenery edits, navigation, placements, changes to the game's own placements, and AI for existing worlds. See [Change a map](/develop/change-a-map/) and [Add placements](/develop/add-placements/). |
| `new_maps` | New areas and their builds, pictures, radar, and totals. See [Add a map](/develop/add-a-map/). |
| `content` | New treasure series, treasures, creatures, and Pikmin types. See [Add a treasure](/develop/add-a-treasure/) and [Add a creature](/develop/add-a-creature/). |
| `shaders` | A binary set of compiled game shaders. See [Add shader sets](/develop/add-shaders/). |

Tsuchi applies table and text changes before your mod's code runs and restores them when
the mod stops. Follow the save and removal rules in the map and content guides, as your
declarations can leave progress in the player's save.

The player's save data can hold two added Pikmin types in total, for every mod and every
save slot. A type keeps its place after its mod is removed, so once two types have been
added, a mod that adds a third won't load with that save data.

## Settings

Declare one entry per setting under `settings`. Its key follows the
[mod ID rules](/develop/ids-and-kinds/#mod-ids). Each setting needs `type` and
`default`. Use `name` and `description` for its display text. Settings can appear
in the game's Settings tabs or on the mod's page.

| Key | Required | Value |
| --- | --- | --- |
| `type` | Yes | `toggle`, `integer`, `number`, `text`, `choice`, or `set`. |
| `default` | Yes | A value of the setting's type that fits its limits and choices. |
| `name` | No | Display name as text. |
| `description` | No | Help text for the setting. |
| `public` | No | `true` lets other mods read the setting. Defaults to `false`. |
| `hidden` | No | `true` hides an internal setting from generated forms and refuses overrides. Defaults to `false`. |
| `overridable` | No | `false` prevents other mods from declaring an override. Visible settings default to `true`. This does not grant direct write access. |
| `min` | No | Inclusive lower bound for `integer` or `number`. |
| `max` | No | Inclusive upper bound for `integer` or `number`. |
| `step` | No | Positive finite increment for a `number` setting in the generated arrow selector; defaults to 1, or one twentieth of a bounded range at most 1 wide. Saved and API values need not lie on that menu grid. |
| `max_length` | No | Maximum UTF-8 bytes for `text`, as a nonnegative whole number. |
| `choices` | For `choice` and `set` | A nonempty list of distinct, nonempty words. |

For a toggle, use `true` or `false`. For an integer, use a whole number. A number
can have a fractional part. Text uses a string, a choice uses one string from
`choices`, and a set uses a list of strings from `choices`.

For example, declare a toggle and a bounded integer:

```json
"settings": {
  "hard_mode": {
    "type": "toggle", "default": false, "name": "Hard mode",
    "description": "Give creatures more life.", "public": true
  },
  "lives": {"type": "integer", "default": 3, "min": 1, "max": 9}
}
```

Tsuchi saves the player's values with your mod's save data. Read and change the
values through the [settings API](/reference/settings/), and subscribe to changes
as [Add settings](/develop/settings/) shows. Another mod reads a public setting
as `<mod id>:<key>` and can't change it.

A running mod can narrow an integer setting's range with
[`settings.set_integer_range`](/reference/settings/#set_integer_range). The normal
form updates its arrows and displayed value. Reads clamp to that range while
saved preferences remain intact, so restoring content restores a previously
chosen higher value. Direct writes must fit the current range. The owner alone
can set it, and stopping that mod releases the range.

## Code

You don't need a manifest key for a script or compiled entry. Put one script in
`main.lua` or `main.js`, and put compiled code in `mod.nro`. A mod with both script
entries is refused before either starts. If a script and compiled code are present,
the script starts first. If either fails, the mod is blocked and startup changes
receive the same cleanup as a stopped mod.

:::note[Coming soon]
Blueprints that start and stop with your mod, Tsuchi's Blueprint nodes, published services, and building-block mods are coming soon.
:::

A package can also ship cooked Blueprints tied to its lifetime and use generated
Tsuchi Blueprint nodes. Published services share logic across languages. A
building-block mod declares reusable content and typed placement values with
defaults, so another mod can depend on it and tune each placement.

## Map modes

If you omit `mode` from a `maps` entry, your mod uses its `levels`, scenery `changes`, and
`navigation` in daytime story play, night expeditions, and Olimar's Shipwreck Tale. Its
`placements` and `ai` apply only in daytime story play.

To limit the whole entry to one mode, write `"mode": "story"`, `"night"`, or `"olimar"`.
If that entry declares `levels`, `changes`, or `navigation`, it replaces your mod's shared
ground for that map. Include every ground field you need, because the entry doesn't
inherit fields from the shared ground.

If you add a mode entry with only placements or AI, your mod keeps its shared ground. To
use no mod ground in one mode, give that mode entry an empty `levels` list. Other mods
keep their own entries.

For example, to use a bridge by day and a broken bridge at night:

```json
"maps": [
  {"map": "/Game/Carrot4/Maps/Main/Area/Area001/Area001_P", "mode": "story",
   "levels": ["/Game/Mods/bridge/Bridge"], "navigation": "maps/day.nav"},
  {"map": "/Game/Carrot4/Maps/Main/Area/Area001/Area001_P", "mode": "night",
   "levels": ["/Game/Mods/bridge/BrokenBridge"], "navigation": "maps/night.nav"}
]
```

To use the same bridge in every mode, omit `mode` from its ground entry. Then add a night
entry with only placements or AI for the night's objects.
