// The game's accepted support contacts, observed after a native contact update.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

// PhysX's 28-byte transform. Returning this aggregate preserves the native
// ARM64 result buffer in X8, with the body in X0.
struct PhysicsPose {
    float rotation[4], location[3];
};
static_assert(sizeof(PhysicsPose) == 28, "the native PhysX pose has seven floats");

} // namespace tsuchi::game

namespace tsuchi {

// Starts the contact observer. A missing hook leaves queries not ready.
void ground_contact_start(TSUCHI_Mod* self);
// The actor's latest completed native contact update. Does not wake its body.
// Not ready until a contact update was observed, or once the sample is stale.
TSUCHI_Result ground_contact(uint64_t actor, bool* out);
// A move, launch, or change to physics invalidates a preceding contact sample.
void ground_contact_forget(uint64_t actor);

} // namespace tsuchi
