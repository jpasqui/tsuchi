// The values a changes file sets on the game's actors: who has a say on each, which say is in force, and what
// the value was before, so that it can be put back.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

struct Json;
// Manifest values contain scalars or nonempty field objects.
bool map_value_well_formed(const Json& value);

struct ValueCounts {
    uint32_t set, refused; // says put in force, and says the game would not take
    uint32_t put_back;     // values put back as they were before any mod's say
};

// A mod's says on one actor that is in the world: `values` is the actor's entry under "set", a path and a value
// each, and `level` the sublevel's entry it is in. `part` names whose they are, for `values_end`. Nothing is
// written until `values_settle`.
void values_say(uint64_t part, TSUCHI_Mod* mod, uint64_t actor, const Json& level, const Json& values);
// Puts in force what has been said since the last call: for each value, the say of the mod started last. The
// other mods' logs are told whose is used.
void values_settle();
// Takes out the says of one part, whose mod has stopped. Each value gets the say that is in force without them,
// or with no say left, what it was.
void values_end(uint64_t part);
// The world is left: every value is put back as it was, where what holds it is still there.
void values_put_back();
ValueCounts value_counts();

} // namespace tsuchi
