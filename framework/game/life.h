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
// Sends the target a thrown Pikmin's landing on it (thrown_hit.cpp): a direct hit from `pikmin` at `point`, which
// presses the target with `press`. TSUCHI_NOT_FOUND for a target without life; TSUCHI_NO_MEMORY; TSUCHI_NOT_READY
// when the game cannot be asked or the target takes no message now, as while paused or put away.
TSUCHI_Result send_thrown_hit(uint64_t target, uint64_t pikmin, const float point[3], bool press);
// Runs one update of a creature's life gauge by itself (life_gauge.cpp), as its own update would, `seconds` on. TSUCHI_INVALID
// for a time outside 0 to 1; TSUCHI_UNSUPPORTED for an actor that isn't a creature, a life without a gauge, or a build
// without the update; TSUCHI_NOT_FOUND for a creature without life, defeated, or gone after the update; TSUCHI_NOT_READY
// with no world, for a creature put away or on its way out, or whose parts the update reads aren't as it expects.
TSUCHI_Result refresh_gauge(uint64_t creature, float seconds, bool show);

} // namespace tsuchi::game
