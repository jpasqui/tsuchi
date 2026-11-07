// Instances and particles between steps.
//
// The game's drawer of instances (the Pikmin's tops, sprouts, carried pieces) clears every one of its instanced
// meshes before the actors update and adds every instance again after, at its owner's place. Nothing of that goes
// through the scene's parts, so its instances would move once a step. After the drawer's update the instances of
// each mesh are kept, and before the drawing each is put at its place between the last two steps; the renderer's
// copy of the mesh is made again from them. The next step's clear throws the values away; the game never reads them.
//
// A material instance's values reach its renderer copy through two calls, one for a vector and one for a scalar,
// which the game makes after it sets a value and only when the value changed. The throw line is drawn from its
// materials' values, its points among them, set once a step. What a step gives the renderer is kept, the last two
// steps of each value; before each picture between steps the renderer is given the value between them, and once a
// value stops changing, its own value again. The game's own copy is never changed.
//
// Cascade particles keep the location they had before their last update next to the one after it, and the
// renderer draws from a copy of both. While a scene is drawn on the render thread, the particles' routes move each
// particle that far between the two that its view family is between the last two steps. The game's thread tells
// that fraction for each view family's world time, and the render thread finds it again by its view's.
#include "presenter_effects.h"

#include <string.h>

#include "game.h"
#include "hooks.h"
#include "native_routes.h"
#include "objects.h"
#include "presenter.h"
#include "presenter_logic.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using namespace presenter;

PresentedMoment moment{};
EffectCounts counts{};
uint64_t (*microseconds)() = nullptr;
void count(uint32_t& counter, uint32_t by = 1) { __atomic_fetch_add(&counter, by, __ATOMIC_RELAXED); }

double at() { return double(moment.steps) - 1 + moment.fraction; }

// ---- Instances -----------------------------------------------------------------------------------------

constexpr unsigned kCrowdParts = 128;
constexpr uint32_t kMostInstances = 4096;
constexpr float kFarthest = 150; // farther than this between steps is another instance in the same place of the list

// The instances of one of the drawer's meshes at the last two steps, matched by their place in its list.
struct CrowdPart {
    uint64_t part, weak, step;
    uint32_t count, room;
    bool used, pair;
    float *before, *now; // count matrices each, two halves of one block
};
CrowdPart crowds[kCrowdParts] = {};

uint64_t drawer() {
    const uint64_t framework = game::global<uint64_t>(t::data::game_framework);
    const uint64_t holder = framework ? game::read_or<uint64_t>(framework + t::game_framework::instance_holder) : 0;
    const uint64_t found = holder ? game::read_or<uint64_t>(holder + t::instance_holder::drawer) : 0;
    return found && game::read_or<uint64_t>(found) == game::main_base() + t::constant::instance_drawer_table ? found : 0;
}

CrowdPart* crowd_of(uint64_t part, bool add) {
    CrowdPart* empty = nullptr;
    for (CrowdPart& entry : crowds) {
        if (entry.used && entry.part == part) return &entry;
        if (!entry.used && !empty) empty = &entry;
    }
    if (!add || !empty) return nullptr;
    empty->used = true, empty->part = part, empty->weak = game::to_weak_value(part), empty->step = 0, empty->count = 0, empty->pair = false;
    return empty;
}

bool make_room(CrowdPart& entry, uint32_t count) {
    if (count <= entry.room) return true;
    float* low = entry.before < entry.now ? entry.before : entry.now;
    auto* grown = static_cast<float*>(allocate(sizeof(float) * 16 * 2 * count));
    if (!grown) return false;
    release(low);
    entry.before = grown, entry.now = grown + 16 * count, entry.room = count, entry.pair = false;
    return true;
}

using DrawerUpdate = decltype(t::code_site::instance_drawer_update)::Function;
DrawerUpdate next_drawer = nullptr;

void drawer_update(void* at) {
    next_drawer(at);
    const uint64_t found = reinterpret_cast<uint64_t>(at);
    if (!moment.active || !(moment.parts & kPartCrowds) || found != drawer() || !on_game_thread()) return;
    const uint64_t list = game::read_or<uint64_t>(found + t::instance_drawer::components);
    const int32_t parts = game::read_or<int32_t>(found + t::instance_drawer::count);
    for (int32_t i = 0; list && i < parts && i < 256; ++i) {
        const uint64_t part = game::read_or<uint64_t>(list + 8 * uint64_t(i));
        if (!part) continue;
        const uint64_t data = game::read_or<uint64_t>(part + t::instanced_mesh::instances);
        const int32_t many = game::read_or<int32_t>(part + t::instanced_mesh::count);
        CrowdPart* entry = crowd_of(part, many > 0);
        if (!entry) continue;
        if (many <= 0 || uint32_t(many) > kMostInstances || !data || !make_room(*entry, uint32_t(many)) ||
            !game::readable(data, uint64_t(many) * 64)) {
            entry->used = false; // no instances, or ones it cannot keep: shown as the game leaves them
            continue;
        }
        // A second update in one step keeps the newest; a count that changed is another list.
        const bool next = entry->step && moment.steps == entry->step + 1 && uint32_t(many) == entry->count;
        if (next) {
            float* swap = entry->before;
            entry->before = entry->now, entry->now = swap;
        }
        entry->pair = next || (entry->pair && moment.steps == entry->step && uint32_t(many) == entry->count);
        memcpy(entry->now, reinterpret_cast<const void*>(data), 64 * uint64_t(many));
        entry->count = uint32_t(many), entry->step = moment.steps;
    }
}

// ---- Material values -----------------------------------------------------------------------------------

constexpr unsigned kMaterialValues = 1024;
constexpr uint64_t kMaterialGap = 4;     // steps apart that two values are still shown between
constexpr float kMaterialJump = 1500;    // a value that changed this much in a step jumped (15 m, for a point)
constexpr uint64_t kMaterialForget = 300; // steps a value stays kept without changing

struct MaterialValue {
    uint64_t material, weak, from, to;
    uint8_t info[16];
    float a[4], b[4], sent;
    bool used, pair, scalar;
};
MaterialValue materials[kMaterialValues] = {};
bool giving = false; // the presenter's own calls are not kept

using MaterialPush = decltype(t::code_site::material_vector_push)::Function;
MaterialPush next_vector = nullptr, next_scalar = nullptr;

MaterialValue* material_of(uint64_t material, const uint8_t info[16], bool scalar, bool add) {
    MaterialValue *empty = nullptr, *oldest = nullptr;
    for (MaterialValue& entry : materials) {
        if (entry.used && entry.material == material && entry.scalar == scalar && !memcmp(entry.info, info, 16)) return &entry;
        if (!entry.used && !empty) empty = &entry;
        if (entry.used && (!oldest || entry.to < oldest->to)) oldest = &entry;
    }
    if (!add) return nullptr;
    // Full: the value unchanged the longest gives up its place.
    MaterialValue* at = empty ? empty : oldest;
    if (!at) return nullptr;
    if (at == oldest && at->sent < 1) return nullptr; // its renderer copy would keep a value between
    *at = {};
    at->used = true, at->material = material, at->weak = game::to_weak_value(material), at->scalar = scalar, at->sent = 1;
    memcpy(at->info, info, 16);
    return at;
}

void keep_material(void* material, const void* parameter, bool scalar) {
    if (giving || !moment.active || !(moment.parts & kPartMaterials) || !material || !parameter || !on_game_thread()) return;
    uint8_t info[16];
    float value[4] = {};
    const uint64_t at = reinterpret_cast<uint64_t>(parameter);
    if (!game::read(at + t::material_parameter::info, info) ||
        !game::readable(at + t::material_parameter::value, scalar ? 4 : 16))
        return;
    memcpy(value, reinterpret_cast<const void*>(at + t::material_parameter::value), scalar ? 4 : 16);
    for (float v : value) if (!finite(v)) return;
    // A value pushed between steps is the game's own business. What was kept is not shown over it.
    if (!presenter_in_step()) {
        MaterialValue* kept = material_of(reinterpret_cast<uint64_t>(material), info, scalar, false);
        if (!kept) return;
        memcpy(kept->b, value, sizeof value);
        kept->pair = false, kept->sent = 1;
        return;
    }
    MaterialValue* entry = material_of(reinterpret_cast<uint64_t>(material), info, scalar, true);
    if (!entry) return;
    const uint64_t step = moment.steps;
    if (!entry->to) {
        memcpy(entry->b, value, sizeof value), entry->to = step;
    } else if (step == entry->to) {
        memcpy(entry->b, value, sizeof value); // set again in the same step: the newest is the step's
    } else {
        float most = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const float d = value[i] > entry->b[i] ? value[i] - entry->b[i] : entry->b[i] - value[i];
            if (d > most) most = d;
        }
        entry->pair = step - entry->to <= kMaterialGap && most < kMaterialJump;
        memcpy(entry->a, entry->b, sizeof value), memcpy(entry->b, value, sizeof value);
        entry->from = entry->to, entry->to = step;
    }
    entry->sent = 1; // the renderer has the game's own value now
}

// A build that makes the push inline in the material instance's own setters: what a setter leaves in the instance's
// values is kept as its push would have been. A value set by name is the one of that name with no index.
uint64_t value_named(uint64_t material, uint64_t name, bool scalar) {
    const uint64_t values = material + (scalar ? t::material_instance_values::scalars : t::material_instance_values::vectors);
    const uint64_t size = scalar ? t::material_scalar_value::size : t::material_vector_value::size;
    uint64_t data = 0;
    int32_t count = 0;
    if (!game::read(values + t::array::data, data) || !game::read(values + t::array::count, count) || !data || count <= 0 ||
        uint32_t(count) > t::constant::max_objects)
        return 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t at = data + uint64_t(i) * size;
        if (game::read_or<uint64_t>(at + t::material_parameter::info, 0) == name && game::read_or<int32_t>(at + t::material_parameter::index, 0) == -1)
            return at;
    }
    return 0;
}
using ScalarSet = decltype(t::code_site::material_scalar_set)::Function;
using ScalarSetIndex = decltype(t::code_site::material_scalar_set_index)::Function;
using VectorSet = decltype(t::code_site::material_vector_set)::Function;
ScalarSet next_scalar_set = nullptr;
ScalarSetIndex next_scalar_set_index = nullptr;
VectorSet next_vector_set = nullptr;
void scalar_set(void* material, uint64_t name, float value) {
    next_scalar_set(material, name, value);
    if (const uint64_t kept = value_named(reinterpret_cast<uint64_t>(material), name, true))
        keep_material(material, reinterpret_cast<const void*>(kept), true);
}
bool scalar_set_index(void* material, int32_t index, float value) {
    const bool set = next_scalar_set_index(material, index, value);
    const uint64_t values = reinterpret_cast<uint64_t>(material) + t::material_instance_values::scalars;
    int32_t count = 0;
    uint64_t data = 0;
    if (set && index >= 0 && game::read(values + t::array::count, count) && index < count && game::read(values + t::array::data, data) && data)
        keep_material(material, reinterpret_cast<const void*>(data + uint64_t(index) * t::material_scalar_value::size), true);
    return set;
}
void vector_set(void* material, uint64_t name, float r, float g, float b, float a) {
    next_vector_set(material, name, r, g, b, a);
    if (const uint64_t kept = value_named(reinterpret_cast<uint64_t>(material), name, false))
        keep_material(material, reinterpret_cast<const void*>(kept), false);
}

void vector_push(void* material, const void* parameter) {
    next_vector(material, parameter);
    keep_material(material, parameter, false);
}
void scalar_push(void* material, const void* parameter) {
    next_scalar(material, parameter);
    keep_material(material, parameter, true);
}

void give(MaterialValue& entry, const float value[4]) {
    alignas(16) uint8_t parameter[32] = {};
    memcpy(parameter + t::material_parameter::info, entry.info, 16);
    memcpy(parameter + t::material_parameter::value, value, entry.scalar ? 4 : 16);
    giving = true;
    (entry.scalar ? next_scalar : next_vector)(reinterpret_cast<void*>(entry.material), parameter);
    giving = false;
}

void materials_present() {
    if (!next_vector || !next_scalar) return;
    const double place = at();
    for (MaterialValue& entry : materials) {
        if (!entry.used) continue;
        if (!entry.weak || game::from_weak_value(entry.weak) != entry.material) {
            entry.used = false;
            continue;
        }
        const bool between = (moment.parts & kPartMaterials) && entry.pair && entry.to == moment.steps;
        const float f = between ? place_between(entry.from, entry.to, place) : 1;
        if (f < 1) {
            float value[4];
            for (unsigned i = 0; i < 4; ++i) value[i] = entry.a[i] + (entry.b[i] - entry.a[i]) * f;
            give(entry, value);
            entry.sent = f;
            count(counts.material_blends);
        } else if (entry.sent < 1) {
            give(entry, entry.b); // it stopped changing: its renderer copy gets the game's value back
            entry.sent = 1;
            count(counts.material_restores);
        } else if (entry.to + kMaterialForget < moment.steps) {
            entry.used = false;
        }
    }
}

// ---- Particles -----------------------------------------------------------------------------------------

// The fraction of each recent view family's picture, by its world time; written on the game's thread, read on the
// render thread. A packed entry is the time's bits and the fraction's.
constexpr unsigned kFamilies = 8;
uint64_t families[kFamilies] = {};
unsigned next_family = 0;
// The fraction of the scene being drawn on the render thread, or negative while none is known, as its bits.
uint32_t drawing_bits = 0xBF800000u;
void set_drawing(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    __atomic_store_n(&drawing_bits, bits, __ATOMIC_RELEASE);
}
float drawing() {
    const uint32_t bits = __atomic_load_n(&drawing_bits, __ATOMIC_ACQUIRE);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

using SceneRender = decltype(t::code_site::scene_render)::Function;
SceneRender next_render = nullptr;

void scene_render(void* command, void* commands) {
    const uint64_t renderer = game::read_or<uint64_t>(reinterpret_cast<uint64_t>(command));
    const uint32_t time = renderer ? game::read_or<uint32_t>(renderer + t::scene_renderer::world_seconds) : 0;
    float fraction = -1;
    for (const uint64_t& packed : families) {
        const uint64_t entry = __atomic_load_n(&packed, __ATOMIC_ACQUIRE);
        if (entry && uint32_t(entry) == time) {
            const uint32_t bits = uint32_t(entry >> 32);
            memcpy(&fraction, &bits, sizeof fraction);
        }
    }
    count(counts.renders);
    if (!(moment.parts & kPartParticles)) fraction = -1; // drawn as the game left them, and still timed
    if (fraction >= 0 && fraction < 1) count(counts.renders_known);
    set_drawing(fraction >= 0 && fraction < 1 ? fraction : -1.0f);
    const uint64_t began = microseconds ? microseconds() : 0;
    static uint64_t last_began = 0; // the render thread's only
    if (last_began && began > last_began) {
        const uint32_t at = __atomic_fetch_add(&counts.render_gaps, 1, __ATOMIC_RELAXED);
        if (at < 240) counts.render_gap_ms[at] = float(began - last_began) / 1000;
    }
    last_began = began;
    next_render(command, commands);
    if (microseconds) {
        const uint64_t took = microseconds() - began;
        __atomic_fetch_add(&counts.rendering_us, took, __ATOMIC_RELAXED);
        const uint32_t at = __atomic_fetch_add(&counts.scenes_timed, 1, __ATOMIC_RELAXED);
        if (at < 240) counts.scene_ms[at] = float(took) / 1000;
    }
    set_drawing(-1.0f);
}

bool finite3(const float v[3]) { return finite(v[0]) && finite(v[1]) && finite(v[2]); }

// A particle's place that far between its location before its last update and after.
bool particle_shown(uint64_t particle, float f, float shown[3]) {
    float old[3], location[3];
    memcpy(old, reinterpret_cast<const void*>(particle + t::particle::old_location), sizeof old);
    memcpy(location, reinterpret_cast<const void*>(particle + t::particle::location), sizeof location);
    for (unsigned i = 0; i < 3; ++i) shown[i] = old[i] + (location[i] - old[i]) * f;
    return finite3(shown);
}

float lane(const NativeFrame& frame, uint64_t q, unsigned i) {
    float value;
    memcpy(&value, frame.q[q] + 4 * i, sizeof value);
    return value;
}
void set_lane(NativeFrame& frame, uint64_t q, unsigned i, float value) { memcpy(frame.q[q] + 4 * i, &value, sizeof value); }

// A route frame's register that may be either kind: 0 to 30 a general register holding a float's bits, 32 to 63 the
// vector register 32 below, its lowest lane.
float either(const NativeFrame& frame, uint64_t at) {
    if (at >= 32) return lane(frame, at - 32, 0);
    const uint32_t bits = uint32_t(frame.x[at]);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
void set_either(NativeFrame& frame, uint64_t at, float value) {
    if (at >= 32) return set_lane(frame, at - 32, 0, value);
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    frame.x[at] = bits;
}

// A sprite's vertices: its location's x and y in a 64-bit vector register and its z in another register, its old
// location in one 64-bit and one 32-bit vector register. Both move by the same amount, so the sprite keeps its
// velocity's direction and length.
void on_sprite(NativeFrame& frame) {
    const float f = drawing();
    if (!(f >= 0 && f < 1)) return;
    namespace r = t::route_frame::particle_sprite;
    float location[3] = {lane(frame, r::location, 0), lane(frame, r::location, 1), either(frame, r::z)};
    float old[3] = {lane(frame, r::old, 0), lane(frame, r::old, 1), lane(frame, r::old_z, 0)};
    float shown[3], before[3];
    for (unsigned i = 0; i < 3; ++i) {
        const float d = location[i] - old[i];
        shown[i] = old[i] + d * f, before[i] = shown[i] - d;
    }
    if (!finite3(shown) || !finite3(before)) return;
    set_lane(frame, r::location, 0, shown[0]), set_lane(frame, r::location, 1, shown[1]);
    set_either(frame, r::z, shown[2]);
    set_lane(frame, r::old, 0, before[0]), set_lane(frame, r::old, 1, before[1]), set_lane(frame, r::old_z, 0, before[2]);
    count(counts.sprites);
}

// Where each thread keeps the place it gives a mesh particle's transform.
struct ThreadPlace {
    uint64_t thread;
    alignas(16) float place[4];
};
constexpr unsigned kThreads = 32;
ThreadPlace places[kThreads] = {};
float* place_of_thread() {
    const uint64_t me = current_thread_token();
    for (ThreadPlace& slot : places)
        if (__atomic_load_n(&slot.thread, __ATOMIC_ACQUIRE) == me) return slot.place;
    for (ThreadPlace& slot : places) {
        uint64_t none = 0;
        if (__atomic_compare_exchange_n(&slot.thread, &none, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return slot.place;
    }
    return nullptr;
}

// A mesh particle's transform is made from the location a register points at: it is given the shown place instead.
void mesh_place(NativeFrame& frame, uint64_t particle, uint64_t location_pointer) {
    const float f = drawing();
    if (!(f >= 0 && f < 1)) return;
    float* place = place_of_thread();
    float shown[3];
    if (!place || !particle_shown(frame.x[particle], f, shown)) return;
    memcpy(place, shown, sizeof shown), place[3] = 0;
    frame.x[location_pointer] = reinterpret_cast<uint64_t>(place);
    count(counts.meshes);
}
// With the camera's offset the location is loaded again: x and y in one 64-bit vector register, then z in a 32-bit one.
void mesh_xy(NativeFrame& frame, uint64_t particle, uint64_t xy) {
    const float f = drawing();
    float shown[3];
    if (!(f >= 0 && f < 1) || !particle_shown(frame.x[particle], f, shown)) return;
    set_lane(frame, xy, 0, shown[0]), set_lane(frame, xy, 1, shown[1]);
}
void mesh_z(NativeFrame& frame, uint64_t particle, uint64_t z) {
    const float f = drawing();
    float shown[3];
    if (!(f >= 0 && f < 1) || !particle_shown(frame.x[particle], f, shown)) return;
    set_lane(frame, z, 0, shown[2]);
}
namespace mesh = t::route_frame::particle_mesh_place;
namespace mesh_x = t::route_frame::particle_mesh_xy;
namespace mesh_h = t::route_frame::particle_mesh_z;
void on_mesh_place(NativeFrame& frame) { mesh_place(frame, mesh::particle, mesh::location_pointer); }
void on_mesh_xy(NativeFrame& frame) { mesh_xy(frame, mesh_x::particle, mesh_x::xy); }
void on_mesh_z(NativeFrame& frame) { mesh_z(frame, mesh_h::particle, mesh_h::z); }
// The same in the transform's own copy, where a build has inlined it in the instance loop as well.
namespace lone = t::route_frame::particle_lone_place;
namespace lone_x = t::route_frame::particle_lone_xy;
namespace lone_h = t::route_frame::particle_lone_z;
void on_lone_place(NativeFrame& frame) { mesh_place(frame, lone::particle, lone::location_pointer); }
void on_lone_xy(NativeFrame& frame) { mesh_xy(frame, lone_x::particle, lone_x::xy); }
void on_lone_z(NativeFrame& frame) { mesh_z(frame, lone_h::particle, lone_h::z); }

} // namespace

bool effects_start(TSUCHI_Mod* self, uint64_t (*clock_microseconds)()) {
    microseconds = clock_microseconds;
    uint64_t id = 0;
    bool all = hook_site(self, t::code_site::instance_drawer_update, &drawer_update, &next_drawer, &id) == TSUCHI_OK;
    all = hook_site(self, t::code_site::scene_render, &scene_render, &next_render, &id) == TSUCHI_OK && all;
    all = hook_site(self, t::code_site::material_vector_push, &vector_push, &next_vector, &id) == TSUCHI_OK && all;
    all = hook_site(self, t::code_site::material_scalar_push, &scalar_push, &next_scalar, &id) == TSUCHI_OK && all;
    // A build whose setters make the push inline names them (same_as on a build where they call the pushes).
    if (t::present(t::code_sites[t::code_site::material_scalar_set.number].address)) {
        all = hook_site(self, t::code_site::material_scalar_set, &scalar_set, &next_scalar_set, &id) == TSUCHI_OK && all;
        all = hook_site(self, t::code_site::material_scalar_set_index, &scalar_set_index, &next_scalar_set_index, &id) == TSUCHI_OK && all;
        all = hook_site(self, t::code_site::material_vector_set, &vector_set, &next_vector_set, &id) == TSUCHI_OK && all;
    }
    // Without the drawing's fraction the particles' routes would have none to go by.
    if (next_render) {
        all = native_route(t::native_route::particle_sprite, &on_sprite) && all;
        all = native_route(t::native_route::particle_mesh_place, &on_mesh_place) && all;
        all = native_route(t::native_route::particle_mesh_xy, &on_mesh_xy) && all;
        all = native_route(t::native_route::particle_mesh_z, &on_mesh_z) && all;
        // A build with the transform once (same_as) has these routes absent: the ones above serve both.
        if (t::present(t::native_routes[t::native_route::particle_lone_place].address)) {
            all = native_route(t::native_route::particle_lone_place, &on_lone_place) && all;
            all = native_route(t::native_route::particle_lone_xy, &on_lone_xy) && all;
            all = native_route(t::native_route::particle_lone_z, &on_lone_z) && all;
        }
    }
    return all;
}

void effects_moment(const PresentedMoment& now) { moment = now; }

void effects_family(float world_seconds, float fraction) {
    uint32_t time, bits;
    memcpy(&time, &world_seconds, sizeof time);
    memcpy(&bits, &fraction, sizeof bits);
    __atomic_store_n(&families[next_family], uint64_t(bits) << 32 | time, __ATOMIC_RELEASE);
    next_family = (next_family + 1) % kFamilies;
}

void effects_present() {
    if (!moment.active) return;
    materials_present();
    if (!(moment.parts & kPartCrowds) || !t::bound(t::fn::mark_render_state)) return;
    const double place = at();
    for (CrowdPart& entry : crowds) {
        if (!entry.used || !entry.pair || entry.step != moment.steps) continue;
        const float f = place_between(entry.step - 1, entry.step, place);
        if (f >= 1) continue;
        if (!entry.weak || game::from_weak_value(entry.weak) != entry.part) {
            entry.used = false;
            continue;
        }
        const uint64_t data = game::read_or<uint64_t>(entry.part + t::instanced_mesh::instances);
        if (!data || game::read_or<int32_t>(entry.part + t::instanced_mesh::count) != int32_t(entry.count) ||
            !game::readable(data, uint64_t(entry.count) * 64))
            continue;
        for (uint32_t i = 0; i < entry.count; ++i) {
            const float* a = entry.before + 16 * i;
            const float* b = entry.now + 16 * i;
            Trs from, to, shown;
            float matrix[16], move = 0;
            if (!to_trs(a, from) || !to_trs(b, to) || (from.s[0] < 0) != (to.s[0] < 0) || jumped(from.t, to.t, kFarthest / 4, move)) {
                memcpy(matrix, b, sizeof matrix);
                count(counts.crowd_held);
            } else {
                blend(from, to, f, shown);
                to_matrix(shown, matrix);
            }
            memcpy(reinterpret_cast<void*>(data + 64 * uint64_t(i)), matrix, sizeof matrix);
        }
        game::write<int32_t>(entry.part + t::instanced_mesh::edits, game::read_or<int32_t>(entry.part + t::instanced_mesh::edits) + 1);
        t::fn::mark_render_state(reinterpret_cast<void*>(entry.part));
        count(counts.crowd_parts);
        count(counts.crowd_instances, entry.count);
    }
}

void effects_forget() {
    for (CrowdPart& entry : crowds) entry.used = false; // its block is kept for the next
    for (MaterialValue& entry : materials) {
        if (entry.used && entry.sent < 1 && next_vector && next_scalar && game::from_weak_value(entry.weak) == entry.material)
            give(entry, entry.b);
        entry.used = false;
    }
    for (uint64_t& packed : families) __atomic_store_n(&packed, uint64_t(0), __ATOMIC_RELEASE);
}

EffectCounts effect_counts(bool clear) {
    EffectCounts out = counts;
    out.material_values = 0;
    for (const MaterialValue& entry : materials) out.material_values += entry.used;
    if (clear) counts = {};
    return out;
}

} // namespace tsuchi
