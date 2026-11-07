---
title: Glossary
description: Terms used in the developer guides.
---

| Term | Meaning |
| --- | --- |
| API | The functions, events, and types your mod uses to talk to Tsuchi. |
| Callback | Your function that runs on the game thread when an event is delivered. |
| Blueprint | Unreal's visual scripting. Tsuchi starts your mod's cooked Blueprints with your mod and stops them when it stops, and Tsuchi's Blueprint nodes let them call the API. |
| Building-block mod | Reusable content with typed placement values and defaults, installed as another mod's dependency. |
| Claim | A declared change. Additions run together, stackable changes follow load order, and an exclusive change needs a player-chosen winner. |
| Container | Your mod's cooked assets as one `.pak`, `.ucas`, and `.utoc` file with the same name, next to `mod.json`. Tsuchi mounts it before the title screen while your mod can run. See [Publish your mod](/develop/publish/#container). |
| Content ID | A name such as `example.gems:ruby` for something a mod adds. |
| Donor | Existing content whose class or values you copy as a starting point. |
| Entry | A function pointer in a game table, or a named block of saved mod bytes in storage. The guide names the table or storage when it uses this word. |
| Handle | A value that names an object or resource. An actor handle stops working when the actor leaves play. |
| Hold | A change Tsuchi keeps for your mod until you release it or your mod stops. If your mod stops while it holds the save, the save slots stay locked until you go back to the title screen. |
| ID | Text that names a mod, content, or a kind. A placement number is a separate numeric ID. |
| Load order | Dependencies first, then the player's order, and mod IDs decide the order before the player chooses one. |
| Manifest | Your mod's `mod.json`, which declares its identity, requirements, settings, and data changes. |
| Mark | The API version stored in a compiled mod by the SDK. |
| Placement | An object declared for a map. Its placement number lets you find its current actor. |
| Presented frame | A picture rendered by the game, which can fall between world steps. |
| Service | A group of related API calls, or logic a mod publishes for other mods to discover and call across languages. |
| Stand-in | Save files your mod uses in place of a save slot. After you use a stand-in, the save slots stay locked until you go back to the title screen. |
| Step | One world update, lasting 1/30 of a game second. |
| Subscription | A registered callback that listens to an event until canceled or the mod stops. |
| Vocabulary | The set of names a call accepts. Open vocabularies can include mod content. Closed vocabularies come from the game's code. |
