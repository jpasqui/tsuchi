---
title: Troubleshooting
description: How to find out why a mod isn't working.
---

Open **Settings > Mods** in the game and select the mod to read why it's blocked, then
check the Tsuchi log for details. See [Manage mods](/players/mod-list/).

## Log

Tsuchi writes a log file to `tsuchi/tsuchi.log` in the game's save folder and also writes
to the system's debug output. A missing file alone doesn't prove Tsuchi failed to load,
as the save may be unavailable. Open the file in a
text editor, and look for the lines that start with the mod's ID. If the mod failed to load,
then one of those lines will say why.

## Errors

| If the log mentions | Try this |
| --- | --- |
| A missing or incompatible dependency | Install and enable the named mod. A numbered requirement needs the same major version and a minor/patch version at least as high. For `1.2.0`, use `1.2.0` or a later `1.x` release, and `2.0.0` won't match. |
| A conflict | Disable one of the conflicting mods on the **Mods** page. |
| The game version | The mod was made for a different version of Pikmin 4, so ask the mod's author if there is a version for yours. |
| The framework version | The mod needs a newer version of Tsuchi, so [update Tsuchi](/players/install-tsuchi/). |
| `mod.json` | Make sure that you didn't rename the mod's folder. If you didn't, then report the problem to the mod's author. |
| A script error | Report the problem to the mod's author, and include the line from the log. |

## Game versions

If Tsuchi doesn't support your version of Pikmin 4, the game runs without mods, and it can show a
message that says so. A version that lacks what the message needs shows none. Update Tsuchi when a
release supports that version. If part of the game changed in a
version, Tsuchi still runs, and the log names the parts it can't serve. Mods that use those parts
get an `unsupported` answer.

## Dependencies

If a mod depends on another mod that failed to load, then it won't load either. Start with
the first error in the log, as fixing it will often fix the others too.

## Reports

When you report a problem to a mod's author, make sure to include the lines from the log that
mention their mod, as this will help them find out what went wrong.
