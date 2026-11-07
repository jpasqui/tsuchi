// Telling mods when an actor loses life. The game's two functions that change
// life are wrapped: the one that takes life for a hit, and the one that sets it
// outright. Life is read before and after each, and a drop is told at the start
// of the next frame, with who did it when the game named anyone.
//
// Pikmin have no life. The function in which the game counts a Pikmin's death is
// wrapped as well, and each death is told the same way, with its cause.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Wraps the game's functions. The hooks belong to `self`, the framework's own
// context. Whatever cannot be wrapped is left alone and shows in the counts.
void life_events_start(TSUCHI_Mod* self);

struct LifeEventCounts {
    uint32_t sites;   // of the three functions, how many are wrapped
    uint32_t drops;   // drops in life seen since the start, while anyone was listening
    uint32_t waiting; // seen and not yet told
    uint32_t deaths;  // Pikmin deaths seen since the start, while anyone was listening
};
const LifeEventCounts& life_event_counts();

} // namespace tsuchi
