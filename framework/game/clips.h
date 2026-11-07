// Clips a mod holds on skeletal meshes. A mesh's animation blueprint is put
// aside while one clip plays on the mesh; the hold ends when the mod stops the
// clip or stops itself, or when the actor leaves play, and the blueprint takes
// over again. One mod at a time holds a mesh.
#pragma once
#include <stdint.h>

#include "tsuchi/animation.h"

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void clips_start();

// Plays a clip on a mesh for a mod and holds it there, or changes the clip the mod holds there.
TSUCHI_Result hold_clip(TSUCHI_Mod* mod, uint64_t part, uint64_t clip, bool looping, float rate);
TSUCHI_Result set_held_clip_rate(TSUCHI_Mod* mod, uint64_t part, float rate);
TSUCHI_Result seek_held_clip(TSUCHI_Mod* mod, uint64_t part, float seconds);
TSUCHI_Result describe_held_clip(uint64_t part, TSUCHI_Clip* out);
// Ends the mod's hold. TSUCHI_NOT_FOUND when it has none on this mesh.
TSUCHI_Result stop_held_clip(TSUCHI_Mod* mod, uint64_t part);

// How many meshes are held now.
uint32_t clip_holds();

} // namespace tsuchi
