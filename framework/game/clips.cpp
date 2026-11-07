#include "clips.h"

#include "game.h"
#include "holds.h"
#include "objects.h"
#include "parts.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// What is kept for a mesh a clip is held on.
struct Held {
    uint64_t clip; // a handle
    bool looping;
};
struct Wanted {
    uint64_t clip;
    bool looping;
    float rate;
};

// Plays the clip on the mesh. The caller has seen that a mesh not held yet is animated by its blueprint.
TSUCHI_Result apply(const Hold& hold, Held& entry, const Wanted& wanted, bool fresh) {
    const uint64_t part = game::from_handle(hold.object);
    if (!game::play_clip_on(part, wanted.clip, wanted.looping, wanted.rate)) {
        if (fresh) game::set_animation_mode(part, uint32_t(t::constant::animation_by_blueprint)); // whatever of it took is undone
        return TSUCHI_NOT_READY;
    }
    entry = Held{game::to_handle(wanted.clip), wanted.looping};
    return TSUCHI_OK;
}

// Hands the mesh back to its animation blueprint, however the hold ends, as long as the mesh is still
// there: also when its actor ends play, since an actor that does may enter play again with the same mesh.
void restore(const Hold& hold, Held&, HoldEnd why) {
    HoldRestore check{hold, why};
    if (const uint64_t part = hold_part_to_restore(hold, why)) game::restore_animation_mode(part, uint32_t(t::constant::animation_by_blueprint), check.guard());
}

Holds<Held, Wanted, apply, nullptr, restore> held{"animation.clip", HoldOn::part, true};

} // namespace

void clips_start() { held.start(); }

TSUCHI_Result hold_clip(TSUCHI_Mod* mod, uint64_t part, uint64_t clip, bool looping, float rate) {
    if (!game::is_skeletal_part(part)) return TSUCHI_UNSUPPORTED;
    if (!game::is_clip(clip)) return TSUCHI_INVALID;
    const uint64_t handle = game::to_handle(part);
    if (!held.find(handle, 0)) { // a mesh the game itself plays one clip on could not be put back as it was
        uint32_t mode = 0;
        if (!game::animation_mode(part, mode)) return TSUCHI_NOT_READY;
        if (mode != t::constant::animation_by_blueprint) return TSUCHI_UNSUPPORTED;
    }
    return held.take(mod, handle, 0, Wanted{clip, looping, rate});
}

TSUCHI_Result set_held_clip_rate(TSUCHI_Mod* mod, uint64_t part, float rate) {
    if (!held.find(mod, game::to_handle(part), 0)) return TSUCHI_NOT_FOUND;
    return game::set_clip_rate_on(part, rate) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result seek_held_clip(TSUCHI_Mod* mod, uint64_t part, float seconds) {
    if (!held.find(mod, game::to_handle(part), 0)) return TSUCHI_NOT_FOUND;
    if (seconds < 0) return TSUCHI_INVALID;
    return game::seek_clip_on(part, seconds) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result describe_held_clip(uint64_t part, TSUCHI_Clip* out) {
    const Held* entry = held.find(game::to_handle(part), 0);
    if (!entry) { // no mod holds a clip on it: that is told by a clip that is none
        *out = TSUCHI_Clip{};
        return TSUCHI_OK;
    }
    TSUCHI_Clip found{};
    if (!game::clip_state(part, found.position, found.rate, found.playing)) return TSUCHI_NOT_READY;
    found.clip.id = entry->clip, found.looping = entry->looping;
    *out = found;
    return TSUCHI_OK;
}

TSUCHI_Result stop_held_clip(TSUCHI_Mod* mod, uint64_t part) { return held.release(mod, game::to_handle(part), 0); }

uint32_t clip_holds() { return held.count(); }

} // namespace tsuchi
