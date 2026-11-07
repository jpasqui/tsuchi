// A captain: its whistle, its cursor, its throws and its dog.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

// The part of a captain that decides for it, or 0 for an actor that is no captain.
uint64_t captain_part(uint64_t actor);

struct Whistle {
    bool active;
    float radius, center[3];
    uint32_t count; // how many it has begun
};
bool read_whistle(uint64_t captain, Whistle& out);
// Begins a whistle at the captain's cursor. False when the game cannot be asked.
bool start_whistle(uint64_t captain);
void end_whistle(uint64_t captain);

// The point the captain aims at: its own cursor's, or its dog's while it rides.
bool read_cursor(uint64_t captain, float out[3]);
// Puts the cursor in use on a point. The game works out the cursor from where
// it is kept relative to the captain, so both are written. TSUCHI_NOT_READY
// without a cursor or a place for the captain.
TSUCHI_Result place_cursor(uint64_t captain, const float point[3]);

// Has the captain pick the Pikmin's type, as a player's type switch does, and
// hold it up to throw. TSUCHI_INVALID for a Pikmin that follows another;
// TSUCHI_NOT_READY with no squad to throw from; TSUCHI_BUSY when the type cannot be picked.
TSUCHI_Result throw_pikmin(uint64_t captain, uint64_t pikmin);

// The captain's dog, whether it rides it or not; 0 without one.
uint64_t partner_dog(uint64_t captain);
// The dog it rides, or 0.
uint64_t mount_of(uint64_t captain);
// Asks the game to seat the captain on its dog. TSUCHI_BUSY when it rides or has
// been asked to; TSUCHI_NOT_READY without a dog, too far from it, or refused.
TSUCHI_Result ride(uint64_t captain);
// True while a ride that was asked for has not been carried out.
bool ride_asked(uint64_t captain);
// The game leaves its note of a ride that was asked for; left there, it seats
// the captain again the moment it gets off. Clears it once the captain rides.
// True when there was nothing left to clear.
bool settle_ride(uint64_t captain);
TSUCHI_Result dismount(uint64_t captain);

} // namespace tsuchi::game
