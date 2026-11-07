// Pikmin: the game's list of them, what each is, whom each follows, and new ones.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

// True for a Pikmin that is in play and not on its way out.
bool is_pikmin(uint64_t actor);
// Calls `visit` for every such Pikmin, in the game's own order, until it returns false.
void each_pikmin(bool (*visit)(void* context, uint64_t pikmin), void* context);

struct PikminInfo {
    uint8_t type, leaf; // as the game numbers them
    bool in_army;       // it counts among a player's Pikmin
    int32_t team;
};
bool describe_pikmin(uint64_t pikmin, PikminInfo& out);

// True for what Pikmin can follow: a captain, or a dog.
bool is_leader(uint64_t actor);
bool is_dog(uint64_t actor);
// The captain or dog a Pikmin follows, or 0.
uint64_t pikmin_leader(uint64_t pikmin);
// False while the game's function for that is not known.
bool can_ask_leader();

// New Pikmin beside a leader and following it, made as an Onion gives them out.
// TSUCHI_NOT_READY when the leader has no place or the game cannot be asked.
TSUCHI_Result add_pikmin(uint64_t leader, uint8_t type, uint8_t leaf, uint32_t count);
// Sends the Pikmin to work on the target, in its leader's name or its own: the
// order a captain's charge gives. TSUCHI_NOT_READY when the target has no place, the
// Pikmin would not take the order, or the game cannot be asked; TSUCHI_NO_MEMORY
// when there is no memory for the order.
TSUCHI_Result send_to_work(uint64_t pikmin, uint64_t target);
// Has the Pikmin follow the leader, at once.
TSUCHI_Result join_squad(uint64_t pikmin, uint64_t leader);
// Has the Pikmin stop following; the game acts on it at the Pikmin's next update.
TSUCHI_Result leave_squad(uint64_t pikmin);
// Dismisses the leader's squad as the player's button does.
TSUCHI_Result dismiss_squad(uint64_t leader);

} // namespace tsuchi::game
