// Collision parts a mod holds switched off. The game keeps a record of each of an
// actor's collision parts: whether it can be hit, whether it hits, whether
// anything can latch on to it. Switching a part off clears those and stops the
// part colliding at all, until the mod switches it on again, stops, or the actor
// leaves play. One mod at a time holds a part off.
//
// This and suspending an actor's solidity both change how a part collides. To
// keep what each remembers true, a part is not switched while its actor's
// solidity is suspended.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void switched_start();

// Switches a part off for a mod. TSUCHI_UNSUPPORTED for a part the game keeps no
// collision record of, or when the game's records cannot be rebuilt safely;
// TSUCHI_BUSY when another mod holds it off or its actor's solidity is suspended.
// Nothing is changed then. TSUCHI_OK and nothing new when the mod already holds it off.
TSUCHI_Result switch_part_off(TSUCHI_Mod* mod, uint64_t part);
// Ends the mod's hold. TSUCHI_NOT_FOUND when it has none on this part; TSUCHI_BUSY while the actor's solidity is suspended.
TSUCHI_Result switch_part_on(TSUCHI_Mod* mod, uint64_t part);
// True while any mod holds the part off.
bool part_switched_off(uint64_t part);

// How many parts are held off now.
uint32_t parts_switched_off();

} // namespace tsuchi
