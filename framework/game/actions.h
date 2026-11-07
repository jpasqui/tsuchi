// What an actor is doing. The part that decides for an actor keeps a tree of
// actions: the one running at the top, the one running within it, and so on
// down. Each has a name, and the names from the top down, joined by '/', say
// what the actor is doing: "Control/Throw", "Follow", "Carry".
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

// A number that is the same while the actor goes on doing the same thing and
// different once it does another. False for an actor nothing decides for, or
// one whose actions cannot be read through.
bool action_signature(uint64_t actor, uint64_t& signature);

// What the actor is doing, as UTF-8 with a terminator; empty when it is doing
// nothing. `needed` gets the bytes it takes. TSUCHI_CAPACITY when that is more than
// `capacity`, and nothing is written; TSUCHI_UNSUPPORTED where action_signature is false.
TSUCHI_Result action_path(uint64_t actor, char* buffer, uint64_t capacity, uint64_t* needed);

} // namespace tsuchi::game
