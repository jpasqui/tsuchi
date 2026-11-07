# Metadata rules

Your manifest declares what your mod adds. These rules keep those additions separate
from the game's own content and from another mod's content.

| Rule | Your mod | Implementation and host checks |
| --- | --- | --- |
| Catalog entries | A treasure needs `catalog` and a series declared by your mod. A creature needs `catalog`, which may be empty. Without it, no catalog row is added. | [Content](../framework/game/content.cpp), [treasure checks](../tests/game/content_test.h), [creature checks](../tests/game/creatures_test.h). |
| Area numbers | A new map has builds for one or more of areas 5, 7, 8 and 9. The world and scene names must agree. The framework assigns a number and keeps it across save slots. | [New maps](../framework/game/new_maps.cpp), [new-map checks](../tests/game/new_maps_checks.h). |
| Removal | Removing scenery is a runtime map edit. Stopping a mod restores its field overrides, but does not recreate actors it destroyed or move them back in the loaded world. | [Map changes](../framework/game/maps.cpp), [field overrides](../framework/game/map_values.cpp), [map checks](../tests/game/objects_test.cpp). |
| Shader metadata | A set names its donor map hash, shader count and replacement slots. Descriptor edits also name the donor descriptor size. The framework checks those against the loaded donor. | [File format](../framework/game/shader_file.h), [material checks](../framework/game/shader_materials.cpp), [shader checks](../tests/game/shader_checks.h). |

Adding a treasure to the catalog raises its total by one. It does not add to the
completion requirement of a native series. The framework writes the treasure's
series number, order and stage actor name after `catalog.values`, so those values
cannot change its catalog identity. Other catalog fields remain customizable.
A creature's catalog values apply after the donor's stage actors are renamed.

A map's area number becomes reserved when the framework writes its used mark,
before it asks the game to create or use a story's area record. This can happen
in the area menu before the player visits it. A
reserved number stays with that map when its mod is absent. An allocation that has
never reached that point can be reused when the map is no longer available.
There is no automatic erasure of a map's progress to free a reserved number.
Every map in your mod gets an allocation or the mod is blocked.

Leave your mod's new map and finish the day before uninstalling it. A save inside
an absent map can return to the title screen because its world cannot be loaded.
Reinstalling the mod supplies that world again. Stopping a mod keeps the scene
and base row that the game may still use, while restoring its menu and text overrides.

Scenery edits name actors in a sublevel. If any named actor is absent when the
sublevel is first checked, that sublevel's entry changes nothing. A removal wins
over a move or field override, and repeated removal names destroy an actor once.
Reloading the sublevel applies the edits again. A successful field override is
restored when the mod stops or the world ends. Scenery without a leave-play callback
has no guarantee of restoration before another world starts in the same tick.

Prepared `maps.levels` declarations join the incoming world's native initial
streaming flush before its actor-initialization continuation. Their containers
and shader libraries open during manifest preparation; scripts start at the
existing safe world boundary. Missing that initial boundary refuses the entry
instead of requesting the same ground hundreds of world steps later. Entries
created by scripts after startup retain the late dynamic path.

Nested descriptors retain their native class, serialized streaming policy,
priority, composed transform and qualified volume references. An authored
unloaded branch stays unloaded, so its deeper graph cannot establish initial
readiness. Its admitted native descriptor keeps its authored policy, but the
whole entry reports unsupported and remains unready; further changes from that
entry stop. Readiness of only the initially required ground would not establish
ownership of that branch's later descendants. There is no delayed fallback for
those descendants. Hidden loaded branches remain hidden. Initial traversal is bounded
to 256 descriptors and eight levels of ancestry. An incomplete or unsupported
graph is reported explicitly; readiness is separate from request acceptance.
[The focused fixture](../tests/game/initial_streaming.inc) checks the actual
backend against fake native adapters. This source candidate still needs its
matching native build and representative cooked-content qualification.

Gameplay objects belong in placement list assets. Accepted lists and their marks
become part of the game's save state. Removing a mod does not erase those saved
generators or reset the defeated and collected states they hold. Keep the mod's
classes and ground available for saves that use them.

A treasure's catalog row becomes unlisted when its mod stops. A creature's row
must be removed to stop contributing to the total. That removal waits while a
loaded world's row list points to it. Content IDs and area allocations kept in
the framework's save records survive uninstalling a mod.

Shader checks establish the donor map's identity and the file's structure. They
do not compile the shader payload or validate material domain, blend mode and
usage flags. Choose a compatible donor when building your set. Restoring material
slots on mod stop does not free copies that the renderer may still use. Those
copies stay until the game ends.

The linked tests use a host stand-in for the game. They exercise opt-in catalogs,
assigned identities, allocation persistence and refusal, removal conflicts,
cleanup, malformed shader files and donor mismatches. They do not establish a
new rendered game result or prove a clean install. Those require separate runs.
