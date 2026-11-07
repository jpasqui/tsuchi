#include "actors.h"
#include "engine_call.h"

#include <string.h>

#include "game.h"
#include "angles.h"
#include "objects.h"

namespace tsuchi::game {
namespace {

namespace t = table;

ClassRef actor_class{t::name::actor_class};
ClassRef game_component_class{t::name::game_component_class};
ClassRef primitive_class{t::name::primitive_class};

uint64_t component_at(uint64_t actor, uint64_t offset) {
    const uint64_t component = read_or<uint64_t>(actor + offset);
    return component && live(component) ? component : 0;
}


// A character turns one of its components, usually its body, and that
// component's forward is +Y where an actor's is +X: a quarter turn apart.
constexpr float kHalfRoot2 = 0.7071067811865476f;
constexpr float kFacingFromComponent[4] = {0, 0, kHalfRoot2, kHalfRoot2};
constexpr float kComponentFromFacing[4] = {0, 0, -kHalfRoot2, kHalfRoot2};

bool read_quaternion(uint64_t component, float q[4]) {
    if (!readable(component + t::scene_component::rotation, 16)) return false;
    for (int i = 0; i < 4; ++i) q[i] = read_or<float>(component + t::scene_component::rotation + 4 * i);
    return true;
}

// The component a character turns to face somewhere, or 0 for an actor that turns its root.
uint64_t facing_component(uint64_t actor, uint64_t root) {
    if (!read_or<uint8_t>(actor + t::game_actor::facing_enabled)) return 0;
    const uint64_t component = read_or<uint64_t>(actor + t::game_actor::facing_component);
    return component && component != root && live(component) && readable(component + t::scene_component::rotation, 16) ? component : 0;
}

// Turns the component a character faces with, so the character faces along `angles`.
void turn_body(uint64_t component, const float angles[3]) {
    alignas(8) unsigned char hit[t::most::sweep_result_bytes] = {}; // where a sweep would report what it hit; nothing is swept
    float wanted[4], body[4], turned[3];
    math::to_quaternion(angles, wanted);
    math::multiply(wanted, kComponentFromFacing, body);
    math::to_angles(body, turned);
    t::fn::set_component_world_rotation(reinterpret_cast<void*>(component), false, hit, true, turned[0], turned[1], turned[2]);
}

} // namespace

uint64_t physics_component(uint64_t actor) {
    const uint64_t component = read_or<uint64_t>(actor + t::game_actor::physics);
    return component && live(component) ? component : 0;
}

uint64_t game_framework() {
    const uint64_t framework = global<uint64_t>(t::data::game_framework);
    return framework && live(framework) && readable(framework + t::game_framework::actor_list, t::actor_node::size) ? framework : 0;
}

bool is_game_actor(uint64_t object) { return is_a(object, actor_class); }
uint64_t game_actor_class() { return find_class(actor_class); }

void each_actor(bool (*visit)(void*, uint64_t), void* context) { each_listed(t::game_framework::actor_list, visit, context); }

void each_listed(uint64_t list_offset, bool (*visit)(void*, uint64_t), void* context) {
    const uint64_t framework = game_framework();
    if (!framework) return;
    // The list is a ring through a link that is part of the framework itself.
    const uint64_t head = framework + list_offset;
    uint64_t node = read_or<uint64_t>(head + t::actor_node::next);
    for (uint32_t guard = 0; node && node != head && guard < 65536; ++guard) {
        if (!readable(node, t::actor_node::size)) return;
        const uint64_t actor = read_or<uint64_t>(node + t::actor_node::actor);
        if (actor && live(actor) && !visit(context, actor)) return;
        node = read_or<uint64_t>(node + t::actor_node::next);
    }
}

Name actor_kind(uint64_t actor) { return read_or<Name>(actor + t::game_actor::kind); }
int32_t actor_team(uint64_t actor) { return read_or<int32_t>(actor + t::game_actor::team, -1); }
uint64_t actor_placement(uint64_t actor) {
    const uint64_t placement = read_or<uint64_t>(actor + t::game_actor::placement);
    return placement == ~uint64_t(0) ? 0 : placement; // the game's own "none"
}

bool get_transform(uint64_t actor, Transform& out) {
    const uint64_t root = read_or<uint64_t>(actor + t::game_actor::root);
    float q[4];
    if (!root || !read_quaternion(root, q) || !readable(root + t::scene_component::location, 12)) return false;
    float body[4];
    if (const uint64_t facing = facing_component(actor, root); facing && read_quaternion(facing, body))
        math::multiply(body, kFacingFromComponent, q);
    for (int i = 0; i < 3; ++i) out.location[i] = read_or<float>(root + t::scene_component::location + 4 * i);
    math::to_angles(q, out.angles);
    return true;
}

TSUCHI_Result set_transform(uint64_t actor, const Transform& to) {
    const uint64_t root = read_or<uint64_t>(actor + t::game_actor::root);
    if (!root || !t::bound(t::fn::set_actor_location_and_rotation) || !t::bound(t::fn::set_component_world_rotation)) return TSUCHI_NOT_READY;
    alignas(8) unsigned char hit[t::most::sweep_result_bytes] = {}; // where a sweep would report what it hit; nothing is swept
    const uint64_t facing = facing_component(actor, root);
    float root_angles[3] = {to.angles[0], to.angles[1], to.angles[2]};
    float q[4];
    if (facing && read_quaternion(root, q)) math::to_angles(q, root_angles); // the root keeps its own; the body turns
    t::fn::set_actor_location_and_rotation(reinterpret_cast<void*>(actor), false, hit, true, to.location[0], to.location[1], to.location[2],
                                           root_angles[0], root_angles[1], root_angles[2]);
    if (facing) turn_body(facing, to.angles);
    return TSUCHI_OK;
}

bool get_velocity(uint64_t actor, float out[3]) {
    namespace a = t::game_actor;
    uint64_t from = actor + a::velocity;
    if (!body_awake(actor)) { // no moving body: what its root says of itself
        const uint64_t root = read_or<uint64_t>(actor + a::root);
        if (!root) return false;
        from = root + t::scene_component::velocity;
    }
    if (!readable(from, 12)) return false;
    for (int i = 0; i < 3; ++i) out[i] = read_or<float>(from + 4 * i);
    return true;
}

bool can_launch(uint64_t actor) {
    namespace a = t::game_actor;
    if (!readable(actor + a::air_steps, 4) || read_or<uint8_t>(actor + a::kill_queued)) return false;
    const uint64_t physics = physics_component(actor);
    if (!physics || read_or<uint64_t>(physics + t::scene_component::attach_parent)) return false; // carried, held or latched on
    return !read_or<uint8_t>(actor + a::simulating_known) || read_or<uint8_t>(actor + a::simulating);
}

void request_velocity(uint64_t actor, const float velocity[3]) {
    if (!t::bound(t::fn::request_velocity)) return;
    write(actor + t::game_actor::speed_clamp, uint8_t(0));
    write(actor + t::game_actor::keep_vertical, uint8_t(0));
    float copy[3] = {velocity[0], velocity[1], velocity[2]};
    t::fn::request_velocity(reinterpret_cast<void*>(actor), copy);
}

void halt(uint64_t actor) {
    if (!t::bound(t::fn::request_velocity) || !can_launch(actor)) return; // nothing of its own to bring to rest
    drop_velocity_requests(actor);
    float still[3] = {0, 0, 0};
    t::fn::request_velocity(reinterpret_cast<void*>(actor), still);
    write(actor + t::game_actor::keep_vertical, uint8_t(1)); // the request is for across the ground only
}

void drop_velocity_requests(uint64_t actor) {
    namespace a = t::game_actor;
    const uint64_t flags[] = {a::velocity_pending, a::velocity_added_1, a::velocity_added_2, a::velocity_added_3, a::keep_vertical};
    for (const uint64_t flag : flags) write(actor + flag, uint8_t(0));
}

bool body_awake(uint64_t actor) { return read_or<uint8_t>(actor + t::game_actor::body_awake) != 0; }
uint32_t air_steps(uint64_t actor) { return read_or<uint32_t>(actor + t::game_actor::air_steps); }

void hold_awake(uint64_t actor, bool hold) {
    const int32_t holds = read_or<int32_t>(actor + t::game_actor::awake_holds);
    if (hold) {
        write(actor + t::game_actor::awake_holds, holds + 1);
        return;
    }
    if (holds > 0) write(actor + t::game_actor::awake_holds, holds - 1);
    write(actor + t::game_actor::freeze_delay, read_or<int32_t>(actor + t::game_actor::freeze_delay) + 1);
}

TSUCHI_Result warp_actor(uint64_t actor, const Transform& to) {
    namespace a = t::game_actor;
    namespace c = t::scene_component;
    const uint64_t framework = game_framework();
    if (!framework || !read_or<uint64_t>(framework + t::game_framework::ground_query) || !t::bound(t::fn::warp_actor)) return TSUCHI_NOT_READY;
    if (read_or<uint8_t>(actor + a::kill_queued) || read_or<uint8_t>(actor + a::terminated)) return TSUCHI_NOT_FOUND;
    // The game's warp moves the actor's own body and root. Both have to be its own, and
    // the root may hang from nothing that belongs to another actor.
    const uint64_t physics = physics_component(actor), root = read_or<uint64_t>(actor + a::root);
    if (!physics || !is_a(physics, primitive_class) || read_or<uint64_t>(physics + c::owner) != actor) return TSUCHI_UNSUPPORTED;
    if (!root || !live(root) || read_or<uint64_t>(root + c::owner) != actor) return TSUCHI_UNSUPPORTED;
    const uint64_t parent = read_or<uint64_t>(root + c::attach_parent);
    if (parent && (!live(parent) || read_or<uint64_t>(parent + c::owner))) return TSUCHI_UNSUPPORTED;
    float location[3] = {to.location[0], to.location[1], to.location[2]}; // the game works on what it is given
    float angles[3] = {to.angles[0], to.angles[1], to.angles[2]};
    t::fn::warp_actor(reinterpret_cast<void*>(actor), location, angles, true, true, true);
    // The game's warp turns the root. A character faces with its body, which does not follow.
    if (const uint64_t facing = facing_component(actor, root); facing && t::fn::set_component_world_rotation) turn_body(facing, to.angles);
    return TSUCHI_OK;
}

bool is_simulating(uint64_t actor) {
    const uint64_t body = physics_component(actor);
    const auto ask = body ? method(body, t::primitive_table::is_simulating) : nullptr;
    return ask && ask(reinterpret_cast<void*>(body), 0);
}

void set_simulating(uint64_t actor, bool on) {
    const uint64_t body = physics_component(actor);
    const auto set = body ? method(body, t::primitive_table::set_simulating) : nullptr;
    if (!set) return;
    set(reinterpret_cast<void*>(body), on);
    // What the actor remembers of its body, as the game's own switch leaves it.
    namespace a = t::game_actor;
    write(actor + a::simulating, uint8_t(on));
    write(actor + a::simulating_known, uint8_t(1));
    write(actor + a::rigid_body, uint64_t(0));
    write(actor + a::body_awake, uint8_t(0));
}

uint8_t collision_of(uint64_t component) {
    const auto ask = method(component, t::primitive_table::get_collision);
    return ask ? ask(reinterpret_cast<void*>(component)) : uint8_t(kCollisionNone);
}

void set_collision(uint64_t component, uint8_t mode) {
    if (const auto set = method(component, t::primitive_table::set_collision)) set(reinterpret_cast<void*>(component), mode);
}

namespace {

// An actor's parts hang below its root a few levels deep. Neither number is a
// limit on an actor: past either, the parts lead back into themselves.
constexpr uint32_t kDeepestPart = 64, kMostParts = 65536;

struct PartWalk {
    uint64_t actor;
    void (*visit)(void*, uint64_t);
    void* context;
    uint32_t walked;
    bool only_solid; // only parts that can collide are visited
};

bool walk_parts(PartWalk& walk, uint64_t component, uint32_t depth) {
    namespace c = t::scene_component;
    if (depth > kDeepestPart || ++walk.walked > kMostParts) return false;
    if (read_or<uint64_t>(component + c::owner) != walk.actor) return true; // attached to it, but another actor's
    if (!walk.only_solid || is_a(component, primitive_class)) walk.visit(walk.context, component);
    const uint64_t children = read_or<uint64_t>(component + c::attach_children + t::array::data);
    const int32_t count = read_or<int32_t>(component + c::attach_children + t::array::count);
    if (!count) return true;
    if (!children || !readable(children, 8 * uint64_t(count))) return false; // a count below zero is far more than can be read
    for (int32_t i = 0; i < count; ++i)
        if (const uint64_t child = read_or<uint64_t>(children + 8 * uint64_t(i)); child && live(child) && !walk_parts(walk, child, depth + 1)) return false;
    return true;
}

struct PartList {
    uint64_t* out;
    uint32_t capacity, found;
};
void list_part(void* context, uint64_t component) {
    PartList& list = *static_cast<PartList*>(context);
    if (list.found < list.capacity) list.out[list.found] = component;
    ++list.found;
}

} // namespace

bool each_primitive(uint64_t actor, void (*visit)(void*, uint64_t), void* context) {
    PartWalk walk{actor, visit, context, 0, true};
    const uint64_t root = read_or<uint64_t>(actor + t::game_actor::root);
    return !root || !live(root) || walk_parts(walk, root, 0);
}

bool each_attached(uint64_t component, void (*visit)(void*, uint64_t), void* context) {
    PartWalk walk{read_or<uint64_t>(component + t::scene_component::owner), visit, context, 0, false};
    return walk_parts(walk, component, 0);
}

uint32_t actor_primitives(uint64_t actor, uint64_t* out, uint32_t capacity) {
    PartList list{out, capacity, 0};
    each_primitive(actor, list_part, &list);
    return list.found;
}

namespace {

inline int32_t max_records() { return int32_t(t::constant::collision_records_most); }

// The physics engine's body behind a component, or 0. False when it cannot be told.
bool physics_body(uint64_t component, uint64_t& body) {
    body = 0;
    const auto get = method(component, t::primitive_table::body_instance);
    if (!get) return false;
    const auto instance = reinterpret_cast<uint64_t>(get(reinterpret_cast<void*>(component), 0, true));
    if (!instance || !readable(instance + t::body_instance::body, 8)) return false;
    body = read_or<uint64_t>(instance + t::body_instance::body);
    return true;
}

// How many shapes a body has, when it is the kind of body that carries the shapes the game borrows. False when it is not.
bool rigid_shape_count(uint64_t body, uint64_t& count) {
    const auto is_kind = method(body, t::physics_body_table::is_kind);
    const auto shapes = method(body, t::physics_body_table::shape_count);
    if (!is_kind || !shapes || !is_kind(reinterpret_cast<void*>(body), t::name::rigid_body)) return false;
    count = shapes(reinterpret_cast<void*>(body));
    return true;
}

} // namespace

uint64_t collision_record_of(uint64_t actor, uint64_t component) {
    namespace tr = t::collision_tree;
    namespace r = t::collision_record;
    const uint64_t tree = read_or<uint64_t>(actor + t::game_actor::collision_tree);
    if (!tree || !live(tree) || read_or<uint64_t>(tree + t::scene_component::owner) != actor || read_or<uint64_t>(tree + tr::actor) != actor) return 0;
    const uint64_t records = read_or<uint64_t>(tree + tr::records);
    const int32_t count = read_or<int32_t>(tree + tr::count), capacity = read_or<int32_t>(tree + tr::capacity);
    if (count <= 0 || count > max_records() || capacity < count || !writable(records, uint64_t(count) * r::size)) return 0;
    uint64_t found = 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t record = records + uint64_t(i) * r::size;
        if (read_or<uint64_t>(record + r::component) != component) continue;
        if (found) return 0; // kept twice: which one the game means cannot be told
        found = record;
    }
    return found;
}

bool rebuild_collision_tree(uint64_t actor) {
    namespace tr = t::collision_tree;
    namespace r = t::collision_record;
    const uint64_t tree = read_or<uint64_t>(actor + t::game_actor::collision_tree);
    if (!tree) return true; // nothing borrowed, nothing to find again
    if (!live(tree) || read_or<uint64_t>(tree + t::scene_component::owner) != actor || read_or<uint64_t>(tree + tr::actor) != actor) return false;
    const uint64_t records = read_or<uint64_t>(tree + tr::records);
    const int32_t count = read_or<int32_t>(tree + tr::count), capacity = read_or<int32_t>(tree + tr::capacity);
    if (count < 0 || count > max_records() || capacity < count || capacity > 4096 || (count && !writable(records, uint64_t(count) * r::size))) return false;
    // Every part has to be the actor's own before the game's rebuild may walk them.
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t component = read_or<uint64_t>(records + uint64_t(i) * r::size + r::component);
        if (component && (!live(component) || read_or<uint64_t>(component + t::scene_component::owner) != actor)) return false;
    }
    // The borrowed shapes are stale from here on, whatever happens below.
    for (int32_t i = 0; i < count; ++i) {
        write(records + uint64_t(i) * r::size + r::shape, uint64_t(0));
        write(records + uint64_t(i) * r::size + r::shape_index, int32_t(-1));
    }
    if (!count) return true;
    if (!read_or<uint64_t>(tree + tr::shapes) && !read_or<uint64_t>(tree + tr::shapes_other)) return true; // it keeps none
    if (!t::bound(t::fn::rebuild_collision_tree)) return false;
    const uint64_t physics = physics_component(actor);
    uint64_t body = 0;
    if (!physics || !physics_body(physics, body)) return false;
    uint64_t total = 0;
    if (body && rigid_shape_count(body, total)) {
        if (total > t::constant::collision_shapes_most) return false;
        // Parts can be welded onto the root's body or have bodies of their own. Count each body once.
        uint64_t seen[t::most::collision_records_most + 1] = {body};
        uint32_t used = 1;
        for (int32_t i = 0; i < count; ++i) {
            const uint64_t component = read_or<uint64_t>(records + uint64_t(i) * r::size + r::component);
            if (!component || !collision_of(component)) continue;
            uint64_t other = 0;
            if (!physics_body(component, other) || !other) return false;
            bool counted = false;
            for (uint32_t j = 0; j < used; ++j) counted = counted || seen[j] == other;
            if (counted) continue;
            uint64_t more = 0;
            if (!rigid_shape_count(other, more) || more > t::constant::collision_shapes_most - total) return false;
            total += more;
            seen[used++] = other;
        }
    }
    t::fn::rebuild_collision_tree(reinterpret_cast<void*>(tree));
    return true;
}

uint64_t table_entry(uint64_t object, uint64_t offset) {
    const uint64_t table = read_or<uint64_t>(object);
    const uint64_t entry = table ? read_or<uint64_t>(table + offset) : 0;
    return entry && executable(entry) ? entry : 0;
}

uint64_t ai_component(uint64_t actor) { return component_at(actor, t::game_actor::ai); }
uint64_t movement_component(uint64_t actor) { return component_at(actor, t::game_actor::movement); }

namespace {
bool ticking(uint64_t object, const char* function, bool& on) {
    EngineCall call(object, function);
    Value value;
    if (call.ready() != TSUCHI_OK || call.invoke() != TSUCHI_OK ||
        call.get("ReturnValue", value) != TSUCHI_OK || value.kind != ValueKind::boolean) return false;
    on = value.boolean;
    return true;
}
}
bool actor_ticking(uint64_t actor, bool& on) {
    uint8_t requested;
    if (!read(actor + t::game_actor::tick_requested, requested)) return false;
    on = requested != 0;
    return true;
}
bool component_ticking(uint64_t component, bool& on) {
    if (!is_a(component, game_component_class)) return ticking(component, "IsComponentTickEnabled", on);
    uint8_t requested;
    if (!read(component + t::game_component::tick_requested, requested)) return false;
    on = requested != 0;
    return true;
}

void set_actor_ticking(uint64_t actor, bool on) {
    if (const auto request = method(actor, t::actor_table::request_tick)) request(reinterpret_cast<void*>(actor), on);
}

void set_component_ticking(uint64_t component, bool on) {
    if (const auto set = method(component, t::component_table::set_tick_enabled)) set(reinterpret_cast<void*>(component), on);
}

void build_message(uint8_t* message, uint64_t size, uint64_t table, uint64_t sender) {
    namespace m = t::message;
    memset(message, 0, size);
    const uint64_t functions = main_base() + table, self = reinterpret_cast<uint64_t>(message);
    const uint64_t from = to_weak_value(sender), none = to_weak_value(0);
    const uint32_t one = 1;
    const uint8_t ready = 1;
    memcpy(message, &functions, 8), memcpy(message + m::self, &self, 8), memcpy(message + m::sender, &from, 8);
    memcpy(message + m::other, &none, 8), memcpy(message + m::references, &one, 4), memcpy(message + m::ready, &ready, 1);
}

bool can_build_message(uint64_t size) {
    namespace m = t::message;
    if (size == t::absent_value || !t::present(m::self) || !t::present(m::other) || !t::present(m::ready)) return false;
    // Every word it writes is inside the message.
    return size >= 8 && m::self + 8 <= size && m::sender + 8 <= size && m::other + 8 <= size && m::references + 4 <= size &&
           m::ready + 1 <= size;
}

uint32_t take_messages(uint64_t actor, void (*visit)(void*, const Message&), void* context) {
    const uint64_t brain = ai_component(actor);
    const uint64_t queue = brain ? read_or<uint64_t>(brain + t::ai_component::messages) : 0;
    // Only the queue this knows: its function table says what it is.
    if (!queue || read_or<uint64_t>(queue) != main_base() + t::data::message_queue_table) return 0;
    const auto pop = method(queue, t::message_queue_table::pop);
    if (!pop) return 0;
    const uint64_t base = main_base();
    uint32_t taken = 0;
    for (; taken < 256; ++taken) { // a frame's worth and more; the rest waits for the next frame
        const auto message = reinterpret_cast<uint64_t>(pop(reinterpret_cast<void*>(queue)));
        if (!message || !readable(message + t::message::references, 4)) break;
        Message out{};
        out.kind = 3;
        out.sender = from_weak_reference(message + t::message::sender);
        const uint64_t table = read_or<uint64_t>(message);
        if (table == base + t::data::damage_message_table && readable(message, t::damage_message::size)) {
            out.kind = 0;
            out.amount = read_or<float>(message + t::damage_message::amount);
            out.attack = read_or<uint8_t>(message + t::damage_message::attack);
            for (int i = 0; i < 3; ++i) out.vector[i] = read_or<float>(message + t::damage_message::origin + 4 * i);
        } else if (table == base + t::data::knock_message_table && readable(message, t::knock_message::size)) {
            out.kind = 1;
            out.amount = read_or<float>(message + t::knock_message::damage);
            for (int i = 0; i < 3; ++i) out.vector[i] = read_or<float>(message + t::knock_message::velocity + 4 * i);
        } else if (table == base + t::data::whistle_message_table) {
            out.kind = 2;
        }
        if (visit) visit(context, out);
        // One message can wait in several actors' queues, as a whistle does. The last to let go of it frees it.
        auto* references = reinterpret_cast<int32_t*>(message + t::message::references);
        if (__atomic_sub_fetch(references, 1, __ATOMIC_ACQ_REL) <= 0)
            if (const auto destroy = method(message, t::message_table::destroy)) destroy(reinterpret_cast<void*>(message));
    }
    return taken;
}

namespace {
// A request as the game's own spawner is given one, with nothing special asked for.
void fill_request(unsigned char* request, uint64_t type, const Transform& at, int32_t team) {
    namespace r = t::spawn_request;
    namespace c = t::constant;
    memset(request, 0, r::size);
    const auto put = [&](uint64_t offset, auto value) { memcpy(request + offset, &value, sizeof value); };
    put(r::class_, type);
    put(r::object_flags, uint32_t(c::spawn_object_flags));
    for (uint32_t i = 0; i < 3; ++i) {
        put(r::location + 4 * i, at.location[i]);
        put(r::rotation + 4 * i, at.angles[i]);
        put(r::scale + 4 * i, 1.0f);
    }
    put(r::team, team < 0 ? int32_t(c::spawn_team_default) : team);
    put(r::team_editor, int32_t(c::spawn_team_editor));
    put(r::color, uint8_t(c::spawn_color_none));
    put(r::leaf, uint8_t(c::spawn_leaf_none));
    put(r::user_value, int32_t(-1));
    put(r::delegate_threshold, int32_t(c::spawn_delegate_threshold));
    if (t::present(r::decoration)) put(r::decoration, uint8_t(c::spawn_decoration_none)); // not every build's requests have it
    put(r::node_self, reinterpret_cast<uint64_t>(request + r::node)); // a list link that points at itself: in no list
}
} // namespace

uint64_t spawn_actor(uint64_t type, const Transform& at, int32_t team) {
    const uint64_t framework = game_framework();
    if (!framework || !t::bound(t::fn::spawn_now) || !t::bound(t::fn::spawn_request_destroy)) return 0;
    alignas(8) unsigned char request[t::spawn_request::most];
    fill_request(request, type, at, team);
    const auto actor = reinterpret_cast<uint64_t>(t::fn::spawn_now(reinterpret_cast<void*>(framework), request));
    t::fn::spawn_request_destroy(request);
    return actor && live(actor) ? actor : 0;
}

bool start_made(uint64_t actor, uint64_t type) {
    const uint64_t root = read_or<uint64_t>(actor + t::game_actor::root);
    Transform at{};
    float q[4];
    if (!root || !t::bound(t::fn::actor_start_spawned) || !t::bound(t::fn::spawn_request_destroy) || !read_quaternion(root, q) || !readable(root + t::scene_component::location, 12)) return false;
    math::to_angles(q, at.angles);
    for (int i = 0; i < 3; ++i) {
        at.location[i] = read_or<float>(root + t::scene_component::location + 4 * i);
        if (at.location[i] - at.location[i] != 0 || at.angles[i] - at.angles[i] != 0) return false; // no place to start it at
    }
    alignas(8) unsigned char request[t::spawn_request::most];
    fill_request(request, type, at, -1);
    t::fn::actor_start_spawned(reinterpret_cast<void*>(actor), request);
    t::fn::spawn_request_destroy(request);
    return true;
}

TSUCHI_Result remove_actor(uint64_t actor, bool quiet, uint8_t cause, uint64_t killer) {
    if (!t::bound(t::fn::kill_record_lock) || !t::bound(t::fn::kill_actor) || !t::bound(t::fn::kill_record_destroy)) return TSUCHI_NOT_READY;
    if (read_or<uint8_t>(actor + t::game_actor::kill_queued)) return TSUCHI_BUSY;
    namespace k = t::kill_record;
    alignas(16) unsigned char record[k::most] = {};
    const auto put = [&](uint64_t offset, auto value) { memcpy(record + offset, &value, sizeof value); };
    put(k::head, reinterpret_cast<uint64_t>(record)); // an empty list of its own
    put(k::tail, reinterpret_cast<uint64_t>(record));
    t::fn::kill_record_lock(record + k::lock, 1, 0);
    put(k::magic, uint32_t(t::constant::kill_magic));
    put(k::quiet, uint8_t(quiet)), put(k::cause, cause), put(k::killer, killer);
    t::fn::kill_actor(record, reinterpret_cast<void*>(actor));
    t::fn::kill_record_destroy(record);
    return TSUCHI_OK;
}

} // namespace tsuchi::game
