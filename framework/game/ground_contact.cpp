#include "ground_contact.h"

#include <string.h>

#include "actor_events.h"
#include "actors.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "motion.h"
#include "objects.h"
#include "registry.h"
#include "suspend.h"
#include "tsuchi/game.h"

namespace tsuchi {
namespace {

namespace g = game;
namespace t = g::table;

g::ClassRef primitive_class{t::name::primitive_class};

struct Snapshot {
    uint64_t body;
    g::PhysicsPose physical;
    float rotation[4], location[3];
};

struct Contact {
    uint64_t actor, world, world_handle, component, component_handle, root, root_handle;
    uint64_t sampled, capture;
    Snapshot pose;
    bool known, supported;
};

Contact* contacts = nullptr;
uint32_t count = 0, capacity = 0;
uint64_t steps = 0, sequence = 0;
uint64_t foreign_revision = 0, seen_foreign_revision = 0;
uint32_t foreign_active = 0;
TSUCHI_Mod* observer_owner = nullptr;
decltype(t::slot::physics_contacts)::Function next_contacts = nullptr;
bool started = false, ready = false, observing = false;

struct SceneRead {
    uint64_t scene = 0;
    decltype(t::physics_scene_table::unlock_read)::Function unlock = nullptr;
    bool ready = false;

    explicit SceneRead(uint64_t body) {
        const auto get_scene = g::method(body, t::physics_body_table::get_scene);
        if (!get_scene) return;
        scene = reinterpret_cast<uint64_t>(get_scene(reinterpret_cast<const void*>(body)));
        if (!scene) return;
        const auto lock = g::method(scene, t::physics_scene_table::lock_read);
        unlock = g::method(scene, t::physics_scene_table::unlock_read);
        if (!lock || !unlock) { unlock = nullptr; return; }
        lock(reinterpret_cast<void*>(scene), nullptr, 0);
        ready = true;
    }
    ~SceneRead() { if (unlock) unlock(reinterpret_cast<void*>(scene)); }
};

bool finite_pose(const Snapshot& pose) {
    for (float value : pose.physical.rotation) if (!finite(value)) return false;
    for (float value : pose.physical.location) if (!finite(value)) return false;
    for (float value : pose.rotation) if (!finite(value)) return false;
    for (float value : pose.location) if (!finite(value)) return false;
    return true;
}

bool same_pose(const Snapshot& a, const Snapshot& b) {
    return a.body == b.body && !memcmp(&a.physical, &b.physical, sizeof a.physical) &&
           !memcmp(a.rotation, b.rotation, sizeof a.rotation) && !memcmp(a.location, b.location, sizeof a.location);
}

// The object's class is checked when a query admits it. During the native hook,
// resolving those same handles requires no allocations or class discovery.
bool identities(const Contact& contact, uint64_t world, uint64_t& actor) {
    actor = g::from_handle(contact.actor);
    return actor && world && world == contact.world && g::from_handle(contact.world_handle) == world &&
           g::from_handle(contact.component_handle) == contact.component && g::from_handle(contact.root_handle) == contact.root &&
           g::read_or<uint64_t>(actor + t::game_actor::physics) == contact.component &&
           g::read_or<uint64_t>(actor + t::game_actor::root) == contact.root;
}

// Only independent, awake dynamic bodies have a current contact sample. The
// qualified PhysX getter reads the body's pose, so a body-only warp is detected
// even when the actor's cached transform has not caught up.
TSUCHI_Result snapshot(uint64_t actor, uint64_t component, uint64_t root, Snapshot& out) {
    namespace a = t::game_actor;
    namespace c = t::scene_component;
    uint64_t component_owner = 0, root_owner = 0, parent = 0;
    if (!g::read(component + c::owner, component_owner) || !g::read(root + c::owner, root_owner) ||
        !g::read(component + c::attach_parent, parent)) return TSUCHI_NOT_READY;
    if (component_owner != actor || root_owner != actor || parent) return TSUCHI_UNSUPPORTED;
    if (!g::read(root + c::attach_parent, parent)) return TSUCHI_NOT_READY;
    if (parent) return TSUCHI_UNSUPPORTED;

    uint8_t known = 0, simulating = 0, awake = 0, ticking = 0;
    uint32_t rest = 0;
    if (!g::read(actor + a::simulating_known, known) || !g::read(actor + a::simulating, simulating) ||
        !g::read(actor + a::body_awake, awake) || !g::read(actor + a::tick_requested, ticking) ||
        !g::read(actor + a::rest, rest)) return TSUCHI_NOT_READY;
    if (!known || !simulating || !awake || !ticking || rest) return TSUCHI_NOT_READY;
    if (g::read_or<uint8_t>(actor + a::kill_queued) || g::read_or<uint8_t>(actor + a::terminated)) return TSUCHI_NOT_FOUND;

    const auto simulates = g::method(component, t::primitive_table::is_simulating);
    const auto body_instance = g::method(component, t::primitive_table::body_instance);
    if (!simulates || !body_instance) return TSUCHI_NOT_READY;
    if (!simulates(reinterpret_cast<void*>(component), 0)) return TSUCHI_NOT_READY;
    const uint64_t instance = reinterpret_cast<uint64_t>(body_instance(reinterpret_cast<void*>(component), 0, true));
    uint64_t body = 0, cached = 0, functions = 0;
    if (!instance || !g::read(instance + t::body_instance::body, body) || !g::read(actor + a::rigid_body, cached)) return TSUCHI_NOT_READY;
    if (!body) return TSUCHI_UNSUPPORTED;
    if (body != cached) return TSUCHI_NOT_READY;
    uint16_t kind = 0;
    if (!g::readable(body, t::physics_dynamic::size) || !g::read(body, functions) ||
        !g::read(body + t::physics_body::kind, kind)) return TSUCHI_NOT_READY;
    if (kind != t::constant::physics_dynamic_kind || functions != g::main_base() + t::data::physics_dynamic_table) return TSUCHI_UNSUPPORTED;
    const auto pose = g::method(body, t::physics_body_table::global_pose);
    const auto sleeping = g::method(body, t::physics_body_table::is_sleeping);
    if (!pose || pose != t::fn::physics_dynamic_pose || !sleeping || sleeping != t::fn::physics_dynamic_sleeping) return TSUCHI_NOT_READY;
    // The collision handler holds its own scene's lock. Other tracked bodies
    // may belong to another scene, so each read takes that body's scene lock.
    // Native PhysX permits nested reads, including reads under our write lock.
    SceneRead lock(body);
    if (!lock.ready) return TSUCHI_NOT_READY;
    uint32_t state = 0;
    if (!g::read(body + t::physics_dynamic::state, state)) return TSUCHI_NOT_READY;
    if (state & t::constant::physics_dynamic_buffered_pose) {
        const uint64_t buffer = g::read_or<uint64_t>(body + t::physics_dynamic::buffer);
        if (!buffer || !g::readable(buffer + t::physics_dynamic_buffer::pose, sizeof(g::PhysicsPose))) return TSUCHI_NOT_READY;
    }
    if (sleeping(reinterpret_cast<const void*>(body))) return TSUCHI_NOT_READY;
    out.body = body;
    out.physical = pose(reinterpret_cast<const void*>(body));
    if (!g::read(root + c::rotation, out.rotation) || !g::read(root + c::location, out.location) || !finite_pose(out)) return TSUCHI_NOT_READY;
    return TSUCHI_OK;
}

void invalidate(Contact& contact) { contact.known = false; contact.capture = 0; }

// A foreign-thread pass still calls the game, but cannot inspect the game-thread
// cache. Its atomic witness prevents an older sample surviving that missed pass.
bool missed_pass() {
    const uint64_t revision = __atomic_load_n(&foreign_revision, __ATOMIC_ACQUIRE);
    const bool active = __atomic_load_n(&foreign_active, __ATOMIC_ACQUIRE) != 0;
    if (active || revision != seen_foreign_revision) {
        for (uint32_t i = 0; i < count; ++i) invalidate(contacts[i]);
        seen_foreign_revision = revision;
    }
    return active;
}

void left(uint64_t, uint64_t handle, ActorLeft) {
    if (!on_game_thread() || !known_game_thread_token()) {
        __atomic_add_fetch(&foreign_revision, 1, __ATOMIC_RELEASE);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) if (contacts[i].actor == handle) contacts[i] = {};
}

void step() {
    ++steps;
    const uint64_t world = g::loaded_world();
    for (uint32_t i = 0; i < count; ++i) {
        Contact& contact = contacts[i];
        if (!contact.actor) continue;
        uint64_t actor = 0;
        if (!identities(contact, world, actor)) contact = {};
        else if (contact.known && steps - contact.sampled > 1) invalidate(contact);
    }
}

void collided(void* handler, void* records) {
    // Collision delivery may happen on another thread. The observer's cache,
    // handles, and allocator belong to the game's thread.
    if (!on_game_thread() || !known_game_thread_token()) {
        __atomic_add_fetch(&foreign_active, 1, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&foreign_revision, 1, __ATOMIC_RELEASE);
        next_contacts(handler, records);
        __atomic_sub_fetch(&foreign_active, 1, __ATOMIC_RELEASE);
        return;
    }
    if (missed_pass()) { next_contacts(handler, records); return; }
    if (observing) {
        for (uint32_t i = 0; i < count; ++i) invalidate(contacts[i]);
        next_contacts(handler, records);
        return;
    }
    observing = true;
    const uint64_t world = g::loaded_world(), token = ++sequence;
    for (uint32_t i = 0; i < count; ++i) {
        Contact& contact = contacts[i];
        uint64_t actor = 0;
        if (!contact.actor || !identities(contact, world, actor)) { invalidate(contact); continue; }
        uint8_t pending = 0;
        Snapshot before{};
        const bool sampled_here = contact.known && contact.sampled == steps;
        if (!g::read(actor + t::game_actor::contact_step, pending) ||
            (!pending && !sampled_here) || snapshot(actor, contact.component, contact.root, before) != TSUCHI_OK ||
            (!pending && !same_pose(contact.pose, before))) { invalidate(contact); continue; }
        contact.pose = before;
        contact.known = false; // queries inside the original call must wait for its completion
        contact.capture = token;
    }

    next_contacts(handler, records);

    // A concurrently delivered pass invalidates every candidate as well.
    missed_pass();

    // No pointer into the table is kept across the original call. An actor can
    // leave play in a native listener, which clears its stable slot here.
    for (uint32_t i = 0; i < count; ++i) {
        Contact& contact = contacts[i];
        if (!contact.actor || contact.capture != token) continue;
        contact.capture = 0;
        uint64_t actor = 0;
        uint8_t pending = 0, supported = 0;
        Snapshot after{};
        if (!identities(contact, g::loaded_world(), actor) || !g::read(actor + t::game_actor::contact_step, pending) || pending ||
            !g::read(actor + t::game_actor::support_contact, supported) ||
            snapshot(actor, contact.component, contact.root, after) != TSUCHI_OK || !same_pose(contact.pose, after)) continue;
        contact.supported = supported != 0;
        contact.sampled = steps;
        contact.known = true;
    }
    missed_pass();
    observing = false;
}

Contact* tracked(uint64_t handle) {
    for (uint32_t i = 0; i < count; ++i) if (contacts[i].actor == handle) return &contacts[i];
    for (uint32_t i = 0; i < count; ++i) if (!contacts[i].actor) return &contacts[i];
    if (count == capacity) {
        if (capacity >= t::constant::max_objects) return nullptr;
        uint32_t next = capacity ? capacity * 2 : 8;
        if (next > uint32_t(t::constant::max_objects)) next = uint32_t(t::constant::max_objects);
        auto* grown = static_cast<Contact*>(allocate(sizeof(Contact) * next));
        if (!grown) return nullptr;
        if (contacts) memcpy(grown, contacts, sizeof(Contact) * count);
        release(contacts);
        contacts = grown;
        capacity = next;
    }
    contacts[count] = {};
    return &contacts[count++];
}

} // namespace

void ground_contact_start(TSUCHI_Mod* self) {
    if (started) return;
    started = true;
    observer_owner = self;
    if (!each_frame(step) || !on_actor_left(left)) return;
    uint64_t id = 0;
    ready = hook_slot(self, t::slot::physics_contacts, collided, &next_contacts, kHookNeedsStock, &id) == TSUCHI_OK;
}

TSUCHI_Result ground_contact(uint64_t actor, bool* out) {
    if (!out) return TSUCHI_INVALID;
    if (!ready || !observer_owner || !mod_running(observer_owner) || observing || !on_game_thread() || !known_game_thread_token()) return TSUCHI_NOT_READY;
    if (missed_pass()) return TSUCHI_NOT_READY;
    const uint64_t world = g::loaded_world();
    if (!world) {
        for (uint32_t i = 0; i < count; ++i) invalidate(contacts[i]);
        return TSUCHI_NOT_READY;
    }
    const uint64_t handle = g::to_handle(actor);
    if (!handle) return TSUCHI_NOT_FOUND;
    uint32_t paused = 0;
    if (!g::paused_kinds(paused) || (paused & TSUCHI_PAUSE_ACTORS) ||
        (suspended_parts(actor) & (kSuspendPhysics | kSuspendCollision)) || motion_launch_pending(actor)) {
        ground_contact_forget(actor);
        return TSUCHI_NOT_READY;
    }
    const uint64_t component = g::physics_component(actor), root = g::read_or<uint64_t>(actor + t::game_actor::root);
    if (!component || !root || !g::live(root) || !g::is_a(component, primitive_class)) {
        ground_contact_forget(actor);
        return TSUCHI_UNSUPPORTED;
    }
    Snapshot current{};
    const TSUCHI_Result result = snapshot(actor, component, root, current);
    if (result != TSUCHI_OK) { ground_contact_forget(actor); return result; }
    Contact* contact = tracked(handle);
    if (!contact) return TSUCHI_NO_MEMORY;
    if (contact->actor != handle || contact->world != world || contact->component != component || contact->root != root ||
        g::from_handle(contact->world_handle) != world || g::from_handle(contact->component_handle) != component ||
        g::from_handle(contact->root_handle) != root) {
        const uint64_t world_handle = g::to_handle(world), component_handle = g::to_handle(component), root_handle = g::to_handle(root);
        if (!world_handle || !component_handle || !root_handle) return TSUCHI_NOT_READY;
        *contact = {};
        contact->actor = handle;
        contact->world = world;
        contact->world_handle = world_handle;
        contact->component = component;
        contact->component_handle = component_handle;
        contact->root = root;
        contact->root_handle = root_handle;
        return TSUCHI_NOT_READY;
    }
    uint64_t same_actor = 0;
    uint8_t pending = 0;
    if (!identities(*contact, world, same_actor) || !g::read(actor + t::game_actor::contact_step, pending) || pending ||
        !contact->known || steps - contact->sampled > 1 || !same_pose(contact->pose, current)) {
        invalidate(*contact);
        return TSUCHI_NOT_READY;
    }
    if (missed_pass() || !contact->known) return TSUCHI_NOT_READY;
    *out = contact->supported;
    return TSUCHI_OK;
}

void ground_contact_forget(uint64_t actor) {
    for (uint32_t i = 0; i < count; ++i)
        if (contacts[i].actor && g::from_handle(contacts[i].actor) == actor) invalidate(contacts[i]);
}

} // namespace tsuchi
