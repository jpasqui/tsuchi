// Life: how much an actor has, setting it, and sending an actor a hit or a
// knock the way the game sends its own.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

struct Life {
    float life, max_life, extra, max_extra;
};

// The part of an actor that keeps its life, or 0 for an actor without any.
uint64_t life_component(uint64_t actor);
// The actor a life part belongs to, or 0.
uint64_t life_owner(uint64_t component);
// The life a life part holds now. False when it cannot be read.
bool current_life(uint64_t component, float& life);
bool read_life(uint64_t actor, Life& out);
// Sets an actor's life through the game's own setter, which keeps the gauge
// right. With `smooth`, the gauge moves there over time; without, it jumps.
// False for an actor without life, or when the game's setter is not known.
bool set_life(uint64_t actor, float life, bool smooth);

// Sends the victim a hit: the game works out what it takes and how the victim
// reacts. `attacker` is 0 for a hit that is nobody's. TSUCHI_NOT_FOUND for a victim
// without life; TSUCHI_NO_MEMORY when there is no memory for the message;
// TSUCHI_NOT_READY when the game cannot be asked or the victim would not take it,
// as when it is asleep.
TSUCHI_Result send_hit(uint64_t victim, uint64_t attacker, float amount, uint32_t kind);
// Sends the victim a knock: it is thrown with this velocity and loses `damage` life.
TSUCHI_Result send_knock(uint64_t victim, uint64_t attacker, const float velocity[3], float damage);

} // namespace tsuchi::game
