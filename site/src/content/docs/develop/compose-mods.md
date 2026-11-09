---
title: Compose mods
description: Require other mods and control their settings without overwriting player preferences.
---

Use `requires` to name your mod's dependencies and `settings_overrides` to choose
their effective settings. The dependency must expose the setting and accept the
value. You can include your own maps, scripts, content, and settings in the same
mod folder. You don't need a special package type or a script to apply overrides.

For example, these illustrative IDs describe a challenge mod that selects a
respawn interval and a field capacity:

```json title="mod.json"
{
  "id": "olimar.challenge",
  "name": "Challenge",
  "version": "1.0.0",
  "requires": {
    "olimar.respawns": "1.0.0",
    "olimar.capacity": "1.0.0"
  },
  "settings_overrides": {
    "olimar.respawns": {"days": 1},
    "olimar.capacity": {"maximum": 60}
  }
}
```

These are example declarations, not bundled or published gameplay mods. Install
the required mods separately; `requires` checks versions and startup order but
doesn't download them. [Manifest requirements](/develop/manifest/#requirements)
describe version matching.

## Values

Each override targets a direct required dependency. Optional dependencies and
your own settings aren't targets. Use the same JSON value as the setting's type:
booleans, whole numbers, numbers, text, a choice word, or an array of set words.
Values must satisfy its bounds and choices. Set words must not repeat.

Tsuchi validates the whole declaration before mod code or early content starts.
Unknown settings, hidden settings, opted-out settings, and invalid values block
your mod with a reason. They don't partially apply. A settings declaration that
doesn't set `hidden` or `overridable` already accepts overrides; its author needn't
anticipate your mod. `public` controls reading settings across mods and has no
effect on overrides.

Two mods may request the same value. Different values for one setting block all
mods requesting that setting, and mods depending on them. Load order doesn't pick
a winner. The Mods menu refuses conflicting activation drafts before saving them;
files installed or changed outside the menu are checked again at startup. Disable
one of the conflicting mods to choose which configuration to use.

## Player preferences

Settings show the effective value and `Controlled by` with the controlling mod
names. Controlled values can't be edited or reset. The original preference remains
in the save and can still load if save storage becomes available after startup.

Disabling the controller and restarting the game restores the player's preference.
A controller that fails to start or is blocked releases its overrides. A development
restart includes its configured dependencies, so their startup sees the selected
values again. Removing the controller doesn't erase the player's settings.

An override controls configuration, not past gameplay. Removing a mod won't undo
enemies already defeated, collected items, or other progress saved while it ran.
