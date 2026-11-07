// The game side of the captains service.

#include "actors.h"
#include "captain.h"
#include "captain_service.h"
#include "frame.h"
#include "holds.h"
#include "list.h"
#include "objects.h"
#include "services.h"

namespace tsuchi {
namespace {

List<uint64_t> rides; // handles of captains a ride was asked of and has not been settled for

// The captain an actor handle names, or 0.
uint64_t captain_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_game_actor(object) && game::captain_part(object) ? object : 0;
}

TSUCHI_Result whistle(TSUCHI_Actor actor, TSUCHI_Whistle* out) {
    const uint64_t captain = captain_of(actor);
    game::Whistle now;
    if (!captain || !game::read_whistle(captain, now)) return TSUCHI_NOT_FOUND;
    *out = TSUCHI_Whistle{now.active, now.radius, {now.center[0], now.center[1], now.center[2]}, now.count};
    return TSUCHI_OK;
}

// ---- A whistle held on a captain ------------------------------------------------------

// Begun at once. Asked again by the mod that holds it, there is nothing more to do.
TSUCHI_Result begin_whistle(const Hold& hold, HoldNothing&, const HoldNothing&, bool fresh) {
    if (!fresh) return TSUCHI_OK;
    return game::start_whistle(game::from_handle(hold.object)) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

// Once a frame: a whistle the game has ended is begun again.
bool keep_whistle(const Hold& hold, HoldNothing&) {
    const uint64_t captain = game::from_handle(hold.object);
    game::Whistle now;
    if (game::read_whistle(captain, now) && !now.active) game::start_whistle(captain);
    return true;
}

// The whistle ends with a hold the mod lets go of, and with the mod. A captain that leaves play is left as it is.
void end_whistle(const Hold& hold, HoldNothing&, HoldEnd why) {
    if (why != HoldEnd::released && why != HoldEnd::stopped) return;
    if (const uint64_t captain = game::from_handle(hold.object)) game::end_whistle(captain);
}

Holds<HoldNothing, HoldNothing, begin_whistle, keep_whistle, end_whistle> whistles{"captain.whistle", HoldOn::actor, true};

TSUCHI_Result hold_whistle(TSUCHI_Mod* mod, TSUCHI_Actor actor) {
    if (!captain_of(actor)) return TSUCHI_NOT_FOUND;
    return whistles.take(mod, actor.id, 0, HoldNothing{});
}

TSUCHI_Result release_whistle(TSUCHI_Mod* mod, TSUCHI_Actor actor) { return whistles.release(mod, actor.id, 0); }

TSUCHI_Result cursor(TSUCHI_Actor actor, TSUCHI_Vec3* out) {
    const uint64_t captain = captain_of(actor);
    float point[3];
    if (!captain) return TSUCHI_NOT_FOUND;
    if (!game::read_cursor(captain, point)) return TSUCHI_NOT_READY;
    *out = {point[0], point[1], point[2]};
    return TSUCHI_OK;
}

// ---- A cursor held on a point, or on an actor ------------------------------------------

// Where a held cursor belongs: on a point, or on an actor wherever it is.
struct Aim {
    uint64_t target; // a handle, or 0 for a point
    float point[3];
};

// Where that is now: its point, or where its target is. False when the target has no place.
bool aim_of(const Aim& aim, float point[3]) {
    if (!aim.target) {
        for (int i = 0; i < 3; ++i) point[i] = aim.point[i];
        return true;
    }
    const uint64_t target = game::from_handle(aim.target);
    game::Transform at;
    if (!target || !game::get_transform(target, at)) return false;
    for (int i = 0; i < 3; ++i) point[i] = at.location[i];
    return true;
}

// Remembers where the cursor is wanted. A hold the mod already has moves. The cursor has been put there already.
TSUCHI_Result aim_at(const Hold&, Aim& aim, const Aim& wanted, bool) {
    aim = wanted;
    return TSUCHI_OK;
}

// Once a frame: the cursor is put back where it belongs. With its target gone the hold is over.
bool keep_cursor(const Hold& hold, Aim& aim) {
    float point[3];
    if (!aim_of(aim, point)) return false;
    game::place_cursor(game::from_handle(hold.object), point);
    return true;
}

// A cursor is left where it is when its hold ends, so there is nothing to restore.
Holds<Aim, Aim, aim_at, keep_cursor, nullptr> cursors{"captain.cursor", HoldOn::actor, true};

// Holds the captain's cursor as `wanted` says, moving the mod's own hold if it has one. The cursor is put
// there first: one that cannot be placed is not held.
TSUCHI_Result hold(TSUCHI_Mod* mod, TSUCHI_Actor actor, const Aim& wanted) {
    const uint64_t captain = captain_of(actor);
    float point[3];
    if (!captain) return TSUCHI_NOT_FOUND;
    if (cursors.find(actor.id, 0) && cursors.holder(actor.id, 0) != mod) return TSUCHI_BUSY;
    if (!aim_of(wanted, point)) return TSUCHI_NOT_READY;
    const TSUCHI_Result placed = game::place_cursor(captain, point);
    return placed == TSUCHI_OK ? cursors.take(mod, actor.id, 0, wanted) : placed;
}

TSUCHI_Result hold_cursor(TSUCHI_Mod* mod, TSUCHI_Actor actor, const TSUCHI_Vec3* point) { return hold(mod, actor, Aim{0, {point->x, point->y, point->z}}); }

TSUCHI_Result hold_cursor_on(TSUCHI_Mod* mod, TSUCHI_Actor actor, TSUCHI_Actor target) {
    const uint64_t object = game::from_handle(target.id);
    if (!object || !game::is_game_actor(object)) return TSUCHI_NOT_FOUND;
    return hold(mod, actor, Aim{target.id, {}});
}

TSUCHI_Result release_cursor(TSUCHI_Mod* mod, TSUCHI_Actor actor) { return cursors.release(mod, actor.id, 0); }

TSUCHI_Result throw_pikmin(TSUCHI_Actor actor, TSUCHI_Actor pikmin) {
    return game::throw_pikmin(captain_of(actor), game::from_handle(pikmin.id)); // nobody is no captain, and nothing no Pikmin
}

TSUCHI_Result riding(TSUCHI_Actor actor, TSUCHI_Actor* out) {
    const uint64_t captain = captain_of(actor);
    if (!captain) return TSUCHI_NOT_FOUND;
    const uint64_t dog = game::mount_of(captain);
    out->id = dog ? game::to_handle(dog) : 0; // a captain on foot has none
    return TSUCHI_OK;
}

TSUCHI_Result ride(TSUCHI_Actor actor) {
    const uint64_t captain = captain_of(actor);
    if (!captain) return TSUCHI_NOT_FOUND;
    uint64_t* noted = rides.add(actor.id); // remembered first: a ride that is asked for has to be settled
    if (!noted) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result asked = game::ride(captain);
    if (asked != TSUCHI_OK) rides.remove(noted);
    return asked;
}

TSUCHI_Result dismount(TSUCHI_Actor actor) {
    const uint64_t captain = captain_of(actor);
    if (!captain) return TSUCHI_NOT_FOUND;
    game::settle_ride(captain); // or the game would seat it again at once
    return game::dismount(captain);
}

// Once a frame: a ride the game has carried out is settled.
void step() {
    for (uint32_t i = rides.count; i--;) {
        const uint64_t captain = game::from_handle(rides[i]);
        if (!captain || game::settle_ride(captain)) rides.remove(&rides[i]);
    }
}

const captain::Backend service{whistle,       hold_whistle, release_whistle, cursor, hold_cursor, hold_cursor_on,
                                release_cursor, throw_pikmin, riding,          ride,   dismount};

} // namespace

void captains_start() {
    captain::set_backend(&service);
    whistles.start();
    cursors.start();
    each_frame(step);
}

} // namespace tsuchi
