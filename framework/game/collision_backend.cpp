// The game's collision records for components, with their original hold behavior.
#include "collision_service.h"
#include "objects.h"
#include "parts.h"
#include "services.h"
#include "switched.h"

namespace tsuchi {
namespace {

uint64_t part_of(TSUCHI_Component component) {
    const uint64_t object = game::from_handle(component.id);
    return object && game::is_part(object) ? object : 0;
}

TSUCHI_Result hold_off(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? switch_part_off(mod, part) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result release_off(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? switch_part_on(mod, part) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result held_off(TSUCHI_Component component, bool* out) {
    const uint64_t part = part_of(component);
    if (!part) return TSUCHI_NOT_FOUND;
    *out = part_switched_off(part);
    return TSUCHI_OK;
}

const collision::Backend service{hold_off, release_off, held_off};

} // namespace

void collision_start() {
    collision::set_backend(&service);
    switched_start();
}

} // namespace tsuchi
