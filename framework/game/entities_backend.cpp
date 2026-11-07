// The entity service: gameplay actors in play, what each is, and where.
#include "presenter_scene.h"
#include "game_table.h"
#include "actors.h"
#include "content.h"
#include "holds.h"
#include "motion.h"
#include "ground_contact.h"
#include "objects.h"
#include "entity_service.h"
#include "reflect.h"
#include "services.h"
#include "suspend.h"

namespace tsuchi {
namespace {

game::ClassRef class_class{game::table::name::class_class};

// The actor a handle names, or 0 when that use of it is over or it never was one.
uint64_t actor_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_game_actor(object) ? object : 0;
}

// The game entity a handle names. A live object that is not one, such as a plain
// Engine actor, is invalid. A handle whose use is over is not found.
TSUCHI_Result entity_of(TSUCHI_Actor actor, uint64_t& out) {
    const uint64_t object = game::from_handle(actor.id);
    out = object && game::is_game_actor(object) ? object : 0;
    return out ? TSUCHI_OK : object ? TSUCHI_INVALID : TSUCHI_NOT_FOUND;
}

// Collects the handles of actors, all of them or those of one class. Counts
// every one and writes those that fit.
struct Collector {
    uint64_t type; // 0 for every actor
    TSUCHI_Actor* buffer;
    uint64_t capacity, count;
};
bool collect(void* context, uint64_t actor) {
    auto& c = *static_cast<Collector*>(context);
    if (c.type && !game::derives_from(game::class_of(actor), c.type)) return true;
    const uint64_t handle = game::to_handle(actor);
    if (!handle) return true;
    if (c.count < c.capacity) c.buffer[c.count].id = handle;
    ++c.count;
    return true;
}

// A list into a caller's buffer, whole or not at all.
TSUCHI_Result list(uint64_t type, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Collector counter{type, nullptr, 0, 0};
    game::each_actor(collect, &counter);
    *out = counter.count;
    if (counter.count > capacity) return TSUCHI_CAPACITY;
    Collector writer{type, buffer, counter.count, 0};
    game::each_actor(collect, &writer);
    return TSUCHI_OK;
}

// Every actor, or with a class given, those of that class.
TSUCHI_Result listed(TSUCHI_Object of_class, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    if (!of_class.id) return list(0, buffer, capacity, out);
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    const uint64_t wanted = game::from_handle(of_class.id);
    if (!wanted) return TSUCHI_NOT_FOUND;
    if (!game::is_a(wanted, class_class)) return TSUCHI_INVALID;
    return list(wanted, buffer, capacity, out);
}

struct Placed {
    uint64_t placement, found;
};
TSUCHI_Result find_placed(uint64_t placement, TSUCHI_Actor* out) {
    if (!placement) return TSUCHI_INVALID;
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Placed search{placement, 0};
    game::each_actor(
        [](void* context, uint64_t actor) {
            auto& search = *static_cast<Placed*>(context);
            if (game::actor_placement(actor) != search.placement || !game::is_game_actor(actor)) return true;
            search.found = actor;
            return false;
        },
        &search);
    const uint64_t handle = game::to_handle(search.found);
    if (!handle) return TSUCHI_NOT_FOUND;
    out->id = handle;
    return TSUCHI_OK;
}

TSUCHI_Result kind(TSUCHI_Actor actor, char* buffer, uint64_t capacity, uint64_t* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    return game::kind_into(game::actor_kind(address), buffer, capacity, out);
}

TSUCHI_Result team(TSUCHI_Actor actor, int32_t* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    *out = game::actor_team(address);
    return TSUCHI_OK;
}

TSUCHI_Result placement(TSUCHI_Actor actor, uint64_t* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    *out = game::actor_placement(address);
    return TSUCHI_OK;
}

TSUCHI_Result get_pose(TSUCHI_Actor actor, TSUCHI_Pose* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    game::Transform transform;
    if (!game::get_transform(address, transform)) return TSUCHI_NOT_READY;
    out->location = {transform.location[0], transform.location[1], transform.location[2]};
    out->rotation = {transform.angles[0], transform.angles[1], transform.angles[2]};
    return TSUCHI_OK;
}

TSUCHI_Result set_pose(TSUCHI_Actor actor, const TSUCHI_Pose* to) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    ground_contact_forget(address);
    const TSUCHI_Result result = game::set_transform(address, game::Transform{{to->location.x, to->location.y, to->location.z}, {to->rotation.pitch, to->rotation.yaw, to->rotation.roll}});
    if (result == TSUCHI_OK) presenter_cut(address); // a move at once: no slide in the pictures between steps
    return result;
}

// ---- Moving --------------------------------------------------------------------

TSUCHI_Result warp(TSUCHI_Actor actor, const TSUCHI_Vec3* location, const TSUCHI_Rotation* rotation) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    ground_contact_forget(address);
    const TSUCHI_Result result = game::warp_actor(address, game::Transform{{location->x, location->y, location->z}, {rotation->pitch, rotation->yaw, rotation->roll}});
    if (result == TSUCHI_OK) motion_end(address); // a throw under way must not carry on from the new place
    return result;
}

TSUCHI_Result get_velocity(TSUCHI_Actor actor, TSUCHI_Vec3* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    float velocity[3];
    if (!game::get_velocity(address, velocity)) return TSUCHI_NOT_READY;
    *out = {velocity[0], velocity[1], velocity[2]};
    return TSUCHI_OK;
}

TSUCHI_Result launch(TSUCHI_Actor actor, const TSUCHI_Vec3* velocity) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    if (!game::can_launch(address)) return TSUCHI_UNSUPPORTED;
    ground_contact_forget(address);
    const float wanted[3] = {velocity->x, velocity->y, velocity->z};
    game::request_velocity(address, wanted);
    motion_begin(address, wanted);
    return TSUCHI_OK;
}

// ---- Suspending ----------------------------------------------------------------

static_assert(TSUCHI_ENTITY_PARTS_LOGIC == kSuspendLogic && TSUCHI_ENTITY_PARTS_MOVEMENT == kSuspendMovement &&
              TSUCHI_ENTITY_PARTS_PHYSICS == kSuspendPhysics && TSUCHI_ENTITY_PARTS_COLLISION == kSuspendCollision,
              "the parts are the same parts");

TSUCHI_Result suspend(TSUCHI_Mod* mod, TSUCHI_Actor actor, TSUCHI_EntityParts parts) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    return suspend_parts(mod, address, parts);
}

TSUCHI_Result resume(TSUCHI_Mod* mod, TSUCHI_Actor actor, TSUCHI_EntityParts parts) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    return resume_parts(mod, address, parts);
}

TSUCHI_Result suspended(TSUCHI_Actor actor, TSUCHI_EntityParts* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    *out = suspended_parts(address);
    return TSUCHI_OK;
}

TSUCHI_Result grounded(TSUCHI_Actor actor, bool* out) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    return ground_contact(address, out);
}

// ---- Actors a mod made --------------------------------------------------------

// A spawned actor belongs to its mod until it leaves play or the mod hands it to the game: the mod
// holds it. When the mod stops, what it spawned goes with it. When the actor leaves by itself, or the
// mod removes it, nothing is owed any more.
void remove_spawned(const Hold& hold, HoldNothing&, HoldEnd why) {
    if (why != HoldEnd::stopped) return;
    if (const uint64_t actor = actor_of(TSUCHI_Actor{hold.object})) game::remove_actor(actor);
}

Holds<HoldNothing, HoldNothing, nullptr, nullptr, remove_spawned> spawned{"entity.spawned", HoldOn::actor, true};

TSUCHI_Result spawn(TSUCHI_Mod* mod, TSUCHI_Object type, const TSUCHI_Vec3* location, const TSUCHI_Rotation* rotation, int32_t team, bool keep, TSUCHI_Actor* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    const uint64_t wanted = game::from_handle(type.id);
    if (!wanted) return TSUCHI_NOT_FOUND;
    if (team < -1 || !game::is_a(wanted, class_class) || !game::derives_from(wanted, game::game_actor_class())) return TSUCHI_INVALID;
    const game::Transform at{{location->x, location->y, location->z}, {rotation->pitch, rotation->yaw, rotation->roll}};
    const uint64_t actor = game::spawn_actor(wanted, at, team);
    const uint64_t handle = game::to_handle(actor);
    if (!handle) return TSUCHI_UNSUPPORTED;
    if (!keep) {
        const TSUCHI_Result owned = spawned.take(mod, handle, 0, HoldNothing{});
        if (owned != TSUCHI_OK) { // no memory to remember it by: better no actor than one nobody will clean up
            game::remove_actor(actor);
            return owned;
        }
    }
    if (game::can_launch(actor)) motion_begin(actor, nullptr); // it settles where it was put before its physics may stop
    out->id = handle;
    return TSUCHI_OK;
}

TSUCHI_Result destroy(TSUCHI_Mod* mod, TSUCHI_Actor actor) {
    uint64_t address = 0;
    if (const TSUCHI_Result found = entity_of(actor, address); found != TSUCHI_OK) return found;
    TSUCHI_Mod* spawner = spawned.holder(actor.id, 0);
    if (spawner && spawner != mod) return TSUCHI_DENIED;
    const TSUCHI_Result result = game::remove_actor(address);
    if (result == TSUCHI_OK && spawner) spawned.release(spawner, actor.id, 0);
    return result;
}

const entity::Backend service{listed,        find_placed, kind,   team,    placement, get_pose,
                               set_pose,      warp,          get_velocity, launch,      suspend, resume, suspended, spawn,
                               destroy, grounded};

} // namespace

void entities_start() {
    entity::set_backend(&service);
    spawned.start();
    motion_start();
    suspend_start();
}

} // namespace tsuchi
