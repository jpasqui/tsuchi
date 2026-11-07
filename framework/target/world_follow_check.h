// The game build's finite navigation-following check.
#pragma once

namespace tsuchi::target {

// Called once per world step. Exists only in TSUCHI_SELFCHECK builds.
void world_follow_check_step();

} // namespace tsuchi::target
