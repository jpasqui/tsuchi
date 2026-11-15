// The people playing: whose captain and dog are whose, which of the two each
// steers, and which controller feeds each.
#pragma once
#include <stdint.h>

#include "game_table.h"
#include "tsuchi/core.h"

namespace tsuchi::game {

// The game has room for this many players. A game's rule decides how many there are.
constexpr uint32_t kPlayerSlots = uint32_t(table::constant::player_slots);

// A player's captain or dog in the loaded world, or 0.
uint64_t player_captain(uint32_t player);
uint64_t player_dog(uint32_t player);
// The one of the two the player is steering, or 0. False when the game cannot be asked.
bool player_controlled(uint32_t player, uint64_t& actor);

// True while a captain is riding its dog.
bool is_riding(uint64_t captain);
// The game's own switch between a player's captain and dog, to `actor`, which is
// one of the two. `at_once` skips the fade. TSUCHI_OK when the switch has begun or
// the actor is steered already; TSUCHI_BUSY while one is fading in; TSUCHI_NOT_READY
// when the game would refuse or cannot be asked.
TSUCHI_Result switch_controlled(uint32_t player, uint64_t actor, bool at_once, bool before_unlock = false);
// Drops a switch that is fading in. The camera still fades.
void drop_pending_switch();
// True while one of the level's scripted events has the game's attention.
bool events_running();

// The number of the controller that feeds a player. False when the game cannot be asked.
bool player_pad(uint32_t player, uint32_t& pad);
void set_player_pad(uint32_t player, uint32_t pad);

} // namespace tsuchi::game
