// Parts a mod adds to actors. An added part belongs to the mod that added it and
// is destroyed when the mod removes it or stops, and when its actor is put away:
// an actor the game keeps to use again leaves as the game made it. With an actor
// the game destroys, the engine destroys the part.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void added_start();

// Adds a part for a mod. See game::add_part for what can go wrong.
TSUCHI_Result add_part_for(TSUCHI_Mod* mod, uint64_t actor, uint64_t type, uint64_t parent, const char* socket, uint64_t& part);
// Destroys a part the mod added. TSUCHI_NOT_FOUND for a part no mod added;
// TSUCHI_DENIED for one another mod added.
TSUCHI_Result remove_part_for(TSUCHI_Mod* mod, uint64_t part);

// How many added parts there are now.
uint32_t added_count();

} // namespace tsuchi
