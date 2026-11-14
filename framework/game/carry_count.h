// A carry counter's number held by a mod (carry_count.cpp), for the carry service.
#pragma once
#include <stdint.h>

#include "carry_service.h"

namespace tsuchi {
// Holds the first number the object's carry counter shows. TSUCHI_INVALID for a count below 0 or an actor outside the
// game's classes; TSUCHI_NOT_FOUND for one that is gone; TSUCHI_UNSUPPORTED without a carry part or on a build without
// the shown count; TSUCHI_BUSY when another mod holds it.
TSUCHI_Result hold_count(TSUCHI_Mod* mod, TSUCHI_Actor actor, int32_t count);
// Ends the mod's hold: the counter shows the carriers' number again. TSUCHI_NOT_FOUND without one.
TSUCHI_Result release_count(TSUCHI_Mod* mod, TSUCHI_Actor actor);
} // namespace tsuchi
