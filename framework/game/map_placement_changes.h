// The "placement_changes" of a `maps` entry: rows of the game's own placement lists that a mod moves or removes,
// so that the game places its own objects where the mod says, or not at all.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

struct Json;
struct Text;

// What has been done with mods' changes to the game's lists since the framework started.
struct PlacementChangeCounts {
    uint32_t lists_changed;        // times the game read a list with a mod's changes in it
    uint32_t rows_moved, rows_removed; // rows the game read moved, and rows it did not read
    uint32_t lists_refused;        // times a mod's changes to a list were not made: a row it names is not there
    uint32_t from_save;            // times the game made a changed map from the save's record instead
};

// Whether the "placement_changes" of an entry is written as it should be. `wrong` says what is not.
bool placement_changes_well_formed(const Json& entry, Text& wrong);
// Wraps the game's two readers of a map's placement lists. False when the framework's patch does not have them.
bool placement_changes_start(TSUCHI_Mod* self);
// Whether those wraps are in place, so that a mod's changes can be made.
bool placement_changes_ready();
// Notes the changes of one part of a running mod, which `entry` is the manifest's entry of. False when there is no
// memory.
bool placement_changes_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry);
// The part's mod has stopped: its changes are not made again.
void placement_changes_end(uint64_t part);
PlacementChangeCounts placement_change_counts();

} // namespace tsuchi
