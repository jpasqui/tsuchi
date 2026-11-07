// Unreal actors: the active world's list and the root's position and rotation.
#include "presenter_scene.h"
#include "actor_service.h"
#include "angles.h"
#include "engine_call.h"
#include "game.h"
#include "objects.h"
#include "parts.h"
#include "reflect.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;
game::ClassRef actor_class{t::name::engine_actor_class};
game::ClassRef statics_class{t::name::gameplay_statics_class};

uint64_t actor_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_engine_actor(object) ? object : 0;
}

TSUCHI_Result listed(TSUCHI_Object of_class, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t world = game::global<uint64_t>(t::data::world);
    if (!world || !game::live(world)) return TSUCHI_NOT_READY;
    const uint64_t every = game::find_class(actor_class);
    if (!every) return TSUCHI_NOT_READY;
    uint64_t wanted = every;
    if (of_class.id) {
        wanted = game::from_handle(of_class.id);
        if (!wanted) return TSUCHI_NOT_FOUND;
        if (!game::is_class(wanted) || !game::derives_from(wanted, every)) return TSUCHI_INVALID;
    }
    const uint64_t statics = game::find_class(statics_class);
    if (!statics) return TSUCHI_NOT_READY;
    // The engine iterator selects the current world's active levels and skips
    // templates and actors being destroyed. The object table alone cannot do that.
    game::EngineCall call(statics, "GetAllActorsOfClass");
    if (call.set("WorldContextObject", game::object_value(game::to_handle(world))) != TSUCHI_OK ||
        call.set("ActorClass", game::object_value(game::to_handle(wanted))) != TSUCHI_OK || call.invoke() != TSUCHI_OK)
        return TSUCHI_NOT_READY;
    uint32_t count = 0;
    if (call.objects("OutActors", count) != TSUCHI_OK) return TSUCHI_NOT_READY;
    uint64_t needed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t object = call.object_at("OutActors", i);
        if (object && game::is_engine_actor(object) && game::to_handle(object)) ++needed;
    }
    *out = needed;
    if (needed > capacity) return TSUCHI_CAPACITY;
    uint64_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t object = call.object_at("OutActors", i);
        const uint64_t handle = object && game::is_engine_actor(object) ? game::to_handle(object) : 0;
        if (handle) buffer[written++].id = handle;
    }
    return written == needed ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result alive(TSUCHI_Actor actor, bool* out) {
    *out = actor_of(actor) != 0;
    return TSUCHI_OK;
}

TSUCHI_Result transform(TSUCHI_Actor actor, TSUCHI_Transform* out) {
    const uint64_t object = actor_of(actor);
    if (!object) return TSUCHI_NOT_FOUND;
    const uint64_t root = game::read_or<uint64_t>(object + t::engine_actor::root);
    float q[4], location[3];
    if (!root || !game::live(root) || !game::read(root + t::scene_component::rotation, q) ||
        !game::read(root + t::scene_component::location, location)) return TSUCHI_NOT_READY;
    float angles[3];
    math::to_angles(q, angles);
    out->location = {location[0], location[1], location[2]};
    out->rotation = {angles[0], angles[1], angles[2]};
    return TSUCHI_OK;
}

TSUCHI_Result set_transform(TSUCHI_Actor actor, const TSUCHI_Transform* to) {
    const uint64_t object = actor_of(actor);
    if (!object) return TSUCHI_NOT_FOUND;
    const uint64_t root = game::read_or<uint64_t>(object + t::engine_actor::root);
    if (!root || !game::live(root) || !t::bound(t::fn::set_actor_location_and_rotation)) return TSUCHI_NOT_READY;
    alignas(8) unsigned char hit[t::most::sweep_result_bytes] = {};
    // This native actor call moves the root without sweeping or changing scale.
    // It does not read GActor's facing part or turn a character's body separately.
    if (!t::fn::set_actor_location_and_rotation(reinterpret_cast<void*>(object), false, hit, true,
        to->location.x, to->location.y, to->location.z, to->rotation.pitch, to->rotation.yaw, to->rotation.roll))
        return TSUCHI_NOT_READY;
    presenter_cut(object); // a move at once: no slide in the pictures between steps
    return TSUCHI_OK;
}

const actor::Backend service{listed, alive, transform, set_transform};

} // namespace

void actors_start() { actor::set_backend(&service); }

} // namespace tsuchi
