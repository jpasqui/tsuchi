// Actors entering and leaving play. The game's own functions for those
// moments are wrapped: an actor registering with the game, an actor being
// destroyed, and an actor taken from or put back in a pool to be used again.
// From them come the Entity service's two events, and the end of a handle
// when its actor is put away.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Wraps the game's functions. The hooks belong to `self`, the framework's own
// context. Whatever cannot be wrapped is left alone and shows in the counts.
void actor_events_start(TSUCHI_Mod* self);

struct ActorEventCounts {
    uint32_t tables;     // function tables whose two entries are wrapped
    uint32_t pool_sites; // of the three pool functions, how many are hooked
    uint32_t entered;    // actors that have entered play
    uint32_t destroyed;  // that left because the game destroyed them
    uint32_t put_away;   // that left because the game put them back in a pool
    uint32_t in_play;    // that are in play now, by this count
};
// How an actor leaves play.
enum class ActorLeft : uint8_t {
    destroyed, // the game is destroying it, and what was changed on it goes with it
    put_away,  // it lives on to be used again: a pool is taking it back, or its level is taken out of the world
};
// Calls `listener` whenever an actor leaves play, with the actor and the handle it had, before
// that handle ends and while the actor is still as it was in play: before the game takes one
// apart that it destroys, and before it switches off and hides one that a pool takes back. Any
// part of the framework may add one. False when there is no memory for it.
bool on_actor_left(void (*listener)(uint64_t actor, uint64_t handle, ActorLeft how));

// Calls `listener` whenever an actor enters play, with the actor and its handle, from inside the game's
// own function for it. A listener notes the actor and does its work at the next frame. Any part of the
// framework may add one. False when there is no memory for it.
bool on_actor_entered(void (*listener)(uint64_t actor, uint64_t handle));

const ActorEventCounts& actor_event_counts();
// True for an actor this has seen enter play and not leave.
bool actor_seen_in_play(uint64_t actor);

} // namespace tsuchi
