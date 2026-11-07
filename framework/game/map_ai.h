// The "ai" of a `maps` entry: a file of AI values for creatures the game places in one of its maps, put in as
// each creature's AI part starts when its map part is ready, or deferred until the levels arrive.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

struct Json;
struct Text;

struct AiCounts {
    uint32_t files, placements; // of running mods: their AI files, and the placements those name
    uint32_t tables;            // function tables of AI parts whose start is wrapped: none, or all the framework knows
    uint32_t enum_walks;        // complete object walks to find a drop enum; misses wait sixty frames before retrying
    uint32_t set, refused;      // successful serializer applications (including deferred reads), and refused placements
};

// Whether the "ai" of an entry is written as it should be. `wrong` says what is not.
bool ai_well_formed(const Json& entry, Text& wrong);
// Reads the file one part of a running mod names, if it names one, and keeps it for as long as the part is
// there. Anything but TSUCHI_OK blocks the mod, and `problem` says why.
TSUCHI_Result ai_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry, Text& problem);
// The part's mod has stopped: its file sets nothing more.
void ai_end(uint64_t part);

} // namespace tsuchi
