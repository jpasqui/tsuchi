// The motions of the game's own animation blueprints, on a skeletal mesh.
//
// Such a blueprint has state machines by name, and each plays one motion by
// name; it also has a move motion and a speed. The game sets them through the
// blueprint's own functions, and so does a mod's hold: one mod at a time holds
// a state machine, a move motion or a speed. A hold sets its value again
// whenever the game has set another, waits while no blueprint of the game's
// own runs on the mesh, and puts back what the game last wanted when it ends.
// While a mod listens, the body of each actor in play is looked at once a frame
// and a change of motion is told.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Starts keeping the holds and looking for changes. Called once when the framework starts.
void blueprint_motions_start();

// The reads. TSUCHI_UNSUPPORTED for a part that is no skeletal mesh, or that no blueprint of the game's own animates now.
TSUCHI_Result state_machines_of(uint64_t part, char* buffer, uint64_t capacity, uint64_t* out);
TSUCHI_Result motion_of(uint64_t part, const char* state_machine, char* buffer, uint64_t capacity, uint64_t* out);
TSUCHI_Result move_motion_of(uint64_t part, char* buffer, uint64_t capacity, uint64_t* out);
TSUCHI_Result speed_of(uint64_t part, float* out);

// The holds, and their ends. A release is TSUCHI_NOT_FOUND when the mod holds nothing of that.
TSUCHI_Result hold_motion(TSUCHI_Mod* mod, uint64_t part, const char* state_machine, const char* motion);
TSUCHI_Result release_motion(TSUCHI_Mod* mod, uint64_t part, const char* state_machine);
TSUCHI_Result hold_move_motion(TSUCHI_Mod* mod, uint64_t part, const char* motion);
TSUCHI_Result release_move_motion(TSUCHI_Mod* mod, uint64_t part);
TSUCHI_Result hold_speed(TSUCHI_Mod* mod, uint64_t part, float speed);
TSUCHI_Result release_speed(TSUCHI_Mod* mod, uint64_t part);

// How many holds there are now, of all three kinds, and how many meshes the events look at.
uint32_t motion_holds();
uint32_t motion_watched();

} // namespace tsuchi
