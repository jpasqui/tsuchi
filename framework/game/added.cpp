#include "added.h"

#include "holds.h"
#include "objects.h"
#include "parts.h"
#include "actors.h"
#include "ordinary_lifecycle.h"

namespace tsuchi {
namespace {

// The part goes when the mod removes it or stops, and when its actor is put away to be used
// again. With an actor the game destroys, the engine destroys the part itself.
void destroy(const Hold& hold, HoldNothing&, HoldEnd why) {
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    HoldRestore check{hold, why};
    if (const uint64_t part = hold_part_to_restore(hold, why)) game::restore_destroy_part(part, check.guard());
}

// A part a mod added is held by that mod, on the part itself.
Holds<HoldNothing, HoldNothing, nullptr, nullptr, destroy> added{"component.added", HoldOn::part, true};

} // namespace

void added_start() { added.start(); }

TSUCHI_Result add_part_for(TSUCHI_Mod* mod, uint64_t actor, uint64_t type, uint64_t parent, const char* socket, uint64_t& part) {
    part = 0;
    const TSUCHI_Result admission = ordinary_lifecycle_watch(actor);
    if (admission != TSUCHI_OK) return admission;
    const uint64_t owner_handle = game::is_game_actor(actor) ? 0 : game::to_handle(actor);
    if (!added.reserve(1)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result made = game::add_part(actor, type, parent, socket, part);
    if (made != TSUCHI_OK) return made;
    const uint64_t part_handle = game::to_handle(part);
    if (!ordinary_lifecycle_restore_part(part_handle, owner_handle)) { part = 0; return TSUCHI_NOT_READY; }
    const TSUCHI_Result held = added.take(mod, part_handle, 0, HoldNothing{});
    if (held != TSUCHI_OK) {
        const Hold rollback{part_handle, 0, 0, mod, owner_handle};
        HoldRestore check{rollback, HoldEnd::released};
        if (const uint64_t surviving = hold_part_to_restore(rollback, HoldEnd::released)) game::restore_destroy_part(surviving, check.guard());
        part = 0;
    }
    return held;
}

TSUCHI_Result remove_part_for(TSUCHI_Mod* mod, uint64_t part) {
    const uint64_t handle = game::to_handle(part);
    if (!added.find(handle, 0)) return TSUCHI_NOT_FOUND;
    if (added.holder(handle, 0) != mod) return TSUCHI_DENIED;
    return added.release(mod, handle, 0);
}

uint32_t added_count() { return added.count(); }

} // namespace tsuchi
