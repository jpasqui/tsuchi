#include "switched.h"

#include "actors.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "parts.h"
#include "suspend.h"

namespace tsuchi {
namespace {

namespace t = game::table;

constexpr uint32_t kFlags = sizeof t::tables::collision_record_flags / sizeof t::tables::collision_record_flags[0];

// What is kept for a part that is held off: how it was.
struct Off {
    uint8_t mode;          // how it collided
    uint8_t flags[kFlags]; // and what its record said of it
};

// The part's record now, with its actor: the game may have made its records anew.
uint64_t record_of(uint64_t part, uint64_t& actor) {
    actor = game::part_owner(part);
    return actor && game::is_game_actor(actor) ? game::collision_record_of(actor, part) : 0;
}

// Clears what makes the part count, and stops it colliding. True when anything had to change.
bool clear(uint64_t part, uint64_t record) {
    bool changed = false;
    for (uint32_t i = 0; i < kFlags; ++i) {
        const uint64_t at = record + t::tables::collision_record_flags[i];
        if (!game::read_or<uint8_t>(at)) continue;
        game::write(at, uint8_t(0));
        changed = true;
    }
    if (game::collision_of(part) != game::kCollisionNone) {
        game::set_collision(part, game::kCollisionNone);
        changed = true;
    }
    return changed;
}

// Puts the part back as it was. What its record said goes back when the game still keeps the
// record; how the part collided goes back either way, so it is never left colliding with nothing.
void put_back(uint64_t part, const Off& entry) {
    uint64_t actor = 0;
    if (const uint64_t record = record_of(part, actor))
        for (uint32_t i = 0; i < kFlags; ++i) game::write(record + t::tables::collision_record_flags[i], entry.flags[i]);
    game::set_collision(part, entry.mode);
    if (actor && game::is_game_actor(actor)) game::rebuild_collision_tree(actor);
}

// Switches the part off. The caller has seen that the game keeps a record of it.
TSUCHI_Result apply(const Hold& hold, Off& entry, const HoldNothing&, bool fresh) {
    if (!fresh) return TSUCHI_OK;
    const uint64_t part = game::from_handle(hold.object);
    uint64_t actor = 0;
    const uint64_t record = record_of(part, actor);
    entry.mode = game::collision_of(part);
    for (uint32_t i = 0; i < kFlags; ++i) entry.flags[i] = game::read_or<uint8_t>(record + t::tables::collision_record_flags[i]);
    clear(part, record);
    if (game::rebuild_collision_tree(actor)) return TSUCHI_OK;
    put_back(part, entry); // the game's records cannot follow: as it was, then
    return TSUCHI_UNSUPPORTED;
}

// Once a frame: a part the game has switched back is switched off again. A part the game no
// longer keeps a record of cannot be held off any more.
bool keep(const Hold& hold, Off&) {
    const uint64_t part = game::from_handle(hold.object);
    uint64_t actor = 0;
    const uint64_t record = record_of(part, actor);
    if (!record) return false;
    if ((suspended_parts(actor) & kSuspendCollision) == 0 && clear(part, record)) game::rebuild_collision_tree(actor);
    return true;
}

// A part that is being destroyed is left alone.
void restore(const Hold& hold, Off& entry, HoldEnd why) {
    if (why == HoldEnd::destroyed) return;
    if (const uint64_t part = game::from_handle(hold.object)) put_back(part, entry);
}

Holds<Off, HoldNothing, apply, keep, restore> off{"collision.off", HoldOn::part, true};

} // namespace

void switched_start() { off.start(); }

TSUCHI_Result switch_part_off(TSUCHI_Mod* mod, uint64_t part) {
    const uint64_t handle = game::to_handle(part);
    if (off.find(handle, 0)) return off.holder(handle, 0) == mod ? TSUCHI_OK : TSUCHI_BUSY;
    uint64_t actor = 0;
    if (!record_of(part, actor)) return TSUCHI_UNSUPPORTED;
    if (suspended_parts(actor) & kSuspendCollision) return TSUCHI_BUSY;
    return off.take(mod, handle, 0, HoldNothing{});
}

TSUCHI_Result switch_part_on(TSUCHI_Mod* mod, uint64_t part) {
    const uint64_t handle = game::to_handle(part);
    if (!off.find(mod, handle, 0)) return TSUCHI_NOT_FOUND;
    if (suspended_parts(game::part_owner(part)) & kSuspendCollision) return TSUCHI_BUSY;
    return off.release(mod, handle, 0);
}

bool part_switched_off(uint64_t part) { return off.find(game::to_handle(part), 0) != nullptr; }
uint32_t parts_switched_off() { return off.count(); }

} // namespace tsuchi
