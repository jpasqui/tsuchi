// Extends the native follower snapshot for the two added colors.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"
namespace tsuchi {
// Call after the native snapshot. A refused preflight preserves the previous added counters.
// The game classifies each actor into private scratch storage. A preflight
// refuses any classifier call that could remove another actor or register a class.
TSUCHI_Result pikmin_snapshot_follow(uint64_t story, uint64_t include_loose, uint32_t include_photon);
}
