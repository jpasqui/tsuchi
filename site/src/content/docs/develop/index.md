---
title: Developer Guides
description: Guides for anyone who wants to create a Tsuchi mod.
---

Your mod can use a manifest, a Lua or JavaScript script, and compiled code. Choose one
script language per mod. You can also ship cooked Blueprints, use generated Tsuchi
Blueprint nodes, or combine these forms in one package:

| Type | What you can do | Where to start |
| --- | --- | --- |
| Manifest | Change tables, text, maps, and content through declarations. | [Change a table](/develop/change-tables/) |
| Lua | React to game events and call the Tsuchi API. All you need is a text editor. | [Write your first mod](/develop/first-mod/) |
| JavaScript | React to events, call the API, and wait for results with `await`. | [Use JavaScript](/develop/javascript/) |
| C or C++ | Call the Tsuchi API and hook the game's functions. | [Build a compiled mod](/develop/compiled-mod/) |
| Blueprints | Ship cooked engine/game logic tied to your mod, or call Tsuchi through generated nodes. | [Reflection and assets](/develop/reflection-and-assets/) |

For maps, start with [Change a map](/develop/change-a-map/),
[Add placements](/develop/add-placements/), or [Add a map](/develop/add-a-map/). For
content, see [Add a treasure](/develop/add-a-treasure/),
[Add a creature](/develop/add-a-creature/), and [Add shader sets](/develop/add-shaders/).

For code that changes play, try [combat](/develop/combat/),
[the squad and captain](/develop/squad-and-captain/),
[creatures](/develop/creatures/), or [travel](/develop/story-and-travel/).
To read the controller or carried objects, see [Read and press buttons](/develop/input/)
and [Read carried objects](/develop/carry/). For multiplayer, see
[Send network messages](/develop/network/).
For the screen and engine objects, try [dialogs and the HUD](/develop/dialog-and-hud/),
[screens](/develop/screens/), or [reflection and assets](/develop/reflection-and-assets/).

Before you share your mod, read [Events and cleanup](/develop/events-and-cleanup/),
[Save your mod data](/develop/save-data/), and [Publish your mod](/develop/publish/).

:::note[Coming soon]
Tsuchi's Blueprint nodes, published services, and building-block mods are coming soon.
:::

To share logic across languages, publish a service that other mods can find. A
*building-block mod* supplies reusable content and declares typed placement values
with defaults. Other creators depend on its ID, tune individual placements, and
create variants under their own IDs. The mod list marks a building-block mod as
installed because another mod needs it.

When you need the details of a specific function, check the [reference](/reference/).

If you want to edit the game's maps in Unreal Engine, check out
[Kadan](https://kadan.jpasqui.dev/).

If you only want to install mods, check out the [Player Guides](/players/).
