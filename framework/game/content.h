// What mods add to the game, and the names the game knows it by. DESIGN.md, "New treasures" and "New creatures".
//
// A thing a mod adds has an ID, `<mod id>:<name>`. Where the game knows a kind of thing by a name, a mod's thing
// has a name of the game's too, which the framework gave it. A service that takes or gives such an ID asks here
// for one by the other, so the game's name is in no mod.
//
// Game thread only.
#pragma once
#include <stdint.h>

#include "ids.h"
#include "names.h"
#include "tsuchi/core.h"

namespace tsuchi::game {

// The name the game knows a running mod's thing by. False for an ID that is no running mod's thing.
bool added_name(const char* id, Name& out);
// The ID of the thing a running mod added that the game knows by a name. False for any other name.
bool added_id(Name name, char out[kIdBytes]);
// A kind's name as text crosses to a mod: the ID of a mod's thing, and the game's own name for anything else.
// TSUCHI_CAPACITY when it does not fit, with nothing copied and `out` still set; TSUCHI_NOT_FOUND for a name that
// cannot be read.
TSUCHI_Result kind_into(Name kind, char* buffer, uint64_t capacity, uint64_t* out);

// Names a thing of a mod's for as long as the mod runs. Whatever adds a mod's thing to the game calls this with
// the name it gave the thing. TSUCHI_INVALID for an ID that is not one of this mod's own, TSUCHI_BUSY when the ID
// or the name already names something, TSUCHI_NO_MEMORY.
TSUCHI_Result add_name(TSUCHI_Mod* mod, const char* id, Name name);

} // namespace tsuchi::game
