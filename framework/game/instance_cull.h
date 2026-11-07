// Owned native instance-cull distances. Selection of foliage and worlds is the caller's policy.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"

namespace tsuchi {
void instance_cull_start();
TSUCHI_Result hold_instance_cull_scale(TSUCHI_Mod* mod, uint64_t part, float scale);
TSUCHI_Result release_instance_cull_scale(TSUCHI_Mod* mod, uint64_t part);
} // namespace tsuchi
