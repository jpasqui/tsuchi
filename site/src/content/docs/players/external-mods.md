---
title: External mods
description: Import asset packs, external modules, patches, and individual cheats.
---

Close Pikmin 4 before importing a mod or applying file changes. Use the external
manager with one merged emulator mod folder, or with an SD card mounted on the
computer. Keep the same root and layout for every command.

This development workflow uses the computer's manager to apply external changes.
Restarting the game alone doesn't apply a saved external request. The game menu
stores requests in its save; export that file before applying it. Console file
layout checks run on the host. Console operation and the new menu rows still
need game qualification.

## Asset packs

For a supported cooked asset pack, put its `.pak`, `.utoc`, and `.ucas` together
in a source folder, then import the pack as a code-free Tsuchi mod:

```sh
python tools/external_mods.py --root path/to/overlay import path/to/pack --kind assets --id external.onion --name "Onion pack"
```

The manager installs unchanged files in the mod's ID folder. Use
the Mods menu to enable or disable that mod, save the choice, and restart. No
script or source-code conversion is required. Remove the pack's old global copy
from `Carrot4/Content/Paks` first; the game loads that copy independently of the
mod's enabled choice. The manager refuses a detected global duplicate.

## Executable and loose-file mods

For an external package, keep its `romfs` and `exefs` folders inside the source
folder. Import creates an inventory and leaves the external files disabled:

```sh
python tools/external_mods.py --root path/to/overlay import path/to/package --id external.example --name "Example"
python tools/external_mods.py --root path/to/overlay inspect
```

The game reads the installed catalog and lists imported external packages beside
Tsuchi mods. **X** opens an external package's details. Change its enabled choice,
then press **B** and save. The status says **Apply with manager** while the files
still have their previous state. External packages have no Tsuchi settings or
load-order controls.

The menu shows at most 128 rows in this version. Tsuchi mods keep their places,
then external packages fill the remaining rows in catalog order. An overflow
notice appears if some packages don't fit. Use the computer manager to inspect
and control those packages; their saved requests remain intact.

After closing the game, export `tsuchi/external-requests.json` from its save, then
apply that file before starting the game again:

```sh
python tools/external_mods.py --root path/to/overlay apply path/to/external-requests.json
```

On an emulator, the request file may be accessible in the emulator's savedata
folder. The manager doesn't search unrelated save folders. To choose activation
on the computer, use `set`:

```sh
python tools/external_mods.py --root path/to/overlay set external.example enabled
python tools/external_mods.py --root path/to/overlay set external.example disabled
```

Use a newly exported request after importing packages or changing their applied
state. The manager refuses a request from an earlier catalog revision.

The saved request can remain in the game's save after apply. If it contains every
current manageable package and each choice matches the observed files, the menu
shows no pending change even when the catalog revision has changed. This means
the file choices match; it doesn't establish that a mod ran. Incomplete or
different stale requests still produce a load warning. Review the choices before
saving another request.

## Mods that use subsdk9

Choose a free slot for Tsuchi before creating an external-manager registry. For
example, build Tsuchi in slot 8 to keep a foreign mod's `subsdk9` unchanged:

```sh
python tools/build_module.py --module-slot 8
```

Install that build's `subsdk8` and matching patch. If replacing an existing Tsuchi
installation, remove the previous Tsuchi module before installing the new one;
keep foreign modules at their original filenames. Configure the manager with the
same slot for every command:

```sh
python tools/external_mods.py --root path/to/overlay --tsuchi-slot 8 import path/to/package --id external.example --name "Example"
```

Slot 9 remains the default. The manager protects the selected Tsuchi slot and
allows the other slots. An existing registry refuses a different slot selection.
Moving Tsuchi resolves the filename collision; each mod still needs compatible
hook sites, patches, permissions, and game-version assumptions.

## Cheats

Put one Atmosphere-format cheat file in the source folder's `cheats` directory.
Name the file with the first 16 hexadecimal characters of the supported game's
build ID, followed by `.txt`, then import the set:

```sh
python tools/external_mods.py --root path/to/overlay import path/to/cheat-set --kind cheats --id external.codes --name "Codes"
```

The manager supports one imported cheat set per installation. Each normal
`[name]` block appears as a separate menu entry, initially off. Save the chosen
codes, close the game, export the requests, and apply them with the manager.
Restart the game afterward. The manager includes a required `{master}` block
when any normal code is selected. Master code has no independent toggle.

The manager writes selected code blocks in source order. Console layouts also
write `cheats/toggles.txt` with those codes enabled. With every code off, the
generated cheat and toggle files are absent. An existing unmanaged cheat or
console toggle file stops import or apply; the manager preserves that file.

Disabling a code stops its execution after apply and restart. It doesn't undo
earlier memory writes or saved effects. Choose compatible codes; the manager
doesn't interpret their behavior or detect every conflict between them.

Names must be unique, nonempty, and at most 63 UTF-8 bytes. The supported format
has up to 127 normal blocks, one master, and 256 words per block. Source and
generated files must be smaller than 64 KiB. Selected blocks, including master
code, may contain at most 1,024 words. These limits bound encoded size; they
don't bound execution time. Comments, malformed words, empty blocks, and
master-only sets are refused.

## Formats

| Payload | Layout |
| --- | --- |
| Loose files under `romfs` | Emulator and console. Tsuchi's own folders are protected. |
| External modules under `exefs/subsdk0` through `exefs/subsdk9` | Emulator and console, excluding Tsuchi's selected slot. Check the author's game-version and slot requirements. |
| Build-matched `.pchtxt` files under `exefs` | Emulator. |
| Build-matched `.ips` files under `ips` | Console. The manager gives each package its own patch directory. |
| Supported cooked container triples | Imported with `--kind assets` and controlled as Tsuchi mods. |
| Build-matched files under `cheats` | Imported with `--kind cheats`; each normal code has a separate next-launch choice. |

For a mounted SD card, add `--layout console` after the root argument. The root
then means the SD card root. The manager uses the supported title's Atmosphere
paths. It refuses `main`, `rtld`, `sdk`, `main.npdm`, Tsuchi's selected module
slot, shared destinations, and unrecognized payload forms.

## Recovery

If applying files stops before completion, keep the game closed and recover the
pending operation:

```sh
python tools/external_mods.py --root path/to/overlay recover
```

The manager checks owned file hashes before moving anything and keeps inactive
payloads outside the game's loaded folders. An unexpected replacement stops the
operation. Preserve that file and inspect the reported conflict before retrying.
Don't launch with an unfinished journal; executable files load before Tsuchi can
report a problem in the game.

The manager checks supported patch formats and shared locations. Those checks
don't establish gameplay compatibility between mods. Follow the authors'
instructions for saves, game versions, and combinations of mods.

Patch comparisons cover registered packages and Tsuchi's reserved locations.
Unmanaged patches with different filenames aren't scanned in this version. Import
starts from a separate package folder; it doesn't adopt files already installed
outside the manager. The runtime still checks managed asset-container identities
when it mounts packs.
