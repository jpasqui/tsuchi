# Tsuchi

Tsuchi is the framework that loads Pikmin 4 mods and the API they are written against.

Guides, downloads and the API reference are on [tsuchi.jpasqui.dev](https://tsuchi.jpasqui.dev). Releases are on
[GitHub](https://github.com/jpasqui/tsuchi/releases), and help is on [Discord](https://discord.gg/Rg5BnvuwmQ).

A mod is a folder with a manifest. Its script can be Lua or JavaScript, and it can
load a compiled module. Mods can also include cooked game and engine Blueprints.
The framework implements settings and manifest content, including new maps, treasures
and creatures.
For catalog, area, removal and shader limits, read [Metadata rules](docs/metadata-rules.md).

- [CONTRIBUTING.md](CONTRIBUTING.md): development setup and pull requests.
- [CI and releases](docs/CI.md): automated checks, candidate artifacts, and draft releases.
- [Discord announcements](docs/DISCORD.md): published release notes and webhook setup.

Tsuchi runs on Pikmin 4 1.1.1 and 2.0.0 for Switch, on Pikmin 4 Nintendo Switch 2 Edition, and
on the Switch 2 demo. `game/table.yaml` holds 1.1.1's values and `game/builds/` the others'.
[Build profiles](docs/CI.md#build-profiles) describes how they are checked.

Tsuchi's source is available under the [MIT license](LICENSE). Dependencies keep their own
licenses and [third-party notices](tools/Tsuchi-ThirdPartyNotices.txt).

## Layout

| Path | Holds |
| --- | --- |
| `schema/` | The API, described once. Headers, bindings and reference pages are generated from it. |
| `tools/gen/` | The generators. |
| `framework/core/` | Mods, what each mod owns, and events. Every service is built on it. Also the core services' own code. |
| `framework/base/` | Reading the game: checked memory access, names, objects, handles, classes, fields and functions. |
| `framework/game/` | Reading this game: its actors, and the code of the services about them. |
| `framework/target/` | The build that runs in the game: start-up, finding the game, the per-frame entry. |
| `game/table.yaml` | Every address and offset the framework uses. |
| `game/fixtures.yaml` | Addresses and paths that only the in-game check mods read. |
| `runtime/lua/` | What the generated Lua bindings are built on, the few bindings written by hand, mods written in Lua, and the C library Lua is built on in the game. |
| `runtime/javascript/` | QuickJS integration, JavaScript bindings, and mods written in JavaScript. |
| `sdk/` | What a compiled mod is built with: the build tool, and the start-up and support code that goes into each mod. |
| `tests/` | The checks, a stand-in for the game they run against, and the mods the in-game checks install. |
| `site/` | The docs site: the manual pages, and the reference pages generated into it from the schema. |
| `third_party/` | Lua 5.4.7 and QuickJS 2025-09-13, with their licenses. |

Generated files go to `build/` and are not committed.

## Frame limit and VSync

In the framework's `romfs/tsuchi/settings.json`, `frame_limit` is 30, 60, 120 or 0
(uncapped). Higher frame rates remain experimental. VSync is a separate optional
boolean; for example:

```json
{"frame_limit": 120, "vsync": false, "presenter_log": true}
```

`vsync: true` waits for the screen's refresh, so a 120 limit still shows 60 frames
a second on a 60 Hz screen. `false` drops that wait, and the frame limit still
applies. Without `vsync`, the game's own setting is kept, and removing `vsync` gives
back the game's latest setting. Eden's own VSync and engine-speed limit are separate
and can cap the result. Turn both off in Eden when testing rates above 60. The
requested frame limit is a ceiling; the scene and machine determine the measured rate.
Restart the game after changing the file. Tsuchi reads it again once a second,
but Eden does not let the file be replaced while the game runs, so changes during play
have only been tested on the host. Deleting the file goes back to 30 and the game's
VSync setting. With logging on, the log gives the requested limit, the requested and
actual VSync, and the measured frames and renderer calls a second. Those counts are
not the screen's own refreshes.

The separate optional `pose_presentation` setting is `selective` by default.
`smooth` also interpolates qualified small skeletal poses and native updates up
to four world steps apart. It adds pose submissions and interpolation delay;
native animation evaluation stays separate. At 30 frames per second no extra
poses are drawn. Mods can own this choice through `time.hold_pose_presentation`
without owning frame limit or VSync. Release restores the latest file preference.

## Video and Mods menus

Tsuchi includes the UI and mod manager. Open the game's Settings
and use L / R to switch tabs. Mods lists installed mods, reviews dependency
changes, and saves activation and order for the next restart. Declared settings
open directly with A; X opens details and order controls.

Mods add their own Settings tabs with the `ui` service and change renderer settings
with `reflection.hold_console_variable`. Frame limit and VSync go through the
presenter, so the world keeps its fixed steps. Emulator output scaling and
emulator VSync remain emulator choices.

The audio service reads and owns global Wwise game parameters by
name. Command acceptance and parameter readback do not measure audible output.
See [Custom sounds and audio parameters](site/src/content/docs/develop/custom-sounds.mdx).

## Checks

CI uses Python 3.11.9, Node.js 24.14.0, and LLVM 22.1.1. Install the pinned Python
dependencies before you run the checks:

```sh
python -m pip install -r requirements-dev.txt
```

[CONTRIBUTING.md](CONTRIBUTING.md#setup) has the environment setup steps.

```
python tools/check.py
```

To generate without running the checks:

```
python tools/generate.py
```

To build the module the game loads, and the patch that goes with it (needs LLD as well):

```
python tools/build_module.py
```

This writes `build/module/exefs/`, which holds the module and a `.pchtxt` patch for emulators,
and `build/module/ips/`, which holds the same patch as an `.ips` file. Install the module
and the patch from the same build together. The framework uses the patch only when the two
were built together, and blocks a mod that needs the patch when they were not.

The framework's own checks run in the game: each takes a part of it through what it is for
and reports what happened. They are being moved, a service at a time, from a build of the
framework with the checks in it to mods that make their checks through the API.

A *check mod* is a mod among `tests/fixtures/mods` whose manifest names a check. It runs on
either build, and writes what it finds to the framework's log. To lay the mods out for the
game, every one or the checks of some names:

```
python tools/stage_test_mods.py
python tools/stage_test_mods.py storage
```

This writes `build/test-mods/`, which is installed beside the module. After the run, the log
is on the SD card at `atmosphere/contents/0100B7C00933A000/tsuchi.log`. An emulator keeps its
SD card in a folder on the computer. If SD mounting or the first write is refused, the log
stays in the game's save folder at `tsuchi/tsuchi.log`. To read the checks out of it:

```
python tools/read_checks.py tsuchi.log --staged build/test-mods/checks.json
```

That prints a row for each check, with why it failed or what it still waits for, and exits
with `0` when every check passed.

The checks not moved yet spawn and kill things, travel and write to the save from inside the
framework, so they are not in the module above. A module with them built in:

```
python tools/build_module.py --selfcheck
```

This writes `build/check-module/` the same way. It is never the build people run. Its checks
write to a block of memory that a tool outside the game reads. Those made in a level run one
after another and take about fifteen minutes together. The names given to
`tools/stage_test_mods.py` choose among them as well: the names are written to
`romfs/tsuchi/checks.txt`, and a check of that build that is not named is passed over. The
checks lean on one another in places, so compare a short run with an earlier run of the same
names, and make the whole run before a commit.

To build a compiled mod from its sources (needs LLD as well):

```
python sdk/build_mod.py main.cpp --out my.mod/mod.nro
```

## Docs site

The site is built with Starlight and needs Node.js. The reference pages are generated from
the schema as part of the build, so it needs Python with PyYAML as well.

```
cd site
npm ci
npm run build
```

This writes `site/dist/`, a folder of plain files any web host can serve. A broken link or
anchor fails the build. `npm run dev` serves the site while pages are being written, and
adds an **Edit text** button to every page: turn it on, click a paragraph, a heading, a list
item or a table cell of a manual page, change its Markdown, and save, and the page's source
file is changed. Reference pages cannot be edited this way, since their text is in the schema. Set
`SITE_URL` to the site's address when building for a host.
