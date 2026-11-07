// The game side of the anim service.

#include "animation_service.h"
#include "clips.h"
#include "objects.h"
#include "parts.h"
#include "services.h"

namespace tsuchi {
namespace {

// The part a handle names, or 0 when it is gone or names something that is no part.
uint64_t part_of(TSUCHI_Component component) {
    const uint64_t part = game::from_handle(component.id);
    return part && game::is_part(part) ? part : 0;
}

TSUCHI_Result play_clip(TSUCHI_Mod* mod, TSUCHI_Component component, TSUCHI_Object clip, bool looping, float rate) {
    const uint64_t part = part_of(component), with = game::from_handle(clip.id);
    return part && with ? hold_clip(mod, part, with, looping, rate) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result set_clip_rate(TSUCHI_Mod* mod, TSUCHI_Component component, float rate) {
    const uint64_t part = part_of(component);
    return part ? set_held_clip_rate(mod, part, rate) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result seek_clip(TSUCHI_Mod* mod, TSUCHI_Component component, float seconds) {
    const uint64_t part = part_of(component);
    return part ? seek_held_clip(mod, part, seconds) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result clip(TSUCHI_Component component, TSUCHI_Clip* out) {
    const uint64_t part = part_of(component);
    return part ? describe_held_clip(part, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result stop_clip(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? stop_held_clip(mod, part) : TSUCHI_NOT_FOUND;
}

const animation::Backend service{play_clip, set_clip_rate, seek_clip, clip, stop_clip};

} // namespace

void anim_start() {
    animation::set_backend(&service);
    clips_start();
}

} // namespace tsuchi
