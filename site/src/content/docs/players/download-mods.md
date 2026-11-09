---
title: Download mods in game
description: Browse public GitHub and GameBanana packages from the title screen.
---

Open **Mods** on the title screen. **Installed** opens first. The sidebar then has
**Search**, **Popular**, **Top**, and **New**. GitHub and GameBanana mods appear together in the
thumbnail grid. Select a card to see its description, pictures, author, and files.
Choose a ZIP and select **Install**. The result says whether its content is available
to new loads or needs a restart.

The grid loads more cards as you scroll. Back returns from a mod to the selected
card and its scroll position; from the grid it returns to the sidebar, then to
the title screen. The footer shows the controls for the current view. Selecting
an author opens their profile and mods.

GitHub lists public repositories with the `tsuchi-mod` topic. You do not need to
enter a repository URL or sign in. Authors can publish on either service. GitHub
files come from published releases; draft and preview releases are excluded.
A repository may appear without an installable ZIP. The installer checks the
package before making it available to the loader.

## Downloads

A mod's detail page shows download progress and offers Cancel. You can leave the
browser while a transfer runs. Cancelled or failed downloads do not replace
installed mods. If one source fails, results from the other remain available
with a retry notice.

The installer accepts Tsuchi packages and existing cooked asset packs containing
complete `.pak`, `.utoc`, and `.ucas` sets. Asset packs may be loose in the archive
or inside a RomFS/release folder. Each set becomes a code-free mod with unchanged
assets and a stable generated ID. Incomplete sets and mixed unsupported payloads
are refused before publication. Loose non-container replacements,
executable patches, native modules, RAR, and 7z are not imported by this path yet.

Tsuchi packages use the [mod layout](/develop/publish/#folder). Required dependencies
and conflicts are checked before publishing either an installation or an update.
If a required dependency is missing, install it first. Bundled dependencies can
be installed together in one ZIP.

## Installed packages

### Cheat packages

ZIPs containing Atmosphere-format `cheats/<build>.txt` files can be installed
without rewriting each cheat. A build name has 16 hexadecimal digits; filenames
are normalized to uppercase while the file contents and cheat names stay intact.
Release folders and Atmosphere title folders are accepted. A title folder naming
another game, malformed cheat text, duplicate build files, or a mixture with other
payloads is refused. This importer accepts ZIPs, not standalone text downloads.

Restart after installing or updating a cheat package so the runtime loads its
files. Open the package in **Installed**, then **Cheats**. Each named cheat starts
off until you enable it; saved choices return on later launches. Its switches work
during the session. Longer lists have **More cheats** and **Previous cheats**.
The runtime selects the file matching the running game's build ID.

Turning a cheat off stops its writes and restores instructions it patched. Values
already written, such as money, remain; a frozen value becomes free to change.
The page shows unavailable cheats with their reason instead of an enabled switch.
Code access is included in normal Tsuchi installations. Complete the metadata
step in [Install Tsuchi](/players/install-tsuchi/) or [Install on a console](/players/install-console/)
before using code-patching cheats. Importing a cheat cannot change launch permissions.
Some problems are discovered when the runtime first tries to run the cheat.

If the choice cannot be saved, the page reports that the change applies for this
session only. A rejected change, including the 1,024-word limit across the master
block and active cheats in a file, restores the displayed switch to its current
state. The files stay inside Tsuchi's mod folder, not the emulator's cheat folder.

### Package controls

**Installed** includes discovered Tsuchi mods and completed downloads. Select a mod
for its enabled choice, declared settings, source page, update check, and package
removal where the downloader owns it. Settings use the same forms as Settings >
Mods, including controlled settings. A legacy asset pack has no invented settings.

**Check for updates** refreshes the source page and compares the published files
with the installed file. A newer timestamp or changed supplied digest identifies
newer content. Other files can be alternative versions or variants; select the
file you want. Failed requests and missing/truncated file lists do not establish
that a mod is up to date. Old installations without comparison metadata can still
open their page, but may report that the version is unknown.

An update is extracted into a fresh directory and validated before its receipt is
published. Only the same source and same set of mod IDs can replace the selected
package. A failed update leaves the previous version selected. Earlier files stay
intact while the running game may still use them, and saved preferences are kept.
Uninstalling an update does not reactivate an older version.

## Applying changes

Imported asset packs can mount during the session for subsequent package loads.
Already loaded objects keep their current assets: reload the affected map or
objects to see a newly mounted replacement. This path does not unload containers.
Disabling mounted content records an off choice for the next launch and says that
it remains active this session. Replacing a retained container with an update may
also need a restart. Native package activation still uses the saved startup choice;
individual mod settings retain their existing live behavior.

Downloaded packages live on the SD card under `tsuchi/downloads`. Removal
records a tombstone for the selected revision; files remain because the running
game may still use them. Keep revision receipts together: they prevent older revisions from becoming active again.

## Development status

The restored frontend has been checked in Eden on game version 1.1.1, including
live GameBanana thumbnails, details, scrolling, and Back. Installed-first opening,
mod activation choices, toggle and integer settings, and returning from management
to the browser have also been checked in-game. Mixed-source browsing, legacy ZIP
import, update publication and rollback, old receipts, loader restart discovery,
and late container mounting have host tests.
Cheat controls have been checked in Eden 1.1.1 with a test package: live on/off,
choices surviving restart, pagination, unavailable instructions, code-access
refusal in an older installation, and returning to Installed. The standard
code-access build was also checked with locally prepared launch metadata: an
authored cheat changed an unused code word, and disabling it restored the original
bytes without restarting. This does not establish compatibility with every
community cheat or qualify console behavior.
Cheat ZIP import and update rollback have host tests; the live test package was
staged locally rather than downloaded from a public release.
The new asset-import path has not yet been qualified end to end with a real cooked
map downloaded and mounted during a game session. Its host tests do not establish
that result. Other game builds need native frontend mappings before enabling it.
