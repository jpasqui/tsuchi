// The manifest's "new_maps": maps of a mod's own, each an area the player chooses in the game's list of areas
// and travels to. DESIGN.md, "A map of a mod's own", has the rules.
#pragma once
#include <stdint.h>

#include "ids.h"
#include "tsuchi/asset.h"
#include "tsuchi/core.h"

namespace tsuchi {

// Adds the part to the manifest. After `mods_start`. `self` is the framework's own context: the hooks on the game's
// menu of areas, put there with the first map, and what is made in the game's table of bases belong to it, and stay
// when a mod stops.
void new_maps_start(TSUCHI_Mod* self);

// The name the game knows a running mod's map by, which is its level's, such as `Area005_P`. TSUCHI_NOT_FOUND for
// an ID that is no running map's, and TSUCHI_NOT_READY while the game has not been given the map's scene yet.
TSUCHI_Result prepare_new_map(const char* id, uint64_t story);
TSUCHI_Result new_map_level(const char* id, char level[10]);
// The ID of the running map whose level has this name. False when it is the level of none.
bool new_map_id(const char* level, char id[kIdBytes]);

// Internal transient preview claims. They never enter the allocation record or
// the area menu. A claim with admitted native references lasts for the process.
TSUCHI_Result reserve_preview_area(uint64_t key, uint32_t* area);
void release_preview_area(uint64_t key);
// Admission registers/pins the scene and base row, without changing StorySave.
// retained becomes true before framework pins or native table/list mutations,
// including partial failures. The caller must then preserve its reservation.
TSUCHI_Result give_preview_area(TSUCHI_Mod* owner, uint64_t key, TSUCHI_AssetBundle bundle,
                               const char* world_package, const char* scene_object, bool* retained);
// After restoring the preview's save baseline, prepare only its current in-memory
// story and base row. Accepts a paused story world, including the preview area.
TSUCHI_Result prepare_preview_area(uint64_t key);

// How many maps of running mods have one of the game's free areas now.
uint32_t new_map_count();
// What has been done for them since the framework started.
struct NewMapCounts {
    uint32_t declared, refused;                    // maps of mods that started, and maps that blocked their mod
    uint32_t scenes_added, scenes_found;           // scenes put in the game's list, and scenes that were in it already
    uint32_t scenes_waiting;                       // maps whose scene the game has not been given yet, now
    uint32_t scene_list;                           // how long the game's list was when last looked at
    uint32_t base_rows_made, base_rows_found, base_rows_failed; // rows of the game's table of bases
    uint32_t names_set, names_failed;              // areas' names given to the game as text
    uint32_t record_reads, record_writes, record_failures, record_damaged; // the record of which map has which area, in the save
    uint32_t menu_hooks;                           // of the two entries of the menu of areas, how many are hooked
    uint32_t menus_opened, menus_listed;           // menus of areas set up, and those that listed new areas
    uint32_t rows_named;                           // rows given a map's name
    uint32_t zooms_grown, zooms_failed;            // the menu's lists of zoom values made longer, and menus left alone because one could not be
    uint32_t panels_fitted;                        // panels drawn smaller for their rows
    uint32_t area_records_made, bases_opened;      // what was written to a story for a new area
    uint32_t ui_rows_made, ui_rows_failed;         // rows of the framework's own in the game's tables of what the list of areas and the radar show: made, and could not be
    uint32_t ui_waits;                             // times what a map shows waited for a table the game could not give
    uint32_t previews_set, radars_set, pikmin_set, totals_set; // maps whose preview, radar, types of Pikmin and totals the game was given
    uint32_t messages_set;                         // rows of the list's messages told they are for a map's area
    uint32_t places_set;                           // rows of types told their place in a list that was opening
    uint32_t insets_shown, insets_put_back;        // times the small map beside the list drew a map's radar, and was given back to the game
    uint32_t counts_written;                       // counts beside the list filled with a declared total where the game wrote none
};
NewMapCounts new_map_counts();

} // namespace tsuchi
