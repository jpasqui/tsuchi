---
title: Install on a console
description: Install, update, and remove Tsuchi on an Atmosphere console.
---

:::caution[WIP]
Tsuchi hasn't been tested on a console yet.
:::

Before you start, use a console that already runs Atmosphere and has Pikmin 4
version 1.1.1 or 2.0.0, or Pikmin 4 Nintendo Switch 2 Edition, installed. Close Pikmin 4 before changing its mod files.

## Install

1. Extract the Tsuchi release archive.
2. Copy the contents of `console` to the SD card's root, merging the `atmosphere`
   folder. Keep both the module and its patch from the same archive.
3. With Python 3.9 or newer, run the installer from the extracted release folder:

   ```sh
   python install.py --npdm "game-dump/exefs/main.npdm" --exefs "SD/atmosphere/contents/0100B7C00933A000/exefs"
   ```

   Replace the paths with your own game dump and SD card. For Switch 2 Edition,
   use its own dumped `main.npdm` and its `0400B7C00933A000/exefs` folder. This
   prepares the launch permissions for native hooks and code-patching cheats.
   Complete this step before starting the game. Keep the generated metadata local;
   the release does not contain game files. If `main.npdm` already exists and differs,
   the installer stops so you can preserve it and resolve any other mod's permissions.
4. Start Pikmin 4 through Atmosphere, then close it. Export Pikmin 4's save
   with your console's save manager. Open `tsuchi/tsuchi.log` in the exported
   save folder. See [Troubleshooting](/players/troubleshooting/) if Tsuchi
   didn't load.

The SD card should contain:

```text
atmosphere/
  contents/
    0100B7C00933A000/
      exefs/
        subsdk9
        main.npdm
  exefs_patches/
    Tsuchi/
      060F827BD2B38703E9EA11E3210D89BC.ips
```

These paths follow [Atmosphere's loader documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md).
Each IPS filename identifies one version of Pikmin 4. Don't rename them. Use the `.pchtxt` file in your emulator. Use the `.ips` file on your console.

The archive holds one `.ips` file for each version of Pikmin 4 that this release supports, and one
`contents` folder for each program ID those versions have. Copy them all. Mods in a `romfs` folder
apply to the program ID whose folder holds them, so a version with its own program ID needs its
own copy of each mod.

## Mods

Extract a Tsuchi mod. Copy the contents of its `romfs` folder into
`atmosphere/contents/0100B7C00933A000/romfs` on the SD card, and for the Switch 2 Edition
into `atmosphere/contents/0400B7C00933A000/romfs` too. For example:

```text
atmosphere/contents/0100B7C00933A000/romfs/
  tsuchi/mods/olimar.clock/
    mod.json
    main.lua
```

Keep the ID folder and its contents as supplied by the author. Multiple mods
share `romfs/tsuchi/mods`, each in its own ID folder, and everything a mod has is
inside its ID folder, including its assets. See [Install a mod](/players/install-a-mod/)
for load failures and save-dependent content.

## Update

If upgrading an older installation without `main.npdm`, complete the installer
step above before launching the new runtime. Code access needs those launch
permissions as well as the module.

Close the game, then replace both `subsdk9` and the Tsuchi IPS patch with the
new archive's copies. Keep the generated `main.npdm`. After a game update,
preserve the old metadata separately and run the installer on a dump of the new
version. Keep one Tsuchi patch set installed. If another mod uses
`subsdk9` or changes a Tsuchi hook site, use versions that the authors have
made compatible before installing them together.

## Remove

Follow each mod author's save instructions before removing Tsuchi or any
map or content mod. Leave an added area and finish the day before removing it.
Then close the game.

To remove one mod, delete its ID folder. Some older mods also put `.pak`, `.ucas`, and
`.utoc` files in `romfs/Carrot4/Content/Paks`. The game loads those by itself, so
delete them too when you remove the mod they came with.
To remove Tsuchi, delete its generated `main.npdm` and `subsdk9` files and the `exefs_patches/Tsuchi`
folder. Leave other files in the game's `exefs` folder in place.

Saved content IDs and Piklopedia progress remain after removal and don't prevent
loading. Absent creature entries stay hidden, and added Pikmin use their saved donors.
A saved destination in an added map, or a saved placement that needs the mod's
class, still needs that mod. Reinstall Tsuchi and the mod to load that destination.
