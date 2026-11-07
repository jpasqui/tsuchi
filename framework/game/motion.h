// Throwing an actor and letting the game's physics carry it.
//
// The game stops simulating an actor that has come to rest, and an actor's own
// movement pulls its velocity back toward walking while it stands on the
// ground. So a launch is more than one request: the actor is held awake, the
// velocity is asked for again on the following frames until the actor has left
// the ground, the actor's own movement requests are dropped while it is in the
// air, and the hold is let go once it has landed. A freshly made actor is held
// the same way until it has settled where it was put.
#pragma once
#include <stdint.h>

namespace tsuchi {

// Starts keeping the holds. Called once when the framework starts.
void motion_start();
// Throws the actor at a velocity in centimeters per second, or with `velocity`
// null only holds it awake until it has settled. The caller has checked that
// the actor can be launched.
void motion_begin(uint64_t actor, const float* velocity);
// Stops following an actor and lets go of its hold: it was placed somewhere. An actor that
// leaves play is let go of without this, unless it is being destroyed.
void motion_end(uint64_t actor);
// How many actors are being followed, and whether one of them is this actor.
uint32_t motion_count();
bool motion_following(uint64_t actor);
// A requested launch that has not yet been observed leaving contact.
bool motion_launch_pending(uint64_t actor);

} // namespace tsuchi
