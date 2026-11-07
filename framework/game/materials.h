// Materials a mod holds in the slots of a mesh. A mod puts a material in a slot
// and holds it there until it restores the slot, stops, or the actor leaves play;
// the slot then goes back to what the game last wanted in it. One mod at a time
// holds a slot.
//
// The game changes materials for reasons of its own. Each held slot is looked at
// every frame: what the game has put there is remembered as what to go back to,
// and the mod's material is put in again.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void materials_start();

// Holds a material in a slot for a mod, or changes the one the mod holds there.
// TSUCHI_UNSUPPORTED for a part that is not a mesh; TSUCHI_INVALID for something that
// is not a material or a slot the mesh does not have; TSUCHI_BUSY when another mod
// holds the slot; TSUCHI_NOT_READY when the engine cannot be asked. Nothing is
// changed then.
TSUCHI_Result hold_material(TSUCHI_Mod* mod, uint64_t part, uint32_t slot, uint64_t material);
// Ends the mod's hold. TSUCHI_NOT_FOUND when it has none on this slot.
TSUCHI_Result restore_material(TSUCHI_Mod* mod, uint64_t part, uint32_t slot);

// The framework fallback's source handle, or zero. Used by shader-set cleanup.
uint64_t shader_slot_source(uint64_t part, uint32_t slot);
// How many slots are held now.
uint32_t material_holds();
// What keeping the held slots has done since the framework started: slots looked at in a step, those in which
// the game had put something other than what was last shown, and the setter calls that put the hold's back.
struct MaterialHoldCounts { uint32_t kept, changed, set, unchanged; }; // unchanged: looked at and found as last step, not resolved again
MaterialHoldCounts material_hold_counts();

} // namespace tsuchi
