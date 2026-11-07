// Native-call model for the real instance-cull hold backend. The synthetic
// object table, class hierarchy and ordinary teardown admission are exercised;
// this does not qualify a shipping foliage vtable or rendered fade behavior.
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fake_game.h"
#include "instance_cull.h"
#include "holds.h"
#include "objects.h"
#include "ordinary_lifecycle.cpp"

using namespace tsuchi;
namespace t = tsuchi::game::table;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)

struct Pair { int32_t start, end; };
bool operator==(Pair a, Pair b) { return a.start == b.start && a.end == b.end; }
struct NativeCall { uint64_t part; Pair distances; };
std::vector<NativeCall> native_calls;
std::vector<void (*)()> frame_steps;
std::map<uint64_t, uint64_t> hidden_range_ends;
uint32_t refused_allocation_attempts = 0;
uint64_t meta_class = 0, core_package = 0, engine_package = 0;
uint64_t actor_type = 0, static_mesh_type = 0;
uint64_t instance_type = 0, hierarchical_type = 0, foliage_type = 0;
uint64_t actor_vtable = 0;
std::unordered_set<uint64_t> gameplay_owners;
void (*after_setter)() = nullptr;
TSUCHI_Mod* callback_mod = nullptr;
TSUCHI_Mod* competing_mod = nullptr;
uint64_t callback_part = 0;
uint64_t callback_owner = 0;
unsigned callback_hazard = 0;
Pair callback_foreign{};
bool check_native_teardown = false;
TSUCHI_Mod* teardown_mod = nullptr;
uint64_t teardown_part = 0;
Pair teardown_distances{};
uint32_t end_calls = 0, destroyed_calls = 0;

Pair read_pair(uint64_t part) {
    // Test-only access to allocated backing bytes, including when framework
    // readability is deliberately denied. This is not the backend's read path.
    return {fake::get<int32_t>(part + t::instanced_static_mesh_component::start_cull_distance),
            fake::get<int32_t>(part + t::instanced_static_mesh_component::end_cull_distance)};
}
void write_pair(uint64_t part, Pair value) {
    fake::put<int32_t>(part + t::instanced_static_mesh_component::start_cull_distance, value.start);
    fake::put<int32_t>(part + t::instanced_static_mesh_component::end_cull_distance, value.end);
}
void fields_readable(uint64_t part, bool readable) {
    for (auto& range : fake::ranges()) {
        if (range.start != part) continue;
        if (readable) {
            const auto found = hidden_range_ends.find(part);
            CHECK(found != hidden_range_ends.end());
            if (found != hidden_range_ends.end()) {
                range.end = found->second;
                hidden_range_ends.erase(found);
            }
        } else {
            const auto first = t::instanced_static_mesh_component::start_cull_distance <
                               t::instanced_static_mesh_component::end_cull_distance ?
                               t::instanced_static_mesh_component::start_cull_distance :
                               t::instanced_static_mesh_component::end_cull_distance;
            CHECK(hidden_range_ends.emplace(part, range.end).second);
            range.end = part + first;
        }
        CHECK(game::live(part));
        CHECK(game::readable(part + t::actor_component::lifecycle_flags, sizeof(uint32_t)));
        CHECK(game::readable(part + t::instanced_static_mesh_component::start_cull_distance,
                             sizeof(int32_t)) == readable);
        CHECK(game::readable(part + t::instanced_static_mesh_component::end_cull_distance,
                             sizeof(int32_t)) == readable);
        return;
    }
    CHECK(false); // the fixture must hide fields, never the whole object
}
void hide_fields_callback() {
    fields_readable(callback_part, false);
    const uint64_t owner = game::part_owner(callback_part);
    CHECK(ordinary_lifecycle_restore_part(game::to_handle(callback_part),
                                          game::to_handle(owner)) == callback_part);
}
void* refuse_allocation(size_t) { ++refused_allocation_attempts; return nullptr; }
void stop_without_allocation(TSUCHI_Mod* mod) {
    const uint32_t before = refused_allocation_attempts;
    set_allocator({refuse_allocation, std::free});
    mod_stop(mod);
    set_allocator({std::malloc, std::free});
    CHECK(refused_allocation_attempts == before);
}
void release_without_allocation(TSUCHI_Mod* mod, uint64_t part) {
    const uint32_t before = refused_allocation_attempts;
    set_allocator({refuse_allocation, std::free});
    CHECK(release_instance_cull_scale(mod, part) == TSUCHI_OK);
    set_allocator({std::malloc, std::free});
    CHECK(refused_allocation_attempts == before);
}
void hide_and_stop_callback() {
    hide_fields_callback();
    stop_without_allocation(callback_mod);
}
void setter(void* raw, int32_t start, int32_t end) {
    const auto part = reinterpret_cast<uint64_t>(raw);
    CHECK(game::live(part));
    native_calls.push_back({part, {start, end}});
    write_pair(part, {start, end});
    // Model SetCullDistances storing both scalars before its native render-state
    // invalidation can synchronously enter framework/mod code.
    if (after_setter) {
        auto callback = after_setter;
        after_setter = nullptr;
        callback();
    }
}
void advance_frame() {
    const auto steps = frame_steps;
    for (auto step : steps) step();
}
void stop_callback() { mod_stop(callback_mod); }
void release_callback() {
    CHECK(release_instance_cull_scale(callback_mod, callback_part) == TSUCHI_OK);
}
void foreign_callback() { write_pair(callback_part, callback_foreign); }
void foreign_stop_callback() { foreign_callback(); mod_stop(callback_mod); }
void competing_callback() {
    CHECK(hold_instance_cull_scale(competing_mod, callback_part, 3.0f) == TSUCHI_BUSY);
}

uint64_t make_class(const char16_t* name, uint64_t super = 0, uint64_t outer = 0) {
    const uint64_t type = fake::object(name, meta_class, 0x200);
    fake::set_super(type, super);
    fake::put<uint64_t>(type + t::object::outer, outer ? outer : engine_package);
    return type;
}
uint64_t make_actor(uint64_t table = 0) {
    const uint64_t owner = fake::object(u"CullOwner", actor_type, 0x200);
    fake::put<uint64_t>(owner, table ? table : actor_vtable);
    fake::put<uint8_t>(owner + t::engine_actor::play_state,
                       uint8_t(t::constant::actor_play_state_begun));
    return owner;
}
uint64_t make_part(uint64_t owner, uint64_t type, Pair distances) {
    const size_t bytes = t::instanced_static_mesh_component::end_cull_distance + sizeof(int32_t);
    const uint64_t part = fake::object(u"CullPart", type, bytes);
    fake::put<uint64_t>(part + t::object::outer, owner);
    write_pair(part, distances);
    // The adapter only reads the fields. A native setter can mutate engine
    // memory even when the framework's generic writable check denies it.
    fake::protect(part, false);
    return part;
}
struct Fixture { TSUCHI_Mod* mod; uint64_t owner, part; };
Fixture fixture(const char* id, Pair pair = {100, 300}, uint64_t type = 0) {
    const uint64_t owner = make_actor();
    return {mod_create(id), owner, make_part(owner, type ? type : instance_type, pair)};
}
Fixture gameplay_fixture(const char* id) {
    auto result = fixture(id);
    gameplay_owners.insert(result.owner);
    return result;
}
void invalidate_gameplay_target(uint64_t part, uint64_t owner, unsigned hazard) {
    if (hazard == 0) {
        const uint64_t replacement = make_actor();
        gameplay_owners.insert(replacement);
        fake::put<uint64_t>(part + t::object::outer, replacement);
    } else if (hazard == 1) {
        fake::put<uint32_t>(part + t::actor_component::lifecycle_flags,
                           uint32_t(t::constant::component_destroying_mask));
    } else if (hazard == 2) {
        const uint64_t old = game::to_handle(owner);
        game::end_life(owner);
        CHECK(!game::from_handle(old) && game::to_handle(owner) != old);
    } else {
        fake::put<uint8_t>(owner + t::engine_actor::play_state,
            uint8_t(t::constant::actor_play_state_begun | t::constant::actor_destroying_mask));
    }
}
void invalidate_target_callback() {
    invalidate_gameplay_target(callback_part, callback_owner, callback_hazard);
}
void stopped(Fixture f) {
    mod_stop(f.mod);
    CHECK(owned_count(f.mod) == 0);
}

struct Hook { uint64_t id, base, wrapper; };
std::map<uint64_t, Hook> hooks;
uint64_t next_hook = 1;
void stock_end(uint64_t, uint8_t) {
    ++end_calls;
    if (check_native_teardown) {
        CHECK(owned_count(teardown_mod) == 0);
        CHECK(read_pair(teardown_part) == teardown_distances);
    }
}
void stock_unregister(uint64_t, bool) {}
void stock_destroyed(uint64_t) {
    ++destroyed_calls;
    if (check_native_teardown) {
        CHECK(owned_count(teardown_mod) == 0);
        CHECK(read_pair(teardown_part) == teardown_distances);
    }
}
void stock_remove(uint64_t, uint64_t, uint32_t) {}
uint64_t make_vtable(bool supported) {
    const uint64_t table = fake::memory(0x80);
    fake::put<uint64_t>(table + t::ordinary_actor_vtable::end_play.offset,
                       game::main_base() + t::constant::ordinary_end_play);
    fake::put<uint64_t>(table + t::ordinary_actor_vtable::unregister.offset,
                       game::main_base() + t::constant::ordinary_unregister);
    fake::put<uint64_t>(table + t::ordinary_actor_vtable::destroyed.offset,
                       supported ? game::main_base() + t::constant::ordinary_destroyed : 0xDEAD);
    return table;
}
}

namespace tsuchi {
bool each_frame(void (*step)()) { frame_steps.push_back(step); return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
HookInvocation hook_invocation(uint64_t) { return {0, 0, 1}; }
bool hook_entry_owned(uint64_t entry) {
    const auto found = hooks.find(entry);
    return found != hooks.end() && game::read_or<uint64_t>(entry) == found->second.wrapper;
}
uint64_t hook_entry_base(uint64_t entry) {
    const auto found = hooks.find(entry);
    return hook_entry_owned(entry) ? found->second.base : game::read_or<uint64_t>(entry);
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* wrapper,
                        uint32_t, uint64_t* id, void** next) {
    CHECK(game::read_or<uint64_t>(entry) == stock);
    if (stock == game::main_base() + t::constant::ordinary_end_play)
        *next = reinterpret_cast<void*>(stock_end);
    else if (stock == game::main_base() + t::constant::ordinary_unregister)
        *next = reinterpret_cast<void*>(stock_unregister);
    else {
        CHECK(stock == game::main_base() + t::constant::ordinary_destroyed);
        *next = reinterpret_cast<void*>(stock_destroyed);
    }
    *id = next_hook++;
    hooks[entry] = {*id, stock, reinterpret_cast<uint64_t>(wrapper)};
    fake::put<uint64_t>(entry, reinterpret_cast<uint64_t>(wrapper));
    return TSUCHI_OK;
}
TSUCHI_Result hook_remove(TSUCHI_Mod*, uint64_t id) {
    for (auto at = hooks.begin(); at != hooks.end(); ++at) {
        if (at->second.id != id) continue;
        fake::put<uint64_t>(at->first, at->second.base);
        hooks.erase(at);
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t, void*, uint64_t* id, void** next) {
    *id = next_hook++;
    *next = reinterpret_cast<void*>(stock_remove);
    return TSUCHI_OK;
}
}
namespace tsuchi::game {
bool is_game_actor(uint64_t actor) { return gameplay_owners.count(actor) != 0; }
uint64_t part_owner(uint64_t part) { return live(part) ? outer_of(part) : 0; }
}

int main() {
    fake::start();
    // Native packages, metaclasses and inheritance are modeled as real objects.
    // Same-short-name classes intentionally precede the actual native anchor.
    meta_class = fake::object(u"Class", 0, 0x200);
    const uint64_t package_class = fake::object(u"Package", meta_class, 0x200);
    core_package = fake::object(u"/Script/CoreUObject", package_class, 0x200);
    fake::put<uint64_t>(meta_class + t::object::outer, core_package);
    fake::put<uint64_t>(package_class + t::object::outer, core_package);
    engine_package = fake::object(u"/Script/Engine", package_class, 0x200);
    const uint64_t object_type = make_class(u"Object", 0, core_package);
    fake::set_super(package_class, object_type);
    actor_type = make_class(u"Actor", object_type);
    const uint64_t component = make_class(u"ActorComponent", object_type);
    const uint64_t scene = make_class(u"SceneComponent", component);
    const uint64_t primitive = make_class(u"PrimitiveComponent", scene);
    static_mesh_type = make_class(u"StaticMeshComponent", primitive);
    const uint64_t blueprint_meta = make_class(u"BlueprintGeneratedClass", meta_class);
    const uint64_t blueprint_package = fake::object(u"/Game/CullFixture", package_class, 0x200);
    const uint64_t blueprint_impostor = fake::object(u"InstancedStaticMeshComponent", blueprint_meta, 0x200);
    fake::set_super(blueprint_impostor, static_mesh_type);
    fake::put<uint64_t>(blueprint_impostor + t::object::outer, blueprint_package);
    const uint64_t wrong_package = fake::object(u"/Script/Unrelated", package_class, 0x200);
    const uint64_t native_impostor = make_class(u"InstancedStaticMeshComponent", static_mesh_type, wrong_package);
    instance_type = make_class(u"InstancedStaticMeshComponent", static_mesh_type);
    hierarchical_type = make_class(u"HierarchicalInstancedStaticMeshComponent", instance_type);
    foliage_type = make_class(u"FoliageInstancedStaticMeshComponent", hierarchical_type);
    actor_vtable = make_vtable(true);
    t::fn::set_instance_cull_distances = setter;
    fake::code().push_back(reinterpret_cast<uint64_t>(setter));
    instance_cull_start();

    auto pending = fixture("cull.pending");
    CHECK(hold_instance_cull_scale(pending.mod, pending.part, 2) == TSUCHI_NOT_READY);
    CHECK(native_calls.empty() && owned_count(pending.mod) == 0);
    ordinary_lifecycle_start(mod_create("cull.framework"));
    stopped(pending);
    for (uint64_t impostor : {blueprint_impostor, native_impostor}) {
        auto f = fixture("cull.impostor", {100, 300}, impostor);
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_UNSUPPORTED);
        CHECK(native_calls.empty() && owned_count(f.mod) == 0);
        CHECK(read_pair(f.part) == (Pair{100, 300})); stopped(f);
    }

    auto normal = fixture("cull.normal");
    auto* other = mod_create("cull.other");
    size_t before = native_calls.size();
    CHECK(hold_instance_cull_scale(normal.mod, normal.part, 2) == TSUCHI_OK);
    CHECK(read_pair(normal.part) == (Pair{200, 600}) && native_calls.size() == before + 1);
    CHECK(owned_count(normal.mod) == 1);
    CHECK(hold_instance_cull_scale(other, normal.part, 3) == TSUCHI_BUSY);
    CHECK(release_instance_cull_scale(other, normal.part) == TSUCHI_NOT_FOUND);
    for (int i = 0; i != 3; ++i) advance_frame();
    CHECK(hold_instance_cull_scale(normal.mod, normal.part, 2) == TSUCHI_OK);
    CHECK(native_calls.size() == before + 1); // never compound or dirty redundantly
    CHECK(hold_instance_cull_scale(normal.mod, normal.part, 1) == TSUCHI_OK);
    CHECK(read_pair(normal.part) == (Pair{100, 300}) && native_calls.size() == before + 2);
    CHECK(release_instance_cull_scale(normal.mod, normal.part) == TSUCHI_OK);
    CHECK(native_calls.size() == before + 2 && owned_count(normal.mod) == 0);
    CHECK(release_instance_cull_scale(normal.mod, normal.part) == TSUCHI_NOT_FOUND);
    stopped(normal); mod_stop(other);

    auto rounded = fixture("cull.rounded", {3, 9});
    CHECK(hold_instance_cull_scale(rounded.mod, rounded.part, 0.5f) == TSUCHI_OK);
    CHECK(read_pair(rounded.part) == (Pair{2, 5}));
    stopped(rounded); CHECK(read_pair(rounded.part) == (Pair{3, 9}));
    for (Pair sentinel : {Pair{0, 0}, Pair{-1, 500}, Pair{100, 0}, Pair{-5, -7}}) {
        auto f = fixture("cull.sentinel", sentinel);
        const Pair expected{sentinel.start > 0 ? sentinel.start * 2 : sentinel.start,
                            sentinel.end > 0 ? sentinel.end * 2 : sentinel.end};
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        CHECK(read_pair(f.part) == expected);
        stopped(f); CHECK(read_pair(f.part) == sentinel);
    }

    for (float scale : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()}) {
        auto f = fixture("cull.invalid-scale"); before = native_calls.size();
        CHECK(hold_instance_cull_scale(f.mod, f.part, scale) == TSUCHI_INVALID);
        CHECK(native_calls.size() == before && owned_count(f.mod) == 0);
        stopped(f);
    }
    for (auto request : {std::pair<Pair, float>{{1, 3}, 0.1f},
                         std::pair<Pair, float>{{100, 101}, 0.01f},
                         std::pair<Pair, float>{{100, INT32_MAX}, 2.0f}}) {
        auto f = fixture("cull.invalid-range", request.first); before = native_calls.size();
        CHECK(hold_instance_cull_scale(f.mod, f.part, request.second) == TSUCHI_INVALID);
        CHECK(native_calls.size() == before && read_pair(f.part) == request.first);
        CHECK(owned_count(f.mod) == 0); stopped(f);
    }
    auto exact_limit = fixture("cull.exact-limit", {1, INT32_MAX});
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(exact_limit.mod, exact_limit.part, 1) == TSUCHI_OK);
    CHECK(native_calls.size() == before && read_pair(exact_limit.part) == (Pair{1, INT32_MAX}));
    stopped(exact_limit);
    // The f32 scale is exact; an intermediate double product rounded too early
    // moves this value across the positive half-integer rounding boundary.
    auto exact_half = fixture("cull.exact-half", {2139095041, 0});
    CHECK(hold_instance_cull_scale(exact_half.mod, exact_half.part,
                                  0.999999940395355224609375f) == TSUCHI_OK);
    CHECK(read_pair(exact_half.part) == (Pair{2139094913, 0}));
    stopped(exact_half); CHECK(read_pair(exact_half.part) == (Pair{2139095041, 0}));

    for (uint64_t type : {hierarchical_type, foliage_type}) {
        auto f = fixture("cull.subclass", {100, 300}, type);
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        CHECK(read_pair(f.part) == (Pair{200, 600})); stopped(f);
        CHECK(read_pair(f.part) == (Pair{100, 300}));
    }
    auto unrelated = fixture("cull.unrelated", {100, 300}, static_mesh_type);
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(unrelated.mod, unrelated.part, 2) == TSUCHI_UNSUPPORTED);
    CHECK(native_calls.size() == before && owned_count(unrelated.mod) == 0); stopped(unrelated);

    auto foreign = fixture("cull.foreign");
    CHECK(hold_instance_cull_scale(foreign.mod, foreign.part, 2) == TSUCHI_OK);
    write_pair(foreign.part, {125, 600}); // only one field changed: adopt the whole pair
    advance_frame(); CHECK(read_pair(foreign.part) == (Pair{250, 1200}));
    before = native_calls.size(); advance_frame(); CHECK(native_calls.size() == before);
    CHECK(release_instance_cull_scale(foreign.mod, foreign.part) == TSUCHI_OK);
    CHECK(read_pair(foreign.part) == (Pair{125, 600})); stopped(foreign);
    auto foreign_release = fixture("cull.foreign-release");
    CHECK(hold_instance_cull_scale(foreign_release.mod, foreign_release.part, 2) == TSUCHI_OK);
    write_pair(foreign_release.part, {111, 222}); before = native_calls.size();
    stopped(foreign_release);
    CHECK(read_pair(foreign_release.part) == (Pair{111, 222}) && native_calls.size() == before);
    auto foreign_overflow = fixture("cull.foreign-overflow");
    CHECK(hold_instance_cull_scale(foreign_overflow.mod, foreign_overflow.part, 2) == TSUCHI_OK);
    write_pair(foreign_overflow.part, {100, INT32_MAX}); before = native_calls.size();
    advance_frame();
    CHECK(owned_count(foreign_overflow.mod) == 0 && native_calls.size() == before);
    CHECK(read_pair(foreign_overflow.part) == (Pair{100, INT32_MAX})); stopped(foreign_overflow);

    // Refusals occur before the exact native setter. A completed setter is an
    // accepted request, not proof that a later readback or rendering succeeded.
    auto refused = fixture("cull.refused", {100, 101});
    CHECK(hold_instance_cull_scale(refused.mod, refused.part, 2) == TSUCHI_OK);
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(refused.mod, refused.part, 0.01f) == TSUCHI_INVALID);
    CHECK(hold_instance_cull_scale(refused.mod, refused.part,
                                  std::numeric_limits<float>::max()) == TSUCHI_INVALID);
    t::fn::set_instance_cull_distances = nullptr;
    CHECK(hold_instance_cull_scale(refused.mod, refused.part, 3) == TSUCHI_NOT_READY);
    t::fn::set_instance_cull_distances = setter;
    CHECK(read_pair(refused.part) == (Pair{200, 202}) && owned_count(refused.mod) == 1);
    CHECK(native_calls.size() == before);
    write_pair(refused.part, {150, 450}); advance_frame();
    CHECK(read_pair(refused.part) == (Pair{300, 900})); // prior accepted scale survives
    stopped(refused); CHECK(read_pair(refused.part) == (Pair{150, 450}));
    auto unavailable = fixture("cull.unavailable");
    t::fn::set_instance_cull_distances = nullptr; before = native_calls.size();
    CHECK(hold_instance_cull_scale(unavailable.mod, unavailable.part, 2) == TSUCHI_NOT_READY);
    CHECK(owned_count(unavailable.mod) == 0 && native_calls.size() == before);
    t::fn::set_instance_cull_distances = setter; stopped(unavailable);
    auto unavailable_keep = fixture("cull.unavailable-keep");
    CHECK(hold_instance_cull_scale(unavailable_keep.mod, unavailable_keep.part, 2) == TSUCHI_OK);
    t::fn::set_instance_cull_distances = nullptr;
    write_pair(unavailable_keep.part, {150, 450}); before = native_calls.size();
    advance_frame(); advance_frame();
    CHECK(owned_count(unavailable_keep.mod) == 1 && native_calls.size() == before);
    CHECK(read_pair(unavailable_keep.part) == (Pair{150, 450}));
    t::fn::set_instance_cull_distances = setter; advance_frame();
    CHECK(read_pair(unavailable_keep.part) == (Pair{300, 900}) && native_calls.size() == before + 1);
    stopped(unavailable_keep); CHECK(read_pair(unavailable_keep.part) == (Pair{150, 450}));
    auto allocation = fixture("cull.allocation-refusal");
    CHECK(hold_instance_cull_scale(allocation.mod, allocation.part, 2) == TSUCHI_OK);
    // An unseen qualifying ordinary vtable needs admission memory. Fail that
    // real allocation for a second target while this mod retains its first.
    const uint64_t allocation_owner = make_actor(make_vtable(true));
    const uint64_t allocation_part = make_part(allocation_owner, instance_type, {100, 300});
    before = native_calls.size();
    const uint32_t allocations_before = refused_allocation_attempts;
    set_allocator({refuse_allocation, std::free});
    CHECK(hold_instance_cull_scale(allocation.mod, allocation_part, 2) == TSUCHI_NO_MEMORY);
    set_allocator({std::malloc, std::free});
    CHECK(refused_allocation_attempts > allocations_before);
    CHECK(native_calls.size() == before && owned_count(allocation.mod) == 1);
    CHECK(read_pair(allocation_part) == (Pair{100, 300}));
    write_pair(allocation.part, {150, 450}); advance_frame();
    CHECK(read_pair(allocation.part) == (Pair{300, 900}));
    stopped(allocation); CHECK(read_pair(allocation.part) == (Pair{150, 450}));

    auto during = fixture("cull.foreign-during-set");
    callback_part = during.part; callback_foreign = {777, 999}; after_setter = foreign_callback;
    CHECK(hold_instance_cull_scale(during.mod, during.part, 2) == TSUCHI_OK);
    CHECK(read_pair(during.part) == callback_foreign && owned_count(during.mod) == 1);
    advance_frame(); CHECK(read_pair(during.part) == (Pair{1554, 1998}));
    stopped(during); CHECK(read_pair(during.part) == (Pair{777, 999}));
    auto update = fixture("cull.foreign-during-update");
    CHECK(hold_instance_cull_scale(update.mod, update.part, 2) == TSUCHI_OK);
    callback_part = update.part; callback_foreign = {888, 999}; after_setter = foreign_callback;
    CHECK(hold_instance_cull_scale(update.mod, update.part, 3) == TSUCHI_OK);
    CHECK(read_pair(update.part) == callback_foreign && owned_count(update.mod) == 1);
    advance_frame(); CHECK(read_pair(update.part) == (Pair{2664, 2997}));
    stopped(update); CHECK(read_pair(update.part) == (Pair{888, 999}));
    auto keep_foreign = fixture("cull.foreign-during-keep");
    CHECK(hold_instance_cull_scale(keep_foreign.mod, keep_foreign.part, 2) == TSUCHI_OK);
    write_pair(keep_foreign.part, {120, 360}); callback_part = keep_foreign.part;
    callback_foreign = {777, 999}; after_setter = foreign_callback;
    advance_frame();
    CHECK(read_pair(keep_foreign.part) == callback_foreign && owned_count(keep_foreign.mod) == 1);
    advance_frame(); CHECK(read_pair(keep_foreign.part) == (Pair{1554, 1998}));
    stopped(keep_foreign); CHECK(read_pair(keep_foreign.part) == (Pair{777, 999}));

    auto reentrant = fixture("cull.stop-in-set");
    callback_mod = reentrant.mod; after_setter = stop_callback; before = native_calls.size();
    CHECK(hold_instance_cull_scale(reentrant.mod, reentrant.part, 2) == TSUCHI_NOT_READY);
    CHECK(read_pair(reentrant.part) == (Pair{100, 300}) && owned_count(reentrant.mod) == 0);
    CHECK(native_calls.size() == before + 2); // deferred restoration follows the native return
    auto release = fixture("cull.release-in-update");
    CHECK(hold_instance_cull_scale(release.mod, release.part, 2) == TSUCHI_OK);
    callback_mod = release.mod; callback_part = release.part; after_setter = release_callback;
    CHECK(hold_instance_cull_scale(release.mod, release.part, 3) == TSUCHI_NOT_READY);
    CHECK(read_pair(release.part) == (Pair{100, 300}) && owned_count(release.mod) == 0); stopped(release);
    auto stop_foreign = fixture("cull.foreign-stop-in-set");
    callback_mod = stop_foreign.mod; callback_part = stop_foreign.part;
    callback_foreign = {444, 555}; after_setter = foreign_stop_callback; before = native_calls.size();
    CHECK(hold_instance_cull_scale(stop_foreign.mod, stop_foreign.part, 2) == TSUCHI_NOT_READY);
    CHECK(read_pair(stop_foreign.part) == callback_foreign && owned_count(stop_foreign.mod) == 0);
    CHECK(native_calls.size() == before + 1); // no blind rollback over the foreign write
    auto nested = fixture("cull.nested-owner"); competing_mod = mod_create("cull.nested-other");
    callback_part = nested.part; after_setter = competing_callback;
    CHECK(hold_instance_cull_scale(nested.mod, nested.part, 2) == TSUCHI_OK);
    CHECK(read_pair(nested.part) == (Pair{200, 600})); stopped(nested); mod_stop(competing_mod);

    // Completion of the trusted void setter accepts the request even when its
    // render notification makes only the cull fields temporarily unreadable.
    // The attempted pair remains a witness; it must never become a new baseline
    // and compound when those fields can be observed again.
    auto unreadable_fresh = fixture("cull.unreadable-fresh");
    auto* unreadable_other = mod_create("cull.unreadable-other");
    callback_part = unreadable_fresh.part; after_setter = hide_fields_callback;
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(unreadable_fresh.mod, unreadable_fresh.part, 2) == TSUCHI_OK);
    CHECK(owned_count(unreadable_fresh.mod) == 1 && read_pair(unreadable_fresh.part) == (Pair{200, 600}));
    CHECK(hold_instance_cull_scale(unreadable_other, unreadable_fresh.part, 3) == TSUCHI_BUSY);
    advance_frame(); advance_frame();
    CHECK(native_calls.size() == before + 1 && owned_count(unreadable_fresh.mod) == 1);
    fields_readable(unreadable_fresh.part, true); advance_frame();
    CHECK(native_calls.size() == before + 1 && read_pair(unreadable_fresh.part) == (Pair{200, 600}));
    release_without_allocation(unreadable_fresh.mod, unreadable_fresh.part);
    CHECK(read_pair(unreadable_fresh.part) == (Pair{100, 300}) && native_calls.size() == before + 2);
    stopped(unreadable_fresh); mod_stop(unreadable_other);

    auto unreadable_update = fixture("cull.unreadable-update");
    CHECK(hold_instance_cull_scale(unreadable_update.mod, unreadable_update.part, 2) == TSUCHI_OK);
    callback_part = unreadable_update.part; after_setter = hide_fields_callback;
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(unreadable_update.mod, unreadable_update.part, 3) == TSUCHI_OK);
    CHECK(owned_count(unreadable_update.mod) == 1 && read_pair(unreadable_update.part) == (Pair{300, 900}));
    advance_frame(); CHECK(native_calls.size() == before + 1);
    fields_readable(unreadable_update.part, true); advance_frame();
    CHECK(native_calls.size() == before + 1 && read_pair(unreadable_update.part) == (Pair{300, 900}));
    write_pair(unreadable_update.part, {120, 360}); advance_frame();
    CHECK(read_pair(unreadable_update.part) == (Pair{360, 1080})); // newly accepted scale is 3
    stopped(unreadable_update); CHECK(read_pair(unreadable_update.part) == (Pair{120, 360}));

    auto unreadable_keep = fixture("cull.unreadable-keep");
    CHECK(hold_instance_cull_scale(unreadable_keep.mod, unreadable_keep.part, 2) == TSUCHI_OK);
    write_pair(unreadable_keep.part, {120, 360});
    callback_part = unreadable_keep.part; after_setter = hide_fields_callback;
    before = native_calls.size(); advance_frame();
    CHECK(owned_count(unreadable_keep.mod) == 1 && read_pair(unreadable_keep.part) == (Pair{240, 720}));
    advance_frame(); advance_frame(); CHECK(native_calls.size() == before + 1);
    fields_readable(unreadable_keep.part, true); advance_frame();
    CHECK(native_calls.size() == before + 1 && read_pair(unreadable_keep.part) == (Pair{240, 720}));
    stopped(unreadable_keep); CHECK(read_pair(unreadable_keep.part) == (Pair{120, 360}));

    // A whole foreign pair that arrives during the unreadable interval is
    // adopted only when readable, using the accepted scale from that request.
    for (bool update_request : {false, true}) {
        auto f = fixture("cull.unreadable-foreign");
        if (update_request) CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        callback_part = f.part; after_setter = hide_fields_callback;
        CHECK(hold_instance_cull_scale(f.mod, f.part, update_request ? 3.0f : 2.0f) == TSUCHI_OK);
        write_pair(f.part, {777, 999}); before = native_calls.size();
        advance_frame(); CHECK(native_calls.size() == before && owned_count(f.mod) == 1);
        fields_readable(f.part, true); advance_frame();
        CHECK(read_pair(f.part) == (update_request ? Pair{2331, 2997} : Pair{1554, 1998}));
        stopped(f); CHECK(read_pair(f.part) == (Pair{777, 999}));
    }

    // Conditional restoration is deliberately limited: release/stop cannot
    // compare an unreadable current pair with its witness, so they end ownership
    // and skip restoration. No hidden post-stop lease later writes or blocks a
    // successor. The old scaled values can remain until a new owner changes them.
    for (bool stop_mod : {false, true}) {
        auto f = fixture("cull.unreadable-end");
        auto* successor = mod_create("cull.unreadable-successor");
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        fields_readable(f.part, false); before = native_calls.size();
        if (stop_mod) stop_without_allocation(f.mod);
        else release_without_allocation(f.mod, f.part);
        CHECK(owned_count(f.mod) == 0 && native_calls.size() == before);
        CHECK(hold_instance_cull_scale(successor, f.part, 3) == TSUCHI_NOT_READY);
        CHECK(owned_count(successor) == 0 && native_calls.size() == before);
        fields_readable(f.part, true); advance_frame(); advance_frame();
        CHECK(read_pair(f.part) == (Pair{200, 600}) && native_calls.size() == before);
        CHECK(hold_instance_cull_scale(successor, f.part, 1) == TSUCHI_OK);
        release_without_allocation(successor, f.part);
        CHECK(native_calls.size() == before && read_pair(f.part) == (Pair{200, 600}));
        stopped(f); mod_stop(successor);
    }
    auto unreadable_stop = fixture("cull.unreadable-native-stop");
    auto* after_stopped = mod_create("cull.unreadable-after-stop");
    callback_part = unreadable_stop.part; callback_mod = unreadable_stop.mod;
    after_setter = hide_and_stop_callback; before = native_calls.size();
    CHECK(hold_instance_cull_scale(unreadable_stop.mod, unreadable_stop.part, 2) == TSUCHI_NOT_READY);
    CHECK(owned_count(unreadable_stop.mod) == 0 && native_calls.size() == before + 1);
    CHECK(hold_instance_cull_scale(after_stopped, unreadable_stop.part, 2) == TSUCHI_NOT_READY);
    fields_readable(unreadable_stop.part, true); advance_frame();
    CHECK(read_pair(unreadable_stop.part) == (Pair{200, 600}) && native_calls.size() == before + 1);
    CHECK(hold_instance_cull_scale(after_stopped, unreadable_stop.part, 1) == TSUCHI_OK);
    mod_stop(after_stopped); CHECK(native_calls.size() == before + 1);

    auto live_end = fixture("cull.live-end");
    CHECK(hold_instance_cull_scale(live_end.mod, live_end.part, 2) == TSUCHI_OK);
    teardown_mod = live_end.mod; teardown_part = live_end.part; teardown_distances = {100, 300};
    check_native_teardown = true; const uint32_t ends_before = end_calls;
    tsuchi::on_end(live_end.owner, 1);
    check_native_teardown = false;
    CHECK(end_calls == ends_before + 1 && read_pair(live_end.part) == teardown_distances);
    stopped(live_end);
    auto destroying = fixture("cull.destroying");
    CHECK(hold_instance_cull_scale(destroying.mod, destroying.part, 2) == TSUCHI_OK);
    fake::put<uint8_t>(destroying.owner + t::engine_actor::play_state,
        uint8_t(t::constant::actor_play_state_begun | t::constant::actor_destroying_mask));
    teardown_mod = destroying.mod; teardown_part = destroying.part; teardown_distances = {200, 600};
    check_native_teardown = true; before = native_calls.size();
    const uint32_t destroyed_before = destroyed_calls; tsuchi::on_destroyed(destroying.owner);
    check_native_teardown = false;
    CHECK(destroyed_calls == destroyed_before + 1 && native_calls.size() == before);
    stopped(destroying);
    auto part_destroying = fixture("cull.part-destroying");
    CHECK(hold_instance_cull_scale(part_destroying.mod, part_destroying.part, 2) == TSUCHI_OK);
    fake::put<uint32_t>(part_destroying.part + t::actor_component::lifecycle_flags,
                       uint32_t(t::constant::component_destroying_mask));
    before = native_calls.size(); stopped(part_destroying);
    CHECK(native_calls.size() == before && read_pair(part_destroying.part) == (Pair{200, 600}));

    for (bool reuse_owner : {false, true}) {
        auto f = fixture("cull.reused-generation");
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        const uint64_t reused = reuse_owner ? f.owner : f.part;
        const uint64_t old = game::to_handle(reused); game::end_life(reused);
        CHECK(game::to_handle(reused) != old && !game::from_handle(old));
        write_pair(f.part, {350, 450}); before = native_calls.size(); advance_frame();
        CHECK(owned_count(f.mod) == 0 && native_calls.size() == before);
        CHECK(read_pair(f.part) == (Pair{350, 450})); stopped(f);
    }
    auto reparent = fixture("cull.reparented");
    CHECK(hold_instance_cull_scale(reparent.mod, reparent.part, 2) == TSUCHI_OK);
    fake::put<uint64_t>(reparent.part + t::object::outer, make_actor());
    write_pair(reparent.part, {350, 450}); before = native_calls.size();
    CHECK(hold_instance_cull_scale(reparent.mod, reparent.part, 3) == TSUCHI_NOT_READY);
    advance_frame(); CHECK(native_calls.size() == before && owned_count(reparent.mod) == 0);
    CHECK(read_pair(reparent.part) == (Pair{350, 450})); stopped(reparent);
    auto gone = fixture("cull.gone");
    CHECK(hold_instance_cull_scale(gone.mod, gone.part, 2) == TSUCHI_OK);
    fake::destroy(gone.part); before = native_calls.size(); stopped(gone);
    CHECK(native_calls.size() == before);

    // Gameplay owners do not fill Hold.ordinary_owner. The instance service
    // must independently preserve the same association and destruction checks.
    // Exercise both a direct update and maintenance after each invalidation.
    for (bool request_update : {false, true}) {
        for (unsigned hazard = 0; hazard != 4; ++hazard) {
            auto f = gameplay_fixture("cull.gameplay-invalidated");
            CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
            invalidate_gameplay_target(f.part, f.owner, hazard);
            before = native_calls.size();
            if (request_update)
                CHECK(hold_instance_cull_scale(f.mod, f.part, 3) == TSUCHI_NOT_READY);
            else advance_frame();
            CHECK(owned_count(f.mod) == 0 && native_calls.size() == before);
            CHECK(read_pair(f.part) == (Pair{200, 600}));
            advance_frame(); stopped(f); CHECK(native_calls.size() == before);
        }
    }
    // A synchronous native render notification can invalidate a gameplay
    // association without emitting an actor-left event. Retire the checkpointed
    // lease, preserve the completed setter's pair and avoid a stale restore.
    for (bool maintenance : {false, true}) {
        for (unsigned hazard = 0; hazard != 4; ++hazard) {
            auto f = gameplay_fixture("cull.gameplay-native-invalidation");
            if (maintenance) {
                CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
                write_pair(f.part, {120, 360});
            }
            callback_part = f.part; callback_owner = f.owner; callback_hazard = hazard;
            after_setter = invalidate_target_callback; before = native_calls.size();
            if (maintenance) advance_frame();
            else CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_NOT_READY);
            CHECK(owned_count(f.mod) == 0 && native_calls.size() == before + 1);
            CHECK(read_pair(f.part) == (maintenance ? Pair{240, 720} : Pair{200, 600}));
            advance_frame(); stopped(f); CHECK(native_calls.size() == before + 1);
        }
    }
    // Unreadable cull fields do not hide a stale owner or destruction flags.
    // Retire that hold immediately, and never revive it when fields
    // or flags become readable/usable again.
    for (unsigned hazard = 0; hazard != 4; ++hazard) {
        auto f = gameplay_fixture("cull.unreadable-invalidated");
        CHECK(hold_instance_cull_scale(f.mod, f.part, 2) == TSUCHI_OK);
        fields_readable(f.part, false);
        invalidate_gameplay_target(f.part, f.owner, hazard); before = native_calls.size();
        advance_frame(); CHECK(owned_count(f.mod) == 0 && native_calls.size() == before);
        if (hazard == 1) fake::put<uint32_t>(f.part + t::actor_component::lifecycle_flags, 0);
        if (hazard == 3) fake::put<uint8_t>(f.owner + t::engine_actor::play_state,
                                         uint8_t(t::constant::actor_play_state_begun));
        fields_readable(f.part, true); advance_frame();
        CHECK(read_pair(f.part) == (Pair{200, 600}) && native_calls.size() == before);
        auto* successor = mod_create("cull.hidden-invalid-successor");
        CHECK(hold_instance_cull_scale(successor, f.part, 1) == TSUCHI_OK);
        mod_stop(successor); stopped(f); CHECK(native_calls.size() == before);
    }

    const uint64_t refused_owner = make_actor(make_vtable(false));
    auto* refused_mod = mod_create("cull.unsupported-owner");
    const uint64_t refused_part = make_part(refused_owner, foliage_type, {100, 300});
    before = native_calls.size();
    CHECK(hold_instance_cull_scale(refused_mod, refused_part, 2) == TSUCHI_UNSUPPORTED);
    CHECK(native_calls.size() == before && owned_count(refused_mod) == 0);
    CHECK(read_pair(refused_part) == (Pair{100, 300})); mod_stop(refused_mod);
    return failures;
}
