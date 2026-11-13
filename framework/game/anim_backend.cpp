// The game side of the anim service.

#include "animation_service.h"
#include "blueprint_motions.h"
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

TSUCHI_Result state_machines(TSUCHI_Component component, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t part = part_of(component);
    return part ? state_machines_of(part, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result motion(TSUCHI_Component component, const char* state_machine, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t part = part_of(component);
    return part ? motion_of(part, state_machine, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result hold_motion_on(TSUCHI_Mod* mod, TSUCHI_Component component, const char* state_machine, const char* motion) {
    const uint64_t part = part_of(component);
    return part ? hold_motion(mod, part, state_machine, motion) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result release_motion_on(TSUCHI_Mod* mod, TSUCHI_Component component, const char* state_machine) {
    const uint64_t part = part_of(component);
    return part ? release_motion(mod, part, state_machine) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result move_motion(TSUCHI_Component component, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t part = part_of(component);
    return part ? move_motion_of(part, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result hold_move_motion_on(TSUCHI_Mod* mod, TSUCHI_Component component, const char* motion) {
    const uint64_t part = part_of(component);
    return part ? hold_move_motion(mod, part, motion) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result release_move_motion_on(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? release_move_motion(mod, part) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result speed(TSUCHI_Component component, float* out) {
    const uint64_t part = part_of(component);
    return part ? speed_of(part, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result hold_speed_on(TSUCHI_Mod* mod, TSUCHI_Component component, float speed) {
    const uint64_t part = part_of(component);
    return part ? hold_speed(mod, part, speed) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result release_speed_on(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? release_speed(mod, part) : TSUCHI_NOT_FOUND;
}

const animation::Backend service{play_clip, set_clip_rate, seek_clip, clip, stop_clip, state_machines, motion, hold_motion_on, release_motion_on,
                                 move_motion, hold_move_motion_on, release_move_motion_on, speed, hold_speed_on, release_speed_on};

} // namespace

void anim_start() {
    animation::set_backend(&service);
    clips_start();
    blueprint_motions_start();
}

} // namespace tsuchi
