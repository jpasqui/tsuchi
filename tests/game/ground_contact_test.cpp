// The production observer around a made-up engine contact pass. These checks
// exercise observation and invalidation; the stand-in does not simulate physics.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "actor_events.h"
#include "actors.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "ground_contact.h"
#include "hooks.h"
#include "motion.h"
#include "objects.h"
#include "registry.h"
#include "suspend.h"

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

namespace {
namespace t = tsuchi::game::table;
using tsuchi::game::PhysicsPose;
using Contacts = decltype(t::slot::physics_contacts)::Function;
struct Range { uint64_t address, size; };
struct Object { uint64_t address, handle; bool entity, primitive, live = true; };
struct Actor {
    alignas(8) std::array<unsigned char, 0x3000> bytes{};
    alignas(8) std::array<unsigned char, 0x200> root{}, component{};
    alignas(8) std::array<unsigned char, 0x120> instance{};
    alignas(8) std::array<unsigned char, 0x180> body{};
    PhysicsPose pose{{0, 0, 0, 1}, {10, 20, 30}};
    bool supporting = true, sleeping = false, simulating = true, launch = false, in_play = true;
    uint32_t suspended = 0, pose_reads = 0;
    uint64_t address() const { return reinterpret_cast<uint64_t>(bytes.data()); }
    uint64_t physical() const { return reinterpret_cast<uint64_t>(component.data()); }
    uint64_t rigid() const { return reinterpret_cast<uint64_t>(body.data()); }
};
std::vector<Range> ranges;
std::vector<Object> objects;
std::deque<Actor> actors;
alignas(8) unsigned char world_bytes[8]{}, other_world_bytes[8]{};
alignas(8) std::array<unsigned char, 0x710> primitive_table{};
alignas(8) std::array<unsigned char, 0x210> dynamic_table{};
alignas(8) std::array<unsigned char, 0x360> scene_table{};
alignas(8) uint64_t scene = 0;
uint64_t world = reinterpret_cast<uint64_t>(world_bytes), thread = 1;
bool fail_allocation = false;
unsigned allocations = 0, releases = 0, keep_calls = 0, wake_calls = 0, velocity_reads = 0;
void* observer_storage = nullptr;
size_t observer_storage_bytes = 0;
unsigned original_calls = 0;
unsigned locks = 0, unlocks = 0, lock_depth = 0, native_dispatch_depth = 0;
unsigned greatest_read_depth = 0;
uint64_t writer_thread = 0;
bool dispatch_under_read = false;
bool consume_contacts = true;
Actor* inside_actor = nullptr;
void (*inside_native)() = nullptr;
void* seen_first = nullptr;
void* seen_second = nullptr;
Contacts contact_hook = nullptr;
void (*step_callback)() = nullptr;
void (*left_callback)(uint64_t, uint64_t, tsuchi::ActorLeft) = nullptr;

void remember(void* memory, size_t size) { ranges.push_back({reinterpret_cast<uint64_t>(memory), size}); }
void add_object(void* memory, size_t size, uint64_t handle, bool entity = false, bool primitive = false) {
    remember(memory, size);
    objects.push_back({reinterpret_cast<uint64_t>(memory), handle, entity, primitive});
}
Object* object(uint64_t address) { for (auto& item : objects) if (item.address == address) return &item; return nullptr; }
Actor* actor_of(uint64_t address) {
    for (auto& actor : actors)
        if (address == actor.address() || address == actor.physical() || address == actor.rigid()) return &actor;
    return nullptr;
}
template<class T, size_t N> void put(std::array<unsigned char, N>& memory, uint64_t offset, T value) {
    CHECK(offset <= N && sizeof value <= N - offset);
    std::memcpy(memory.data() + offset, &value, sizeof value);
}
bool native_simulating(void* component, uint64_t bone) {
    CHECK(bone == 0);
    auto* actor = actor_of(reinterpret_cast<uint64_t>(component)); CHECK(actor);
    return actor->simulating;
}
void* native_instance(void* component, uint64_t bone, bool welded) {
    CHECK(bone == 0 && welded);
    auto* actor = actor_of(reinterpret_cast<uint64_t>(component)); CHECK(actor);
    return actor->instance.data();
}
PhysicsPose native_pose(const void* body) {
    auto* actor = actor_of(reinterpret_cast<uint64_t>(body)); CHECK(actor);
    CHECK(tsuchi::game::live(actor->address()));
    CHECK(lock_depth);
    ++actor->pose_reads;
    if (tsuchi::game::read_or<uint32_t>(actor->rigid() + t::physics_dynamic::state) & t::constant::physics_dynamic_buffered_pose) {
        const uint64_t buffer = tsuchi::game::read_or<uint64_t>(actor->rigid() + t::physics_dynamic::buffer);
        PhysicsPose buffered{};
        CHECK(tsuchi::game::read(buffer + t::physics_dynamic_buffer::pose, buffered));
        return buffered;
    }
    return actor->pose;
}
bool native_sleeping(const void* body) {
    auto* actor = actor_of(reinterpret_cast<uint64_t>(body)); CHECK(actor);
    CHECK(lock_depth);
    return actor->sleeping;
}
void* native_scene(const void* body) { CHECK(actor_of(reinterpret_cast<uint64_t>(body))); return &scene; }
void native_lock(void* requested, const char* file, uint32_t line) {
    CHECK(requested == &scene && !file && line == 0);
    CHECK(!writer_thread || writer_thread == thread);
    ++locks; ++lock_depth;
    if (lock_depth > greatest_read_depth) greatest_read_depth = lock_depth;
}
void native_unlock(void* requested) { CHECK(requested == &scene && lock_depth > 0); ++unlocks; --lock_depth; }
void native_contacts(void* first, void* second) {
    ++original_calls; seen_first = first; seen_second = second;
    for (auto& actor : actors) {
        if (!consume_contacts || !actor.in_play || !tsuchi::game::read_or<uint8_t>(actor.address() + t::game_actor::contact_step)) continue;
        // A completed engine pass, with a support decision supplied by the test.
        // Floor, wall, and ceiling can all reset the engine's contact counter.
        put(actor.bytes, t::game_actor::contact_step, uint8_t(0));
        put(actor.bytes, t::game_actor::support_contact, uint8_t(actor.supporting));
        put(actor.bytes, t::game_actor::air_steps, uint32_t(0));
    }
    if (inside_native) inside_native();
}
Actor& make_actor(uint64_t handle) {
    actors.emplace_back(); auto& actor = actors.back();
    add_object(actor.bytes.data(), actor.bytes.size(), handle, true);
    add_object(actor.root.data(), actor.root.size(), handle + 10000);
    add_object(actor.component.data(), actor.component.size(), handle + 20000, false, true);
    remember(actor.instance.data(), actor.instance.size()); remember(actor.body.data(), actor.body.size());
    put(actor.bytes, t::game_actor::root, reinterpret_cast<uint64_t>(actor.root.data()));
    put(actor.bytes, t::game_actor::physics, actor.physical());
    put(actor.bytes, t::game_actor::rigid_body, actor.rigid());
    put(actor.bytes, t::game_actor::body_awake, uint8_t(1));
    put(actor.bytes, t::game_actor::tick_requested, uint8_t(1));
    put(actor.bytes, t::game_actor::simulating, uint8_t(1));
    put(actor.bytes, t::game_actor::simulating_known, uint8_t(1));
    put(actor.bytes, t::game_actor::support_contact, uint8_t(1)); // construction may set this
    put(actor.component, 0, reinterpret_cast<uint64_t>(primitive_table.data()));
    put(actor.component, t::scene_component::owner, actor.address());
    put(actor.root, t::scene_component::owner, actor.address());
    put(actor.root, t::scene_component::rotation + 12, 1.0f);
    put(actor.instance, t::body_instance::body, actor.rigid());
    put(actor.body, 0, reinterpret_cast<uint64_t>(dynamic_table.data()));
    put(actor.body, t::physics_body::kind, uint8_t(t::constant::physics_dynamic_kind));
    return actor;
}
void expect_at(Actor& actor, int line, TSUCHI_Result result, bool expected = false) {
    bool out = !expected;
    const TSUCHI_Result returned = tsuchi::ground_contact(actor.address(), &out);
    if (returned != result || out != (result == TSUCHI_OK ? expected : !expected)) {
        std::fprintf(stderr, "line %d: ground_contact returned %u, expected %u; output %u\n", line,
                     unsigned(returned), unsigned(result), unsigned(out));
        std::exit(1);
    }
}
#define expect(actor, ...) expect_at(actor, __LINE__, __VA_ARGS__)
void run_contacts(void* first = nullptr, void* second = nullptr) {
    const uint64_t outer_writer = writer_thread;
    ++native_dispatch_depth;
    if (thread == 1) {
        if (dispatch_under_read) native_lock(&scene, nullptr, 0);
        else writer_thread = thread;
    }
    contact_hook(first, second);
    if (thread == 1 && dispatch_under_read) native_unlock(&scene);
    writer_thread = outer_writer;
    --native_dispatch_depth;
}
void pass(Actor& actor, bool supporting) {
    actor.supporting = supporting;
    put(actor.bytes, t::game_actor::contact_step, uint8_t(1));
    const unsigned before = original_calls;
    const unsigned allocation_mark = allocations;
    const unsigned lock_mark = locks;
    auto* first = reinterpret_cast<void*>(uintptr_t(0x1234));
    auto* second = reinterpret_cast<void*>(uintptr_t(0x5678));
    run_contacts(first, second);
    CHECK(original_calls == before + 1 && seen_first == first && seen_second == second);
    CHECK(allocations == allocation_mark);
    CHECK(locks == unlocks && lock_depth == 0);
    if (thread != 1) CHECK(locks == lock_mark);
}
void fresh(Actor& actor, bool supporting = true) {
    tsuchi::ground_contact_forget(actor.address());
    expect(actor, TSUCHI_NOT_READY);
    pass(actor, supporting);
    expect(actor, TSUCHI_OK, supporting);
}
} // namespace

namespace tsuchi {
void* allocate(size_t bytes) {
    ++allocations;
    if (fail_allocation) return nullptr;
    void* memory = std::malloc(bytes);
    if (memory) { observer_storage = memory; observer_storage_bytes = bytes; }
    return memory;
}
void release(void* block) {
    if (block) ++releases;
    if (block == observer_storage) { observer_storage = nullptr; observer_storage_bytes = 0; }
    std::free(block);
}
bool on_game_thread() { return thread == 1; }
bool mod_running(const TSUCHI_Mod*) { return true; }
uint64_t known_game_thread_token() { return thread == 1 ? 1 : 0; }
bool each_frame(void (*callback)()) { CHECK(!step_callback); step_callback = callback; return true; }
bool on_actor_left(void (*callback)(uint64_t, uint64_t, ActorLeft)) { CHECK(!left_callback); left_callback = callback; return true; }
uint32_t suspended_parts(uint64_t address) { auto* actor = actor_of(address); return actor ? actor->suspended : 0; }
bool motion_launch_pending(uint64_t address) { auto* actor = actor_of(address); return actor && actor->launch; }
bool actor_seen_in_play(uint64_t address) { auto* actor = actor_of(address); return actor && actor->in_play; }
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** original) {
    CHECK(entry == game::main_base() + t::slot::physics_contacts.table + t::slot::physics_contacts.offset);
    CHECK(stock == game::main_base() + t::slot::physics_contacts.stock);
    CHECK(flags & kHookNeedsStock);
    CHECK(!contact_hook);
    contact_hook = reinterpret_cast<Contacts>(function); *original = reinterpret_cast<void*>(native_contacts); *id = 1;
    return TSUCHI_OK;
}
} // namespace tsuchi

namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(dynamic_table.data()) - t::data::physics_dynamic_table; }
bool readable(uint64_t address, uint64_t size) {
    for (auto range : ranges)
        if (address >= range.address && address - range.address <= range.size && size <= range.size - (address - range.address)) return true;
    return false;
}
bool writable(uint64_t address, uint64_t size) { return readable(address, size); }
bool executable(uint64_t address) {
    return address == reinterpret_cast<uint64_t>(native_simulating) || address == reinterpret_cast<uint64_t>(native_instance) ||
           address == reinterpret_cast<uint64_t>(native_pose) || address == reinterpret_cast<uint64_t>(native_sleeping) ||
           address == reinterpret_cast<uint64_t>(native_scene) || address == reinterpret_cast<uint64_t>(native_lock) ||
           address == reinterpret_cast<uint64_t>(native_unlock);
}
bool live(uint64_t address) { const auto* item = object(address); return item && item->live; }
uint64_t to_handle(uint64_t address) { const auto* item = object(address); return item && item->live ? item->handle : 0; }
uint64_t from_handle(uint64_t handle) { for (auto item : objects) if (item.handle == handle && item.live) return item.address; return 0; }
bool is_game_actor(uint64_t address) { const auto* item = object(address); return item && item->live && item->entity; }
bool is_a(uint64_t address, ClassRef&) { const auto* item = object(address); return item && item->live && item->primitive; }
uint64_t physics_component(uint64_t address) { auto* actor = actor_of(address); return actor ? actor->physical() : 0; }
uint64_t loaded_world() { return world; }
bool paused_kinds(uint32_t& kinds) { kinds = 0; return true; }
bool keep_object(uint64_t, bool) { ++keep_calls; return false; }
void hold_awake(uint64_t, bool) { ++wake_calls; }
bool get_velocity(uint64_t, float[3]) { ++velocity_reads; return false; }
} // namespace tsuchi::game

int main() {
    remember(primitive_table.data(), primitive_table.size()); remember(dynamic_table.data(), dynamic_table.size());
    remember(scene_table.data(), scene_table.size()); remember(&scene, sizeof scene);
    scene = reinterpret_cast<uint64_t>(scene_table.data());
    add_object(world_bytes, sizeof world_bytes, 1); add_object(other_world_bytes, sizeof other_world_bytes, 2);
    put(primitive_table, t::primitive_table::is_simulating.offset, reinterpret_cast<uint64_t>(native_simulating));
    put(primitive_table, t::primitive_table::body_instance.offset, reinterpret_cast<uint64_t>(native_instance));
    put(dynamic_table, t::physics_body_table::global_pose.offset, reinterpret_cast<uint64_t>(native_pose));
    put(dynamic_table, t::physics_body_table::is_sleeping.offset, reinterpret_cast<uint64_t>(native_sleeping));
    put(dynamic_table, t::physics_body_table::get_scene.offset, reinterpret_cast<uint64_t>(native_scene));
    put(scene_table, t::physics_scene_table::lock_read.offset, reinterpret_cast<uint64_t>(native_lock));
    put(scene_table, t::physics_scene_table::unlock_read.offset, reinterpret_cast<uint64_t>(native_unlock));
    t::fn::physics_dynamic_pose = native_pose;
    t::fn::physics_dynamic_sleeping = native_sleeping;
    auto* self = reinterpret_cast<TSUCHI_Mod*>(uintptr_t(1));
    tsuchi::ground_contact_start(self);
    CHECK(contact_hook && step_callback && left_callback);

    auto& captain = make_actor(101);
    // Allocating a first watch may fail; construction support remains untrusted.
    fail_allocation = true;
    expect(captain, TSUCHI_NO_MEMORY);
    fail_allocation = false;
    expect(captain, TSUCHI_NOT_READY);
    run_contacts(); // no actor update was consumed
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    dispatch_under_read = true;
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    dispatch_under_read = false;
    CHECK(greatest_read_depth >= 2);
    const auto original_bytes = captain.bytes;
    const unsigned before_allocations = allocations;
    expect(captain, TSUCHI_OK, true); expect(captain, TSUCHI_OK, true);
    CHECK(captain.bytes == original_bytes && allocations == before_allocations);

    consume_contacts = false;
    pass(captain, true); expect(captain, TSUCHI_NOT_READY); // original did not consume TickPre's marker
    consume_contacts = true;
    run_contacts(); expect(captain, TSUCHI_OK, true);
    inside_actor = &captain;
    inside_native = [] { expect(*inside_actor, TSUCHI_NOT_READY); };
    pass(captain, true); // a query reentered by a game callback waits for completion
    inside_native = nullptr;
    expect(captain, TSUCHI_OK, true);
    inside_native = [] { inside_actor->pose.location[0] += 1; };
    pass(captain, true);
    inside_native = nullptr;
    expect(captain, TSUCHI_NOT_READY); // a callback warped the body during the pass
    pass(captain, true); expect(captain, TSUCHI_OK, true);

    // A reset contact counter alone does not imply support. Neither zero nor
    // nonzero speed replaces the decision supplied by the native contact pass.
    pass(captain, false); expect(captain, TSUCHI_OK, false);
    put(captain.bytes, t::game_actor::velocity + 8, 1200.0f);
    pass(captain, false); expect(captain, TSUCHI_OK, false);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(captain.bytes, t::game_actor::velocity + 8, 0.0f);
    put(captain.bytes, t::game_actor::contact_step, uint8_t(1));
    expect(captain, TSUCHI_NOT_READY); // TickPre has started a later update
    run_contacts(); expect(captain, TSUCHI_OK, true);
    step_callback(); expect(captain, TSUCHI_OK, true);
    step_callback(); expect(captain, TSUCHI_NOT_READY); // no completed pass for two steps
    pass(captain, true); expect(captain, TSUCHI_OK, true);

    captain.launch = true; expect(captain, TSUCHI_NOT_READY);
    captain.launch = false; expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    for (const uint32_t part : {tsuchi::kSuspendPhysics, tsuchi::kSuspendCollision}) {
        captain.suspended = part; expect(captain, TSUCHI_NOT_READY);
        captain.suspended = 0; expect(captain, TSUCHI_NOT_READY);
        pass(captain, true); expect(captain, TSUCHI_OK, true);
    }
    captain.suspended = tsuchi::kSuspendLogic | tsuchi::kSuspendMovement;
    expect(captain, TSUCHI_OK, true); // physics still owns a completed supporting contact
    captain.suspended = 0;

    captain.pose.location[0] += 1; expect(captain, TSUCHI_NOT_READY); // raw body moves while root cache stays still
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    captain.pose.rotation[2] = 0.5f; expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(captain.root, t::scene_component::location, 33.0f); expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    captain.pose.location[1] = NAN; expect(captain, TSUCHI_NOT_READY);
    captain.pose.location[1] = 20; pass(captain, true); expect(captain, TSUCHI_OK, true);

    // Qualifying the native getter includes the body's full prefix, an active
    // buffered pose's storage, and a readable scene with both lock methods.
    const unsigned before_bad_buffer = captain.pose_reads;
    put(captain.body, t::physics_dynamic::state, uint32_t(t::constant::physics_dynamic_buffered_pose));
    expect(captain, TSUCHI_NOT_READY);
    CHECK(captain.pose_reads == before_bad_buffer);
    alignas(8) std::array<unsigned char, 0xF0> buffered{};
    remember(buffered.data(), t::physics_dynamic_buffer::pose + sizeof(PhysicsPose) - 1);
    put(captain.body, t::physics_dynamic::buffer, reinterpret_cast<uint64_t>(buffered.data()));
    expect(captain, TSUCHI_NOT_READY);
    CHECK(captain.pose_reads == before_bad_buffer);
    ranges.back().size = buffered.size();
    put(buffered, t::physics_dynamic_buffer::pose, captain.pose);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(buffered, t::physics_dynamic_buffer::pose, PhysicsPose{{0, 0, 0, 1}, {50, 20, 30}});
    expect(captain, TSUCHI_NOT_READY);
    put(captain.body, t::physics_dynamic::state, uint32_t(0));
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    for (auto& range : ranges) if (range.address == captain.rigid()) {
        const uint64_t full_size = range.size;
        range.size = t::physics_dynamic::size - 1;
        expect(captain, TSUCHI_NOT_READY);
        range.size = full_size;
    }
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(captain.body, t::physics_body::kind, uint16_t(0)); expect(captain, TSUCHI_UNSUPPORTED);
    put(captain.body, t::physics_body::kind, uint16_t(t::constant::physics_dynamic_kind));
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    t::fn::physics_dynamic_pose = nullptr; expect(captain, TSUCHI_NOT_READY);
    t::fn::physics_dynamic_pose = native_pose;
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    t::fn::physics_dynamic_sleeping = nullptr; expect(captain, TSUCHI_NOT_READY);
    t::fn::physics_dynamic_sleeping = native_sleeping;
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(dynamic_table, t::physics_body_table::get_scene.offset, uint64_t(0)); expect(captain, TSUCHI_NOT_READY);
    put(dynamic_table, t::physics_body_table::get_scene.offset, reinterpret_cast<uint64_t>(native_scene));
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(scene_table, t::physics_scene_table::unlock_read.offset, uint64_t(0)); expect(captain, TSUCHI_NOT_READY);
    put(scene_table, t::physics_scene_table::unlock_read.offset, reinterpret_cast<uint64_t>(native_unlock));
    pass(captain, true); expect(captain, TSUCHI_OK, true);

    put(captain.component, t::scene_component::attach_parent, uint64_t(123)); expect(captain, TSUCHI_UNSUPPORTED);
    put(captain.component, t::scene_component::attach_parent, uint64_t(0)); expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    captain.sleeping = true; expect(captain, TSUCHI_NOT_READY);
    captain.sleeping = false; expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    captain.simulating = false; expect(captain, TSUCHI_NOT_READY);
    captain.simulating = true; expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    for (const uint32_t field : {t::game_actor::simulating_known, t::game_actor::tick_requested}) {
        put(captain.bytes, field, uint8_t(0)); expect(captain, TSUCHI_NOT_READY);
        put(captain.bytes, field, uint8_t(1)); pass(captain, true); expect(captain, TSUCHI_OK, true);
    }
    put(captain.bytes, t::game_actor::rest, uint32_t(1)); expect(captain, TSUCHI_NOT_READY);
    put(captain.bytes, t::game_actor::rest, uint32_t(0)); pass(captain, true); expect(captain, TSUCHI_OK, true);
    put(captain.bytes, t::game_actor::body_awake, uint8_t(0)); expect(captain, TSUCHI_NOT_READY);
    put(captain.bytes, t::game_actor::body_awake, uint8_t(1)); pass(captain, true); expect(captain, TSUCHI_OK, true);

    // A watched actor is neither retained nor awakened. An unqueried actor
    // receives the native pass but costs the observer no pose reads or storage.
    auto& unqueried = make_actor(102);
    const unsigned allocation_mark = allocations;
    pass(unqueried, true);
    CHECK(unqueried.pose_reads == 0 && allocations == allocation_mark);
    expect(unqueried, TSUCHI_NOT_READY);
    pass(unqueried, false); expect(unqueried, TSUCHI_OK, false);

    // A foreign-thread call must forward both native arguments once while
    // recording no pose or first sample for a watched actor.
    tsuchi::ground_contact_forget(captain.address());
    expect(captain, TSUCHI_NOT_READY);
    const unsigned reads_before_foreign = captain.pose_reads;
    thread = 2; pass(captain, false); thread = 1;
    CHECK(captain.pose_reads == reads_before_foreign);
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, false); expect(captain, TSUCHI_OK, false);
    fresh(captain, true);
    const unsigned previous_reads = captain.pose_reads;
    thread = 2; pass(captain, false); thread = 1;
    CHECK(captain.pose_reads == previous_reads);
    expect(captain, TSUCHI_NOT_READY); // a foreign pass cannot leave an older true answer visible
    pass(captain, false); expect(captain, TSUCHI_OK, false);
    fresh(captain, true);
    CHECK(observer_storage && observer_storage_bytes);
    const auto* cache_bytes = static_cast<const unsigned char*>(observer_storage);
    const std::vector<unsigned char> previous_cache(cache_bytes, cache_bytes + observer_storage_bytes);
    inside_native = [] { left_callback(inside_actor->address(), 101, tsuchi::ActorLeft::put_away); };
    thread = 2; pass(captain, false); thread = 1;
    inside_native = nullptr;
    CHECK(!std::memcmp(observer_storage, previous_cache.data(), previous_cache.size()));
    expect(captain, TSUCHI_NOT_READY); // the game-thread witness now invalidates the older sample
    pass(captain, false); expect(captain, TSUCHI_OK, false);

    // A replacement body cannot inherit its predecessor's sample.
    auto& replacement = make_actor(103);
    fresh(captain, true);
    put(captain.bytes, t::game_actor::rigid_body, replacement.rigid());
    put(captain.instance, t::body_instance::body, replacement.rigid());
    expect(captain, TSUCHI_NOT_READY);
    put(captain.bytes, t::game_actor::rigid_body, captain.rigid());
    put(captain.instance, t::body_instance::body, captain.rigid());
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    tsuchi::ground_contact_forget(captain.address()); expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    left_callback(captain.address(), 101, tsuchi::ActorLeft::put_away);
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    inside_native = [] {
        left_callback(inside_actor->address(), tsuchi::game::to_handle(inside_actor->address()), tsuchi::ActorLeft::destroyed);
        object(inside_actor->address())->live = false;
    };
    pass(captain, true);
    inside_native = nullptr;
    expect(captain, TSUCHI_NOT_FOUND);
    object(captain.address())->live = true;
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    object(captain.address())->handle = 201;
    expect(captain, TSUCHI_NOT_READY); // the same address now names a new actor life
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    object(captain.address())->live = false;
    expect(captain, TSUCHI_NOT_FOUND);
    step_callback(); object(captain.address())->live = true;
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);

    world = reinterpret_cast<uint64_t>(other_world_bytes);
    expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    world = 0; expect(captain, TSUCHI_NOT_READY);
    world = reinterpret_cast<uint64_t>(other_world_bytes); expect(captain, TSUCHI_NOT_READY);
    pass(captain, true); expect(captain, TSUCHI_OK, true);
    CHECK(keep_calls == 0 && wake_calls == 0 && velocity_reads == 0);
    CHECK(locks > 0 && locks == unlocks && lock_depth == 0 && native_dispatch_depth == 0);
    std::puts("ground-contact observer stand-in checks passed");
}
