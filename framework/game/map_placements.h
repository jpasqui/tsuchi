// The "placements" of a `maps` entry: lists of gameplay objects a mod has the game place in one of its maps,
// so that the game makes them, saves them and restores them as it does its own.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

struct Json;
struct Text;

// What has been done with mods' lists since the framework started.
struct PlacementCounts {
    uint32_t lists_given;   // lists whose rows all went in, and that have their mark in the map
    uint32_t lists_known;   // lists a map had the mark of already: nothing of them was given
    uint32_t lists_refused; // lists that could not be loaded, or were not laid out as a list is
    uint32_t lists_partial; // lists a row of which was refused: they got no mark
    uint32_t rows_given, rows_known, rows_refused; // rows that went in, that the map had already, and that did not go in
    uint32_t actors_made;   // actors the game made for the rows that went in
};

// Whether the "placements" of an entry is written as it should be. `wrong` says what is not.
bool placements_well_formed(const Json& entry, Text& wrong);
// Uses the existing framework context for classes the generators retain.
void placements_start(TSUCHI_Mod* context);
// Notes the lists of one part of a running mod, which `entry` is the manifest's entry of. Nothing is loaded
// until the map is. False when there is no memory.
bool placements_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry);
// The part's mod has stopped: it gives no more lists.
void placements_end(uint64_t part);
// A look at the loaded world, taken once every level asked for is in it. Gives the lists of every part on the
// map, once for a world, when the game has made the map's generators. `failed` tells whether a part is one whose
// levels could not be loaded.
void placements_look(uint64_t world, const char* map, bool (*failed)(uint64_t part));

} // namespace tsuchi
