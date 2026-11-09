---
title: Publish your mod
description: Package your mod and keep IDs, dependencies, and saved data stable.
---

Before you share your mod, [install it](/players/install-a-mod/) from the folder you
plan to send and check its [log](/develop/logs/). Test with the Tsuchi and game
versions you list as requirements, and test an update with saved data from the
previous version.

## Folder

Package the mod under `romfs/tsuchi/mods/<mod id>/`. For example:

```text
romfs/
  tsuchi/
    mods/
      example.gems/
        mod.json
        main.lua
        lib/
          gems.lua
        Gems.pak
        Gems.ucas
        Gems.utoc
```

For scripts, include `main.lua` or `main.js`, using one script language per mod. For
compiled code, put `mod.nro` in the mod folder. If you also include a script, Tsuchi
starts it before your compiled code. Include every file your code or
manifest reads.

## Container

If your mod has cooked assets, such as Blueprints, meshes, textures, or a map, put them in
one *container*: the `.pak`, `.ucas`, and `.utoc` files that Kadan or Unreal's packaging
tools write. Keep the three files next to `mod.json` in your mod's folder. You can name them
whatever you want, as long as all three share the name and it only uses letters, digits,
`.`, `_`, and `-`. Tsuchi mounts your container before the title screen, as long as your
mod can run, so a player who removes or blocks your mod doesn't get its assets either.

A package in your container at one of the game's paths replaces the game's package. If
another mod replaces the same package, the game uses the one from the mod that loads later,
and both mods' logs name the package. The game loads some packages as it boots, before
Tsuchi mounts anything, such as its boot map and what that map uses, so those can't be
replaced.

Don't put your container in `romfs/Carrot4/Content/Paks`. The game loads everything there
by itself as it starts, so your assets would stay in the game even when your mod is blocked
or removed from the mod list. Tsuchi writes a line in its log for each container it finds
there.

Zip the `romfs` folder so players can follow [Install a mod](/players/install-a-mod/).
Keep build tools, source-only files, your own save data, and extracted game assets
out of the player archive. Include only assets you made and files your mod needs.
Keep references to the game's package and object names, and ship supported edits
through your manifest or map changes file.

## In-game discovery

To appear in the GitHub browser, make the repository public, add the **`tsuchi-mod`**
topic in its About settings, and attach your ZIP to a GitHub release. The browser
uses release attachments; GitHub's generated source archives are not installable
packages. Draft releases are excluded, and players must opt into prereleases.
See [GitHub's topic settings](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/classifying-your-repository-with-topics)
and [release documentation](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases).

Use the folder layout above, with one or more mod ID folders under
`romfs/tsuchi/mods/`. A single outer release folder is accepted. Outside that tree,
only `README.md`, `README.txt`, `LICENSE`, and `LICENSE.txt` files are ignored.
Other files are refused. Do not include links, hidden paths, or parent-directory
components. GameBanana downloads use the same package rules.

Limits are 512 MiB for the ZIP, 1 GiB expanded, 256 archive entries, 16 bundled mods,
and 511 bytes per path. Stored and DEFLATE compression are supported; encrypted,
ZIP64, and split archives are refused. Keep manifests below 1 MiB. Every bundled
mod must declare a compatible game/framework requirement and a unique ID. Required
mods must already be installed or included in the package. Repository names and
release tags do not substitute for manifest IDs or versions.

The downloader checks each extracted file's CRC and a release's SHA-256 digest when
the provider supplies one. A valid package is published only after manifest and
dependency checks. A topic identifies a source for discovery; it does not endorse
its contents.

## Identity

Choose a unique [ID](/develop/ids-and-kinds/) and keep it for updates. The mod's
folder and `mod.json` must use the same ID. For `example.gems`, a treasure named
`ruby` has the full ID `example.gems:ruby`.

Changing the mod ID also changes where Tsuchi looks for settings and saved mod
data. Changing a content ID can leave saved progress referring to the old content.
Keep those IDs stable unless you've planned and tested a migration.

## Dependencies

Declare required mods under `requires`, optional integrations under `optional`, and
mods that can't run with yours under `conflicts`. A numbered requirement matches
the same major version with a minor/patch version at least as high, and `*` accepts any
version. Tsuchi starts dependencies first, then follows the player's order, and
explains blocked requirements in the mod list and log.

:::note[Coming soon]
The mod list, the player's load order, published services, and building-block mods are coming soon. Until then, Tsuchi loads mods in order of mod ID after their dependencies.
:::

If other mods need to call your logic, publish a service so they can call it from
any language. If you make a building-block mod for other creators, declare its
placement values with their types and defaults, so mods that depend on yours can tune
each placement and make variants under their own IDs. If your mod replaces something
that only one mod can replace, such as one of the game's areas, players who also have
another mod that replaces it will choose one of them in the mod list before the world
loads.

Set `framework` to your minimum whole-number API version. Set `game` if the mod
depends on one game version. Read [Compatibility](/develop/versions/) before
allowing an older framework than your SDK.

## Updates

Raise the mod's three-part `version` when you release an update. Describe the
changes players need to know, including changed settings, dependencies, and any
save migration. Keep a version marker in your [saved data](/develop/save-data/)
so the new code can recognize the previous format.

Tell players to close the game before replacing the mod's files. A compiled mod's
code stays loaded for the whole run, so replacing `mod.nro` won't replace code
already in memory. Include the content guides' removal rules with your release
when the mod adds treasures, creatures, Pikmin types, or areas.
