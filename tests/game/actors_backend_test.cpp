// Focused stand-in checks for the Engine actor backend. Native iterator behavior needs game evidence.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "actor_service.h"
#include "angles.h"
#include "engine_call.h"
#include "game.h"
#include "objects.h"
#include "parts.h"
#include "reflect.h"
#include "services.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
namespace {
namespace t = tsuchi::game::table;
const tsuchi::actor::Backend* backend;
struct Item { uint64_t address, handle, type; bool actor, active = true; };
std::vector<Item> items;
struct Range { uint64_t first, size; };
std::vector<Range> ranges;
uint64_t world = 0;
constexpr uint64_t actor_type = 10, child_type = 11, unrelated_type = 12, statics_type = 13;
bool invoke_ok = true, setter_ok = true;
uint64_t requested_world = 0, requested_class = 0;
std::vector<uint64_t> returned;
unsigned unreadable_reads = 0, setter_calls = 0;
float wanted[6];
Item* item(uint64_t address) { for (auto& x : items) if (x.address == address) return &x; return nullptr; }
void remember(void* bytes, size_t size) { ranges.push_back({reinterpret_cast<uint64_t>(bytes), size}); }
void add(void* bytes, uint64_t handle, uint64_t type, bool actor) { items.push_back({reinterpret_cast<uint64_t>(bytes), handle, type, actor}); }
bool setter(void* object, bool sweep, void* hit, bool teleport, float x, float y, float z, float pitch, float yaw, float roll) {
    CHECK(item(reinterpret_cast<uint64_t>(object))->actor);
    CHECK(!sweep && teleport && hit);
    ++setter_calls;
    const float values[6] = {x, y, z, pitch, yaw, roll};
    memcpy(wanted, values, sizeof wanted);
    return setter_ok;
}
} // namespace
namespace tsuchi::actor { void set_backend(const Backend* b) { backend = b; } }
// The presenter is told of a root moved at once, so its pictures do not slide there.
static uint64_t cut_actor = 0;
static unsigned cuts = 0;
namespace tsuchi { void presenter_cut(uint64_t actor) { cut_actor = actor, ++cuts; } }
namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(&world) - t::data::world; }
bool readable(uint64_t address, uint64_t size) {
    for (auto r : ranges) if (address >= r.first && address - r.first <= r.size && size <= r.size - (address - r.first)) return true;
    ++unreadable_reads;
    return false;
}
bool live(uint64_t address) { return item(address) != nullptr; }
uint64_t from_handle(uint64_t handle) { for (auto& x : items) if (x.handle == handle) return x.address; return 0; }
uint64_t to_handle(uint64_t address) { const auto* x = item(address); return x ? x->handle : 0; }
bool is_engine_actor(uint64_t address) { const auto* x = item(address); return x && x->actor; }
bool is_class(uint64_t address) { return address >= actor_type && address <= statics_type; }
bool derives_from(uint64_t type, uint64_t base) { return type == base || (type == child_type && base == actor_type); }
uint64_t find_class(ClassRef& ref) { return !strcmp(ref.name, t::name::engine_actor_class) ? actor_type : statics_type; }
EngineCall::EngineCall(uint64_t object, const char* function) : object_(object), state_(TSUCHI_OK) {
    CHECK(object == statics_type && !strcmp(function, "GetAllActorsOfClass"));
}
EngineCall::~EngineCall() = default;
TSUCHI_Result EngineCall::set(const char* name, const Value& value) {
    CHECK(value.kind == ValueKind::object);
    if (!strcmp(name, "WorldContextObject")) requested_world = from_handle(value.object);
    else { CHECK(!strcmp(name, "ActorClass")); requested_class = from_handle(value.object); }
    return TSUCHI_OK;
}
TSUCHI_Result EngineCall::invoke() {
    if (!invoke_ok) return TSUCHI_NOT_READY;
    CHECK(requested_world == world);
    returned.clear();
    for (auto x : items) if (x.actor && x.active && derives_from(x.type, requested_class)) returned.push_back(x.address);
    return TSUCHI_OK;
}
TSUCHI_Result EngineCall::objects(const char* name, uint32_t& count) { CHECK(!strcmp(name, "OutActors")); count = uint32_t(returned.size()); return TSUCHI_OK; }
uint64_t EngineCall::object_at(const char* name, uint32_t index) { CHECK(!strcmp(name, "OutActors") && index < returned.size()); return returned[index]; }
} // namespace tsuchi::game
int main() {
    alignas(8) unsigned char actor[0x138] = {}, other[0x138] = {}, root[0x200] = {}, world_bytes[8] = {};
    // Only the native Actor root pointer is readable. Any attempted GActor facing read fails.
    remember(&world, sizeof world); remember(actor, sizeof actor); remember(other, sizeof other); remember(root, sizeof root);
    add(actor, 101, actor_type, true); add(other, 102, child_type, true); add(root, 103, unrelated_type, false); add(world_bytes, 104, unrelated_type, false);
    items.push_back({actor_type, 201, actor_type, false}); items.push_back({child_type, 202, actor_type, false});
    items.push_back({unrelated_type, 203, unrelated_type, false}); items.push_back({statics_type, 204, unrelated_type, false});
    tsuchi::actors_start(); CHECK(backend);
    bool alive = false;
    CHECK(backend->alive({101}, &alive) == TSUCHI_OK && alive);
    CHECK(backend->alive({103}, &alive) == TSUCHI_OK && !alive);
    CHECK(backend->alive({999}, &alive) == TSUCHI_OK && !alive);
    uint64_t count = 99; TSUCHI_Actor list[2]{{777}, {888}};
    CHECK(backend->list({}, list, 2, &count) == TSUCHI_NOT_READY && count == 99);
    world = reinterpret_cast<uint64_t>(world_bytes);
    CHECK(backend->list({}, list, 1, &count) == TSUCHI_CAPACITY && count == 2 && list[0].id == 777 && list[1].id == 888);
    CHECK(backend->list({}, list, 2, &count) == TSUCHI_OK && count == 2 && list[0].id == 101 && list[1].id == 102);
    CHECK(requested_class == actor_type);
    CHECK(backend->list({202}, list, 2, &count) == TSUCHI_OK && count == 1 && list[0].id == 102);
    CHECK(backend->list({203}, list, 2, &count) == TSUCHI_INVALID);
    CHECK(backend->list({103}, list, 2, &count) == TSUCHI_INVALID);
    CHECK(backend->list({999}, list, 2, &count) == TSUCHI_NOT_FOUND);
    items[1].active = false;
    CHECK(backend->list({}, list, 2, &count) == TSUCHI_OK && count == 1 && list[0].id == 101);
    // Unloaded but retained actors stay valid handles even while absent from the active world list.
    CHECK(backend->alive({102}, &alive) == TSUCHI_OK && alive);
    invoke_ok = false; list[0].id = 777;
    CHECK(backend->list({}, list, 2, &count) == TSUCHI_NOT_READY && list[0].id == 777);
    invoke_ok = true;
    TSUCHI_Transform pose{{7,8,9},{10,11,12}};
    CHECK(backend->transform({999}, &pose) == TSUCHI_NOT_FOUND && pose.location.x == 7);
    CHECK(backend->transform({101}, &pose) == TSUCHI_NOT_READY && pose.location.x == 7);
    const uint64_t root_address = reinterpret_cast<uint64_t>(root);
    memcpy(actor + t::engine_actor::root, &root_address, sizeof root_address);
    float angles[3] = {17,83,-21}, q[4], location[3] = {125,-50,20}; tsuchi::math::to_quaternion(angles,q);
    memcpy(root+t::scene_component::rotation,q,sizeof q); memcpy(root+t::scene_component::location,location,sizeof location);
    CHECK(backend->transform({101}, &pose) == TSUCHI_OK);
    CHECK(pose.location.x == 125 && pose.location.y == -50 && pose.location.z == 20);
    CHECK(__builtin_fabsf(pose.rotation.pitch - 17) < .01f && __builtin_fabsf(pose.rotation.yaw - 83) < .01f && __builtin_fabsf(pose.rotation.roll + 21) < .01f);
    unsigned char before[sizeof root]; memcpy(before,root,sizeof root);
    t::fn::set_actor_location_and_rotation = setter;
    CHECK(backend->set_transform({101}, &pose) == TSUCHI_OK && setter_calls == 1);
    CHECK(cuts == 1 && cut_actor != 0);
    CHECK(wanted[0] == 125 && __builtin_fabsf(wanted[4] - 83) < .01f);
    CHECK(!memcmp(before,root,sizeof root)); // backend must not write scale or any root fields around the native call
    setter_ok = false; CHECK(backend->set_transform({101}, &pose) == TSUCHI_NOT_READY && setter_calls == 2 && cuts == 1);
    t::fn::set_actor_location_and_rotation = nullptr; CHECK(backend->set_transform({101}, &pose) == TSUCHI_NOT_READY && setter_calls == 2);
    items[0].handle = 301; // the central registry owns the generation, with no private actor identity
    CHECK(backend->alive({101}, &alive) == TSUCHI_OK && !alive);
    CHECK(backend->transform({101}, &pose) == TSUCHI_NOT_FOUND);
    CHECK(backend->alive({301}, &alive) == TSUCHI_OK && alive);
    CHECK(unreadable_reads == 0);
    puts("Engine actor backend: focused stand-in checks passed");
}
