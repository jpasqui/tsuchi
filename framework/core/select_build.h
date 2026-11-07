// Puts a build's numbers in the table, and what it cannot serve in families_absent.
#pragma once
#include "families.h"
#include "game_table.h"

namespace tsuchi {

inline void select_build(unsigned build) {
    game::table::select_build(build);
    families_absent = game::table::builds[build].absent_families;
}

} // namespace tsuchi
