// Hiding parts of actors. A mod hides a part, alone or with the same actor's
// parts attached below it, and holds it hidden until it shows it again, stops,
// or the actor leaves play. A part stays hidden while any mod holds it so, and
// goes back to what it was when the last hold ends.
//
// The game shows and hides parts for reasons of its own, so each hidden part is
// looked at every frame and hidden again when the game has shown it.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void hidden_start();

// Hides a part for a mod. TSUCHI_UNSUPPORTED for a part with no place in the world,
// or when the parts below it cannot all be found; TSUCHI_NOT_READY when the engine
// cannot be asked to hide it. Nothing is changed then. TSUCHI_OK and nothing new
// when the mod already holds this part hidden the same way; with the other choice
// of `children`, its hold covers what is asked for now.
TSUCHI_Result hide_part(TSUCHI_Mod* mod, uint64_t part, bool children);
// Ends the mod's hold. TSUCHI_NOT_FOUND when it has none on this part.
TSUCHI_Result show_part(TSUCHI_Mod* mod, uint64_t part);
// True while any mod holds the part hidden.
bool part_hidden(uint64_t part);

struct HiddenCounts {
    uint32_t parts; // hidden now
    uint32_t holds; // held by mods now
};
HiddenCounts hidden_counts();

} // namespace tsuchi
