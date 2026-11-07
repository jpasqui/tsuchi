// Navigation queries through the engine's own reflected query family.
#include <string.h>
#include "engine_call.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "list.h"
#include "objects.h"
#include "object_retention.h"
#include "reflect.h"
#include "world_service.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef navigation_class{t::name::navigation_system_class};
game::ClassRef navmesh_class{t::name::navmesh_class};
uint64_t cached_world = 0, cached_mesh = 0;
uint32_t retry_steps = 0;
void discovery_step() { if (retry_steps) --retry_steps; }
struct WorldGuard {
    uint64_t world;
    static bool allowed(void* context) {
        const auto& captured = *static_cast<WorldGuard*>(context);
        return game::from_handle(captured.world) != 0 && game::world_begun() &&
            game::to_handle(game::global<uint64_t>(t::data::world)) == captured.world;
    }
    game::CallGuard guard() { return {allowed, this}; }
};
bool finite(const TSUCHI_Vec3& value) {
    return __builtin_isfinite(value.x) && __builtin_isfinite(value.y) && __builtin_isfinite(value.z);
}
uint64_t ready_world() {
    if (!game::world_begun()) return 0;
    const uint64_t world = game::global<uint64_t>(t::data::world);
    const uint64_t handle = game::to_handle(world);
    if (!handle) return 0;
    if (cached_world != handle) { cached_world = handle; cached_mesh = 0; retry_steps = 0; }
    const uint64_t actor = game::from_handle(cached_mesh);
    if (actor) {
        const uint64_t implementation = game::read_or<uint64_t>(actor + t::navmesh_actor::implementation);
        if (implementation && game::read_or<uint64_t>(implementation + t::navmesh_implementation::mesh)) return world;
    }
    if (retry_steps) return 0;
    // Empty and still-loading worlds retry at most once a second, regardless
    // of how many mods ask for ground or a path in that interval.
    retry_steps = 30;
    struct Search { uint64_t world, mesh; } search{world, 0};
    game::each_object([](void* context, uint64_t object) {
        auto& found = *static_cast<Search*>(context);
        if (!game::is_a(object, navmesh_class) ||
            (game::read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags)) return true;
        const uint64_t level = game::outer_of(object);
        if (!level || game::outer_of(level) != found.world) return true;
        const uint64_t implementation = game::read_or<uint64_t>(object + t::navmesh_actor::implementation);
        if (!implementation || !game::read_or<uint64_t>(implementation + t::navmesh_implementation::mesh)) return true;
        found.mesh = game::to_handle(object); return false;
    }, &search);
    cached_mesh = search.mesh;
    if (cached_mesh) retry_steps = 0;
    return cached_mesh ? world : 0;
}
TSUCHI_Result set_vector(game::EngineCall& call, const char* x, const char* y, const char* z, const TSUCHI_Vec3& value) {
    TSUCHI_Result result = call.set(x, game::number_value(value.x));
    if (result == TSUCHI_OK) result = call.set(y, game::number_value(value.y));
    if (result == TSUCHI_OK) result = call.set(z, game::number_value(value.z));
    return result;
}
bool get_vector(game::EngineCall& call, const char* x, const char* y, const char* z, TSUCHI_Vec3& out) {
    game::Value a, b, c;
    if (call.get(x, a) != TSUCHI_OK || call.get(y, b) != TSUCHI_OK || call.get(z, c) != TSUCHI_OK ||
        a.kind != game::ValueKind::number || b.kind != game::ValueKind::number || c.kind != game::ValueKind::number) return false;
    out = {float(a.number), float(b.number), float(c.number)};
    return finite(out);
}
TSUCHI_Result ground(const TSUCHI_Vec3* point, const TSUCHI_Vec3* extent, TSUCHI_Vec3* out) {
    if (!finite(*point) || !finite(*extent) || extent->x <= 0 || extent->y <= 0 || extent->z <= 0) return TSUCHI_INVALID;
    const uint64_t world = ready_world(), navigation = game::find_class(navigation_class);
    if (!world || !navigation) return TSUCHI_NOT_READY;
    WorldGuard guard{game::to_handle(world)};
    game::EngineCall call(navigation, "K2_ProjectPointToNavigation", guard.guard());
    if (call.ready() != TSUCHI_OK) return TSUCHI_NOT_READY;
    if (call.set("WorldContextObject", game::object_value(guard.world)) != TSUCHI_OK ||
        set_vector(call, "Point.X", "Point.Y", "Point.Z", *point) != TSUCHI_OK ||
        set_vector(call, "QueryExtent.X", "QueryExtent.Y", "QueryExtent.Z", *extent) != TSUCHI_OK ||
        call.invoke() != TSUCHI_OK || !guard.guard().permits()) return TSUCHI_NOT_READY;
    game::Value success;
    if (call.get("ReturnValue", success) != TSUCHI_OK || success.kind != game::ValueKind::boolean) return TSUCHI_NOT_READY;
    if (!success.boolean) return TSUCHI_NOT_FOUND;
    TSUCHI_Vec3 projected;
    if (!get_vector(call, "ProjectedLocation.X", "ProjectedLocation.Y", "ProjectedLocation.Z", projected)) return TSUCHI_NOT_READY;
    *out = projected; return TSUCHI_OK;
}
bool path_state(uint64_t path, const char* name, bool& out, game::CallGuard guard) {
    game::EngineCall call(path, name, guard);
    game::Value value;
    if (call.invoke() != TSUCHI_OK || call.get("ReturnValue", value) != TSUCHI_OK || value.kind != game::ValueKind::boolean) return false;
    out = value.boolean; return true;
}
// Builds one reflected vector field path without a formatted-print dependency.
void coordinate_path(uint32_t index, char lane, char out[40]) {
    const char prefix[] = "PathPoints[";
    memcpy(out, prefix, sizeof(prefix) - 1);
    uint32_t used = sizeof(prefix) - 1, count = 0;
    char digits[10];
    do { digits[count++] = char('0' + index % 10); index /= 10; } while (index);
    while (count) out[used++] = digits[--count];
    out[used++] = ']'; out[used++] = '.'; out[used++] = lane; out[used] = 0;
}
struct RetainedPath {
    uint64_t handle;
    ~RetainedPath() { release_game_object(handle); }
};
TSUCHI_Result path(const TSUCHI_Vec3* start, const TSUCHI_Vec3* finish, TSUCHI_Vec3* buffer, uint64_t capacity, uint64_t* out) {
    if (!finite(*start) || !finite(*finish)) return TSUCHI_INVALID;
    const uint64_t world = ready_world(), navigation = game::find_class(navigation_class);
    if (!world || !navigation) return TSUCHI_NOT_READY;
    WorldGuard guard{game::to_handle(world)};
    game::EngineCall call(navigation, "FindPathToLocationSynchronously", guard.guard());
    if (call.ready() != TSUCHI_OK) return TSUCHI_NOT_READY;
    if (call.set("WorldContextObject", game::object_value(guard.world)) != TSUCHI_OK ||
        set_vector(call, "PathStart.X", "PathStart.Y", "PathStart.Z", *start) != TSUCHI_OK ||
        set_vector(call, "PathEnd.X", "PathEnd.Y", "PathEnd.Z", *finish) != TSUCHI_OK ||
        call.invoke() != TSUCHI_OK || !guard.guard().permits()) return TSUCHI_NOT_READY;
    game::Value answer;
    if (call.get("ReturnValue", answer) != TSUCHI_OK || answer.kind != game::ValueKind::object) return TSUCHI_NOT_READY;
    const uint64_t result = game::from_handle(answer.object);
    if (!result) return TSUCHI_NOT_FOUND;
    if (!retain_game_object(answer.object)) return TSUCHI_NO_MEMORY;
    RetainedPath retained{answer.object};
    bool valid = false, partial = true;
    if (!path_state(result, "IsValid", valid, guard.guard()) || !path_state(result, "IsPartial", partial, guard.guard())) return TSUCHI_NOT_READY;
    if (!valid || partial) return TSUCHI_NOT_FOUND;
    game::Place place;
    game::Value points;
    const uint64_t type = game::type_of(result);
    if (game::find_place(type, result, "PathPoints", place) != TSUCHI_OK ||
        game::read_value(place, points) != TSUCHI_OK || points.kind != game::ValueKind::array || !points.count) return TSUCHI_NOT_FOUND;
    *out = points.count;
    if (capacity < points.count) return TSUCHI_CAPACITY;
    List<TSUCHI_Vec3> values;
    if (!values.reserve(points.count)) return TSUCHI_NO_MEMORY;
    TSUCHI_Result status = TSUCHI_OK;
    for (uint32_t i = 0; i < points.count && status == TSUCHI_OK; ++i) {
        TSUCHI_Vec3 point{};
        float* lanes[] = {&point.x, &point.y, &point.z};
        for (uint32_t lane = 0; lane < 3; ++lane) {
            char field[40]; coordinate_path(i, "XYZ"[lane], field);
            game::Value coordinate;
            if (!guard.guard().permits() || !game::from_handle(answer.object) ||
                game::find_place(type, result, field, place) != TSUCHI_OK || game::read_value(place, coordinate) != TSUCHI_OK ||
                coordinate.kind != game::ValueKind::number || !__builtin_isfinite(coordinate.number)) { status = TSUCHI_NOT_READY; break; }
            *lanes[lane] = float(coordinate.number);
        }
        if (!finite(point)) status = TSUCHI_NOT_READY;
        values.items[i] = point;
    }
    if (!guard.guard().permits()) status = TSUCHI_NOT_READY;
    if (status == TSUCHI_OK) memcpy(buffer, values.items, sizeof(TSUCHI_Vec3) * points.count);
    values.clear(); return status;
}
const world::Backend service{ground, path};
} // namespace
void world_start() { world::set_backend(&service); each_frame(discovery_step); }
} // namespace tsuchi
