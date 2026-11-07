// Private early cleanup for component leases on ordinary Unreal actors.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"
namespace tsuchi {
void ordinary_lifecycle_start(TSUCHI_Mod* self);
// Qualifies the owner's native table before a part hold changes anything.
// Gameplay actors keep their existing event/pool path. Unsupported overrides
// are refused, rather than relying on a later stale-handle observation.
TSUCHI_Result ordinary_lifecycle_watch(uint64_t actor);
// Resolves the same part and owner generations, refusing accepted destruction.
uint64_t ordinary_lifecycle_restore_part(uint64_t part_handle, uint64_t owner_handle);
// Adds a teardown listener. Every listener hears each teardown, in the order they were added.
void ordinary_lifecycle_listener(void (*listener)(uint64_t actor, uint64_t handle, bool destroying));
} // namespace tsuchi
