#include "records.h"
// Checks of the objects service against a made-up game: first from compiled
// code, then from the script given on the command line. Exits non-zero on failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "compiled_mods.h"
#include "content_path.h"
#include "sha256.h"
#include "actor_events.h"
#include "actors.h"
#include "ai_file.h"
#include "added.h"
#include "callable_thunks.h"
#include "clips.h"
#include "engine_call.h"
#include "fake_engine.h"
#include "fake_game.h"
#include "fake_modules.h"
#include "frame.h"
#include "hidden.h"
#include "holds.h"
#include "life_events.h"
#include "materials.h"
#include "map_ai.h"
#include "map_navigation.h"
#include "map_placement_changes.h"
#include "map_placements.h"
#include "map_values.h"
#include "placement_mode.h"
#include "mods.h"
#include "mod_choices.h"
#include "modules.h"
#include "nav_file.h"
#include "new_maps.h"
#include "host.h"
#include "ids.h"
#include "motion.h"
#include "angles.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "tsuchi_lua.h"
#include "captain.h"
#include "people.h"
#include "shared.h"
#include "pikmin.h"
#include "pikmin_service.h"
#include "parts.h"
#include "reflect.h"
#include "reflection.h"
#include "script_mods.h"
#include "tsuchi_js.h"
#include <fstream>
#include <sstream>
#include "registry.h"
#include "sd_card.h"
#include "services.h"
#include "suspend.h"
#include "switched.h"
#include "check_mods_data.h"
#include "status.h"

namespace tsuchi::target {
Status status{};
// The production module supplies this sink; the host fixture has no system log.
void log_line(const char*, const char*) {}
}

extern "C" {
#include "lualib.h"
}

namespace {

using namespace tsuchi::game;
namespace t = fake::t;

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

const TSUCHI_EntityService& api = *tsuchi::host.entity;
TSUCHI_Mod* mod = nullptr;

struct World {
    uint64_t root, component_class, primitive_class, actor_class, pikmin_class, creature_class, bulborb_class;
    uint64_t rock = 0, stone = 0;
    uint64_t framework, last_node;
    uint64_t red, gate, bulborb, captain, body;
    uint64_t hero = 0, hound = 0, chief = 0, mutt = 0, thrown_red = 0, thrown_blue = 0, sprig = 0, loner = 0, beast = 0, corpse = 0, idol_asset = 0, creature_table = 0;
} w;

bool near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) < tolerance; }
TSUCHI_Actor handle(uint64_t object) { return {to_handle(object)}; }

// The engine's own formula with the standard library's trigonometry, to check the framework's against.
void quaternion_of(float pitch, float yaw, float roll, float q[4]) {
    const double h = 3.14159265358979323846 / 360;
    const double sp = std::sin(pitch * h), cp = std::cos(pitch * h), sy = std::sin(yaw * h), cy = std::cos(yaw * h), sr = std::sin(roll * h),
                 cr = std::cos(roll * h);
    q[0] = float(cr * sp * sy - sr * cp * cy);
    q[1] = float(-cr * sp * cy - sr * cp * sy);
    q[2] = float(cr * cp * sy - sr * sp * cy);
    q[3] = float(cr * cp * cy + sr * sp * sy);
}

uint64_t component(float x, float y, float z, float yaw) {
    const uint64_t address = fake::object(u"Part", w.component_class, 0x80);
    fake::put(address + t::scene_component::location, x), fake::put(address + t::scene_component::location + 4, y);
    fake::put(address + t::scene_component::location + 8, z);
    float q[4];
    quaternion_of(0, yaw, 0, q);
    for (int i = 0; i < 4; ++i) fake::put(address + t::scene_component::rotation + 4 * i, q[i]);
    return address;
}

// An actor at the end of the game's ring of actor.
uint64_t actor(const std::u16string& name, uint64_t type, const std::u16string& kind, int32_t team, uint64_t placement, uint64_t root) {
    const uint64_t address = fake::object(name, type, 0x100);
    fake::put<uint8_t>(address + t::game_actor::tick_requested, 1);
    fake::put(address + t::game_actor::kind, fake::name(kind));
    fake::put(address + t::game_actor::team, team);
    fake::put(address + t::game_actor::placement, placement);
    fake::put(address + t::game_actor::root, root);
    const uint64_t node = fake::memory(t::actor_node::size), head = w.framework + t::game_framework::actor_list;
    fake::put(node + t::actor_node::actor, address);
    fake::put(node + t::actor_node::next, head);
    fake::put(w.last_node + t::actor_node::next, node);
    w.last_node = node;
    return address;
}

// What the game's two functions were last asked to do. They do it, too.
struct Moved {
    int actor_calls = 0, component_calls = 0;
    uint64_t actor = 0, component = 0;
    bool sweep = true, teleport = false, hit_given = false;
    float angles[3] = {}, component_angles[3] = {};
} moved;
bool set_location_and_rotation(void* actor, bool sweep, void* hit, bool teleport, float x, float y, float z, float pitch, float yaw, float roll) {
    ++moved.actor_calls;
    moved.actor = reinterpret_cast<uint64_t>(actor), moved.sweep = sweep, moved.teleport = teleport, moved.hit_given = hit != nullptr;
    moved.angles[0] = pitch, moved.angles[1] = yaw, moved.angles[2] = roll;
    const uint64_t root = fake::get<uint64_t>(moved.actor + t::game_actor::root);
    fake::put(root + t::scene_component::location, x), fake::put(root + t::scene_component::location + 4, y);
    fake::put(root + t::scene_component::location + 8, z);
    float q[4];
    quaternion_of(pitch, yaw, roll, q);
    for (int i = 0; i < 4; ++i) fake::put(root + t::scene_component::rotation + 4 * i, q[i]);
    return true;
}
void set_world_rotation(void* part, bool, void*, bool, float pitch, float yaw, float roll) {
    ++moved.component_calls;
    moved.component = reinterpret_cast<uint64_t>(part);
    moved.component_angles[0] = pitch, moved.component_angles[1] = yaw, moved.component_angles[2] = roll;
    float q[4];
    quaternion_of(pitch, yaw, roll, q);
    for (int i = 0; i < 4; ++i) fake::put(moved.component + t::scene_component::rotation + 4 * i, q[i]);
}

void install_events();
void early_native_initialize(uint64_t level, int32_t budget, bool reconstruct, void* context);
void initial_native_flush(uint64_t world, uint32_t mode);
// The game's level removal: what the guard in the game's context said while it ran.
uint8_t removal_guard_seen = 0xFF;
void game_remove_level(uint64_t, uint64_t, uint32_t) {
    const uint64_t context = fake::get<uint64_t>(fake::main_image() + t::data::game_context);
    removal_guard_seen = fake::get<uint8_t>(context + t::game_context::level_removal_guard);
}
// The game's two readers of a map's placement lists: each row they read, with where it stands, as text.
struct Listing {
    std::string log;
    bool restored = false;
    uint32_t held_at = 0x40, rows_at = 0x40; // where a list of lists holds its lists, and a list its rows
    uint32_t settings_at = 0x30;             // where the world's settings keep the lists of lists
    std::function<void(uint64_t)> peek;      // a test's look at the lists as the game reads them
} list_reads;
void read_listed(uint64_t lists) {
    namespace g = t::generator;
    namespace x = t::engine_transform;
    const uint64_t held = fake::get<uint64_t>(lists + t::array::data);
    for (int32_t i = 0; i < fake::get<int32_t>(lists + t::array::count); ++i) {
        const uint64_t set = fake::get<uint64_t>(held + uint64_t(i) * 8), inner = fake::get<uint64_t>(set + list_reads.held_at + t::array::data);
        for (int32_t j = 0; j < fake::get<int32_t>(set + list_reads.held_at + t::array::count); ++j) {
            const uint64_t list = fake::get<uint64_t>(inner + uint64_t(j) * 8), rows = fake::get<uint64_t>(list + list_reads.rows_at + t::array::data);
            for (int32_t r = 0; r < fake::get<int32_t>(list + list_reads.rows_at + t::array::count); ++r) {
                const uint64_t row = rows + uint64_t(r) * g::size;
                char text[64];
                std::snprintf(text, sizeof text, "%llx@%g/%g ", static_cast<unsigned long long>(fake::get<uint64_t>(row + g::placement)),
                              double(fake::get<float>(row + g::transform + x::translation)), double(fake::get<float>(row + g::init_transform + x::translation)));
                list_reads.log += text;
            }
        }
    }
}
void game_build_lists(void*, void*, void* lists) {
    list_reads.log += "build: ";
    if (list_reads.peek) list_reads.peek(reinterpret_cast<uint64_t>(lists));
    read_listed(reinterpret_cast<uint64_t>(lists));
}
bool game_restore_lists(void*, void* world, uint32_t) {
    const uint64_t level = fake::get<uint64_t>(reinterpret_cast<uint64_t>(world) + t::world::persistent_level);
    const uint64_t settings = fake::get<uint64_t>(level + t::engine_level::world_settings);
    list_reads.log += "restore: ";
    read_listed(settings + list_reads.settings_at);
    return list_reads.restored;
}

void build() {
    fake::start();
    fake::patch_mark(); // the patch this build was made with; each section changes the sites it uses
    CHECK(tsuchi::find_patch() == tsuchi::Patch::right);
    w.root = fake::object(u"Class", 0);
    w.component_class = fake::object(u"SceneComponent", w.root);
    w.primitive_class = fake::object(u"PrimitiveComponent", w.root);
    fake::set_super(w.primitive_class, w.component_class);
    w.actor_class = fake::object(u"GActor", w.root);
    w.pikmin_class = fake::object(u"Pikmin", w.root);
    fake::set_super(w.pikmin_class, w.actor_class);
    w.creature_class = fake::object(u"Creature", w.root);
    fake::set_super(w.creature_class, w.actor_class);
    w.bulborb_class = fake::object(u"Bulborb", w.root);
    fake::set_super(w.bulborb_class, w.creature_class);
    w.framework = fake::object(u"GameFramework", w.component_class, 0x100);
    w.last_node = w.framework + t::game_framework::actor_list;
    fake::put(w.last_node + t::actor_node::next, w.last_node); // an empty ring points at itself
    tsuchi::hooks_start();
    tsuchi::actors_start();
    tsuchi::entities_start();
    tsuchi::components_start();
    tsuchi::collision_start();
    tsuchi::behavior_start();
    tsuchi::combat_start();
    tsuchi::game_start();
    tsuchi::players_start();
    tsuchi::input_start(tsuchi::mod_create("tsuchi.self.input"));
    tsuchi::captains_start();
    tsuchi::pikmin_start();
    tsuchi::squad_start();
    tsuchi::creatures_start();
    tsuchi::assets_start();
    tsuchi::tables_start();
    tsuchi::text_start();
    tsuchi::hud_start();
    tsuchi::audio_start();
    tsuchi::anim_start();
    tsuchi::mods_start();
    tsuchi::dev_folders_start(); // restarts run after each step; the checks give the development folders a stand-in
    tsuchi::declared_start();
    tsuchi::maps_start();
    TSUCHI_Mod* framework_maps = tsuchi::mod_create("tsuchi.self.maps");
    TSUCHI_Mod* game_maps = tsuchi::mod_create("the.game.maps");
    const auto& prepare = t::code_sites[t::code_site::level_prepare.number];
    fake::put<uint32_t>(fake::main_image() + prepare.address, 0x14000000u | uint32_t((prepare.entry - prepare.address) >> 2));
    uint64_t map_hook = 0; void* previous_prepare = nullptr;
    CHECK(tsuchi::hook_code_site(game_maps, t::code_site::level_prepare.number,
          reinterpret_cast<void*>(&early_native_initialize), &map_hook, &previous_prepare) == TSUCHI_OK);
    // The game's own level removal is the far end of its chain, put on before the framework's hooks.
    const auto& remove_level = t::code_sites[t::code_site::world_remove_level.number];
    fake::put<uint32_t>(fake::main_image() + remove_level.address, 0x14000000u | uint32_t((remove_level.entry - remove_level.address) >> 2));
    void* previous_remove = nullptr;
    CHECK(tsuchi::hook_code_site(game_maps, t::code_site::world_remove_level.number,
          reinterpret_cast<void*>(&game_remove_level), &map_hook, &previous_remove) == TSUCHI_OK);
    // So are the game's two readers of placement lists.
    for (const auto& [site, reader] : {std::pair<unsigned, void*>{t::code_site::placement_build.number, reinterpret_cast<void*>(&game_build_lists)},
                                       std::pair<unsigned, void*>{t::code_site::placement_restore.number, reinterpret_cast<void*>(&game_restore_lists)}}) {
        const auto& reading = t::code_sites[site];
        fake::put<uint32_t>(fake::main_image() + reading.address, 0x14000000u | uint32_t((reading.entry - reading.address) >> 2));
        void* previous = nullptr;
        CHECK(tsuchi::hook_code_site(game_maps, site, reader, &map_hook, &previous) == TSUCHI_OK);
    }
    // The focused incoming-world fixture supplies the far end of this call-site chain.
    fake::patch_site(t::code_site::map_initial_flush.number);
    void* previous_initial_flush = nullptr;
    CHECK(tsuchi::hook_code_site(game_maps, t::code_site::map_initial_flush.number,
          reinterpret_cast<void*>(&initial_native_flush), &map_hook, &previous_initial_flush) == TSUCHI_OK);
    tsuchi::map_changes_start(framework_maps);
    tsuchi::new_maps_start(framework_maps);
    tsuchi::placements_start(framework_maps);
    tsuchi::content_start();
    tsuchi::shaders_start();
    tsuchi::storage_start();
    tsuchi::settings_start();
    {
        // The patch's branch at the collection-credit site, which carry hooks for its delivered event.
        const auto& credit = t::code_sites[t::code_site::carry_credit.number];
        fake::put<uint32_t>(fake::main_image() + credit.address, 0x14000000u | uint32_t((credit.entry - credit.address) >> 2));
    }
    tsuchi::carry_start(framework_maps);
    tsuchi::story_start();
    tsuchi::reflect_start();
    mod = tsuchi::mod_create("actor.test");
    install_events();
}

std::vector<uint64_t> listed(TSUCHI_Result expected = TSUCHI_OK) {
    TSUCHI_Actor buffer[32];
    uint64_t count = 99;
    const TSUCHI_Result result = api.list(mod, {}, buffer, 32, &count);
    CHECK(result == expected);
    std::vector<uint64_t> out;
    for (uint64_t i = 0; result == TSUCHI_OK && i < count; ++i) out.push_back(from_handle(buffer[i].id));
    return out;
}

std::string kind_of(uint64_t address) {
    char buffer[32] = "untouched";
    uint64_t needed = 0;
    return api.kind(mod, handle(address), buffer, sizeof buffer, &needed) == TSUCHI_OK && needed == std::string(buffer).size() + 1 ? buffer : "<failed>";
}

void arithmetic() {
    for (float degrees = -720; degrees <= 720; degrees += 7.3f) {
        const float radians = degrees * tsuchi::math::kPi / 180;
        CHECK(near(tsuchi::math::sine(radians), std::sin(radians), 1e-4f) && near(tsuchi::math::cosine(radians), std::cos(radians), 1e-4f));
    }
    for (float x = -50; x <= 50; x += 0.37f) CHECK(near(tsuchi::math::arctangent(x), std::atan(x), 1e-5f));
    for (float y : {-3.0f, 0.0f, 2.0f})
        for (float x : {-4.0f, 0.0f, 5.0f}) CHECK(near(tsuchi::math::arctangent(y, x), std::atan2(y, x), 1e-5f));
    for (float x : {0.0f, 1.0f, 2.0f, 1e6f}) CHECK(near(tsuchi::math::square_root(x), std::sqrt(x), 1e-3f * (1 + x)));
    // Angles to a quaternion and back, against the standard library's version of the same formula.
    for (float pitch : {-80.0f, -10.0f, 0.0f, 35.0f, 89.0f})
        for (float yaw : {-179.0f, -90.0f, 0.0f, 45.0f, 170.0f})
            for (float roll : {-120.0f, 0.0f, 15.0f}) {
                const float angles[3] = {pitch, yaw, roll};
                float q[4], reference[4], back[3];
                tsuchi::math::to_quaternion(angles, q);
                quaternion_of(pitch, yaw, roll, reference);
                for (int i = 0; i < 4; ++i) CHECK(near(q[i], reference[i], 1e-4f));
                tsuchi::math::to_angles(q, back);
                CHECK(near(back[0], pitch, 0.02f) && near(back[1], yaw, 0.02f) && near(back[2], roll, 0.02f));
            }
    const float straight_up[3] = {90, 30, 0};
    float q[4], back[3];
    tsuchi::math::to_quaternion(straight_up, q);
    tsuchi::math::to_angles(q, back);
    CHECK(near(back[0], 90, 0.1f)); // at the pole only the pitch is certain
}

void lists() {
    // Before a world there is nothing to list, and it says so.
    TSUCHI_Actor buffer[32];
    uint64_t count = 99;
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, 0);
    CHECK(api.list(mod, {}, buffer, 32, &count) == TSUCHI_NOT_READY && count == 99);
    CHECK(api.list(mod, {to_handle(w.creature_class)}, buffer, 32, &count) == TSUCHI_NOT_READY);
    TSUCHI_Actor found{};
    CHECK(api.find_placed(mod, 77, &found) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, w.framework);
    CHECK(listed().empty());
    CHECK(api.list(mod, {}, nullptr, 0, &count) == TSUCHI_OK && count == 0);

    w.red = actor(u"Red_1", w.pikmin_class, u"PikminRed", 0, ~uint64_t(0), component(100, 200, 300, 90));
    w.gate = actor(u"Gate_1", w.actor_class, u"Gate", 4, 77, component(0, 0, 0, 0));
    w.bulborb = actor(u"Bulborb_1", w.bulborb_class, u"Kochappy", 5, 78, component(-50, 0, 10, -45));
    w.body = component(0, 0, 0, -90);
    w.captain = actor(u"Captain_1", w.creature_class, u"Olimar", 0, 0, component(1, 2, 3, 30));
    fake::put<uint8_t>(w.captain + t::game_actor::facing_enabled, 1);
    fake::put(w.captain + t::game_actor::facing_component, w.body);

    // In the game's order.
    CHECK((listed() == std::vector<uint64_t>{w.red, w.gate, w.bulborb, w.captain}));
    // A buffer that is too small gets nothing, and learns how many there are.
    TSUCHI_Actor small[2] = {{7}, {7}};
    CHECK(api.list(mod, {}, small, 2, &count) == TSUCHI_CAPACITY && count == 4 && small[0].id == 7 && small[1].id == 7);
    CHECK(api.list(mod, {}, nullptr, 0, &count) == TSUCHI_CAPACITY && count == 4);
    CHECK(api.list(mod, {}, nullptr, 3, &count) == TSUCHI_INVALID && api.list(mod, {}, buffer, 32, nullptr) == TSUCHI_INVALID);
    CHECK(api.list(mod, {}, buffer, 4, &count) == TSUCHI_OK && count == 4 && from_handle(buffer[3].id) == w.captain);

    // By class: the class and whatever is made from it.
    CHECK(api.list(mod, {to_handle(w.creature_class)}, buffer, 32, &count) == TSUCHI_OK && count == 2 &&
          from_handle(buffer[0].id) == w.bulborb && from_handle(buffer[1].id) == w.captain);
    CHECK(api.list(mod, {to_handle(w.actor_class)}, buffer, 32, &count) == TSUCHI_OK && count == 4);
    CHECK(api.list(mod, {to_handle(w.component_class)}, buffer, 32, &count) == TSUCHI_OK && count == 0);
    CHECK(api.list(mod, {to_handle(w.red)}, buffer, 32, &count) == TSUCHI_INVALID); // an actor is not a class
    CHECK(api.list(mod, {12345}, buffer, 32, &count) == TSUCHI_NOT_FOUND);
    CHECK(api.list(mod, {to_handle(w.creature_class)}, small, 1, &count) == TSUCHI_CAPACITY && count == 2 && small[0].id == 7);

    // By the number a level placed it under.
    CHECK(api.find_placed(mod, 77, &found) == TSUCHI_OK && from_handle(found.id) == w.gate);
    CHECK(api.find_placed(mod, 78, &found) == TSUCHI_OK && from_handle(found.id) == w.bulborb);
    CHECK(api.find_placed(mod, 79, &found) == TSUCHI_NOT_FOUND && api.find_placed(mod, 0, &found) == TSUCHI_INVALID);
    CHECK(api.find_placed(mod, ~uint64_t(0), &found) == TSUCHI_NOT_FOUND); // the game's "none" is not a number to look up
}

void descriptions() {
    int32_t team = 99;
    uint64_t placement = 99;
    CHECK(kind_of(w.red) == "PikminRed" && kind_of(w.bulborb) == "Kochappy");
    CHECK(api.team(mod, handle(w.bulborb), &team) == TSUCHI_OK && team == 5);
    CHECK(api.placement(mod, handle(w.gate), &placement) == TSUCHI_OK && placement == 77);
    CHECK(api.placement(mod, handle(w.red), &placement) == TSUCHI_OK && placement == 0); // the game's "none"
    char small[4] = "zzz";
    uint64_t needed = 0;
    CHECK(api.kind(mod, handle(w.red), small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 10 && std::string(small) == "zzz");

    // Something that is alive but is not an actor.
    const TSUCHI_Actor part{to_handle(w.body)};
    CHECK(api.team(mod, part, &team) == TSUCHI_INVALID && api.placement(mod, part, &placement) == TSUCHI_INVALID && kind_of(w.body) == "<failed>");
    CHECK(api.team(mod, {0}, &team) == TSUCHI_INVALID && api.team(nullptr, handle(w.red), &team) == TSUCHI_INVALID);
}

// Sets a pose from its two parts. With either part missing there is no pose to give.
TSUCHI_Result set_pose(const TSUCHI_EntityService& api, TSUCHI_Mod* mod, TSUCHI_Actor actor, const TSUCHI_Vec3* location, const TSUCHI_Rotation* rotation) {
    if (!location || !rotation) return api.set_pose(mod, actor, nullptr);
    const TSUCHI_Pose to{*location, *rotation};
    return api.set_pose(mod, actor, &to);
}

void poses() {
    t::fn::set_actor_location_and_rotation = set_location_and_rotation;
    t::fn::set_component_world_rotation = set_world_rotation;
    TSUCHI_Pose at{};
    CHECK(api.pose(mod, handle(w.red), &at) == TSUCHI_OK);
    CHECK(near(at.location.x, 100) && near(at.location.y, 200) && near(at.location.z, 300));
    CHECK(near(at.rotation.pitch, 0) && near(at.rotation.yaw, 90) && near(at.rotation.roll, 0));
    CHECK(api.pose(mod, handle(w.bulborb), &at) == TSUCHI_OK && near(at.rotation.yaw, -45));
    // A character faces where its body does, a quarter turn from the body's own angle.
    CHECK(api.pose(mod, handle(w.captain), &at) == TSUCHI_OK && near(at.rotation.yaw, 0) && near(at.location.x, 1));

    // A body counts only while the actor says it turns one, and only when it is not the root itself.
    fake::put(w.bulborb + t::game_actor::facing_component, w.body);
    CHECK(api.pose(mod, handle(w.bulborb), &at) == TSUCHI_OK && near(at.rotation.yaw, -45));
    fake::put<uint8_t>(w.bulborb + t::game_actor::facing_enabled, 1);
    fake::put(w.bulborb + t::game_actor::facing_component, fake::get<uint64_t>(w.bulborb + t::game_actor::root));
    CHECK(api.pose(mod, handle(w.bulborb), &at) == TSUCHI_OK && near(at.rotation.yaw, -45));

    // An ordinary actor is moved and turned as one.
    const TSUCHI_Vec3 there{10, 20, 30};
    const TSUCHI_Rotation facing{0, 135, 0};
    CHECK(set_pose(api, mod, handle(w.red), &there, &facing) == TSUCHI_OK);
    CHECK(moved.actor_calls == 1 && moved.component_calls == 0 && moved.actor == w.red && !moved.sweep && moved.teleport && moved.hit_given);
    CHECK(near(moved.angles[1], 135));
    CHECK(api.pose(mod, handle(w.red), &at) == TSUCHI_OK && near(at.location.z, 30) && near(at.rotation.yaw, 135));

    // A character's root keeps its own angle, and its body is turned.
    const TSUCHI_Rotation turned{0, 100, 0};
    CHECK(set_pose(api, mod, handle(w.captain), &there, &turned) == TSUCHI_OK);
    CHECK(moved.actor_calls == 2 && moved.component_calls == 1 && moved.component == w.body);
    CHECK(near(moved.angles[1], 30) && near(moved.component_angles[1], 10));
    CHECK(api.pose(mod, handle(w.captain), &at) == TSUCHI_OK && near(at.rotation.yaw, 100) && near(at.location.y, 20));
    // A body that leans: the quarter turn goes on the body's own side of the product, which
    // only shows once the turns are not all about the same axis.
    const auto product = [](const float a[4], const float b[4], float out[4]) { // a's turn after b's
        out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    };
    const float quarter[4] = {0, 0, 0.70710678f, 0.70710678f}, quarter_back[4] = {0, 0, -0.70710678f, 0.70710678f};
    float leaning[4], expected[4], angles[3];
    quaternion_of(25, -90, 10, leaning);
    for (int i = 0; i < 4; ++i) fake::put(w.body + t::scene_component::rotation + 4 * i, leaning[i]);
    product(leaning, quarter, expected);
    tsuchi::math::to_angles(expected, angles);
    CHECK(api.pose(mod, handle(w.captain), &at) == TSUCHI_OK && near(at.rotation.pitch, angles[0], 0.05f) &&
          near(at.rotation.yaw, angles[1], 0.05f) && near(at.rotation.roll, angles[2], 0.05f) && !near(at.rotation.pitch, 25, 5));
    const TSUCHI_Rotation lean{15, 40, -5};
    float wanted[4];
    quaternion_of(15, 40, -5, wanted);
    product(wanted, quarter_back, expected);
    tsuchi::math::to_angles(expected, angles);
    CHECK(set_pose(api, mod, handle(w.captain), &there, &lean) == TSUCHI_OK && near(moved.component_angles[0], angles[0], 0.05f) &&
          near(moved.component_angles[1], angles[1], 0.05f) && near(moved.component_angles[2], angles[2], 0.05f));
    CHECK(api.pose(mod, handle(w.captain), &at) == TSUCHI_OK && near(at.rotation.pitch, 15, 0.05f) && near(at.rotation.yaw, 40, 0.05f) &&
          near(at.rotation.roll, -5, 0.05f));

    // Looking up and tilting come through too.
    const TSUCHI_Rotation tilted{20, -60, 5};
    CHECK(set_pose(api, mod, handle(w.red), &there, &tilted) == TSUCHI_OK && api.pose(mod, handle(w.red), &at) == TSUCHI_OK);
    CHECK(near(at.rotation.pitch, 20, 0.05f) && near(at.rotation.yaw, -60, 0.05f) && near(at.rotation.roll, 5, 0.05f));

    // What is refused.
    const int calls = moved.actor_calls;
    const TSUCHI_Vec3 nowhere{NAN, 0, 0};
    CHECK(set_pose(api, mod, handle(w.red), &nowhere, &facing) == TSUCHI_INVALID && set_pose(api, mod, handle(w.red), &there, nullptr) == TSUCHI_INVALID);
    CHECK(set_pose(api, mod, {to_handle(w.body)}, &there, &facing) == TSUCHI_INVALID);
    fake::put<uint64_t>(w.gate + t::game_actor::root, 0);
    CHECK(api.pose(mod, handle(w.gate), &at) == TSUCHI_NOT_READY && set_pose(api, mod, handle(w.gate), &there, &facing) == TSUCHI_NOT_READY);
    t::fn::set_actor_location_and_rotation = nullptr;
    CHECK(set_pose(api, mod, handle(w.red), &there, &facing) == TSUCHI_NOT_READY && moved.actor_calls == calls);
    t::fn::set_actor_location_and_rotation = set_location_and_rotation;
}

void lives() {
    // The game reuses an actor: the old handle is over, and the list gives the new one.
    const TSUCHI_Actor before = handle(w.red);
    int32_t team = 0;
    end_life(w.red);
    CHECK(api.team(mod, before, &team) == TSUCHI_NOT_FOUND);
    TSUCHI_Actor buffer[8];
    uint64_t count = 0;
    CHECK(api.list(mod, {}, buffer, 8, &count) == TSUCHI_OK && count == 4 && buffer[0].id != before.id && from_handle(buffer[0].id) == w.red);

    // The game destroys one: it leaves the list even while its link is still there.
    fake::destroy(w.gate);
    CHECK((listed() == std::vector<uint64_t>{w.red, w.bulborb, w.captain}));
    TSUCHI_Actor found{};
    CHECK(api.find_placed(mod, 77, &found) == TSUCHI_NOT_FOUND);

    // A ring that never comes back round does not hang.
    const uint64_t loop_a = fake::memory(t::actor_node::size), loop_b = fake::memory(t::actor_node::size);
    fake::put(loop_a + t::actor_node::next, loop_b), fake::put(loop_b + t::actor_node::next, loop_a);
    const uint64_t kept = fake::get<uint64_t>(w.last_node + t::actor_node::next);
    fake::put(w.last_node + t::actor_node::next, loop_a);
    CHECK(api.list(mod, {}, nullptr, 0, &count) == TSUCHI_CAPACITY && count == 3);
    fake::put(w.last_node + t::actor_node::next, kept);
}

// ---- Actors entering and leaving play ----------------------------------------
//
// The made-up game has three actor function datatable. Two hold its own functions
// and one holds something else. The test stands in for the game on both sides:
// it calls the tables' entries and the two pool functions as the game would,
// and its own functions sit at the far end of each chain as the game's own.

struct Heard {
    bool spawned;
    uint64_t handle;
    std::string kind;
    int32_t team;
    uint64_t placement;
    float x;
};
std::vector<Heard> heard;
std::string game_did; // which of the game's own functions ran, in order
uint64_t pool_gives = 0; // the actor the pool hands out next
const uint64_t kPool = 0x1111, kOtherPool = 0x2222;
int spawn_inside_handler = 0;
uint64_t extra_actor = 0;

uint64_t table_a() { return fake::main_image() + t::tables::actor_tables[0]; }
uint64_t table_b() { return fake::main_image() + t::tables::actor_tables[1]; }
uint64_t table_c() { return fake::main_image() + t::tables::actor_tables[2]; }
// A yes-or-no argument fills one byte; the rest of what arrives is whatever was there.
uint64_t game_register_a(uint64_t, uint64_t with, uint64_t, uint64_t) { return game_did += (with & 0xFF) ? "register-a+ " : "register-a ", 0; }
uint64_t game_register_b(uint64_t, uint64_t, uint64_t, uint64_t) { return game_did += "register-b ", 0; }
uint64_t game_end_a(uint64_t which, uint64_t reason, uint64_t, uint64_t) {
    // The game takes the actor apart here, so whoever wants to know where it was has to have looked already.
    if (const uint64_t root = fake::get<uint64_t>(which + t::game_actor::root)) fake::put(root + t::scene_component::location, -999.0f);
    return game_did += "end-a" + std::to_string(reason & 0xFF) + " ", 0;
}
uint64_t game_end_b(uint64_t, uint64_t, uint64_t, uint64_t) { return game_did += "end-b ", 0; }
uint64_t game_take(uint64_t, uint64_t, uint64_t, uint64_t) { return game_did += "take ", pool_gives; }
uint64_t game_park(uint64_t which, uint64_t, uint64_t, uint64_t);
uint64_t park_bystander = 0;
uint64_t game_return(uint64_t pool, uint64_t which, uint64_t, uint64_t) {
    game_did += "return ";
    // Whatever else the game parks along the way is not what the pool was asked to take back.
    if (park_bystander) thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::actor_park), park_bystander, 0);
    // Only its own actors are parked, and parking is a function of the game's own.
    if (pool == kPool && which) thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::actor_park), which, 0);
    return 0;
}
const std::string* park_watches = nullptr; // a log of some section's, and what it held when the game last parked an actor
std::string park_saw;
uint64_t game_park(uint64_t which, uint64_t, uint64_t, uint64_t) {
    game_did += "park ";
    if (park_watches) park_saw = *park_watches;
    fake::put<uint32_t>(which + t::game_actor::rest, uint32_t(t::constant::rest_parked));
    return 0;
}

// The game calling one of an actor's table entries, and the two pool functions.
void game_calls(uint64_t which, uint32_t offset, uint64_t argument) { thunks::call(fake::get<uint64_t>(fake::get<uint64_t>(which) + offset), which, argument); }
void registers(uint64_t which, bool with_children = false) { game_calls(which, uint32_t(t::slot_group::actor_register.offset), with_children); }
void ends_play(uint64_t which, uint8_t reason = 0) { game_calls(which, uint32_t(t::slot_group::actor_end_play.offset), reason); }
uint64_t pool_take() { return thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::actor_pool_take), kPool, 0); }
void pool_return(uint64_t pool, uint64_t which) { thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::actor_pool_return), pool, which); }

uint64_t tabled(uint64_t table, const std::u16string& name, const std::u16string& kind, uint64_t placement, float x) {
    const uint64_t address = actor(name, w.actor_class, kind, 3, placement, component(x, 0, 0, 0));
    fake::put(address, table);
    return address;
}

void on_spawned(void*, const TSUCHI_EntitySpawned* event) {
    heard.push_back({true, event->actor.id, event->kind, event->team, event->placement, event->location.x});
    if (spawn_inside_handler-- > 0) registers(extra_actor);
}
void on_destroyed(void*, const TSUCHI_EntityDestroyed* event) {
    heard.push_back({false, event->actor.id, event->kind, event->team, event->placement, event->location.x});
}

void install_events() {
    const uint64_t main = fake::main_image();
    const uint64_t enter = t::slot_group::actor_register.offset, leave = t::slot_group::actor_end_play.offset;
    const uint64_t stock[3][2] = {{0x1300, 0x1500}, {0x1400, 0x1500}, {0x1300, 0x1700}}; // the third table's second entry is nobody's
    for (int i = 0; i < 3; ++i) {
        const uint64_t table = main + t::tables::actor_tables[i];
        fake::put(table + enter, main + stock[i][0]), fake::put(table + leave, main + stock[i][1]);
        fake::code().push_back(main + stock[i][0]), fake::code().push_back(main + stock[i][1]);
    }
    // The far end of each chain: hooks of the test's own, put on first, that stand in for the game's functions.
    TSUCHI_Mod* game = tsuchi::mod_create("the.game");
    uint64_t id = 0;
    void* unused = nullptr;
    const auto under = [&](uint64_t entry, uint64_t (*function)(uint64_t, uint64_t, uint64_t, uint64_t)) {
        CHECK(tsuchi::hook_wrap(game, entry, 0, reinterpret_cast<void*>(function), 0, &id, &unused) == TSUCHI_OK);
    };
    under(table_a() + enter, game_register_a), under(table_a() + leave, game_end_a);
    under(table_b() + enter, game_register_b), under(table_b() + leave, game_end_b);
    for (unsigned site : {unsigned(t::code_site::actor_pool_take), unsigned(t::code_site::actor_pool_return), unsigned(t::code_site::actor_park)}) {
        const t::CodeSite& info = t::code_sites[site];
        fake::put<uint32_t>(main + info.address, 0x14000000u | uint32_t((info.entry - info.address) >> 2)); // as the patch leaves it
    }
    CHECK(tsuchi::hook_code_site(game, t::code_site::actor_pool_take, reinterpret_cast<void*>(&game_take), &id, &unused) == TSUCHI_OK);
    CHECK(tsuchi::hook_code_site(game, t::code_site::actor_pool_return, reinterpret_cast<void*>(&game_return), &id, &unused) == TSUCHI_OK);
    CHECK(tsuchi::hook_code_site(game, t::code_site::actor_park, reinterpret_cast<void*>(&game_park), &id, &unused) == TSUCHI_OK);

    const uint64_t c_enter = fake::get<uint64_t>(table_c() + enter), c_leave = fake::get<uint64_t>(table_c() + leave);
    tsuchi::actor_events_start(tsuchi::mod_create("tsuchi.self"));
    CHECK(tsuchi::actor_event_counts().tables == 2 && tsuchi::actor_event_counts().pool_sites == 3);
    // A table that holds something unfamiliar is left exactly as it was.
    CHECK(fake::get<uint64_t>(table_c() + enter) == c_enter && fake::get<uint64_t>(table_c() + leave) == c_leave);
}

void events() {
    const tsuchi::ActorEventCounts& counts = tsuchi::actor_event_counts();
    TSUCHI_Subscription spawned{}, destroyed{};
    CHECK(api.on_spawned(mod, on_spawned, nullptr, &spawned) == TSUCHI_OK && api.on_destroyed(mod, on_destroyed, nullptr, &destroyed) == TSUCHI_OK);

    // An actor enters play. The game's own function runs with what it was given, and the
    // event waits for the next frame, where it tells of the actor as it is by then.
    const uint64_t sprout = tabled(table_a(), u"Sprout_1", u"Sprout", 500, 10);
    registers(sprout, true);
    CHECK(game_did == "register-a+ " && counts.entered == 1 && counts.in_play == 1 && heard.empty());
    fake::put(fake::get<uint64_t>(sprout + t::game_actor::root) + t::scene_component::location, 25.0f);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && heard[0].spawned && from_handle(heard[0].handle) == sprout && heard[0].kind == "Sprout" && heard[0].team == 3 &&
          heard[0].placement == 500 && heard[0].x == 25);
    const uint64_t sprout_handle = heard[0].handle;
    // Registering again, as the game does for an actor attached to another, is not entering again.
    registers(sprout);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && counts.entered == 1 && game_did == "register-a+ register-a ");

    // The other table leads to its own function.
    const uint64_t gate = tabled(table_b(), u"Gate_9", u"Gate", 501, 40);
    game_did.clear();
    registers(gate);
    CHECK(game_did == "register-b " && counts.in_play == 2);

    // An actor is destroyed. The event tells of it as it was, with the handle it had.
    game_did.clear();
    ends_play(sprout, 3);
    fake::destroy(sprout);
    CHECK(game_did == "end-a3 " && counts.destroyed == 1 && counts.in_play == 1 && heard.size() == 1);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 3 && heard[1].spawned && from_handle(heard[1].handle) == gate);
    CHECK(!heard[2].spawned && heard[2].handle == sprout_handle && !from_handle(heard[2].handle) && heard[2].kind == "Sprout" &&
          heard[2].placement == 500 && heard[2].x == 25);
    ends_play(sprout); // twice is not leaving twice
    CHECK(counts.destroyed == 1);

    // Entering and leaving within one frame: both are told, in order, and the first has nothing left to describe.
    heard.clear();
    const uint64_t spark = tabled(table_a(), u"Spark", u"Spark", 0, 7);
    registers(spark);
    ends_play(spark);
    fake::destroy(spark);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 2 && heard[0].spawned && heard[0].kind.empty() && !heard[1].spawned && heard[1].kind == "Spark" && heard[1].x == 7 &&
          heard[0].handle == heard[1].handle);

    // An actor taken from a pool enters play, however the game goes about it.
    heard.clear(), game_did.clear();
    const uint64_t pikmin = tabled(table_a(), u"Pikmin_4", u"PikminRed", 0, 60);
    pool_gives = pikmin;
    CHECK(pool_take() == pikmin && game_did == "take ");
    registers(pikmin);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && heard[0].spawned && from_handle(heard[0].handle) == pikmin && counts.entered == 4);
    const TSUCHI_Actor first_use{heard[0].handle};

    // Asked to put it in a pool that is not its own, the game does nothing, and neither does this,
    // even for an actor the game has already marked as finished.
    int32_t first_team = 0;
    fake::put<uint8_t>(pikmin + t::game_actor::terminated, 1);
    pool_return(kOtherPool, pikmin);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && counts.put_away == 0 && api.team(mod, first_use, &first_team) == TSUCHI_OK);

    // Put away for real: it leaves play, and the handle to that use of it is over though the object lives on.
    pool_return(kPool, pikmin);
    CHECK(counts.put_away == 1 && live(pikmin) && !from_handle(first_use.id) && api.team(mod, first_use, &first_team) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 2 && !heard[1].spawned && heard[1].handle == first_use.id && heard[1].kind == "PikminRed" && heard[1].x == 60);
    pool_return(kPool, pikmin); // again, as the game sometimes does
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 2 && counts.put_away == 1);

    // Taken out again, it is a new use with a new handle.
    fake::put<uint32_t>(pikmin + t::game_actor::rest, 0);
    CHECK(pool_take() == pikmin);
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 3 && heard[2].spawned && heard[2].handle != first_use.id && from_handle(heard[2].handle) == pikmin);
    // A pool with nothing to give gives nothing, and nothing is told.
    pool_gives = 0;
    CHECK(pool_take() == 0 && counts.entered == 5);

    // One the game parked for some other reason, such as being far from everyone, is still in play
    // until a pool takes it back; and a pool that is not its own still does nothing to it.
    const uint64_t dozer = tabled(table_a(), u"Dozer", u"Dozer", 0, 9);
    registers(dozer);
    fake::put<uint32_t>(dozer + t::game_actor::rest, uint32_t(t::constant::rest_parked));
    const uint32_t away = counts.put_away;
    pool_return(kOtherPool, dozer);
    CHECK(counts.put_away == away && from_handle(to_handle(dozer)) == dozer);
    const uint64_t dozer_handle = to_handle(dozer);
    park_bystander = tabled(table_a(), u"Bystander", u"Bystander", 0, 0);
    pool_return(kOtherPool, dozer); // the game parks someone else meanwhile: still nothing to do with this one
    park_bystander = 0;
    CHECK(counts.put_away == away && from_handle(dozer_handle) == dozer);
    pool_return(kPool, dozer);
    CHECK(counts.put_away == away + 1 && !from_handle(dozer_handle));
    tsuchi::frame(0.03f);

    // A handler that makes something else happen: that is told on the next frame, and not lost.
    heard.clear();
    extra_actor = tabled(table_b(), u"Echo", u"Echo", 0, 1);
    spawn_inside_handler = 1;
    registers(tabled(table_a(), u"Caller", u"Caller", 0, 2));
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && heard[0].kind == "Caller");
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 2 && heard[1].kind == "Echo");

    // A kind with a very long name is told whole.
    heard.clear();
    registers(tabled(table_a(), u"Verbose", std::u16string(300, u'K').c_str(), 0, 2));
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && heard[0].kind == std::string(300, 'K'));

    // With nobody listening the count goes on and nothing is kept for later.
    heard.clear();
    CHECK(tsuchi::host.cancel(mod, spawned) == TSUCHI_OK && tsuchi::host.cancel(mod, destroyed) == TSUCHI_OK);
    const uint32_t entered = counts.entered;
    const uint64_t quiet = tabled(table_a(), u"Quiet", u"Quiet", 0, 3);
    registers(quiet);
    ends_play(quiet);
    CHECK(counts.entered == entered + 1);
    CHECK(api.on_spawned(mod, on_spawned, nullptr, &spawned) == TSUCHI_OK && api.on_destroyed(mod, on_destroyed, nullptr, &destroyed) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard.empty());

    // Something that is not one of the game's actors goes through untouched.
    const uint64_t stranger = fake::object(u"Stranger", w.component_class, 0x100);
    fake::put(stranger, table_a());
    game_did.clear();
    registers(stranger);
    ends_play(stranger);
    tsuchi::frame(0.03f);
    CHECK(game_did == "register-a end-a0 " && heard.empty() && counts.entered == entered + 1);
    CHECK(tsuchi::host.cancel(mod, spawned) == TSUCHI_OK && tsuchi::host.cancel(mod, destroyed) == TSUCHI_OK);
}

uint64_t physical(uint64_t which);

// ---- Spawning and removing -----------------------------------------------------
//
// The test stands in for the game's spawner and its kill. The spawner makes an
// actor from the request it is given and registers it, as the game's does; the
// kill only queues, and the removal happens at the next world update.

struct Spawner {
    int made = 0, requests_destroyed = 0, locks = 0, kills = 0, records_destroyed = 0;
    bool refuse = false, request_good = false, record_good = false, physical = false;
    int32_t team = 0;
    int last_quiet = -1, last_cause = -1; // what the last kill was told
    uint64_t last_killer = 0;
    std::vector<uint64_t> queued; // actors the game will remove at its next world update
} spawner;

void* game_spawn_now(void* framework, void* request_pointer) {
    const auto request = reinterpret_cast<uint64_t>(request_pointer);
    namespace r = t::spawn_request;
    namespace c = t::constant;
    spawner.team = fake::get<int32_t>(request + r::team);
    spawner.request_good = reinterpret_cast<uint64_t>(framework) == w.framework &&
                           fake::get<uint32_t>(request + r::object_flags) == c::spawn_object_flags &&
                           fake::get<float>(request + r::scale) == 1 && fake::get<float>(request + r::scale + 4) == 1 &&
                           fake::get<float>(request + r::scale + 8) == 1 && fake::get<float>(request + r::velocity) == 0 &&
                           fake::get<int32_t>(request + r::team_editor) == int32_t(c::spawn_team_editor) &&
                           fake::get<uint8_t>(request + r::color) == c::spawn_color_none && fake::get<uint8_t>(request + r::leaf) == c::spawn_leaf_none &&
                           fake::get<int32_t>(request + r::user_value) == -1 &&
                           fake::get<int32_t>(request + r::delegate_threshold) == int32_t(c::spawn_delegate_threshold) &&
                           fake::get<uint8_t>(request + r::decoration) == c::spawn_decoration_none &&
                           fake::get<uint64_t>(request + r::node_self) == request + r::node && fake::get<uint64_t>(request + r::node) == 0;
    if (spawner.refuse) return nullptr;
    ++spawner.made;
    const uint64_t made = actor(u"Made", fake::get<uint64_t>(request + r::class_), u"Made", spawner.team, ~uint64_t(0),
                                component(fake::get<float>(request + r::location), fake::get<float>(request + r::location + 4),
                                          fake::get<float>(request + r::location + 8), fake::get<float>(request + r::rotation + 4)));
    fake::put(made, table_a());
    if (spawner.physical) physical(made);
    registers(made);
    return reinterpret_cast<void*>(made);
}
void game_request_destroy(void*) { ++spawner.requests_destroyed; }
uint64_t locked_at = 0;
void game_record_lock(void* lock, uint64_t one, uint64_t zero) {
    spawner.locks += one == 1 && zero == 0;
    locked_at = reinterpret_cast<uint64_t>(lock);
}
void game_kill(void* record_pointer, void* which) {
    const auto record = reinterpret_cast<uint64_t>(record_pointer), target = reinterpret_cast<uint64_t>(which);
    namespace k = t::kill_record;
    spawner.record_good = fake::get<uint64_t>(record + k::head) == record && fake::get<uint64_t>(record + k::tail) == record &&
                          locked_at == record + k::lock && fake::get<uint32_t>(record + k::magic) == t::constant::kill_magic &&
                          fake::get<uint8_t>(record + k::quiet) == 1 && fake::get<uint8_t>(record + k::cause) == 0 &&
                          fake::get<uint64_t>(record + k::killer) == 0 && record % 16 == 0;
    spawner.last_quiet = fake::get<uint8_t>(record + k::quiet), spawner.last_cause = fake::get<uint8_t>(record + k::cause);
    spawner.last_killer = fake::get<uint64_t>(record + k::killer);
    ++spawner.kills;
    fake::put<uint8_t>(target + t::game_actor::kill_queued, 1);
    spawner.queued.push_back(target);
}
void game_record_destroy(void*) { ++spawner.records_destroyed; }

// The game's next world update: what was queued for removal goes, destroyed or back to its pool.
void world_update(bool pooled = false) {
    for (uint64_t which : spawner.queued) {
        if (pooled) {
            pool_return(kPool, which);
        } else {
            ends_play(which);
            fake::destroy(which);
        }
    }
    spawner.queued.clear();
}

void spawning() {
    const tsuchi::ActorEventCounts& counts = tsuchi::actor_event_counts();
    t::fn::spawn_now = game_spawn_now, t::fn::spawn_request_destroy = game_request_destroy;
    t::fn::kill_record_lock = game_record_lock, t::fn::kill_actor = game_kill, t::fn::kill_record_destroy = game_record_destroy;
    TSUCHI_Subscription spawned{}, destroyed{};
    CHECK(api.on_spawned(mod, on_spawned, nullptr, &spawned) == TSUCHI_OK && api.on_destroyed(mod, on_destroyed, nullptr, &destroyed) == TSUCHI_OK);
    heard.clear();
    const TSUCHI_Object pikmin_class{to_handle(w.pikmin_class)};
    const TSUCHI_Vec3 there{1, 2, 3};
    const TSUCHI_Rotation facing{0, 90, 0};
    const uint32_t owned = tsuchi::owned_count(mod);

    // A spawned actor is in play at once, where it was asked for, and belongs to the mod.
    TSUCHI_Actor made{};
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, false, &made) == TSUCHI_OK && spawner.made == 1 && spawner.request_good);
    const uint64_t made_address = from_handle(made.id);
    CHECK(made_address && class_of(made_address) == w.pikmin_class && spawner.requests_destroyed == 1 && tsuchi::owned_count(mod) == owned + 1);
    CHECK(spawner.team == int32_t(t::constant::spawn_team_default)); // the class's own team
    TSUCHI_Pose at{};
    CHECK(api.pose(mod, made, &at) == TSUCHI_OK && at.location.x == 1 && at.location.z == 3 && near(at.rotation.yaw, 90));
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 1 && heard[0].spawned && heard[0].handle == made.id && heard[0].kind == "Made");
    TSUCHI_Actor on_team{};
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, 1, false, &on_team) == TSUCHI_OK && spawner.team == 1);

    // What cannot be spawned.
    TSUCHI_Actor none{77};
    const TSUCHI_Vec3 nowhere{NAN, 0, 0};
    const int before = spawner.made;
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -2, false, &none) == TSUCHI_INVALID && api.spawn(mod, pikmin_class, &nowhere, &facing, -1, false, &none) == TSUCHI_INVALID);
    CHECK(api.spawn(mod, {to_handle(w.component_class)}, &there, &facing, -1, false, &none) == TSUCHI_INVALID); // a class, but not of actor
    CHECK(api.spawn(mod, {made.id}, &there, &facing, -1, false, &none) == TSUCHI_INVALID);                         // an actor, not a class
    CHECK(api.spawn(mod, {12345}, &there, &facing, -1, false, &none) == TSUCHI_NOT_FOUND);
    spawner.refuse = true;
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, false, &none) == TSUCHI_UNSUPPORTED && spawner.requests_destroyed == 3);
    spawner.refuse = false;
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, 0);
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, false, &none) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, w.framework);
    CHECK(spawner.made == before && none.id == 77 && tsuchi::owned_count(mod) == owned + 2);

    // Removing: the game is asked quietly, once, and the actor leaves at its next world update.
    heard.clear();
    CHECK(api.destroy(mod, made) == TSUCHI_OK && spawner.kills == 1 && spawner.record_good && spawner.locks == 1 && spawner.records_destroyed == 1);
    int32_t made_team = 0;
    CHECK(api.team(mod, made, &made_team) == TSUCHI_OK && api.destroy(mod, made) == TSUCHI_BUSY && spawner.kills == 1);
    CHECK(tsuchi::owned_count(mod) == owned + 1); // no longer the mod's to clean up
    world_update();
    tsuchi::frame(0.03f);
    CHECK(heard.size() == 2 && !heard[1].spawned && heard[1].handle == made.id && api.destroy(mod, made) == TSUCHI_NOT_FOUND);

    // Another mod may remove what the game made, and not what this mod made.
    TSUCHI_Mod* other = tsuchi::mod_create("actor.other");
    CHECK(api.destroy(other, on_team) == TSUCHI_DENIED && spawner.kills == 1);
    CHECK(api.destroy(other, handle(w.bulborb)) == TSUCHI_OK && spawner.kills == 2);
    spawner.queued.clear(); // the made-up game lets the bulborb live

    // An actor from a pool goes back to it, and its handle ends there.
    CHECK(api.destroy(mod, on_team) == TSUCHI_OK);
    const uint32_t put_away = counts.put_away;
    const uint64_t pooled = from_handle(on_team.id);
    world_update(true);
    CHECK(counts.put_away == put_away + 1 && live(pooled) && !from_handle(on_team.id) && tsuchi::owned_count(mod) == owned);

    // A spawned actor the game removes for its own reasons is nothing the mod has to clean up.
    TSUCHI_Actor doomed{};
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, false, &doomed) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    ends_play(from_handle(doomed.id));
    fake::destroy(from_handle(doomed.id));
    CHECK(tsuchi::owned_count(mod) == owned);

    // A mod that stops takes what it spawned with it, except what it handed to the game.
    TSUCHI_Mod* careless = tsuchi::mod_create("actor.careless");
    TSUCHI_Actor mine{}, kept{};
    CHECK(api.spawn(careless, pikmin_class, &there, &facing, -1, false, &mine) == TSUCHI_OK && api.spawn(careless, pikmin_class, &there, &facing, -1, true, &kept) == TSUCHI_OK);
    CHECK(tsuchi::owned_count(careless) == 1);
    const int kills = spawner.kills;
    tsuchi::mod_stop(careless);
    CHECK(spawner.kills == kills + 1 && spawner.queued.size() == 1 && spawner.queued[0] == from_handle(mine.id));
    world_update();
    CHECK(!from_handle(mine.id) && from_handle(kept.id));
    CHECK(api.destroy(mod, kept) == TSUCHI_OK); // handed to the game, so anyone's to remove
    world_update();

    // Without the game's functions nothing is attempted.
    TSUCHI_Actor last{};
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, true, &last) == TSUCHI_OK);
    t::fn::kill_actor = nullptr;
    CHECK(api.destroy(mod, last) == TSUCHI_NOT_READY);
    t::fn::kill_actor = game_kill;
    t::fn::spawn_now = nullptr;
    CHECK(api.spawn(mod, pikmin_class, &there, &facing, -1, false, &none) == TSUCHI_UNSUPPORTED);
    t::fn::spawn_now = game_spawn_now;
    tsuchi::frame(0.03f);
    CHECK(tsuchi::host.cancel(mod, spawned) == TSUCHI_OK && tsuchi::host.cancel(mod, destroyed) == TSUCHI_OK);
}

// ---- Velocity, throwing and the grounded move ---------------------------------

struct Mover {
    int requests = 0, warps = 0;
    uint64_t actor = 0;
    float velocity[3] = {}, warped_to[3] = {}, warped_angles[3] = {};
    bool warp_flags = false;
} mover;

void game_request_velocity(void* which, void* vector) {
    ++mover.requests;
    mover.actor = reinterpret_cast<uint64_t>(which);
    std::memcpy(mover.velocity, vector, sizeof mover.velocity);
    std::memcpy(reinterpret_cast<void*>(mover.actor + t::game_actor::velocity), vector, 12); // the game applies it
}
void game_warp(void* which, void* location, void* angles, bool turn, bool ground, bool height) {
    ++mover.warps;
    mover.actor = reinterpret_cast<uint64_t>(which);
    std::memcpy(mover.warped_to, location, 12), std::memcpy(mover.warped_angles, angles, 12);
    mover.warp_flags = turn && ground && height;
    const uint64_t root = fake::get<uint64_t>(mover.actor + t::game_actor::root);
    fake::put(root + t::scene_component::location, mover.warped_to[0]), fake::put(root + t::scene_component::location + 4, mover.warped_to[1]);
    fake::put(root + t::scene_component::location + 8, mover.warped_to[2] + 5); // set down on the ground, a little higher
}

// Gives an actor a physics body of its own that is running, standing on the ground.
uint64_t physical(uint64_t which) {
    namespace a = t::game_actor;
    const uint64_t body = fake::object(u"Body", w.primitive_class, 0x80);
    fake::put(body + t::scene_component::owner, which);
    fake::put(fake::get<uint64_t>(which + a::root) + t::scene_component::owner, which);
    fake::put(which + a::physics, body);
    fake::put<uint8_t>(which + a::body_awake, 1), fake::put<uint8_t>(which + a::simulating, 1), fake::put<uint8_t>(which + a::simulating_known, 1);
    return body;
}
int32_t holds(uint64_t which) { return fake::get<int32_t>(which + t::game_actor::awake_holds); }
void frames(int count) {
    for (int i = 0; i < count; ++i) tsuchi::frame(0.03f);
}

void moving() {
    namespace a = t::game_actor;
    t::fn::request_velocity = game_request_velocity, t::fn::warp_actor = game_warp;
    fake::put<uint64_t>(w.framework + t::game_framework::ground_query, fake::memory(16));
    const uint64_t rock = tabled(table_a(), u"Rock", u"Rock", 0, 0);
    registers(rock);
    const uint64_t body = physical(rock);

    // How fast it is going: its body's word while that is awake, else its root's.
    TSUCHI_Vec3 speed{};
    fake::put(rock + a::velocity, 3.0f), fake::put(rock + a::velocity + 4, 4.0f), fake::put(rock + a::velocity + 8, 5.0f);
    fake::put(fake::get<uint64_t>(rock + a::root) + t::scene_component::velocity, 7.0f);
    CHECK(api.velocity(mod, handle(rock), &speed) == TSUCHI_OK && speed.x == 3 && speed.y == 4 && speed.z == 5);
    fake::put<uint8_t>(rock + a::body_awake, 0);
    CHECK(api.velocity(mod, handle(rock), &speed) == TSUCHI_OK && speed.x == 7 && speed.y == 0);
    fake::put<uint8_t>(rock + a::body_awake, 1);

    // What cannot be thrown.
    const TSUCHI_Vec3 up{100, 0, 300}, nowhere{0, NAN, 0};
    CHECK(api.launch(mod, handle(w.red), &up) == TSUCHI_UNSUPPORTED); // nothing to throw: no physics body
    fake::put<uint64_t>(body + t::scene_component::attach_parent, w.body);
    CHECK(api.launch(mod, handle(rock), &up) == TSUCHI_UNSUPPORTED); // being carried
    fake::put<uint64_t>(body + t::scene_component::attach_parent, 0);
    fake::put<uint8_t>(rock + a::simulating, 0);
    CHECK(api.launch(mod, handle(rock), &up) == TSUCHI_UNSUPPORTED); // its physics is switched off
    fake::put<uint8_t>(rock + a::simulating_known, 0);
    CHECK(api.launch(mod, handle(rock), &nowhere) == TSUCHI_INVALID && api.launch(mod, {12345}, &up) == TSUCHI_NOT_FOUND && mover.requests == 0);

    // A throw: asked for at once, whole, and the actor is held awake.
    fake::put<uint8_t>(rock + a::speed_clamp, 1), fake::put<uint8_t>(rock + a::keep_vertical, 1);
    CHECK(api.launch(mod, handle(rock), &up) == TSUCHI_OK && mover.requests == 1 && mover.actor == rock && mover.velocity[0] == 100 && mover.velocity[2] == 300);
    CHECK(!fake::get<uint8_t>(rock + a::speed_clamp) && !fake::get<uint8_t>(rock + a::keep_vertical) && holds(rock) == 1 && tsuchi::motion_count() == 1);
    // While it still stands on the ground, where its own movement drags the velocity back, it is asked for again.
    frames(3);
    CHECK(mover.requests == 4 && holds(rock) == 1);
    // Thrown again before it has left: one actor followed, one hold, the new velocity.
    const TSUCHI_Vec3 harder{0, 0, 900};
    CHECK(api.launch(mod, handle(rock), &harder) == TSUCHI_OK && tsuchi::motion_count() == 1 && holds(rock) == 1 && mover.velocity[2] == 900);
    // In the air it is left to the game's physics: nothing more is asked, and its own movement's requests are dropped.
    fake::put<uint32_t>(rock + a::air_steps, 5);
    const int asked = mover.requests;
    for (const uint32_t flag : {a::velocity_pending, a::velocity_added_1, a::velocity_added_2, a::velocity_added_3, a::keep_vertical}) fake::put<uint8_t>(rock + flag, 1);
    frames(2);
    CHECK(mover.requests == asked && holds(rock) == 1);
    for (const uint32_t flag : {a::velocity_pending, a::velocity_added_1, a::velocity_added_2, a::velocity_added_3, a::keep_vertical}) CHECK(!fake::get<uint8_t>(rock + flag));
    // Landed: the hold is let go, with the frame's grace the game gives, and it is followed no more.
    fake::put<uint32_t>(rock + a::air_steps, 0);
    const int32_t grace = fake::get<int32_t>(rock + a::freeze_delay);
    frames(1);
    CHECK(holds(rock) == 0 && fake::get<int32_t>(rock + a::freeze_delay) == grace + 1 && tsuchi::motion_count() == 0);
    fake::put<uint8_t>(rock + a::velocity_pending, 1);
    frames(2);
    CHECK(fake::get<uint8_t>(rock + a::velocity_pending) == 1 && mover.requests == asked);

    // A newly placed actor can already count as off-ground while its queued launch
    // has not run. Actor pause preserves the request beyond the active hold budget;
    // timer-only pause permits a fresh delivery, then an independent native update
    // consumes it before the framework may clear further velocity requests.
    const auto prior_mover = mover;
    const uint64_t instance_slot = fake::main_image() + t::data::game_instance;
    const uint64_t prior_instance = fake::get<uint64_t>(instance_slot);
    const uint64_t pause_instance = fake::memory(t::game_instance::pause_manager + sizeof(uint64_t));
    const uint64_t pause_manager = fake::memory(t::pause_manager::size);
    fake::put(instance_slot, pause_instance);
    fake::put(pause_instance + t::game_instance::pause_manager, pause_manager);
    fake::put(pause_manager, fake::main_image() + t::data::pause_manager_table);
    const uint64_t paused_actor = tabled(table_a(), u"PausedLaunch", u"Rock", 0, 0);
    registers(paused_actor), physical(paused_actor);
    fake::put<uint32_t>(paused_actor + a::air_steps, 5);
    fake::put<uint32_t>(pause_manager + t::pause_manager::active, TSUCHI_PAUSE_ACTORS | TSUCHI_PAUSE_TIMER);
    CHECK(api.launch(mod, handle(paused_actor), &harder) == TSUCHI_OK);
    const int queued_before_pause = mover.requests;
    fake::put<uint8_t>(paused_actor + a::velocity_pending, 1);
    frames(320);
    CHECK(mover.requests == queued_before_pause && holds(paused_actor) == 1 && tsuchi::motion_following(paused_actor));
    CHECK(fake::get<uint8_t>(paused_actor + a::velocity_pending) == 1);
    fake::put<uint32_t>(pause_manager + t::pause_manager::active, TSUCHI_PAUSE_TIMER);
    frames(1);
    CHECK(mover.requests == queued_before_pause + 1 && mover.actor == paused_actor && mover.velocity[2] == 900);
    CHECK(fake::get<uint8_t>(paused_actor + a::velocity_pending) == 1 && holds(paused_actor) == 1);
    // The stand-in game's next active actor update applies the queued velocity.
    fake::put(paused_actor + a::velocity + 8, mover.velocity[2]);
    fake::put<uint8_t>(paused_actor + a::velocity_pending, 0);
    frames(1);
    CHECK(fake::get<float>(paused_actor + a::velocity + 8) == 900 && mover.requests == queued_before_pause + 1);
    // Pausing an already delivered flight must not relaunch it.
    fake::put<uint32_t>(pause_manager + t::pause_manager::active, TSUCHI_PAUSE_ACTORS);
    frames(3);
    fake::put<uint32_t>(pause_manager + t::pause_manager::active, 0);
    frames(1);
    CHECK(mover.requests == queued_before_pause + 1);
    fake::put<uint32_t>(paused_actor + a::air_steps, 0);
    frames(1);
    CHECK(holds(paused_actor) == 0 && !tsuchi::motion_following(paused_actor));
    ends_play(paused_actor), fake::destroy(paused_actor);
    fake::put(instance_slot, prior_instance);
    mover = prior_mover;

    // One that never leaves the ground is asked for a few more times and then let be.
    CHECK(api.launch(mod, handle(rock), &up) == TSUCHI_OK);
    frames(20);
    CHECK(mover.requests == asked + 1 + 8 && holds(rock) == 1 && tsuchi::motion_count() == 1);
    frames(300);
    CHECK(holds(rock) == 0 && tsuchi::motion_count() == 0);

    // Destroyed in flight, there is nothing left to let go of.
    const uint64_t pebble = tabled(table_a(), u"Pebble", u"Pebble", 0, 0);
    registers(pebble);
    physical(pebble);
    CHECK(api.launch(mod, handle(pebble), &up) == TSUCHI_OK && tsuchi::motion_count() == 1);
    ends_play(pebble);
    CHECK(tsuchi::motion_count() == 0 && holds(pebble) == 1); // forgotten at once, and the actor is not touched on its way out
    fake::destroy(pebble);
    frames(1);
    CHECK(tsuchi::motion_count() == 0);
    // Put away in flight, the actor lives on, so its hold is let go there and then.
    const uint64_t sprite = tabled(table_a(), u"Sprite", u"Sprite", 0, 0);
    registers(sprite);
    physical(sprite);
    CHECK(api.launch(mod, handle(sprite), &up) == TSUCHI_OK && holds(sprite) == 1);
    pool_return(kPool, sprite);
    CHECK(holds(sprite) == 0 && tsuchi::motion_count() == 0);

    // A newly made actor is held awake until it has settled where it was put, with nothing thrown.
    spawner.physical = true;
    TSUCHI_Actor made{};
    const TSUCHI_Vec3 there{0, 0, 50};
    const TSUCHI_Rotation facing{0, 0, 0};
    const int before = mover.requests;
    CHECK(api.spawn(mod, {to_handle(w.pikmin_class)}, &there, &facing, -1, true, &made) == TSUCHI_OK && holds(from_handle(made.id)) == 1);
    frames(2);
    CHECK(holds(from_handle(made.id)) == 1);
    frames(1);
    CHECK(holds(from_handle(made.id)) == 0 && tsuchi::motion_count() == 0 && mover.requests == before);
    spawner.physical = false;

    // The grounded move: the game's own warp, given copies of what was asked, with everything it can do switched on.
    const TSUCHI_Vec3 place{10, 20, 30};
    const TSUCHI_Rotation turned{0, 45, 0};
    TSUCHI_Pose at{};
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_OK && mover.warps == 1 && mover.actor == rock && mover.warp_flags);
    CHECK(mover.warped_to[0] == 10 && mover.warped_to[2] == 30 && mover.warped_angles[1] == 45);
    CHECK(api.pose(mod, handle(rock), &at) == TSUCHI_OK && at.location.x == 10 && at.location.z == 35); // the ground decides the height
    // A character is set down the same way, and turned by its body so it faces as asked.
    physical(w.captain);
    const int turns = moved.component_calls;
    const TSUCHI_Rotation about{0, -30, 0};
    CHECK(api.warp(mod, handle(w.captain), &place, &about) == TSUCHI_OK && moved.component_calls == turns + 1 && moved.component == w.body);
    CHECK(api.pose(mod, handle(w.captain), &at) == TSUCHI_OK && near(at.rotation.yaw, -30) && at.location.z == 35);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_OK && moved.component_calls == turns + 1); // nothing to turn on a rock
    // It calls off a throw.
    CHECK(api.launch(mod, handle(rock), &up) == TSUCHI_OK && holds(rock) == 1);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_OK && holds(rock) == 0 && tsuchi::motion_count() == 0);

    // What cannot be warped.
    const int warps = mover.warps;
    CHECK(api.warp(mod, handle(w.red), &place, &turned) == TSUCHI_UNSUPPORTED); // no body
    const uint64_t root_of = fake::get<uint64_t>(rock + a::root);
    fake::put(body + t::scene_component::owner, w.red);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_UNSUPPORTED); // a body that is another actor's
    fake::put(body + t::scene_component::owner, rock);
    fake::put(rock + a::physics, w.body);
    fake::put(w.body + t::scene_component::owner, rock);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_UNSUPPORTED); // its own part, but not a physics body
    fake::put<uint64_t>(w.body + t::scene_component::owner, 0);
    fake::put(rock + a::physics, body);
    fake::put(root_of + t::scene_component::owner, w.red);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_UNSUPPORTED); // a root that is another actor's
    fake::put(root_of + t::scene_component::owner, rock);
    const uint64_t others_part = fake::object(u"Hand", w.primitive_class, 0x80);
    fake::put(others_part + t::scene_component::owner, w.red);
    fake::put(root_of + t::scene_component::attach_parent, others_part);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_UNSUPPORTED); // hanging from another actor
    fake::put<uint64_t>(others_part + t::scene_component::owner, 0);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_OK && mover.warps == warps + 1); // hanging from nobody's part is fine
    fake::put<uint64_t>(root_of + t::scene_component::attach_parent, 0);
    fake::put<uint8_t>(rock + a::kill_queued, 1);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_NOT_FOUND && api.launch(mod, handle(rock), &up) == TSUCHI_UNSUPPORTED);
    fake::put<uint8_t>(rock + a::kill_queued, 0);
    fake::put<uint8_t>(rock + a::terminated, 1);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_NOT_FOUND);
    fake::put<uint8_t>(rock + a::terminated, 0);
    const uint64_t query = fake::get<uint64_t>(w.framework + t::game_framework::ground_query);
    fake::put<uint64_t>(w.framework + t::game_framework::ground_query, 0);
    CHECK(api.warp(mod, handle(rock), &place, &turned) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(w.framework + t::game_framework::ground_query, query);
    CHECK(api.warp(mod, handle(rock), &nowhere, &turned) == TSUCHI_INVALID && mover.warps == warps + 1);
    w.rock = rock;
}

// ---- Suspending ------------------------------------------------------------------

struct Switches {
    std::string log; // every switch the game was asked to throw, in order
    std::deque<uint64_t> waiting; // messages in the one queue the test has
    int destroyed = 0;

    std::vector<tsuchi::TakenMessage> taken;
} switches;
uint64_t brute = 0, brain = 0, legs = 0; // an actor with a mind and legs of its own

std::map<uint64_t, bool> tick_states;
uint64_t tick_function_type = 0, tick_component_class = 0;
bool tick_on(uint64_t object) { const auto found = tick_states.find(object); return found == tick_states.end() || found->second; }
void game_request_tick(void* which, bool on) { fake::put<uint8_t>(reinterpret_cast<uint64_t>(which) + t::game_actor::tick_requested, on); tick_states[reinterpret_cast<uint64_t>(which)] = on; switches.log += std::string(reinterpret_cast<uint64_t>(which) == brute ? "actor" : "other") + (on ? "+ " : "- "); }
void game_component_tick(void* which, bool on) {
    const auto component = reinterpret_cast<uint64_t>(which);
    if (tick_component_class && derives_from(class_of(component), tick_component_class))
        fake::put<uint8_t>(component + t::game_component::tick_requested, on);
    tick_states[component] = on;
    switches.log += std::string(component == brain ? "brain" : component == legs ? "mover" : "part") + (on ? "+ " : "- ");
}
void* game_pop(void*) {
    if (switches.waiting.empty()) return nullptr;
    const uint64_t next = switches.waiting.front();
    switches.waiting.pop_front();
    return reinterpret_cast<void*>(next);
}
void game_message_destroy(void*) { ++switches.destroyed; }
void message_taken(const tsuchi::TakenMessage& message) { switches.taken.push_back(message); }

// A message of a kind, as the game makes one: a function table that says what it is, who sent it, and how many queues hold it.
uint64_t message(uint64_t table_offset, uint64_t sender, int32_t references = 1) {
    const uint64_t at = fake::memory(0x40), table = fake::main_image() + table_offset;
    fake::put(at, table);
    fake::put(table + t::message_table::destroy, &game_message_destroy);
    if (sender) {
        to_handle(sender); // gives it a serial, as the game does when it first refers to an object weakly
        fake::put(at + t::message::sender, fake::get<int32_t>(sender + t::object::index));
        fake::put(at + t::message::sender + 4, fake::get<int32_t>(fake::item_at(fake::get<int32_t>(sender + t::object::index)) + t::object_item::serial));
    } else {
        fake::put(at + t::message::sender, int32_t(-1));
    }
    fake::put(at + t::message::references, references);
    return at;
}

void suspending() {
    fake::start_engine();
    constexpr TSUCHI_EntityParts kLogic = TSUCHI_ENTITY_PARTS_LOGIC, kMovement = TSUCHI_ENTITY_PARTS_MOVEMENT;
    const uint64_t main = fake::main_image();
    for (auto function : {reinterpret_cast<uint64_t>(&game_request_tick), reinterpret_cast<uint64_t>(&game_component_tick),
                          reinterpret_cast<uint64_t>(&game_pop), reinterpret_cast<uint64_t>(&game_message_destroy)})
        fake::code().push_back(function);
    fake::put(table_a() + t::actor_table::request_tick, &game_request_tick);
    const uint64_t component_table = fake::memory(0x40);
    fake::put(component_table + t::component_table::set_tick_enabled, &game_component_tick);
    fake::put(w.red, fake::engine().vtable);
    fake::put(fake::engine().vtable + t::component_table::set_tick_enabled, &game_component_tick);
    tick_function_type = fake::object(u"Function", w.root);
    for (uint64_t table : {table_a(), table_b(), component_table}) fake::put(table + t::object_vtable::process_event, &fake::engine_process);
    for (const auto specification : {std::pair<uint64_t, const char16_t*>{w.actor_class, u"IsActorTickEnabled"}, {w.component_class, u"IsComponentTickEnabled"}}) {
        const uint64_t function = fake::function(specification.first, tick_function_type, specification.second, 1, 1,
            [](uint64_t object, uint64_t arguments) { fake::put<uint8_t>(arguments, tick_on(object)); });
        fake::flag(fake::bit_field(function, u"ReturnValue", 0, 0, 0xFF), t::constant::property_param | t::constant::property_out | t::constant::property_return);
    }
    fake::put(main + t::data::message_queue_table + t::message_queue_table::pop, &game_pop);
    const uint64_t queue = fake::memory(0x40);
    fake::put(queue, main + t::data::message_queue_table);

    brute = tabled(table_a(), u"Brute", u"Brute", 0, 0);
    registers(brute);
    tick_component_class = fake::object(u"CarrotActorComponent", w.root);
    fake::set_super(tick_component_class, w.component_class);
    brain = fake::object(u"Brain", tick_component_class, 0x80), legs = fake::object(u"Legs", tick_component_class, 0x80);
    fake::put<uint8_t>(brain + t::game_component::tick_requested, 1);
    fake::put<uint8_t>(legs + t::game_component::tick_requested, 1);
    // Engine ticks can be off while the game still requests updates after waking.
    tick_states[brute] = tick_states[brain] = tick_states[legs] = false;
    fake::put(brain, component_table), fake::put(legs, component_table);
    fake::put(brain + t::ai_component::messages, queue);
    fake::put(brute + t::game_actor::ai, brain), fake::put(brute + t::game_actor::movement, legs);
    tsuchi::on_message_taken(message_taken);
    const uint32_t owned = tsuchi::owned_count(mod);
    TSUCHI_EntityParts parts = 99;

    // Nothing is suspended to begin with, and nothing is taken from the actor's queue.
    switches.waiting.push_back(message(t::data::damage_message_table, w.red));
    tsuchi::frame(0.03f);
    CHECK(api.suspended(mod, handle(brute), &parts) == TSUCHI_OK && parts == 0 && switches.waiting.size() == 1 && switches.taken.empty());
    switches.waiting.clear();

    // Its decisions: its own update and the part that decides for it. Its legs carry on.
    CHECK(api.hold_suspended(mod, handle(brute), kLogic) == TSUCHI_OK && switches.log == "actor- brain- " && tsuchi::owned_count(mod) == owned + 1);
    CHECK(api.suspended(mod, handle(brute), &parts) == TSUCHI_OK && parts == kLogic);
    // Then its movement as well; what is already stopped is not stopped again.
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(brute), kMovement) == TSUCHI_OK && switches.log == "mover- " && tsuchi::owned_count(mod) == owned + 1);
    CHECK(api.hold_suspended(mod, handle(brute), kLogic | kMovement) == TSUCHI_OK && switches.log == "mover- ");
    CHECK(api.suspended(mod, handle(brute), &parts) == TSUCHI_OK && parts == (kLogic | kMovement) && tsuchi::suspend_counts().actors == 1);

    // One mod at a time.
    TSUCHI_Mod* other = tsuchi::mod_create("actor.rival");
    CHECK(api.hold_suspended(other, handle(brute), kLogic) == TSUCHI_BUSY && api.release_suspended(other, handle(brute), kLogic) == TSUCHI_NOT_FOUND);
    CHECK(api.hold_suspended(mod, handle(brute), 0) == TSUCHI_INVALID && api.hold_suspended(mod, handle(brute), 16) == TSUCHI_INVALID && api.release_suspended(mod, handle(brute), 0) == TSUCHI_INVALID);
    CHECK(api.hold_suspended(mod, {12345}, kLogic) == TSUCHI_NOT_FOUND && api.suspended(mod, {12345}, &parts) == TSUCHI_NOT_FOUND);

    // What the game sends it meanwhile is taken each frame, told, and let go the game's way.
    const uint64_t hit = message(t::data::damage_message_table, w.red);
    fake::put(hit + t::damage_message::amount, 12.5f), fake::put<uint8_t>(hit + t::damage_message::attack, 4);
    fake::put(hit + t::damage_message::origin, 1.0f), fake::put(hit + t::damage_message::origin + 8, 3.0f);
    const uint64_t knock = message(t::data::knock_message_table, w.bulborb);
    fake::put(knock + t::knock_message::damage, 2.0f), fake::put(knock + t::knock_message::velocity + 4, 80.0f);
    const uint64_t whistle = message(t::data::whistle_message_table, w.captain, 2); // another actor's queue holds it too
    const uint64_t unknown = message(0x3500, 0);
    // A sender that has since been replaced by another object in the same place is nobody.
    const uint64_t stale = message(t::data::damage_message_table, w.red);
    fake::put(stale + t::message::sender + 4, fake::get<int32_t>(stale + t::message::sender + 4) + 1);
    for (uint64_t each : {hit, knock, whistle, unknown, stale}) switches.waiting.push_back(each);
    tsuchi::frame(0.03f);
    CHECK(switches.waiting.empty() && switches.taken.size() == 5 && tsuchi::suspend_counts().messages == 5 && switches.taken[4].sender == 0);
    switches.taken.pop_back();
    --switches.destroyed;
    if (switches.taken.size() == 4) {
        const auto& got = switches.taken;
        CHECK(got[0].kind == tsuchi::MessageKind::damage && got[0].actor == brute && got[0].sender == w.red && got[0].amount == 12.5f &&
              got[0].attack == 4 && got[0].vector[0] == 1 && got[0].vector[2] == 3);
        CHECK(got[1].kind == tsuchi::MessageKind::knock && got[1].sender == w.bulborb && got[1].amount == 2 && got[1].vector[1] == 80);
        CHECK(got[2].kind == tsuchi::MessageKind::whistle && got[2].sender == w.captain && got[3].kind == tsuchi::MessageKind::other && got[3].sender == 0);
    }
    // Each is freed when the last queue lets go of it: the whistle is still held elsewhere.
    CHECK(switches.destroyed == 3 && fake::get<int32_t>(whistle + t::message::references) == 1 && fake::get<int32_t>(hit + t::message::references) == 0);
    // A queue of some other make is left alone, whatever its function table offers.
    fake::put(main + 0x3500 + t::message_queue_table::pop, &game_pop);
    fake::put<uint64_t>(queue, main + 0x3500);
    switches.waiting.push_back(message(t::data::damage_message_table, 0));
    tsuchi::frame(0.03f);
    CHECK(switches.waiting.size() == 1 && switches.taken.size() == 4);
    fake::put(queue, main + t::data::message_queue_table);

    // Its decisions given back: the game answers its own messages again. Its legs stay stopped.
    switches.log.clear();
    CHECK(api.release_suspended(mod, handle(brute), kLogic) == TSUCHI_OK && switches.log == "actor+ brain+ ");
    CHECK(api.suspended(mod, handle(brute), &parts) == TSUCHI_OK && parts == kMovement && tsuchi::owned_count(mod) == owned + 1);
    tsuchi::frame(0.03f);
    CHECK(switches.waiting.size() == 1 && switches.taken.size() == 4);
    switches.waiting.clear();
    // Everything given back: the actor is the game's, and nothing is owed.
    switches.log.clear();
    CHECK(api.release_suspended(mod, handle(brute), kLogic | kMovement) == TSUCHI_OK && switches.log == "mover+ " && tsuchi::owned_count(mod) == owned);
    CHECK(api.release_suspended(mod, handle(brute), kMovement) == TSUCHI_NOT_FOUND && tsuchi::suspend_counts().actors == 0);
    CHECK(api.hold_suspended(other, handle(brute), kMovement) == TSUCHI_OK && api.release_suspended(other, handle(brute), kMovement) == TSUCHI_OK); // free for another now

    // A mod that stops gives everything back.
    TSUCHI_Mod* careless = tsuchi::mod_create("actor.puppeteer");
    CHECK(api.hold_suspended(careless, handle(brute), kLogic | kMovement) == TSUCHI_OK);
    switches.log.clear();
    tsuchi::mod_stop(careless);
    CHECK(switches.log == "actor+ brain+ mover+ " && api.suspended(mod, handle(brute), &parts) == TSUCHI_OK && parts == 0);

    // An actor put away while suspended goes back to its pool as the game made it: everything is given
    // back before the game switches it off and hides it there, which giving back afterward would undo.
    CHECK(api.hold_suspended(mod, handle(brute), kLogic) == TSUCHI_OK);
    switches.log.clear();
    park_watches = &switches.log;
    pool_return(kPool, brute);
    park_watches = nullptr;
    CHECK(switches.log == "actor+ brain+ " && park_saw == "actor+ brain+ " && tsuchi::owned_count(mod) == owned && tsuchi::suspend_counts().actors == 0);

    // One that was walking would coast on for meters, so it is also brought to rest: across the
    // ground, with whatever its own movement asked for dropped, and its fall left to gravity. The
    // game applies that during the actor's own update, so the update is switched off only afterward.
    namespace a = t::game_actor;
    const uint64_t walker = tabled(table_a(), u"Walker", u"Walker", 0, 0);
    registers(walker);
    physical(walker);
    fake::put(walker + a::velocity, 90.0f), fake::put<uint8_t>(walker + a::velocity_pending, 1);
    int asked = mover.requests;
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(walker), kLogic | kMovement) == TSUCHI_OK && mover.requests == asked + 1 && mover.actor == walker);
    CHECK(mover.velocity[0] == 0 && mover.velocity[1] == 0 && mover.velocity[2] == 0 && fake::get<uint8_t>(walker + a::keep_vertical) == 1 &&
          !fake::get<uint8_t>(walker + a::velocity_pending) && holds(walker) == 0);
    CHECK(switches.log.empty()); // its update still runs
    frames(2);
    CHECK(switches.log.empty() && mover.requests == asked + 3);
    frames(1);
    CHECK(switches.log == "other- " && mover.requests == asked + 4); // at rest: now its update stops
    frames(5);
    CHECK(switches.log == "other- " && mover.requests == asked + 4);
    CHECK(api.release_suspended(mod, handle(walker), kLogic | kMovement) == TSUCHI_OK && switches.log == "other- other+ ");

    // Its decisions alone touch nothing of its body. Its movement stopped afterward switches
    // the update back on for as long as the body takes to come to rest.
    asked = mover.requests, switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(walker), kLogic) == TSUCHI_OK && mover.requests == asked && switches.log == "other- ");
    CHECK(api.hold_suspended(mod, handle(walker), kMovement) == TSUCHI_OK && mover.requests == asked + 1 && switches.log == "other- other+ ");
    frames(3);
    CHECK(switches.log == "other- other+ other- ");
    CHECK(api.release_suspended(mod, handle(walker), kLogic | kMovement) == TSUCHI_OK && switches.log == "other- other+ other- other+ ");

    // Given back before it has come to rest: whatever is still suspended is settled there and then.
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(walker), kLogic | kMovement) == TSUCHI_OK && api.release_suspended(mod, handle(walker), kMovement) == TSUCHI_OK);
    CHECK(switches.log == "other- "); // its decisions stay stopped, and there is nothing left to wait for
    asked = mover.requests;
    frames(4);
    CHECK(mover.requests == asked && switches.log == "other- ");
    CHECK(api.release_suspended(mod, handle(walker), kLogic) == TSUCHI_OK && switches.log == "other- other+ ");
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(walker), kLogic | kMovement) == TSUCHI_OK && api.release_suspended(mod, handle(walker), kLogic) == TSUCHI_OK);
    CHECK(switches.log.empty()); // its update was never switched off, so there is nothing to switch back
    frames(4);
    CHECK(switches.log.empty() && api.release_suspended(mod, handle(walker), kMovement) == TSUCHI_OK);
    // One with no body of its own that is running has nothing to bring to rest, and nothing to wait for.
    asked = mover.requests;
    CHECK(api.hold_suspended(mod, handle(w.red), kMovement) == TSUCHI_OK && mover.requests == asked && api.release_suspended(mod, handle(w.red), kMovement) == TSUCHI_OK);

    // An actor with no mind of its own still has its own update to stop.
    const uint64_t stone = tabled(table_a(), u"Stone", u"Stone", 0, 0);
    registers(stone);
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(stone), kLogic | kMovement) == TSUCHI_OK && switches.log == "other- ");
    tsuchi::frame(0.03f);
    CHECK(api.release_suspended(mod, handle(stone), kLogic | kMovement) == TSUCHI_OK && switches.log == "other- other+ ");
    w.stone = stone;

    // An actor the game destroys while it is suspended is left as it is: nothing of it is switched
    // back on, and the hold is over.
    const uint64_t doomed = tabled(table_a(), u"Doomed", u"Doomed", 0, 0);
    registers(doomed);
    CHECK(api.hold_suspended(mod, handle(doomed), kLogic) == TSUCHI_OK && tsuchi::suspend_counts().actors == 1 && tsuchi::owned_count(mod) == owned + 1);
    switches.log.clear();
    ends_play(doomed);
    CHECK(switches.log.empty() && tsuchi::suspend_counts().actors == 0 && tsuchi::owned_count(mod) == owned);
    fake::destroy(doomed);
    // One whose level is taken out of the world lives on, and gets everything back as one put away does.
    const uint64_t shelved = tabled(table_a(), u"Shelved", u"Shelved", 0, 0);
    registers(shelved);
    CHECK(api.hold_suspended(mod, handle(shelved), kLogic) == TSUCHI_OK);
    switches.log.clear();
    ends_play(shelved, uint8_t(t::constant::end_play_removed));
    CHECK(switches.log == "other+ " && tsuchi::suspend_counts().actors == 0 && tsuchi::owned_count(mod) == owned);
    // One that is gone without its leaving having been seen: the hold ends on the next frame.
    const uint64_t vanished = tabled(table_a(), u"Vanished", u"Vanished", 0, 0);
    registers(vanished);
    CHECK(api.hold_suspended(mod, handle(vanished), kLogic) == TSUCHI_OK && tsuchi::suspend_counts().actors == 1);
    fake::destroy(vanished);
    switches.log.clear();
    tsuchi::frame(0.03f);
    CHECK(switches.log.empty() && tsuchi::suspend_counts().actors == 0 && tsuchi::owned_count(mod) == owned);
    // Resume and mod cleanup must preserve parts that were disabled before the hold.
    game_request_tick(reinterpret_cast<void*>(brute), false);
    game_component_tick(reinterpret_cast<void*>(brain), false);
    game_component_tick(reinterpret_cast<void*>(legs), false);
    switches.log.clear();
    CHECK(api.hold_suspended(mod, handle(brute), kLogic | kMovement) == TSUCHI_OK);
    CHECK(api.release_suspended(mod, handle(brute), kLogic | kMovement) == TSUCHI_OK);
    CHECK(!tick_on(brute) && !tick_on(brain) && !tick_on(legs));
    CHECK(switches.log.find('+') == std::string::npos);
    TSUCHI_Mod* disabled_owner = tsuchi::mod_create("actor.disabled_ticks");
    CHECK(api.hold_suspended(disabled_owner, handle(brute), kLogic) == TSUCHI_OK);
    tsuchi::mod_stop(disabled_owner);
    CHECK(!tick_on(brute) && !tick_on(brain));
    game_request_tick(reinterpret_cast<void*>(brute), true);
    game_component_tick(reinterpret_cast<void*>(brain), true);
    game_component_tick(reinterpret_cast<void*>(legs), true);
    tick_states.erase(brute); tick_states.erase(brain); tick_states.erase(legs);
    // Replacing the held AI component must not enable the replacement on release.
    const uint64_t replacement = fake::object(u"ReplacementBrain", w.component_class, 0x80);
    tick_states[replacement] = false;
    CHECK(api.hold_suspended(mod, handle(brute), kLogic) == TSUCHI_OK);
    fake::put(brute + t::game_actor::ai, replacement);
    CHECK(api.release_suspended(mod, handle(brute), kLogic) == TSUCHI_OK && !tick_on(replacement));
    fake::put(brute + t::game_actor::ai, brain);
}

// ---- Suspending physics and solidity ---------------------------------------------
//
// The made-up game's parts that can collide keep their state here: whether the
// physics moves them, how they collide, and which of the physics engine's bodies
// each one is. The game's own rebuild of an actor's collision records is stood in
// for by a function that finds the borrowed shapes again.

struct PartState {
    bool simulating = false;
    uint8_t mode = 0;
    uint64_t instance = 0; // the record that leads to its physics body
};
std::map<uint64_t, PartState> part_states;
std::string body_log;
int rebuilds = 0, body_shapes = 4;
bool body_is_rigid = true;
uint64_t primitive_table = 0;

bool game_is_simulating(void* which, uint64_t) { return part_states[reinterpret_cast<uint64_t>(which)].simulating; }
uint8_t game_get_collision(void* which) { return part_states[reinterpret_cast<uint64_t>(which)].mode; }
void game_set_simulating(void* which, bool on) {
    part_states[reinterpret_cast<uint64_t>(which)].simulating = on;
    body_log += on ? "sim+ " : "sim- ";
}
void game_set_collision(void* which, uint32_t mode) {
    part_states[reinterpret_cast<uint64_t>(which)].mode = uint8_t(mode);
    body_log += "mode" + std::to_string(mode) + " ";
}
void* game_body_instance(void* which, uint64_t, bool) { return reinterpret_cast<void*>(part_states[reinterpret_cast<uint64_t>(which)].instance); }
bool physics_is_kind(void*, const char* name) { return body_is_rigid && std::string(name) == "PxRigidBody"; }
uint32_t physics_shape_count(void*) { return uint32_t(body_shapes); }
void game_rebuild_tree(void* tree) {
    ++rebuilds;
    const auto at = reinterpret_cast<uint64_t>(tree);
    const uint64_t records = fake::get<uint64_t>(at + t::collision_tree::records);
    for (int32_t i = 0; i < fake::get<int32_t>(at + t::collision_tree::count); ++i) // the borrowed shapes, found again
        fake::put<uint64_t>(records + uint64_t(i) * t::collision_record::size + t::collision_record::shape, 0x5000 + uint64_t(i));
}

// The framework's memory, watched: which blocks are out, and a size it can be refused.
std::map<void*, size_t> blocks_out;
int watched_fail_after = -1;
size_t refused_size = 0, refused_multiple = 0; // one size, and every size that is a multiple of another
int allocations_left = -1;                     // below 0: as many as are asked for
void* watched_allocate(size_t bytes) {
    if (watched_fail_after == 0) return nullptr;
    if (watched_fail_after > 0) --watched_fail_after;
    if (bytes == refused_size || (refused_multiple && bytes % refused_multiple == 0)) return nullptr;
    if (allocations_left == 0) return nullptr;
    if (allocations_left > 0) --allocations_left;
    void* block = std::malloc(bytes);
    std::memset(block, 0xA5, bytes); // nothing may count on what new memory holds
    blocks_out[block] = bytes;
    return block;
}
void watched_release(void* block) {
    blocks_out.erase(block);
    std::free(block);
}
int blocks_of(size_t bytes) {
    int count = 0;
    for (const auto& [block, size] : blocks_out) count += size == bytes;
    return count;
}

// One of the physics engine's bodies, and the record through which a part reaches it.
uint64_t physics_body() {
    const uint64_t table = fake::memory(0x40), body = fake::memory(0x10);
    fake::put(table + t::physics_body_table::is_kind, &physics_is_kind);
    fake::put(table + t::physics_body_table::shape_count, &physics_shape_count);
    fake::put(body, table);
    return body;
}
uint64_t instance_of(uint64_t body) {
    const uint64_t instance = fake::memory(0x20);
    fake::put(instance + t::body_instance::body, body);
    return instance;
}
// A part that can collide, attached under `parent`, belonging to `owner`.
uint64_t solid_part(uint64_t parent, uint64_t owner, uint8_t mode, uint64_t body) {
    const uint64_t part = fake::object(u"Solid", w.primitive_class, 0x80);
    fake::put(part, primitive_table);
    fake::put(part + t::scene_component::owner, owner);
    part_states[part] = {false, mode, instance_of(body)};
    const uint64_t list = parent + t::scene_component::attach_children;
    const int32_t count = fake::get<int32_t>(list + t::array::count);
    if (!fake::get<uint64_t>(list + t::array::data)) fake::put<uint64_t>(list + t::array::data, fake::memory(8 * 8));
    fake::put(fake::get<uint64_t>(list + t::array::data) + 8 * uint64_t(count), part);
    fake::put(list + t::array::count, count + 1);
    return part;
}

void bodies() {
    namespace a = t::game_actor;
    namespace tr = t::collision_tree;
    namespace r = t::collision_record;
    constexpr TSUCHI_EntityParts kLogic = TSUCHI_ENTITY_PARTS_LOGIC, kPhysics = TSUCHI_ENTITY_PARTS_PHYSICS, kCollision = TSUCHI_ENTITY_PARTS_COLLISION;
    for (auto function : {reinterpret_cast<uint64_t>(&game_is_simulating), reinterpret_cast<uint64_t>(&game_get_collision),
                          reinterpret_cast<uint64_t>(&game_set_simulating), reinterpret_cast<uint64_t>(&game_set_collision),
                          reinterpret_cast<uint64_t>(&game_body_instance), reinterpret_cast<uint64_t>(&physics_is_kind),
                          reinterpret_cast<uint64_t>(&physics_shape_count)})
        fake::code().push_back(function);
    tsuchi::set_allocator({watched_allocate, watched_release});
    primitive_table = fake::memory(0x60);
    fake::put(primitive_table + t::primitive_table::is_simulating, &game_is_simulating);
    fake::put(primitive_table + t::primitive_table::get_collision, &game_get_collision);
    fake::put(primitive_table + t::primitive_table::set_simulating, &game_set_simulating);
    fake::put(primitive_table + t::primitive_table::set_collision, &game_set_collision);
    fake::put(primitive_table + t::primitive_table::body_instance, &game_body_instance);
    t::fn::rebuild_collision_tree = game_rebuild_tree;

    // A golem: a body the physics moves, a shell that blocks without being found, a horn that
    // is only found, and something of another actor's riding on it.
    const uint64_t golem = tabled(table_a(), u"Golem", u"Golem", 0, 0);
    registers(golem);
    const uint64_t root_of = fake::get<uint64_t>(golem + a::root), main_body = physics_body();
    fake::put(root_of + t::scene_component::owner, golem);
    const uint64_t body = solid_part(root_of, golem, kCollisionBoth, main_body);
    const uint64_t shell = solid_part(root_of, golem, kCollisionPhysicsOnly, main_body);
    const uint64_t horn = solid_part(shell, golem, kCollisionQueryOnly, main_body);
    const uint64_t rider = solid_part(root_of, w.red, kCollisionBoth, main_body);
    fake::put(golem + a::physics, body);
    part_states[body].simulating = true;
    fake::put<uint8_t>(golem + a::simulating, 1), fake::put<uint8_t>(golem + a::simulating_known, 1), fake::put<uint8_t>(golem + a::body_awake, 1);
    fake::put<uint64_t>(golem + a::rigid_body, 0xABCD);
    // The game's record of its collision parts, each with a shape borrowed from the physics engine.
    const uint64_t tree = fake::object(u"Tree", w.component_class, 0x80), records = fake::memory(200 * r::size);
    uint64_t own_parts[8];
    CHECK(actor_primitives(golem, own_parts, 8) == 3); // not its root, which cannot collide, nor its rider
    fake::put(tree + t::scene_component::owner, golem), fake::put(tree + tr::actor, golem);
    fake::put(tree + tr::records, records), fake::put<int32_t>(tree + tr::count, 2), fake::put<int32_t>(tree + tr::capacity, 4);
    fake::put<uint64_t>(tree + tr::shapes, 0x77);
    fake::put(records + r::component, body), fake::put(records + r::size + r::component, shell);
    const auto borrow = [&] {
        for (int i = 0; i < 2; ++i) fake::put<uint64_t>(records + uint64_t(i) * r::size + r::shape, 0x9000 + uint64_t(i));
    };
    const auto shape_at = [&](int i) { return fake::get<uint64_t>(records + uint64_t(i) * r::size + r::shape); };
    borrow();
    fake::put(golem + a::collision_tree, tree);
    const uint32_t owned = tsuchi::owned_count(mod);
    TSUCHI_EntityParts parts = 0;
    const TSUCHI_Vec3 up{0, 0, 300};

    // Its physics: the body stops being moved, the actor's own notes of its body are reset, and the
    // game's collision records are rebuilt, since the physics engine has made the body's shapes anew.
    CHECK(api.hold_suspended(mod, handle(golem), kPhysics) == TSUCHI_OK && body_log == "sim- " && rebuilds == 1 && !part_states[body].simulating);
    CHECK(!fake::get<uint8_t>(golem + a::simulating) && fake::get<uint8_t>(golem + a::simulating_known) == 1 && !fake::get<uint8_t>(golem + a::body_awake) &&
          !fake::get<uint64_t>(golem + a::rigid_body) && shape_at(0) == 0x5000 && shape_at(1) == 0x5001);
    CHECK(api.suspended(mod, handle(golem), &parts) == TSUCHI_OK && parts == kPhysics && api.launch(mod, handle(golem), &up) == TSUCHI_UNSUPPORTED);
    // The game's own code switches it back: it is stopped again on the next frame.
    part_states[body].simulating = true;
    tsuchi::frame(0.03f);
    CHECK(body_log == "sim- sim- " && rebuilds == 2 && !part_states[body].simulating);
    tsuchi::frame(0.03f);
    CHECK(rebuilds == 2); // nothing changed, nothing rebuilt
    CHECK(api.release_suspended(mod, handle(golem), kPhysics) == TSUCHI_OK && body_log == "sim- sim- sim+ " && rebuilds == 3 && part_states[body].simulating &&
          tsuchi::owned_count(mod) == owned);

    // Its solidity: each part that blocks stops blocking and keeps whatever else it did. A part
    // that only gets found, and another actor's part, are left alone.
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && body_log == "mode1 mode0 " && rebuilds == 4);
    CHECK(part_states[body].mode == kCollisionQueryOnly && part_states[shell].mode == kCollisionNone && part_states[horn].mode == kCollisionQueryOnly &&
          part_states[rider].mode == kCollisionBoth && part_states[body].simulating);
    part_states[shell].mode = kCollisionPhysicsOnly; // the game puts one back
    tsuchi::frame(0.03f);
    CHECK(part_states[shell].mode == kCollisionNone && rebuilds == 5);
    body_log.clear();
    CHECK(api.release_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && body_log == "mode3 mode2 " && rebuilds == 6);

    // A part that has passed to another actor meanwhile is no longer this one's to put back.
    CHECK(api.hold_suspended(mod, handle(golem), kCollision) == TSUCHI_OK);
    fake::put(shell + t::scene_component::owner, w.red);
    fake::put<uint64_t>(records + r::size + r::component, 0);
    body_log.clear();
    CHECK(api.release_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && body_log == "mode3 ");
    fake::put(shell + t::scene_component::owner, golem);
    fake::put(records + r::size + r::component, shell);
    part_states[shell].mode = kCollisionPhysicsOnly;
    rebuilds = 6;

    // Both at once is one rebuild; and a part destroyed meanwhile is not put back.
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(golem), kPhysics | kCollision) == TSUCHI_OK && body_log == "sim- mode1 mode0 " && rebuilds == 7);
    fake::destroy(shell);
    fake::put<uint64_t>(records + r::size + r::component, 0); // the game drops its record of the part too
    body_log.clear();
    CHECK(api.release_suspended(mod, handle(golem), kPhysics | kCollision) == TSUCHI_OK && body_log == "mode3 sim+ " && rebuilds == 8);
    fake::place(shell, fake::get<int32_t>(shell + t::object::index));
    fake::put(records + r::size + r::component, shell);
    part_states[shell].mode = kCollisionPhysicsOnly;

    // A body that was still already is left still when its physics is given back.
    part_states[body].simulating = false;
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(golem), kPhysics) == TSUCHI_OK && body_log.empty() && rebuilds == 9); // rebuilt all the same
    CHECK(api.release_suspended(mod, handle(golem), kPhysics) == TSUCHI_OK && body_log.empty() && rebuilds == 9 && !part_states[body].simulating);
    part_states[body].simulating = true;

    // When the game's records are not as expected, nothing is suspended and everything is as it was.
    fake::put(tree + tr::actor, w.red);
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(golem), kPhysics | kCollision) == TSUCHI_UNSUPPORTED && rebuilds == 9 && tsuchi::owned_count(mod) == owned);
    CHECK(part_states[body].simulating && part_states[body].mode == kCollisionBoth && part_states[shell].mode == kCollisionPhysicsOnly);
    CHECK(api.suspended(mod, handle(golem), &parts) == TSUCHI_OK && parts == 0);
    // With something else of it already suspended, that stays.
    CHECK(api.hold_suspended(mod, handle(golem), kLogic) == TSUCHI_OK && api.hold_suspended(mod, handle(golem), kPhysics) == TSUCHI_UNSUPPORTED);
    CHECK(api.suspended(mod, handle(golem), &parts) == TSUCHI_OK && parts == kLogic && api.release_suspended(mod, handle(golem), kLogic) == TSUCHI_OK);
    fake::put(tree + tr::actor, golem);

    // The checks made before the game's rebuild is allowed to run. Whenever the records
    // themselves are sound, the borrowed shapes are dropped whatever else happens.
    const auto refused = [&](bool dropped) {
        borrow();
        const int before = rebuilds;
        const bool ok = rebuild_collision_tree(golem);
        return !ok && rebuilds == before && (shape_at(0) == 0) == dropped;
    };
    fake::put<int32_t>(tree + tr::count, 200), fake::put<int32_t>(tree + tr::capacity, 200);
    CHECK(refused(false)); // more parts than the game's rebuild has room for
    fake::put<int32_t>(tree + tr::count, 2), fake::put<int32_t>(tree + tr::capacity, 4);
    fake::put(records + r::size + r::component, rider);
    CHECK(refused(false)); // a part that is another actor's
    fake::put(records + r::size + r::component, shell);
    body_shapes = 200;
    CHECK(refused(true)); // more shapes than it has room for
    body_shapes = 70;
    part_states[shell].instance = instance_of(physics_body()); // a part with a body of its own: 70 and 70
    CHECK(refused(true));
    body_shapes = 60;
    borrow();
    CHECK(rebuild_collision_tree(golem) && rebuilds == 10 && shape_at(1) == 0x5001); // 60 and 60 fit
    part_states[shell].instance = 0;
    CHECK(refused(true)); // a part whose body cannot be reached
    part_states[shell].instance = part_states[body].instance; // welded onto the root's body: counted once
    body_shapes = 100;
    borrow();
    CHECK(rebuild_collision_tree(golem) && rebuilds == 11);
    body_is_rigid = false; // a body of another sort has no shapes to count
    body_shapes = 500;
    borrow();
    CHECK(rebuild_collision_tree(golem) && rebuilds == 12);
    body_is_rigid = true, body_shapes = 4;
    t::fn::rebuild_collision_tree = nullptr;
    CHECK(refused(true));
    t::fn::rebuild_collision_tree = game_rebuild_tree;
    // Records that borrow nothing need no rebuild, and an actor with no records needs nothing at all.
    fake::put<uint64_t>(tree + tr::shapes, 0);
    borrow();
    CHECK(rebuild_collision_tree(golem) && rebuilds == 12 && shape_at(0) == 0);
    fake::put<uint64_t>(tree + tr::shapes, 0x77);
    CHECK(rebuild_collision_tree(w.red) && rebuilds == 12);

    // However many solid parts an actor has, each is remembered and put back. Forty-one here:
    // twenty-one one below another, and twenty side by side.
    const uint64_t hedgehog = tabled(table_a(), u"Hedgehog", u"Hedgehog", 0, 0);
    registers(hedgehog);
    const uint64_t hedgehog_root = fake::get<uint64_t>(hedgehog + a::root);
    fake::put(hedgehog_root + t::scene_component::owner, hedgehog);
    fake::put<uint64_t>(hedgehog_root + t::scene_component::attach_children + t::array::data, fake::memory(8 * 32));
    std::vector<uint64_t> spines;
    std::vector<uint8_t> spine_modes;
    uint64_t spine = hedgehog_root;
    for (int i = 0; i < 21; ++i) spines.push_back(spine = solid_part(spine, hedgehog, kCollisionBoth, main_body)), spine_modes.push_back(kCollisionBoth);
    for (int i = 0; i < 20; ++i) {
        spine_modes.push_back(i % 2 ? kCollisionBoth : kCollisionPhysicsOnly);
        spines.push_back(solid_part(hedgehog_root, hedgehog, spine_modes.back(), main_body));
    }
    const auto spines_are = [&](bool solid) {
        bool all = true;
        for (size_t i = 0; i < spines.size(); ++i)
            all = all && part_states[spines[i]].mode == (solid ? spine_modes[i] : without_blocking(spine_modes[i]));
        return all;
    };
    const size_t list_bytes = 41 * 16; // what remembering forty-one parts takes
    CHECK(actor_primitives(hedgehog, nullptr, 0) == 41 && blocks_of(list_bytes) == 0);
    CHECK(api.hold_suspended(mod, handle(hedgehog), kCollision) == TSUCHI_OK && spines_are(false) && blocks_of(list_bytes) == 1);
    part_states[spines[20]].mode = kCollisionBoth, part_states[spines[40]].mode = kCollisionBoth; // the game puts the deepest and the last back
    tsuchi::frame(0.03f);
    CHECK(spines_are(false));
    // Given back, the list goes with it.
    CHECK(api.release_suspended(mod, handle(hedgehog), kCollision) == TSUCHI_OK && spines_are(true) && blocks_of(list_bytes) == 0);
    // Also when something else of it stays suspended.
    CHECK(api.hold_suspended(mod, handle(hedgehog), kLogic | kCollision) == TSUCHI_OK && blocks_of(list_bytes) == 1);
    CHECK(api.release_suspended(mod, handle(hedgehog), kCollision) == TSUCHI_OK && spines_are(true) && blocks_of(list_bytes) == 0);
    CHECK(api.release_suspended(mod, handle(hedgehog), kLogic) == TSUCHI_OK && blocks_of(list_bytes) == 0);
    // With no memory for the list, nothing is touched.
    refused_size = list_bytes;
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(hedgehog), kCollision) == TSUCHI_NO_MEMORY && body_log.empty() && spines_are(true) && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    // An actor that is gone before it could be put back: its list is given back all the same.
    TSUCHI_Mod* forgetful = tsuchi::mod_create("actor.forgetful");
    CHECK(api.hold_suspended(forgetful, handle(hedgehog), kCollision) == TSUCHI_OK && blocks_of(list_bytes) == 1);
    fake::destroy(hedgehog);
    tsuchi::mod_stop(forgetful);
    CHECK(blocks_of(list_bytes) == 0);

    // Parts that lead back into themselves cannot all be found, so none is changed. The same
    // for parts whose list cannot be read.
    const uint64_t knot = tabled(table_a(), u"Knot", u"Knot", 0, 0);
    registers(knot);
    const uint64_t knot_root = fake::get<uint64_t>(knot + a::root);
    fake::put(knot_root + t::scene_component::owner, knot);
    const uint64_t strand = solid_part(knot_root, knot, kCollisionBoth, main_body);
    const uint64_t strand_children = strand + t::scene_component::attach_children, back = fake::memory(8);
    fake::put(back, knot_root);
    fake::put(strand_children + t::array::data, back), fake::put<int32_t>(strand_children + t::array::count, 1);
    body_log.clear();
    CHECK(api.hold_suspended(mod, handle(knot), kCollision) == TSUCHI_UNSUPPORTED && body_log.empty() && tsuchi::owned_count(mod) == owned);
    fake::put<uint64_t>(strand_children + t::array::data, 0), fake::put<int32_t>(strand_children + t::array::count, 3);
    CHECK(api.hold_suspended(mod, handle(knot), kCollision) == TSUCHI_UNSUPPORTED && body_log.empty() && tsuchi::owned_count(mod) == owned);
    part_states[strand].mode = kCollisionQueryOnly; // refused even when no part that blocks was found on the way
    CHECK(api.hold_suspended(mod, handle(knot), kCollision) == TSUCHI_UNSUPPORTED && body_log.empty() && tsuchi::owned_count(mod) == owned);
    part_states[strand].mode = kCollisionBoth;
    fake::put<int32_t>(strand_children + t::array::count, -1);
    CHECK(api.hold_suspended(mod, handle(knot), kCollision) == TSUCHI_UNSUPPORTED && body_log.empty());
    fake::put<int32_t>(strand_children + t::array::count, 0);
    CHECK(api.hold_suspended(mod, handle(knot), kCollision) == TSUCHI_OK && api.release_suspended(mod, handle(knot), kCollision) == TSUCHI_OK);

    // One collision part switched off: what its record says of it is cleared, it stops colliding,
    // and the game's records are rebuilt. The rest of the record, and the other parts, are as before.
    {
        const TSUCHI_CollisionService& collision_api = *tsuchi::host.collision;
        const auto part = [](uint64_t object) { return TSUCHI_Component{to_handle(object)}; };
        const uint64_t shell_record = records + r::size;
        const auto says = [&](uint64_t record) {
            std::string out;
            for (uint64_t flag : t::tables::collision_record_flags) out += std::to_string(fake::get<uint8_t>(record + flag));
            return out;
        };
        const auto say = [&](uint64_t record, std::initializer_list<int> values) {
            int i = 0;
            for (int value : values) fake::put<uint8_t>(record + t::tables::collision_record_flags[i++], uint8_t(value));
        };
        say(records, {1, 1, 1}), say(shell_record, {1, 0, 1});
        fake::put<uint8_t>(shell_record + 6, 7); // something else the record keeps
        bool is_off = true;
        const uint32_t held = tsuchi::owned_count(mod);
        int built = rebuilds;
        // No memory to remember it by: left alone.
        refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // the first room for sixteen holds
        body_log.clear();
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_NO_MEMORY && body_log.empty() && says(shell_record) == "101");
        refused_size = 0;
        CHECK(collision_api.off(mod, part(shell), &is_off) == TSUCHI_OK && !is_off);
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_OK && body_log == "mode0 " && says(shell_record) == "000" && says(records) == "111" &&
              fake::get<uint8_t>(shell_record + 6) == 7 && rebuilds == built + 1 && tsuchi::owned_count(mod) == held + 1 && tsuchi::parts_switched_off() == 1);
        CHECK(collision_api.off(mod, part(shell), &is_off) == TSUCHI_OK && is_off && collision_api.off(mod, part(body), &is_off) == TSUCHI_OK && !is_off);
        // Asked again, by the same mod and by another.
        TSUCHI_Mod* rival = tsuchi::mod_create("actor.locksmith");
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_OK && collision_api.hold_off(rival, part(shell)) == TSUCHI_BUSY &&
              collision_api.release_off(rival, part(shell)) == TSUCHI_NOT_FOUND && body_log == "mode0 " && tsuchi::owned_count(mod) == held + 1);
        // The game switches something back: off again on the next frame. Nothing changed, nothing done.
        say(shell_record, {0, 1, 0});
        built = rebuilds;
        tsuchi::frame(0.03f);
        CHECK(says(shell_record) == "000" && rebuilds == built + 1);
        part_states[shell].mode = kCollisionBoth;
        body_log.clear();
        tsuchi::frame(0.03f);
        CHECK(body_log == "mode0 " && rebuilds == built + 2);
        tsuchi::frame(0.03f);
        CHECK(rebuilds == built + 2);
        // On again: as it was.
        body_log.clear();
        CHECK(collision_api.release_off(mod, part(shell)) == TSUCHI_OK && body_log == "mode2 " && says(shell_record) == "101" && rebuilds == built + 3 &&
              tsuchi::owned_count(mod) == held && tsuchi::parts_switched_off() == 0);
        CHECK(collision_api.release_off(mod, part(shell)) == TSUCHI_NOT_FOUND);
        // What the game keeps no record of, keeps twice, or is no part at all.
        CHECK(collision_api.hold_off(mod, part(horn)) == TSUCHI_UNSUPPORTED && collision_api.hold_off(mod, part(golem)) == TSUCHI_NOT_FOUND);
        fake::put(records + r::component, shell);
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_UNSUPPORTED);
        fake::put(records + r::component, body);
        fake::put(tree + tr::actor, w.red); // records that are not as expected: nothing is tried
        body_log.clear();
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_UNSUPPORTED && body_log.empty());
        fake::put(tree + tr::actor, golem);
        const uint64_t drifting = fake::engine_object(u"Drifting", w.primitive_class, 0x80); // a part of no actor
        CHECK(collision_api.hold_off(mod, part(drifting)) == TSUCHI_UNSUPPORTED && tsuchi::owned_count(mod) == held);
        // Something that is no actor, with records laid out as an actor's would be.
        const uint64_t mimic = fake::object(u"Mimic", w.component_class, 0x100), mimic_tree = fake::object(u"Tree", w.component_class, 0x80);
        const uint64_t mimic_records = fake::memory(r::size);
        fake::put(mimic + a::collision_tree, mimic_tree), fake::put(mimic_tree + t::scene_component::owner, mimic), fake::put(mimic_tree + tr::actor, mimic);
        fake::put(mimic_tree + tr::records, mimic_records), fake::put<int32_t>(mimic_tree + tr::count, 1), fake::put<int32_t>(mimic_tree + tr::capacity, 1);
        fake::put(drifting + t::scene_component::owner, mimic), fake::put(mimic_records + r::component, drifting);
        CHECK(collision_api.hold_off(mod, part(drifting)) == TSUCHI_UNSUPPORTED && tsuchi::parts_switched_off() == 0);
        // No memory to record it against the mod: nothing is changed.
        TSUCHI_Mod* pauper = tsuchi::mod_create("actor.tinker");
        refused_multiple = 40;
        body_log.clear();
        CHECK(collision_api.hold_off(pauper, part(shell)) == TSUCHI_NO_MEMORY && body_log.empty() && says(shell_record) == "101" && tsuchi::parts_switched_off() == 0);
        refused_multiple = 0;
        // The game's records cannot be rebuilt: everything is put back.
        t::fn::rebuild_collision_tree = nullptr;
        body_log.clear();
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_UNSUPPORTED && body_log == "mode0 mode2 " && says(shell_record) == "101" &&
              tsuchi::owned_count(mod) == held && tsuchi::parts_switched_off() == 0);
        t::fn::rebuild_collision_tree = game_rebuild_tree;
        // While the actor's solidity is suspended a part is not switched, either way; and a part
        // that is off is left out of what suspending remembers.
        CHECK(api.hold_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && collision_api.hold_off(mod, part(shell)) == TSUCHI_BUSY);
        CHECK(api.release_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && collision_api.hold_off(mod, part(shell)) == TSUCHI_OK);
        body_log.clear();
        CHECK(api.hold_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && body_log == "mode1 " && collision_api.release_off(mod, part(shell)) == TSUCHI_BUSY);
        say(shell_record, {1, 1, 1});
        tsuchi::frame(0.03f);
        CHECK(says(shell_record) == "111"); // left alone until the solidity is given back
        body_log.clear();
        CHECK(api.release_suspended(mod, handle(golem), kCollision) == TSUCHI_OK && body_log == "mode3 " && part_states[shell].mode == kCollisionNone);
        tsuchi::frame(0.03f);
        CHECK(says(shell_record) == "000");
        // A mod that stops has its parts switched on again; so has an actor that is put away, further down.
        CHECK(collision_api.release_off(mod, part(shell)) == TSUCHI_OK && collision_api.hold_off(rival, part(shell)) == TSUCHI_OK);
        body_log.clear();
        tsuchi::mod_stop(rival);
        CHECK(body_log == "mode2 " && says(shell_record) == "101" && tsuchi::parts_switched_off() == 0);
        // A part the game drops from its records while it is off: the hold ends by itself, and the
        // part collides again as it did, though there is no record left to put back.
        CHECK(collision_api.hold_off(mod, part(shell)) == TSUCHI_OK && tsuchi::owned_count(mod) == held + 1 && part_states[shell].mode == kCollisionNone);
        fake::put<uint64_t>(shell_record + r::component, 0);
        body_log.clear();
        tsuchi::frame(0.03f);
        CHECK(tsuchi::parts_switched_off() == 0 && tsuchi::owned_count(mod) == held);
        CHECK(body_log == "mode2 " && part_states[shell].mode == kCollisionPhysicsOnly);
        fake::put(shell_record + r::component, shell);
        say(shell_record, {1, 0, 1});
        part_states[shell].mode = kCollisionPhysicsOnly;
        CHECK(collision_api.hold_off(mod, part(body)) == TSUCHI_OK); // left on for the actor's leaving, below
        rebuilds = 0;
    }

    // Another actor leaving play changes nothing of this one's.
    const uint64_t passer = tabled(table_a(), u"Passer", u"Passer", 0, 0);
    registers(passer);
    pool_return(kPool, passer);
    CHECK(tsuchi::parts_switched_off() == 1);

    // Given back when the mod stops, and when the actor is put away.
    TSUCHI_Mod* careless = tsuchi::mod_create("actor.sculptor");
    CHECK(api.hold_suspended(careless, handle(golem), kPhysics | kCollision) == TSUCHI_OK);
    body_log.clear();
    tsuchi::mod_stop(careless);
    CHECK(body_log == "mode2 sim+ " && part_states[body].simulating); // its body is switched off, so only its shell was made unsolid
    CHECK(api.hold_suspended(mod, handle(golem), kCollision) == TSUCHI_OK);
    body_log.clear();
    pool_return(kPool, golem);
    CHECK(body_log == "mode2 mode3 " && tsuchi::owned_count(mod) == owned && tsuchi::parts_switched_off() == 0); // its shell solid again, its body switched on

    // An actor the game destroys is left as it is: its part is not switched on again first.
    const uint64_t husk = tabled(table_a(), u"Husk", u"Husk", 0, 0);
    registers(husk);
    const uint64_t husk_root = fake::get<uint64_t>(husk + a::root);
    fake::put(husk_root + t::scene_component::owner, husk);
    const uint64_t plate = solid_part(husk_root, husk, kCollisionBoth, main_body);
    const uint64_t husk_tree = fake::object(u"Tree", w.component_class, 0x80), husk_records = fake::memory(r::size);
    fake::put(husk_tree + t::scene_component::owner, husk), fake::put(husk_tree + tr::actor, husk);
    fake::put(husk_tree + tr::records, husk_records), fake::put<int32_t>(husk_tree + tr::count, 1), fake::put<int32_t>(husk_tree + tr::capacity, 1);
    fake::put(husk_records + r::component, plate);
    fake::put(husk + a::collision_tree, husk_tree);
    CHECK(tsuchi::host.collision->hold_off(mod, TSUCHI_Component{to_handle(plate)}) == TSUCHI_OK && tsuchi::parts_switched_off() == 1);
    body_log.clear();
    ends_play(husk);
    CHECK(body_log.empty() && part_states[plate].mode == kCollisionNone && tsuchi::parts_switched_off() == 0 && tsuchi::owned_count(mod) == owned);
    fake::destroy(plate), fake::destroy(husk);
}

// ---- Parts, and hiding them ----------------------------------------------------
//
// The made-up engine lists an actor's parts when asked, and shows or hides one
// part at a time. Both are functions it describes, called by name.

std::map<uint64_t, std::vector<uint64_t>> parts_of; // what the engine lists for each actor
std::string part_log;
constexpr int32_t kVisibleAt = 0x7C;
constexpr uint8_t kVisibleBit = 0x02;
uint64_t part_base = 0, mover_class = 0, base_only_class = 0, idol = 0;
uint64_t mesh_class = 0, material_class = 0, stock_material = 0, red_material = 0;
// What a skeletal mesh is doing, as the made-up engine keeps it: the clip played on it, if any.
struct Motion {
    uint64_t clip = 0;
    bool looping = false, playing = false;
    float rate = 1, position = 0;
};
std::map<uint64_t, Motion> motions;
uint64_t skeletal_class = 0, clip_class = 0, walk_clip = 0, dancer_mesh = 0;
bool engine_starves_after_playing = false;
constexpr int32_t kAnimationModeAt = 0x94;
// What a mesh keeps of its one clip (FSingleAnimationPlayData): the looping and playing bits, a place and a rate.
constexpr int32_t kAnimationDataAt = 0x98, kSavedBitsAt = kAnimationDataAt + 8, kSavedPositionAt = kAnimationDataAt + 12, kSavedRateAt = kAnimationDataAt + 16;
constexpr uint8_t kSavedLooping = 0x01, kSavedPlaying = 0x02;
// What the engine starts with on a new mesh.
void saved_by_default(uint64_t mesh) {
    fake::put<uint8_t>(mesh + kSavedBitsAt, kSavedLooping | kSavedPlaying);
    fake::put<float>(mesh + kSavedPositionAt, 0), fake::put<float>(mesh + kSavedRateAt, 1);
}
// The engine registers a mesh anew, as a change of its mobility does: its one clip starts again from what the mesh kept.
void registers_anew(uint64_t mesh) {
    Motion& motion = motions[mesh];
    const uint8_t bits = fake::get<uint8_t>(mesh + kSavedBitsAt);
    motion.looping = (bits & kSavedLooping) != 0, motion.playing = (bits & kSavedPlaying) != 0;
    motion.position = fake::get<float>(mesh + kSavedPositionAt), motion.rate = fake::get<float>(mesh + kSavedRateAt);
}
std::map<uint64_t, int32_t> slots_of; // how many material slots each mesh has
constexpr int32_t kOverridesAt = 0x80;
bool engine_starves_after_counting = false;
// What has been put in a slot of a mesh over its own material, as the engine keeps it.
uint64_t override_in(uint64_t mesh, int32_t slot) {
    const uint64_t data = fake::get<uint64_t>(mesh + kOverridesAt + t::array::data);
    return data && slot < fake::get<int32_t>(mesh + kOverridesAt + t::array::count) ? fake::get<uint64_t>(data + 8 * uint64_t(slot)) : 0;
}
void put_override(uint64_t mesh, int32_t slot, uint64_t material) {
    const uint64_t list = mesh + kOverridesAt;
    if (!fake::get<uint64_t>(list + t::array::data)) {
        fake::put<uint64_t>(list + t::array::data, fake::memory(8 * 16));
        fake::put<int32_t>(list + t::array::max, 16);
    }
    if (fake::get<int32_t>(list + t::array::count) <= slot) fake::put(list + t::array::count, slot + 1);
    fake::put(fake::get<uint64_t>(list + t::array::data) + 8 * uint64_t(slot), material);
}
bool engine_will_not_attach = false;

bool drawn(uint64_t part) { return (fake::get<uint8_t>(part + kVisibleAt) & kVisibleBit) != 0; }
void draw(uint64_t part, bool on) { fake::put<uint8_t>(part + kVisibleAt, on ? kVisibleBit : 0); }
std::string part_name(uint64_t part) {
    char text[64] = "?";
    name_text(name_of(part), text, sizeof text);
    return text;
}
void attach_under(uint64_t parent, uint64_t part) {
    const uint64_t list = parent + t::scene_component::attach_children;
    if (!fake::get<uint64_t>(list + t::array::data)) fake::put<uint64_t>(list + t::array::data, fake::memory(8 * 64));
    const int32_t count = fake::get<int32_t>(list + t::array::count);
    fake::put(fake::get<uint64_t>(list + t::array::data) + 8 * uint64_t(count), part);
    fake::put(list + t::array::count, count + 1);
}
// A part with a place in the world, attached under `parent`, belonging to `owner`.
uint64_t scene_part(const std::u16string& name, uint64_t parent, uint64_t owner, bool visible = true, bool callable = true) {
    const uint64_t part = callable ? fake::engine_object(name, w.component_class, 0x80) : fake::object(name, w.component_class, 0x80);
    fake::put(part + t::scene_component::owner, owner);
    draw(part, visible);
    if (parent) attach_under(parent, part);
    parts_of[owner].push_back(part);
    return part;
}

uint64_t actor_base_class = 0, function_type = 0; // the engine's Actor and Function classes, once `install_parts` has made them

uint64_t ordinary_actor_model_table = 0;
uint64_t ordinary_model_stock(uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
void install_ordinary_actor_model() {
    // Only ordinary Actor instances get these synthetic base entries. Keep the
    // shared object/struct table intact, including initialize/destroy signatures.
    ordinary_actor_model_table = fake::memory(0x40);
    std::memcpy(reinterpret_cast<void*>(ordinary_actor_model_table), reinterpret_cast<void*>(fake::engine().vtable), 0x40);
    TSUCHI_Mod* engine = tsuchi::mod_create("the.game.ordinary");
    for (const auto entry : {std::pair<uint64_t, uint64_t>{t::ordinary_actor_vtable::end_play.offset, t::constant::ordinary_end_play},
                            {t::ordinary_actor_vtable::unregister.offset, t::constant::ordinary_unregister},
                            {t::ordinary_actor_vtable::destroyed.offset, t::constant::ordinary_destroyed}}) {
        const uint64_t at = ordinary_actor_model_table + entry.first, stock = fake::main_image() + entry.second;
        fake::put(at, stock);
        fake::code().push_back(stock);
        uint64_t id = 0;
        void* next = nullptr;
        CHECK(tsuchi::hook_wrap(engine, at, stock, reinterpret_cast<void*>(ordinary_model_stock), 0, &id, &next) == TSUCHI_OK);
    }
    // The game's level removal, the far end of the chain the lifecycle hooks, is put on with the maps in `build`.
    tsuchi::ordinary_lifecycle_start(tsuchi::mod_create("tsuchi.self.ordinary.model"));
    fake::engine().object_created = [](uint64_t object) {
        if (is_engine_actor(object) && !is_game_actor(object)) fake::put(object, ordinary_actor_model_table);
    };
    // This model admits existing scenery leases. It does not drive qualified
    // native teardown provenance or replace the game-only lifetime fixture.
}

void install_parts() {
    const uint64_t kIn = t::constant::property_param, kResult = kIn | t::constant::property_out | t::constant::property_return;
    const uint64_t actor_base = fake::object(u"Actor", w.root), function_class = tick_function_type;
    fake::set_super(w.actor_class, actor_base);
    actor_base_class = actor_base, function_type = function_class;
    install_ordinary_actor_model();
    part_base = fake::object(u"ActorComponent", w.root);
    fake::set_super(w.component_class, part_base);
    const uint64_t tick = fake::function(part_base, function_class, u"IsComponentTickEnabled", 1, 1,
        [](uint64_t object, uint64_t arguments) { fake::put<uint8_t>(arguments, tick_on(object)); });
    fake::flag(fake::bit_field(tick, u"ReturnValue", 0, 0, 0xFF), kResult);
    mover_class = fake::object(u"MovementComponent", w.root);
    fake::set_super(mover_class, part_base);
    fake::bit_field(w.component_class, u"bVisible", kVisibleAt, 0, kVisibleBit);
    fake::field(actor_base, u"RootComponent", u"ObjectProperty", int32_t(t::game_actor::root), 8, w.component_class);
    for (uint64_t table : {table_a(), table_b()}) fake::put(table + t::object_vtable::process_event, &fake::engine_process);

    const uint64_t list = fake::function(actor_base, function_class, u"K2_GetComponentsByClass", 0x18, 8, [](uint64_t object, uint64_t arguments) {
        const uint64_t wanted = fake::get<uint64_t>(arguments);
        std::vector<uint64_t> found;
        for (uint64_t part : parts_of[object])
            if (live(part) && derives_from(class_of(part), wanted)) found.push_back(part);
        const uint64_t data = found.empty() ? 0 : reinterpret_cast<uint64_t>(fake::engine_allocate(nullptr, 8 * found.size(), 0));
        for (size_t i = 0; i < found.size(); ++i) fake::put(data + 8 * i, found[i]);
        fake::put(arguments + 8 + t::array::data, data);
        fake::put(arguments + 8 + t::array::count, int32_t(found.size())), fake::put(arguments + 8 + t::array::max, int32_t(found.size()));
    });
    fake::flag(fake::field(list, u"ComponentClass", u"ClassProperty", 0, 8, w.root), kIn); // it holds a class
    fake::flag(fake::field(list, u"ReturnValue", u"ArrayProperty", 8, 0x10, fake::loose_field(u"ObjectProperty", 8, part_base)), kResult);
    const uint64_t set = fake::function(w.component_class, function_class, u"SetVisibility", 2, 1, [](uint64_t object, uint64_t arguments) {
        const bool on = fake::get<uint8_t>(arguments) != 0;
        draw(object, on);
        part_log += (on ? "show:" : "hide:") + part_name(object) + (fake::get<uint8_t>(arguments + 1) ? "+below " : " ");
    });
    fake::flag(fake::bit_field(set, u"bNewVisibility", 0, 0, 0xFF), kIn);
    fake::flag(fake::bit_field(set, u"bPropagateToChildren", 1, 0, 0xFF), kIn);

    // Making a part, attaching one, and destroying one.
    const auto structure = [] {
        const uint64_t type = fake::memory(0x80);
        fake::put<uint64_t>(type, fake::engine().vtable);
        return type;
    };
    const uint64_t vector = structure(), quaternion = structure(), transform = structure();
    for (int i = 0; i < 3; ++i) fake::field(vector, std::u16string(1, char16_t(u'X' + i)), u"FloatProperty", 4 * i, 4);
    for (int i = 0; i < 4; ++i) fake::field(quaternion, std::u16string(1, u"XYZW"[i]), u"FloatProperty", 4 * i, 4);
    fake::field(transform, u"Rotation", u"StructProperty", 0, 16, quaternion);
    fake::field(transform, u"Translation", u"StructProperty", 0x10, 12, vector);
    fake::field(transform, u"Scale3D", u"StructProperty", 0x20, 12, vector);
    base_only_class = fake::object(u"BaseOnlyComponent", w.root);
    fake::set_super(base_only_class, w.component_class);
    const uint64_t make = fake::function(actor_base, function_class, u"AddComponentByClass", 0x50, 8, [](uint64_t object, uint64_t arguments) {
        const uint64_t type = fake::get<uint64_t>(arguments);
        const bool manual = fake::get<uint8_t>(arguments + 8) != 0, deferred = fake::get<uint8_t>(arguments + 0x40) != 0;
        char text[64] = "?";
        name_text(name_of(type), text, sizeof text);
        part_log += std::string("add:") + text + (manual ? " manual" : " auto") + (deferred ? " deferred" : "") + " turn" +
                    std::to_string(int(fake::get<float>(arguments + 0x10 + 12))) + " size" + std::to_string(int(fake::get<float>(arguments + 0x30))) +
                    std::to_string(int(fake::get<float>(arguments + 0x34))) + std::to_string(int(fake::get<float>(arguments + 0x38))) + " ";
        if (type == base_only_class) return; // the engine makes nothing of a class that is only a base
        const uint64_t part = fake::engine_object(u"Added", type, 0x80);
        fake::put(part + t::scene_component::owner, object);
        if (derives_from(type, w.component_class)) {
            draw(part, true);
            if (!manual) attach_under(fake::get<uint64_t>(object + t::game_actor::root), part);
        }
        parts_of[object].push_back(part);
        fake::put(arguments + 0x48, part);
    });
    fake::flag(fake::field(make, u"Class", u"ClassProperty", 0, 8, w.root), kIn);
    fake::flag(fake::bit_field(make, u"bManualAttachment", 8, 0, 0xFF), kIn);
    fake::flag(fake::field(make, u"RelativeTransform", u"StructProperty", 0x10, 0x30, transform), kIn);
    fake::flag(fake::bit_field(make, u"bDeferredFinish", 0x40, 0, 0xFF), kIn);
    fake::flag(fake::field(make, u"ReturnValue", u"ObjectProperty", 0x48, 8, part_base), kResult);
    const uint64_t attach = fake::function(w.component_class, function_class, u"K2_AttachToComponent", 0x18, 8, [](uint64_t object, uint64_t arguments) {
        const uint64_t parent = fake::get<uint64_t>(arguments);
        char socket[64] = "";
        name_text(fake::get<Name>(arguments + 8), socket, sizeof socket);
        part_log += "attach:" + part_name(object) + ">" + part_name(parent) + "@" + socket + " ";
        if (engine_will_not_attach) return;
        attach_under(parent, object);
        fake::put<uint8_t>(arguments + 0x14, 1);
    });
    fake::flag(fake::field(attach, u"Parent", u"ObjectProperty", 0, 8, w.component_class), kIn);
    fake::flag(fake::field(attach, u"SocketName", u"NameProperty", 8, 8), kIn);
    fake::flag(fake::bit_field(attach, u"bWeldSimulatedBodies", 0x13, 0, 0xFF), kIn);
    fake::flag(fake::bit_field(attach, u"ReturnValue", 0x14, 0, 0xFF), kResult);
    const uint64_t destroy = fake::function(part_base, function_class, u"K2_DestroyComponent", 8, 8, [](uint64_t object, uint64_t arguments) {
        const uint64_t asked_by = fake::get<uint64_t>(arguments);
        part_log += "destroy:" + part_name(object) + " ";
        if (asked_by == object || asked_by == fake::get<uint64_t>(object + t::scene_component::owner)) fake::destroy(object); // the engine's own rule
    });
    const uint64_t anything = fake::object(u"Object", w.root); // what every object is
    fake::set_super(part_base, anything), fake::set_super(actor_base, anything);
    fake::flag(fake::field(destroy, u"Object", u"ObjectProperty", 0, 8, anything), kIn);

    // Meshes: how many slots one has, the material a slot is drawn with, and putting one in.
    mesh_class = fake::object(u"MeshComponent", w.root);
    fake::set_super(mesh_class, w.primitive_class);
    material_class = fake::object(u"MaterialInterface", w.root);
    fake::set_super(material_class, anything);
    stock_material = fake::engine_object(u"Stock", material_class);
    fake::field(mesh_class, u"OverrideMaterials", u"ArrayProperty", kOverridesAt, 0x10, fake::loose_field(u"ObjectProperty", 8, material_class));
    const uint64_t slots = fake::function(w.primitive_class, function_class, u"GetNumMaterials", 4, 4, [](uint64_t object, uint64_t arguments) {
        fake::put<int32_t>(arguments, slots_of[object]);
        if (engine_starves_after_counting) fake::engine().out_of_memory = true;
    });
    fake::flag(fake::field(slots, u"ReturnValue", u"IntProperty", 0, 4), kResult);
    const uint64_t get = fake::function(w.primitive_class, function_class, u"GetMaterial", 0x10, 8, [](uint64_t object, uint64_t arguments) {
        const int32_t slot = fake::get<int32_t>(arguments);
        const uint64_t put_there = override_in(object, slot);
        fake::put(arguments + 8, put_there ? put_there : slot < slots_of[object] ? stock_material : 0);
    });
    fake::flag(fake::field(get, u"ElementIndex", u"IntProperty", 0, 4), kIn);
    fake::flag(fake::field(get, u"ReturnValue", u"ObjectProperty", 8, 8, material_class), kResult);
    const uint64_t put = fake::function(w.primitive_class, function_class, u"SetMaterial", 0x10, 8, [](uint64_t object, uint64_t arguments) {
        const int32_t slot = fake::get<int32_t>(arguments);
        const uint64_t material = fake::get<uint64_t>(arguments + 8);
        put_override(object, slot, material);
        part_log += "set:" + part_name(object) + "[" + std::to_string(slot) + "]=" + (material ? part_name(material) : "none") + " ";
    });
    fake::flag(fake::field(put, u"ElementIndex", u"IntProperty", 0, 4), kIn);
    fake::flag(fake::field(put, u"Material", u"ObjectProperty", 8, 8, material_class), kIn);

    // Skeletal meshes: what animates one, and one clip played on it.
    skeletal_class = fake::object(u"SkeletalMeshComponent", w.root);
    fake::set_super(skeletal_class, mesh_class);
    clip_class = fake::object(u"AnimationAsset", w.root);
    fake::set_super(clip_class, anything);
    fake::field(skeletal_class, u"AnimationMode", u"ByteProperty", kAnimationModeAt, 1);
    const uint64_t saved = fake::memory(0x80);
    fake::put<uint64_t>(saved, fake::engine().vtable);
    fake::field(saved, u"AnimToPlay", u"ObjectProperty", 0, 8, clip_class);
    fake::bit_field(saved, u"bSavedLooping", 8, 0, kSavedLooping);
    fake::bit_field(saved, u"bSavedPlaying", 8, 0, kSavedPlaying);
    fake::field(saved, u"SavedPosition", u"FloatProperty", 12, 4);
    fake::field(saved, u"SavedPlayRate", u"FloatProperty", 16, 4);
    fake::field(skeletal_class, u"AnimationData", u"StructProperty", kAnimationDataAt, 24, saved);
    const uint64_t play = fake::function(skeletal_class, function_class, u"PlayAnimation", 0x10, 8, [](uint64_t object, uint64_t arguments) {
        Motion& motion = motions[object];
        motion.clip = fake::get<uint64_t>(arguments), motion.looping = fake::get<uint8_t>(arguments + 8) != 0, motion.playing = true, motion.position = 0;
        fake::put<uint8_t>(object + kAnimationModeAt, uint8_t(t::constant::animation_single));
        part_log += "play:" + part_name(motion.clip) + (motion.looping ? "+loop " : " ");
        if (engine_starves_after_playing) fake::engine().out_of_memory = true;
    });
    fake::flag(fake::field(play, u"NewAnimToPlay", u"ObjectProperty", 0, 8, clip_class), kIn);
    fake::flag(fake::bit_field(play, u"bLooping", 8, 0, 0xFF), kIn);
    const uint64_t mode = fake::function(skeletal_class, function_class, u"SetAnimationMode", 1, 1, [](uint64_t object, uint64_t arguments) {
        const uint8_t wanted = fake::get<uint8_t>(arguments);
        fake::put<uint8_t>(object + kAnimationModeAt, wanted);
        if (wanted == t::constant::animation_by_blueprint) motions.erase(object);
        part_log += "mode" + std::to_string(wanted) + ":" + part_name(object) + " ";
    });
    fake::flag(fake::field(mode, u"InAnimationMode", u"ByteProperty", 0, 1), kIn);
    const uint64_t rate = fake::function(skeletal_class, function_class, u"SetPlayRate", 4, 4, [](uint64_t object, uint64_t arguments) {
        motions[object].rate = fake::get<float>(arguments);
        char text[32];
        std::snprintf(text, sizeof text, "rate:%g ", double(fake::get<float>(arguments)));
        part_log += text;
    });
    fake::flag(fake::field(rate, u"Rate", u"FloatProperty", 0, 4), kIn);
    const uint64_t seek = fake::function(skeletal_class, function_class, u"SetPosition", 8, 4, [](uint64_t object, uint64_t arguments) {
        motions[object].position = fake::get<float>(arguments);
        char text[32];
        std::snprintf(text, sizeof text, "seek:%g%s ", double(fake::get<float>(arguments)), fake::get<uint8_t>(arguments + 4) ? "+notify" : "");
        part_log += text;
    });
    fake::flag(fake::field(seek, u"InPos", u"FloatProperty", 0, 4), kIn);
    fake::flag(fake::bit_field(seek, u"bFireNotifies", 4, 0, 0xFF), kIn);
    const uint64_t where = fake::function(skeletal_class, function_class, u"GetPosition", 4, 4, [](uint64_t object, uint64_t arguments) { fake::put(arguments, motions[object].position); });
    fake::flag(fake::field(where, u"ReturnValue", u"FloatProperty", 0, 4), kResult);
    const uint64_t speed = fake::function(skeletal_class, function_class, u"GetPlayRate", 4, 4, [](uint64_t object, uint64_t arguments) { fake::put(arguments, motions[object].rate); });
    fake::flag(fake::field(speed, u"ReturnValue", u"FloatProperty", 0, 4), kResult);
    const uint64_t going = fake::function(skeletal_class, function_class, u"IsPlaying", 1, 1, [](uint64_t object, uint64_t arguments) {
        fake::put<uint8_t>(arguments, motions[object].playing);
    });
    fake::flag(fake::bit_field(going, u"ReturnValue", 0, 0, 0xFF), kResult);
}

void parts() {
    namespace a = t::game_actor;
    const TSUCHI_ComponentService& parts_api = *tsuchi::host.component;
    const auto part = [](uint64_t object) { return TSUCHI_Component{to_handle(object)}; };

    // A totem: a root, a mesh on it, an eye and a lash on the mesh, something that moves it,
    // and another actor's hat riding on the mesh.
    const uint64_t totem = tabled(table_a(), u"Totem", u"Totem", 0, 0);
    registers(totem);
    const uint64_t root_of = fake::get<uint64_t>(totem + a::root);
    fake::put(root_of + t::scene_component::owner, totem);
    parts_of[totem].push_back(root_of);
    draw(root_of, true);
    const uint64_t mesh = scene_part(u"Mesh", root_of, totem), eye = scene_part(u"Eye", mesh, totem);
    const uint64_t lash = scene_part(u"Lash", eye, totem, false); // not drawn to begin with
    const uint64_t hat = scene_part(u"Hat", mesh, w.red);
    const uint64_t mover = fake::engine_object(u"Mover", mover_class, 0x80);
    fake::put(mover + t::scene_component::owner, totem);
    parts_of[totem].push_back(mover);

    // Its parts, in the engine's order, with or without room for them.
    TSUCHI_Component listed[8];
    uint64_t count = 0;
    const int freed = fake::engine().freed;
    CHECK(parts_api.list(mod, handle(totem), listed, 8, &count) == TSUCHI_OK && count == 5);
    CHECK(fake::engine().freed == freed + 2); // asked twice, and the memory of each asking went back
    CHECK(from_handle(listed[0].id) == root_of && from_handle(listed[1].id) == mesh && from_handle(listed[2].id) == eye &&
          from_handle(listed[3].id) == lash && from_handle(listed[4].id) == mover);
    TSUCHI_Component few[2] = {{7}, {7}};
    CHECK(parts_api.list(mod, handle(totem), few, 2, &count) == TSUCHI_CAPACITY && count == 5 && few[0].id == 7 && few[1].id == 7);
    CHECK(parts_api.list(mod, handle(mesh), listed, 8, &count) == TSUCHI_NOT_FOUND); // a part is not an actor
    // By name, and whose it is.
    TSUCHI_Component found{};
    TSUCHI_Actor whose{};
    CHECK(parts_api.find(mod, handle(totem), "Eye", &found) == TSUCHI_OK && from_handle(found.id) == eye);
    CHECK(parts_api.find(mod, handle(totem), "mOVER", &found) == TSUCHI_OK && from_handle(found.id) == mover);
    CHECK(parts_api.find(mod, handle(totem), "Hat", &found) == TSUCHI_NOT_FOUND && from_handle(found.id) == mover); // another actor's
    CHECK(parts_api.find(mod, handle(totem), "Ey", &found) == TSUCHI_NOT_FOUND && parts_api.find(mod, handle(totem), "Eyes", &found) == TSUCHI_NOT_FOUND);
    CHECK(parts_api.owner(mod, part(eye), &whose) == TSUCHI_OK && from_handle(whose.id) == totem);
    CHECK(parts_api.owner(mod, part(mover), &whose) == TSUCHI_OK && from_handle(whose.id) == totem);
    CHECK(parts_api.owner(mod, part(hat), &whose) == TSUCHI_OK && from_handle(whose.id) == w.red);
    CHECK(parts_api.owner(mod, part(totem), &whose) == TSUCHI_NOT_FOUND); // an actor is not a part
    const uint64_t loose = fake::engine_object(u"Loose", w.component_class, 0x80);
    CHECK(parts_api.owner(mod, part(loose), &whose) == TSUCHI_NOT_FOUND);
    fake::put(loose + t::scene_component::owner, mesh); // what it says owns it is not an actor
    CHECK(parts_api.owner(mod, part(loose), &whose) == TSUCHI_NOT_FOUND);
    const uint64_t ghost = tabled(table_a(), u"Ghost", u"Ghost", 0, 0);
    fake::put(loose + t::scene_component::owner, ghost);
    CHECK(parts_api.owner(mod, part(loose), &whose) == TSUCHI_OK && from_handle(whose.id) == ghost);
    fake::destroy(ghost); // its actor is gone
    CHECK(parts_api.owner(mod, part(loose), &whose) == TSUCHI_NOT_FOUND);
    // Whether a part is drawn can only be told of one with a place in the world; and what a
    // function gave back is a list of objects only when it is one.
    bool told = true;
    CHECK(part_visible(mesh, told) && told && !part_visible(mover, told));
    {
        EngineCall call(mesh, "SetVisibility");
        uint32_t length = 9;
        CHECK(call.ready() == TSUCHI_OK && call.objects("bNewVisibility", length) == TSUCHI_INVALID && length == 0);
        EngineCall none(mesh, "Explode");
        CHECK(none.ready() == TSUCHI_NOT_FOUND && none.invoke() == TSUCHI_NOT_FOUND);
    }

    // Hidden with what is attached below it: each of the actor's own parts by itself, none of
    // another actor's, and one that was not drawn is held without being touched.
    const uint32_t owned = tsuchi::owned_count(mod);
    bool is_hidden = false;
    const auto hidden = [&](uint64_t which) { return parts_api.hidden(mod, part(which), &is_hidden) == TSUCHI_OK && is_hidden; };
    const size_t three = 3 * sizeof(uint64_t); // what the list of three covered parts takes
    CHECK(blocks_of(three) == 0);
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_OK && part_log == "hide:Mesh hide:Eye " && tsuchi::owned_count(mod) == owned + 1);
    CHECK(blocks_of(three) == 1);
    CHECK(!drawn(mesh) && !drawn(eye) && !drawn(lash) && drawn(hat) && drawn(root_of));
    CHECK(hidden(mesh) && hidden(eye) && hidden(lash) && !hidden(hat) && !hidden(root_of) && !hidden(mover));
    CHECK(tsuchi::hidden_counts().parts == 3 && tsuchi::hidden_counts().holds == 1);
    // Asked again by the same mod: nothing new.
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_OK && part_log == "hide:Mesh hide:Eye " && tsuchi::owned_count(mod) == owned + 1);
    // The game shows two of them: hidden again on the next frame, and only those.
    draw(eye, true), draw(lash, true);
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK((part_log == "hide:Eye hide:Lash " || part_log == "hide:Lash hide:Eye ") && !drawn(eye) && !drawn(lash));
    tsuchi::frame(0.03f);
    CHECK(part_log.size() == 19);
    // Shown: each goes back to what it was.
    part_log.clear();
    CHECK(parts_api.release_hidden(mod, part(mesh)) == TSUCHI_OK && part_log == "show:Mesh show:Eye " && !drawn(lash) && tsuchi::owned_count(mod) == owned);
    CHECK(blocks_of(three) == 0);
    CHECK(!hidden(mesh) && !hidden(eye) && !hidden(lash) && tsuchi::hidden_counts().parts == 0 && tsuchi::hidden_counts().holds == 0);
    CHECK(parts_api.release_hidden(mod, part(mesh)) == TSUCHI_NOT_FOUND && parts_api.release_hidden(mod, part(eye)) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(part_log == "show:Mesh show:Eye " && drawn(mesh));

    // By itself: what is attached below it stays.
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), false) == TSUCHI_OK && part_log == "hide:Mesh " && drawn(eye) && hidden(mesh) && !hidden(eye));
    CHECK(parts_api.release_hidden(mod, part(mesh)) == TSUCHI_OK && drawn(mesh));
    // Asked again with the other choice of `children`, the mod's one hold covers what is asked for now.
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), false) == TSUCHI_OK && parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_OK && part_log == "hide:Mesh hide:Eye " &&
          hidden(eye) && hidden(lash) && tsuchi::hidden_counts().holds == 1 && tsuchi::hidden_counts().parts == 3 && tsuchi::owned_count(mod) == owned + 1);
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), false) == TSUCHI_OK && part_log == "show:Eye " && hidden(mesh) && !hidden(eye) && !hidden(lash) &&
          tsuchi::hidden_counts().holds == 1 && tsuchi::hidden_counts().parts == 1 && blocks_of(three) == 0);
    CHECK(parts_api.release_hidden(mod, part(mesh)) == TSUCHI_OK && drawn(mesh) && tsuchi::hidden_counts().parts == 0 && tsuchi::owned_count(mod) == owned);
    // A part with no place in the world is never drawn, and an actor is not a part.
    CHECK(parts_api.hold_hidden(mod, part(mover), true) == TSUCHI_UNSUPPORTED && parts_api.hold_hidden(mod, part(totem), true) == TSUCHI_NOT_FOUND);
    CHECK(parts_api.hidden(mod, part(totem), &is_hidden) == TSUCHI_NOT_FOUND && tsuchi::owned_count(mod) == owned);

    // Two mods: a part stays hidden while either holds it.
    TSUCHI_Mod* other = tsuchi::mod_create("actor.painter");
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_OK && parts_api.hold_hidden(other, part(eye), false) == TSUCHI_OK && part_log == "hide:Mesh hide:Eye ");
    CHECK(parts_api.release_hidden(other, part(mesh)) == TSUCHI_NOT_FOUND); // that hold is the first mod's
    part_log.clear();
    CHECK(parts_api.release_hidden(mod, part(mesh)) == TSUCHI_OK && part_log == "show:Mesh " && !drawn(eye) && hidden(eye) && !hidden(mesh));
    CHECK(parts_api.release_hidden(other, part(eye)) == TSUCHI_OK && part_log == "show:Mesh show:Eye " && drawn(eye) && !hidden(eye));
    // A mod that stops has its holds ended.
    CHECK(parts_api.hold_hidden(other, part(mesh), true) == TSUCHI_OK && !drawn(mesh));
    part_log.clear();
    tsuchi::mod_stop(other);
    CHECK(part_log == "show:Mesh show:Eye " && drawn(mesh) && tsuchi::hidden_counts().parts == 0 && tsuchi::hidden_counts().holds == 0);

    // The engine cannot be asked: nothing is hidden and nothing is held.
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, 0);
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_NOT_READY && part_log.empty() && drawn(mesh) && !hidden(mesh) && tsuchi::owned_count(mod) == owned);
    CHECK(parts_api.list(mod, handle(totem), listed, 8, &count) == TSUCHI_NOT_READY && parts_api.find(mod, handle(totem), "Eye", &found) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, fake::engine().allocator);
    // One part of several cannot be hidden: the ones before it are shown again.
    const uint64_t stub = scene_part(u"Stub", eye, totem, true, false);
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_NOT_READY && part_log == "hide:Mesh hide:Eye show:Mesh show:Eye " && drawn(mesh) && drawn(eye));
    CHECK(!hidden(mesh) && !hidden(eye) && !hidden(stub) && tsuchi::owned_count(mod) == owned && tsuchi::hidden_counts().holds == 0);
    fake::destroy(stub);
    // No memory for the list of what a hold covers: nothing is changed.
    refused_size = three;
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_NO_MEMORY && part_log.empty() && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    // No memory to record the hold against the mod: the same, and the list goes back.
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.poor");
    refused_multiple = 40; // the size of one of a mod's records
    CHECK(parts_api.hold_hidden(poor, part(mesh), true) == TSUCHI_NO_MEMORY && part_log.empty() && drawn(mesh) && blocks_of(three) == 0 &&
          tsuchi::hidden_counts().holds == 0);
    refused_multiple = 0;
    tsuchi::mod_stop(poor);
    // Parts that lead back into themselves cannot all be found.
    attach_under(lash, mesh);
    refused_size = 65 * sizeof(uint64_t); // refused before any list is made: 65 is how far the walk got
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_UNSUPPORTED && part_log.empty() && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    fake::put<int32_t>(lash + t::scene_component::attach_children + t::array::count, 0);

    // A part destroyed while hidden is forgotten, and a hold whose own part is destroyed ends.
    CHECK(parts_api.hold_hidden(mod, part(mesh), true) == TSUCHI_OK && tsuchi::hidden_counts().parts == 3);
    fake::destroy(eye);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::hidden_counts().parts == 2 && tsuchi::hidden_counts().holds == 1 && tsuchi::owned_count(mod) == owned + 1);
    fake::destroy(mesh);
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK(tsuchi::hidden_counts().parts == 0 && tsuchi::hidden_counts().holds == 0 && tsuchi::owned_count(mod) == owned && part_log.empty());

    // However many parts are attached below one, each is hidden and shown again.
    const uint64_t crown = scene_part(u"Crown", root_of, totem);
    for (int i = 0; i < 50; ++i) scene_part(u"Jewel", crown, totem);
    // Listed in the engine's order however many there are, and the first of a name is the one found.
    TSUCHI_Component many[64];
    std::vector<uint64_t> expected;
    for (uint64_t each : parts_of[totem])
        if (live(each)) expected.push_back(each);
    bool in_order = parts_api.list(mod, handle(totem), many, 64, &count) == TSUCHI_OK && count == expected.size() && count > 50;
    for (size_t i = 0; in_order && i < expected.size(); ++i) in_order = from_handle(many[i].id) == expected[i];
    CHECK(in_order);
    CHECK(parts_api.find(mod, handle(totem), "Jewel", &found) == TSUCHI_OK && from_handle(found.id) == parts_of[totem][parts_of[totem].size() - 50]);
    part_log.clear();
    CHECK(parts_api.hold_hidden(mod, part(crown), true) == TSUCHI_OK && tsuchi::hidden_counts().parts == 51 && part_log.size() == 11 + 50 * 11);
    part_log.clear();
    CHECK(parts_api.release_hidden(mod, part(crown)) == TSUCHI_OK && tsuchi::hidden_counts().parts == 0 && part_log.size() == 11 + 50 * 11);

    // An actor put away for another use leaves as the game made it. A hold on another actor's part stays.
    CHECK(parts_api.hold_hidden(mod, part(crown), false) == TSUCHI_OK && !drawn(crown) && parts_api.hold_hidden(mod, part(hat), false) == TSUCHI_OK);
    CHECK(tsuchi::owned_count(mod) == owned + 2);
    part_log.clear();
    pool_return(kPool, totem);
    CHECK(part_log == "show:Crown " && drawn(crown) && !drawn(hat) && hidden(hat) && tsuchi::owned_count(mod) == owned + 1);
    CHECK(parts_api.release_hidden(mod, part(hat)) == TSUCHI_OK && drawn(hat) && tsuchi::owned_count(mod) == owned && tsuchi::hidden_counts().holds == 0);

    // An actor the game destroys is left as it is: its parts are not shown again first, and the holds are over.
    const uint64_t effigy = tabled(table_a(), u"Effigy", u"Effigy", 0, 0);
    registers(effigy);
    const uint64_t effigy_root = fake::get<uint64_t>(effigy + a::root);
    fake::put(effigy_root + t::scene_component::owner, effigy);
    parts_of[effigy].push_back(effigy_root);
    const uint64_t mask = scene_part(u"Mask", effigy_root, effigy);
    scene_part(u"Plume", mask, effigy);
    CHECK(parts_api.hold_hidden(mod, part(mask), true) == TSUCHI_OK && tsuchi::hidden_counts().parts == 2);
    part_log.clear();
    ends_play(effigy);
    CHECK(part_log.empty() && !drawn(mask) && tsuchi::hidden_counts().parts == 0 && tsuchi::hidden_counts().holds == 0 && tsuchi::owned_count(mod) == owned);

    // For the script: an idol with a face and a nose.
    idol = tabled(table_a(), u"Idol", u"Idol", 0, 0);
    registers(idol);
    const uint64_t idol_root = fake::get<uint64_t>(idol + a::root);
    fake::put(idol_root + t::scene_component::owner, idol);
    parts_of[idol].push_back(idol_root);
    scene_part(u"Nose", scene_part(u"Face", idol_root, idol), idol);
}

// Parts a mod adds to an actor.
void added_parts() {
    namespace a = t::game_actor;
    const TSUCHI_ComponentService& parts_api = *tsuchi::host.component;
    const auto part = [](uint64_t object) { return TSUCHI_Component{to_handle(object)}; };
    const auto type = [](uint64_t object) { return TSUCHI_Object{to_handle(object)}; };
    const TSUCHI_Component none{};
    const uint32_t owned = tsuchi::owned_count(mod);

    // A shrine with a pillar on its root.
    const uint64_t shrine = tabled(table_a(), u"Shrine", u"Shrine", 0, 0);
    registers(shrine);
    const uint64_t shrine_root = fake::get<uint64_t>(shrine + a::root);
    fake::put(shrine_root + t::scene_component::owner, shrine);
    parts_of[shrine].push_back(shrine_root);
    const uint64_t pillar = scene_part(u"Pillar", shrine_root, shrine);

    // No memory to remember a part by: the engine is not asked to make one.
    TSUCHI_Component made{7};
    part_log.clear();
    refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // the first room for sixteen of them
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), none, "", &made) == TSUCHI_NO_MEMORY && part_log.empty() && made.id == 7);
    refused_size = 0;

    // Added under the actor's root, where the root is, with no turn and its own size. It is the
    // actor's, and the mod's.
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), none, "", &made) == TSUCHI_OK && part_log == "add:SceneComponent auto turn1 size111 ");
    const uint64_t first = from_handle(made.id);
    TSUCHI_Actor whose{};
    CHECK(first && class_of(first) == w.component_class && parts_api.owner(mod, made, &whose) == TSUCHI_OK && from_handle(whose.id) == shrine);
    CHECK(tsuchi::owned_count(mod) == owned + 1 && tsuchi::added_count() == 1);
    TSUCHI_Component listed[8];
    uint64_t count = 0;
    CHECK(parts_api.list(mod, handle(shrine), listed, 8, &count) == TSUCHI_OK && count == 3 && listed[2].id == made.id);
    // Under one of its parts, at a socket.
    part_log.clear();
    TSUCHI_Component tip{};
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), part(pillar), "Tip", &tip) == TSUCHI_OK &&
          part_log == "add:SceneComponent manual turn1 size111 attach:Added>Pillar@Tip " && tip.id != made.id && tsuchi::added_count() == 2);
    part_log.clear();
    TSUCHI_Component plain{};
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), part(pillar), "", &plain) == TSUCHI_OK &&
          part_log == "add:SceneComponent manual turn1 size111 attach:Added>Pillar@ ");
    // A part with no place in the world can be added, and attached nowhere.
    TSUCHI_Component mover{};
    CHECK(parts_api.add(mod, handle(shrine), type(mover_class), none, "", &mover) == TSUCHI_OK && tsuchi::added_count() == 4);
    part_log.clear();
    made.id = 7;
    CHECK(parts_api.add(mod, handle(shrine), type(mover_class), part(pillar), "", &made) == TSUCHI_INVALID);
    // What is not a class of part, and a parent that is not the actor's own or has no place.
    CHECK(parts_api.add(mod, handle(shrine), type(pillar), none, "", &made) == TSUCHI_INVALID && parts_api.add(mod, handle(shrine), type(w.actor_class), none, "", &made) == TSUCHI_INVALID);
    // An object that is not a class is not read as one, whatever lies where a class keeps its parent.
    const uint64_t decoy = fake::object(u"Decoy", w.component_class, 0x100);
    fake::put(decoy + t::struct_type::super, part_base);
    CHECK(parts_api.add(mod, handle(shrine), type(decoy), none, "", &made) == TSUCHI_INVALID && part_log.empty());
    const uint64_t elsewhere = scene_part(u"Elsewhere", 0, w.red);
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), part(elsewhere), "", &made) == TSUCHI_INVALID);
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), mover, "", &made) == TSUCHI_INVALID);
    // What is gone, or is not what it should be.
    CHECK(parts_api.add(mod, handle(pillar), type(w.component_class), none, "", &made) == TSUCHI_NOT_FOUND); // not an actor
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), TSUCHI_Component{to_handle(shrine)}, "", &made) == TSUCHI_NOT_FOUND); // not a part
    const uint64_t gone_class = fake::object(u"GoneComponent", w.root);
    const TSUCHI_Object gone = type(gone_class);
    fake::destroy(gone_class);
    CHECK(parts_api.add(mod, handle(shrine), gone, none, "", &made) == TSUCHI_NOT_FOUND);
    CHECK(part_log.empty() && made.id == 7 && tsuchi::added_count() == 4 && tsuchi::owned_count(mod) == owned + 4);
    // The engine makes nothing, or will not attach what it made: nothing is left behind.
    CHECK(parts_api.add(mod, handle(shrine), type(base_only_class), none, "", &made) == TSUCHI_UNSUPPORTED && part_log == "add:BaseOnlyComponent auto turn1 size111 ");
    engine_will_not_attach = true;
    part_log.clear();
    const size_t parts_before = parts_of[shrine].size();
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), part(pillar), "", &made) == TSUCHI_UNSUPPORTED &&
          part_log == "add:SceneComponent manual turn1 size111 attach:Added>Pillar@ destroy:Added " && !live(parts_of[shrine][parts_before]));
    engine_will_not_attach = false;
    // The engine cannot be asked.
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, 0);
    CHECK(parts_api.add(mod, handle(shrine), type(w.component_class), none, "", &made) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, fake::engine().allocator);
    // No memory to record it against the mod: the part the engine made is destroyed again.
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.pauper");
    refused_multiple = 40;
    part_log.clear();
    CHECK(parts_api.add(poor, handle(shrine), type(w.component_class), none, "", &made) == TSUCHI_NO_MEMORY &&
          part_log == "add:SceneComponent auto turn1 size111 destroy:Added " && made.id == 7);
    refused_multiple = 0;
    CHECK(tsuchi::added_count() == 4 && tsuchi::owned_count(mod) == owned + 4);

    // Removed by the mod that added it, and by no other. A part of the game's own is not removed.
    part_log.clear();
    CHECK(parts_api.remove(poor, tip) == TSUCHI_DENIED && parts_api.remove(mod, part(pillar)) == TSUCHI_NOT_FOUND && part_log.empty());
    CHECK(parts_api.remove(mod, tip) == TSUCHI_OK && part_log == "destroy:Added " && !from_handle(tip.id) && tsuchi::added_count() == 3 && tsuchi::owned_count(mod) == owned + 3);
    CHECK(parts_api.remove(mod, tip) == TSUCHI_NOT_FOUND);
    // The game destroys one itself: forgotten on the next frame.
    fake::destroy(from_handle(plain.id));
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK(tsuchi::added_count() == 2 && tsuchi::owned_count(mod) == owned + 2 && part_log.empty());
    // A mod that stops has its parts destroyed.
    TSUCHI_Component theirs{};
    CHECK(parts_api.add(poor, handle(shrine), type(w.component_class), none, "", &theirs) == TSUCHI_OK && tsuchi::added_count() == 3);
    part_log.clear();
    tsuchi::mod_stop(poor);
    CHECK(part_log == "destroy:Added " && !from_handle(theirs.id) && tsuchi::added_count() == 2);
    // An actor put away for another use leaves without what mods added; another actor's stay.
    TSUCHI_Component kept{};
    CHECK(parts_api.add(mod, handle(idol), type(w.component_class), none, "", &kept) == TSUCHI_OK && tsuchi::added_count() == 3);
    part_log.clear();
    pool_return(kPool, shrine);
    CHECK(part_log == "destroy:Added destroy:Added " && !from_handle(mover.id) && from_handle(kept.id) && tsuchi::added_count() == 1 &&
          tsuchi::owned_count(mod) == owned + 1);
    CHECK(parts_api.remove(mod, kept) == TSUCHI_OK && tsuchi::added_count() == 0 && tsuchi::owned_count(mod) == owned);
    // An actor the game destroys: the engine destroys its parts with it, and the framework only forgets the one a mod added.
    const uint64_t cairn = tabled(table_a(), u"Cairn", u"Cairn", 0, 0);
    registers(cairn);
    const uint64_t cairn_root = fake::get<uint64_t>(cairn + a::root);
    fake::put(cairn_root + t::scene_component::owner, cairn);
    parts_of[cairn].push_back(cairn_root);
    CHECK(parts_api.add(mod, handle(cairn), type(w.component_class), none, "", &kept) == TSUCHI_OK && tsuchi::added_count() == 1);
    part_log.clear();
    ends_play(cairn);
    CHECK(part_log.empty() && tsuchi::added_count() == 0 && tsuchi::owned_count(mod) == owned);
}

// However many parts of the framework listen for an actor leaving, each is told, once.
int leave_calls = 0;
template <int N>
void leave_listener(uint64_t, uint64_t, tsuchi::ActorLeft) { ++leave_calls; }
void leaving() {
    void (*const listeners[])(uint64_t, uint64_t, tsuchi::ActorLeft) = {leave_listener<0>,  leave_listener<1>,  leave_listener<2>,  leave_listener<3>,  leave_listener<4>,
                                                     leave_listener<5>,  leave_listener<6>,  leave_listener<7>,  leave_listener<8>,  leave_listener<9>,
                                                     leave_listener<10>, leave_listener<11>, leave_listener<12>, leave_listener<13>, leave_listener<14>,
                                                     leave_listener<15>, leave_listener<16>, leave_listener<17>, leave_listener<18>, leave_listener<19>};
    for (auto* listener : listeners) CHECK(tsuchi::on_actor_left(listener));
    CHECK(tsuchi::on_actor_left(listeners[3])); // added already: not twice
    const uint64_t leaver = tabled(table_a(), u"Leaver", u"Leaver", 0, 0);
    registers(leaver);
    pool_return(kPool, leaver);
    CHECK(leave_calls == 20);
}

// A material held in a slot of a mesh.
void materials() {
    namespace a = t::game_actor;
    const TSUCHI_ComponentService& parts_api = *tsuchi::host.component;
    const auto part = [](uint64_t object) { return TSUCHI_Component{to_handle(object)}; };
    const auto object = [](uint64_t address) { return TSUCHI_Object{to_handle(address)}; };
    const auto mesh_part = [](const std::u16string& name, uint64_t parent, uint64_t owner, int32_t slots) {
        const uint64_t mesh = fake::engine_object(name, mesh_class, 0x100);
        fake::put(mesh + t::scene_component::owner, owner);
        draw(mesh, true);
        attach_under(parent, mesh);
        parts_of[owner].push_back(mesh);
        slots_of[mesh] = slots;
        return mesh;
    };
    const uint32_t owned = tsuchi::owned_count(mod);

    // A statue with a body of three slots and a marker that is no mesh; and a few materials.
    const uint64_t statue = tabled(table_a(), u"Statue", u"Statue", 0, 0);
    registers(statue);
    const uint64_t statue_root = fake::get<uint64_t>(statue + a::root);
    fake::put(statue_root + t::scene_component::owner, statue);
    parts_of[statue].push_back(statue_root);
    const uint64_t body = mesh_part(u"Body", statue_root, statue, 3), marker = scene_part(u"Marker", statue_root, statue);
    red_material = fake::engine_object(u"Red", material_class);
    const uint64_t blue = fake::engine_object(u"Blue", material_class), glow = fake::engine_object(u"Glow", material_class);

    // What a slot is drawn with.
    TSUCHI_Object shown{};
    CHECK(parts_api.material(mod, part(body), 0, &shown) == TSUCHI_OK && from_handle(shown.id) == stock_material);
    CHECK(parts_api.material(mod, part(body), 3, &shown) == TSUCHI_INVALID && parts_api.material(mod, part(marker), 0, &shown) == TSUCHI_UNSUPPORTED);
    CHECK(parts_api.material(mod, part(statue), 0, &shown) == TSUCHI_NOT_FOUND && from_handle(shown.id) == stock_material);
    slots_of[body] = 4; // a slot the mesh has, with nothing to draw it with
    stock_material = 0;
    CHECK(parts_api.material(mod, part(body), 3, &shown) == TSUCHI_NOT_FOUND);
    stock_material = from_handle(shown.id), slots_of[body] = -1;
    CHECK(parts_api.material(mod, part(body), 0, &shown) == TSUCHI_NOT_READY); // a count that cannot be one
    slots_of[body] = 3;
    // What has been put in a slot over the mesh's own, read where the engine keeps it: only of a
    // mesh, only within the list, and only a material that is still there.
    const uint64_t lookalike = fake::engine_object(u"Lookalike", w.component_class, 0x100);
    const uint64_t passing = fake::engine_object(u"Passing", material_class);
    put_override(lookalike, 0, passing), put_override(body, 2, passing);
    CHECK(slot_override(body, 2) == passing && slot_override(body, 1) == 0 && slot_override(lookalike, 0) == 0);
    fake::put<int32_t>(body + kOverridesAt + t::array::count, 1); // the list is shorter now; what lay beyond is still in memory
    CHECK(slot_override(body, 2) == 0);
    put_override(body, 0, passing);
    fake::destroy(passing);
    CHECK(slot_override(body, 0) == 0);
    fake::put<int32_t>(body + kOverridesAt + t::array::count, 0);

    // No memory to remember a hold by, or to record it against the mod: the slot is left alone.
    part_log.clear();
    refused_size = 16 * (sizeof(tsuchi::Hold) + 32); // the first room for sixteen material holds
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_NO_MEMORY && part_log.empty());
    refused_size = 0;
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.beggar");
    refused_multiple = 40;
    CHECK(parts_api.hold_material(poor, part(body), 1, object(red_material)) == TSUCHI_NO_MEMORY && part_log.empty() && tsuchi::material_holds() == 0);
    refused_multiple = 0;
    // The engine counts the slots and then cannot be asked for more: nothing is held.
    engine_starves_after_counting = true;
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_NOT_READY && tsuchi::material_holds() == 0 && tsuchi::owned_count(mod) == owned);
    engine_starves_after_counting = false, fake::engine().out_of_memory = false;
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, 0);
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_NOT_READY && parts_api.material(mod, part(body), 0, &shown) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, fake::engine().allocator);
    CHECK(part_log.empty() && !override_in(body, 1));

    // Put in a slot and held there; put again, it is another material in the same hold.
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_OK && part_log == "set:Body[1]=Red " && override_in(body, 1) == red_material);
    CHECK(parts_api.material(mod, part(body), 1, &shown) == TSUCHI_OK && from_handle(shown.id) == red_material);
    CHECK(tsuchi::owned_count(mod) == owned + 1 && tsuchi::material_holds() == 1);
    part_log.clear();
    CHECK(parts_api.hold_material(mod, part(body), 1, object(blue)) == TSUCHI_OK && part_log == "set:Body[1]=Blue " && tsuchi::owned_count(mod) == owned + 1 &&
          tsuchi::material_holds() == 1);
    // What is not a material, a slot the mesh lacks, a part that is no mesh, and what is gone.
    part_log.clear();
    CHECK(parts_api.hold_material(mod, part(body), 0, object(marker)) == TSUCHI_INVALID && parts_api.hold_material(mod, part(body), 3, object(red_material)) == TSUCHI_INVALID);
    CHECK(parts_api.hold_material(mod, part(marker), 0, object(red_material)) == TSUCHI_UNSUPPORTED);
    const uint64_t faded = fake::engine_object(u"Faded", material_class);
    const TSUCHI_Object faded_handle = object(faded);
    fake::destroy(faded);
    CHECK(parts_api.hold_material(mod, part(body), 0, faded_handle) == TSUCHI_NOT_FOUND && parts_api.hold_material(mod, part(statue), 0, object(red_material)) == TSUCHI_NOT_FOUND);
    CHECK(part_log.empty() && tsuchi::material_holds() == 1);
    // One mod to a slot; another slot is another matter.
    CHECK(parts_api.hold_material(poor, part(body), 1, object(red_material)) == TSUCHI_BUSY && parts_api.release_material(poor, part(body), 1) == TSUCHI_NOT_FOUND);
    CHECK(parts_api.hold_material(poor, part(body), 2, object(red_material)) == TSUCHI_OK && tsuchi::material_holds() == 2 && override_in(body, 1) == blue);

    // The game puts something else in a held slot: it is taken again on the next frame, and
    // what the game put there is what the slot goes back to.
    put_override(body, 1, glow);
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK(part_log == "set:Body[1]=Blue " && override_in(body, 1) == blue);
    tsuchi::frame(0.03f);
    CHECK(part_log == "set:Body[1]=Blue ");
    part_log.clear();
    CHECK(parts_api.release_material(mod, part(body), 1) == TSUCHI_OK && part_log == "set:Body[1]=Glow " && override_in(body, 1) == glow &&
          tsuchi::owned_count(mod) == owned && tsuchi::material_holds() == 1);
    CHECK(parts_api.release_material(mod, part(body), 1) == TSUCHI_NOT_FOUND);
    // A slot the game had already put something in goes back to that.
    part_log.clear();
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_OK && parts_api.release_material(mod, part(body), 1) == TSUCHI_OK &&
          part_log == "set:Body[1]=Red set:Body[1]=Glow ");
    // A slot that showed the mesh's own material goes back to that.
    part_log.clear();
    CHECK(parts_api.hold_material(mod, part(body), 0, object(red_material)) == TSUCHI_OK && parts_api.release_material(mod, part(body), 0) == TSUCHI_OK);
    CHECK(part_log == "set:Body[0]=Red set:Body[0]=none " && !override_in(body, 0) && parts_api.material(mod, part(body), 0, &shown) == TSUCHI_OK &&
          from_handle(shown.id) == stock_material);
    // A mod that stops has its slots put back.
    part_log.clear();
    tsuchi::mod_stop(poor);
    CHECK(part_log == "set:Body[2]=none " && tsuchi::material_holds() == 0);

    // A material that is destroyed while held is not put in again.
    const uint64_t brief = fake::engine_object(u"Brief", material_class);
    CHECK(parts_api.hold_material(mod, part(body), 0, object(brief)) == TSUCHI_OK);
    fake::destroy(brief);
    put_override(body, 0, glow);
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK(part_log.empty() && tsuchi::material_holds() == 1 && parts_api.release_material(mod, part(body), 0) == TSUCHI_OK && part_log == "set:Body[0]=none ");
    // A mesh destroyed while a slot is held: the hold ends by itself.
    const uint64_t arm = mesh_part(u"Arm", statue_root, statue, 1);
    CHECK(parts_api.hold_material(mod, part(arm), 0, object(red_material)) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    fake::destroy(arm);
    part_log.clear();
    tsuchi::frame(0.03f);
    CHECK(part_log.empty() && tsuchi::material_holds() == 0 && tsuchi::owned_count(mod) == owned);
    // An actor put away for another use leaves as the game made it; another actor's slot stays held.
    const uint64_t skin = mesh_part(u"Skin", fake::get<uint64_t>(idol + a::root), idol, 2);
    CHECK(parts_api.hold_material(mod, part(body), 1, object(red_material)) == TSUCHI_OK && parts_api.hold_material(mod, part(skin), 1, object(blue)) == TSUCHI_OK);
    part_log.clear();
    CHECK(parts_api.release_material(mod, part(skin), 1) == TSUCHI_OK && part_log == "set:Skin[1]=none " && override_in(body, 1) == red_material);
    CHECK(parts_api.release_material(mod, part(body), 1) == TSUCHI_OK && tsuchi::material_holds() == 0);
    CHECK(parts_api.hold_material(mod, part(body), 2, object(red_material)) == TSUCHI_OK && parts_api.hold_material(mod, part(skin), 1, object(blue)) == TSUCHI_OK);
    part_log.clear();
    pool_return(kPool, statue);
    CHECK(part_log == "set:Body[2]=none " && tsuchi::material_holds() == 1 && tsuchi::owned_count(mod) == owned + 1 && override_in(skin, 1) == blue);
    CHECK(parts_api.release_material(mod, part(skin), 1) == TSUCHI_OK && tsuchi::material_holds() == 0 && tsuchi::owned_count(mod) == owned);
    // An actor the game destroys is left as it is: its slot is not set once more, and the hold is over.
    const uint64_t relic = tabled(table_a(), u"Relic", u"Relic", 0, 0);
    registers(relic);
    const uint64_t relic_root = fake::get<uint64_t>(relic + a::root);
    fake::put(relic_root + t::scene_component::owner, relic);
    const uint64_t face = mesh_part(u"Face", relic_root, relic, 1);
    CHECK(parts_api.hold_material(mod, part(face), 0, object(red_material)) == TSUCHI_OK && tsuchi::material_holds() == 1);
    part_log.clear();
    ends_play(relic);
    CHECK(part_log.empty() && tsuchi::material_holds() == 0 && tsuchi::owned_count(mod) == owned);
    fake::destroy(face), fake::destroy(relic);
}

// ---- What an actor is doing -----------------------------------------------------

struct HeardChange {
    uint64_t handle;
    std::string action, previous;
};
std::vector<HeardChange> heard_changes;
TSUCHI_Subscription change_subscription{};
bool stop_listening_when_told = false;
void on_changed(void*, const TSUCHI_BehaviorChanged* event) {
    heard_changes.push_back({event->actor.id, event->action, event->previous});
    if (stop_listening_when_told) tsuchi::host.cancel(mod, change_subscription);
}
// Gives an actor the part that decides for it, with no action running.
uint64_t give_brain(uint64_t which) {
    const uint64_t brain = fake::engine_object(u"Brain", w.component_class, 0x100), tree = fake::memory(0x40);
    fake::put(which + t::game_actor::ai, brain);
    fake::put(brain + t::ai_component::actions, tree);
    return tree;
}
// Makes the actions an actor is running, from the top down.
void now_doing(uint64_t tree, std::initializer_list<const char16_t*> names) {
    uint64_t parent = tree;
    for (const char16_t* name : names) {
        const uint64_t action = fake::memory(0x40);
        fake::put(action + t::action::name, fake::name(name));
        fake::put(parent + t::action::child, action);
        parent = action;
    }
    fake::put<uint64_t>(parent + t::action::child, 0);
}

void behaviors() {
    const TSUCHI_BehaviorService& doing_api = *tsuchi::host.behavior;
    const auto doing = [&](uint64_t which) -> std::string {
        char small[4] = "zzz";
        uint64_t needed = 0;
        const TSUCHI_Result first = doing_api.action(mod, handle(which), small, sizeof small, &needed);
        if (first == TSUCHI_OK) return needed == std::string(small).size() + 1 ? small : "<wrong size>";
        if (first != TSUCHI_CAPACITY || std::string(small) != "zzz") return "<failed " + std::to_string(first) + ">";
        std::string text(size_t(needed), '?');
        uint64_t again = 0;
        if (doing_api.action(mod, handle(which), text.data(), needed, &again) != TSUCHI_OK || again != needed) return "<failed>";
        text.pop_back();
        return text;
    };
    const uint64_t thinker = tabled(table_a(), u"Thinker", u"Thinker", 0, 0), walker = tabled(table_a(), u"Walker", u"Walker", 0, 0);
    const uint64_t thinker_tree = give_brain(thinker), walker_tree = give_brain(walker), captain_tree = give_brain(w.captain);

    // What an actor is doing, from the outermost action in. Nothing running is nothing said.
    CHECK(doing(thinker) == "");
    now_doing(thinker_tree, {u"Control", u"Throw"}), now_doing(walker_tree, {u"Follow"}), now_doing(captain_tree, {u"Control", u"Walk"});
    CHECK(doing(thinker) == "Control/Throw" && doing(walker) == "Follow");
    fake::put(fake::get<uint64_t>(walker_tree + t::action::child) + t::action::name, fake::name(u"Wait", 3));
    CHECK(doing(walker) == "Wait_2");
    now_doing(walker_tree, {u"A", u"B", u"C", u"D", u"E", u"F", u"G", u"H", u"I", u"J"});
    CHECK(doing(walker) == "A/B/C/D/E/F/G/H/I/J"); // however many, one within another
    now_doing(walker_tree, {std::u16string(300, u'N').c_str(), u"Step"});
    CHECK(doing(walker) == std::string(300, 'N') + "/Step");
    now_doing(walker_tree, {u"Follow"});
    // Actors nothing decides for, actions that lead back into themselves, and what is gone.
    char text[64] = "untouched";
    uint64_t needed = 99;
    CHECK(doing_api.action(mod, handle(w.rock), text, sizeof text, &needed) == TSUCHI_UNSUPPORTED && std::string(text) == "untouched");
    const uint64_t dull = tabled(table_a(), u"Dull", u"Dull", 0, 0);
    fake::put(dull + t::game_actor::ai, fake::object(u"Brain", w.component_class, 0x100)); // a part that decides, with nothing to run
    CHECK(doing_api.action(mod, handle(dull), text, sizeof text, &needed) == TSUCHI_UNSUPPORTED);
    const uint64_t dizzy = tabled(table_a(), u"Dizzy", u"Dizzy", 0, 0), dizzy_tree = give_brain(dizzy);
    now_doing(dizzy_tree, {u"Spin"});
    const uint64_t spin = fake::get<uint64_t>(dizzy_tree + t::action::child);
    fake::put(spin + t::action::child, spin);
    CHECK(doing_api.action(mod, handle(dizzy), text, sizeof text, &needed) == TSUCHI_UNSUPPORTED && std::string(text) == "untouched");
    fake::put<uint64_t>(spin + t::action::child, 0);
    CHECK(doing_api.action(mod, handle(fake::get<uint64_t>(thinker + t::game_actor::ai)), text, sizeof text, &needed) == TSUCHI_NOT_FOUND); // a part is not an actor
    const Name spin_name = fake::get<Name>(spin + t::action::name);
    fake::put(spin + t::action::name, Name{0x7FFFFFF0, 0}); // an action whose name is nowhere
    CHECK(doing_api.action(mod, handle(dizzy), text, sizeof text, &needed) == TSUCHI_UNSUPPORTED && std::string(text) == "untouched");
    fake::put(spin + t::action::name, spin_name);
    const TSUCHI_Actor was = handle(dull);
    fake::destroy(dull);
    CHECK(doing_api.action(mod, was, text, sizeof text, &needed) == TSUCHI_NOT_FOUND);

    // Nobody listening: nobody watched.
    tsuchi::frame(0.03f);
    CHECK(tsuchi::behavior_watched() == 0);
    // Listening starts from what each actor is doing then: nothing is told for that.
    CHECK(doing_api.on_changed(mod, on_changed, nullptr, &change_subscription) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty() && tsuchi::behavior_watched() == 4); // thinker, walker, dizzy and the captain
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty());
    // One actor does something else: told once, with what it was doing.
    now_doing(thinker_tree, {u"Control", u"Whistle"});
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1 && heard_changes[0].handle == to_handle(thinker) && heard_changes[0].action == "Control/Whistle" &&
          heard_changes[0].previous == "Control/Throw");
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1);
    // Two at once, in the order the game keeps its actors; and stopping altogether is a change.
    heard_changes.clear();
    now_doing(walker_tree, {}), now_doing(thinker_tree, {u"Control"});
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 2 && heard_changes[0].handle == to_handle(thinker) && heard_changes[0].action == "Control" &&
          heard_changes[0].previous == "Control/Whistle" && heard_changes[1].handle == to_handle(walker) && heard_changes[1].action == "" &&
          heard_changes[1].previous == "Follow");
    // The same names in another arrangement are another thing to be doing, and so is the same name with another number.
    heard_changes.clear();
    now_doing(thinker_tree, {u"Con", u"trol"});
    tsuchi::frame(0.03f);
    now_doing(thinker_tree, {u"Control"});
    tsuchi::frame(0.03f);
    const uint64_t top = fake::get<uint64_t>(thinker_tree + t::action::child);
    Name numbered = fake::get<Name>(top + t::action::name);
    numbered.number = 2; // the same name, numbered
    fake::put(top + t::action::name, numbered);
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 3 && heard_changes[0].action == "Con/trol" && heard_changes[1].action == "Control" && heard_changes[2].action == "Control_1");
    // The same two actions the other way round.
    now_doing(thinker_tree, {u"Hop", u"Skip"});
    tsuchi::frame(0.03f);
    const uint64_t outer = fake::get<uint64_t>(thinker_tree + t::action::child), inner = fake::get<uint64_t>(outer + t::action::child);
    const Name hop = fake::get<Name>(outer + t::action::name), skip = fake::get<Name>(inner + t::action::name);
    fake::put(outer + t::action::name, skip), fake::put(inner + t::action::name, hop);
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 5 && heard_changes[4].action == "Skip/Hop" && heard_changes[4].previous == "Hop/Skip");
    now_doing(thinker_tree, {u"Control"});
    tsuchi::frame(0.03f);
    // An actor that enters play is not told for what it arrives doing, and is from then on.
    heard_changes.clear();
    const uint64_t late = tabled(table_a(), u"Late", u"Late", 0, 0), late_tree = give_brain(late);
    now_doing(late_tree, {u"Arrive"});
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty() && tsuchi::behavior_watched() == 5);
    const std::u16string settle(76, u'S'), stay(98, u'T');
    now_doing(late_tree, {settle.c_str()});
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1 && heard_changes[0].action == std::string(76, 'S') && heard_changes[0].previous == "Arrive");
    // What it was doing is kept in room of its own, which goes back when it grows and when the actor leaves.
    // The framework keeps one more room of the same size, for the words of the longest change so far.
    CHECK(blocks_of(77) == 2);
    now_doing(late_tree, {stay.c_str()});
    tsuchi::frame(0.03f);
    CHECK(blocks_of(77) == 0 && blocks_of(99) == 2);
    now_doing(late_tree, {settle.c_str()});
    tsuchi::frame(0.03f);
    CHECK(blocks_of(77) == 0 && blocks_of(99) == 2); // the larger room serves
    heard_changes.erase(heard_changes.begin() + 1, heard_changes.end());
    // One that leaves is forgotten, and one whose change cannot be read is passed over.
    fake::destroy(late);
    fake::put(spin + t::action::child, spin);
    heard_changes.clear();
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty() && tsuchi::behavior_watched() == 3 && blocks_of(99) == 1);
    fake::put<uint64_t>(spin + t::action::child, 0);
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty() && tsuchi::behavior_watched() == 4); // seen afresh: where it starts from again
    // A handler that stops listening while two changes wait: the first is told, and nothing breaks.
    now_doing(thinker_tree, {u"One"}), now_doing(walker_tree, {u"Two"});
    stop_listening_when_told = true;
    tsuchi::frame(0.03f);
    stop_listening_when_told = false;
    CHECK(heard_changes.size() == 1 && heard_changes[0].action == "One");
    tsuchi::frame(0.03f);
    CHECK(tsuchi::behavior_watched() == 0);
    // Listening again starts afresh: what changed meanwhile is not told.
    heard_changes.clear();
    now_doing(thinker_tree, {u"Three"});
    CHECK(doing_api.on_changed(mod, on_changed, nullptr, &change_subscription) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty() && tsuchi::behavior_watched() == 4);
    now_doing(thinker_tree, {u"Four"});
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1 && heard_changes[0].previous == "Three");
    // No memory to keep what an actor was doing: it is left unwatched until there is.
    const uint64_t odd = tabled(table_a(), u"Odd", u"Odd", 0, 0), odd_tree = give_brain(odd);
    now_doing(odd_tree, {u"AnActionOfThirtyOneCharactersXY"});
    refused_size = 32;
    tsuchi::frame(0.03f);
    CHECK(tsuchi::behavior_watched() == 4);
    refused_size = 0;
    tsuchi::frame(0.03f);
    CHECK(tsuchi::behavior_watched() == 5 && heard_changes.size() == 1);
    // No memory for the words of a change, longer than any before: it is told once there is.
    heard_changes.clear();
    now_doing(odd_tree, {std::u16string(600, u'W').c_str()});
    refused_size = 601;
    tsuchi::frame(0.03f);
    CHECK(heard_changes.empty());
    refused_size = 0;
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1 && heard_changes[0].action == std::string(600, 'W') && heard_changes[0].previous == "AnActionOfThirtyOneCharactersXY");
    tsuchi::frame(0.03f);
    CHECK(heard_changes.size() == 1);
    // When nobody listens any more, what was kept goes back.
    CHECK(tsuchi::host.cancel(mod, change_subscription) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::behavior_watched() == 0 && blocks_of(601) == 1); // one such room is left: the framework's own, for the words of a change
    now_doing(captain_tree, {u"Control", u"Walk"});
}

// ---- Life, hits and knocks -------------------------------------------------------
//
// The made-up game takes life for a hit and sets life outright through two
// functions the framework wraps, and makes and takes messages through three more.

std::string life_log;
float next_hit = 0, life_to_set = 0; // decimal numbers do not pass through the test's thunks
uint64_t life_class = 0;
bool game_takes_messages = true;
struct Posted {
    uint64_t victim, attacker;
    bool knock;
    float amount, x, y, z, damage;
    int attack, riding;
};
std::vector<Posted> posted;

// The far ends of the two chains: what the game itself does.
uint64_t game_apply_damage(uint64_t life, uint64_t, uint64_t kind, uint64_t) {
    const float now = fake::get<float>(life + t::life_component::current);
    fake::put(life + t::life_component::current, now - next_hit < 0 ? 0.0f : now - next_hit);
    return life_log += "hit" + std::to_string(int(kind)) + " ", 0;
}
uint64_t game_set_life(uint64_t life, uint64_t jump, uint64_t, uint64_t) {
    fake::put(life + t::life_component::current, life_to_set);
    return life_log += "set" + std::to_string(int(life_to_set)) + (jump & 0xFF ? "jump " : "glide "), 0;
}
// The game's setter as the framework calls it: through whatever now stands at its start.
void call_set_life(void* life, bool jump, uint64_t attacker, float value) {
    life_to_set = value;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::life_set), reinterpret_cast<uint64_t>(life), jump, attacker);
}
// The game hits: as its own code calls the function that takes life.
void game_hits(uint64_t life, uint64_t attacker, int kind, float amount) {
    next_hit = amount;
    if (attacker) to_handle(attacker); // a weak reference gives an object its serial, as the game's own would have
    const uint64_t item = attacker ? fake::item_at(fake::get<int32_t>(attacker + t::object::index)) : 0;
    const uint64_t weak = attacker ? uint64_t(uint32_t(fake::get<int32_t>(attacker + t::object::index))) |
                                         uint64_t(uint32_t(fake::get<int32_t>(item + t::object_item::serial))) << 32
                                   : 0xFFFFFFFFull;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::life_apply_damage), life, weak, uint64_t(kind));
}
void make_damage_message(void* memory, void* attacker) {
    const auto message = reinterpret_cast<uint64_t>(memory);
    std::memset(memory, 0, t::damage_message::size);
    fake::put(message, fake::main_image() + t::data::damage_message_table);
    fake::put(message + 8, reinterpret_cast<uint64_t>(attacker)); // the made-up game keeps who sent it here
    fake::put(message + t::damage_message::amount, 99.0f);
}
void make_knock_message(void* memory, void* attacker) {
    const auto message = reinterpret_cast<uint64_t>(memory);
    std::memset(memory, 0, t::knock_message::size);
    fake::put(message, fake::main_image() + t::data::knock_message_table);
    fake::put(message + 8, reinterpret_cast<uint64_t>(attacker));
}
bool take_message(void* victim, void* memory) {
    const auto message = reinterpret_cast<uint64_t>(memory);
    Posted one{reinterpret_cast<uint64_t>(victim), fake::get<uint64_t>(message + 8), false, 0, 0, 0, 0, 0, 0, 0};
    if (fake::get<uint64_t>(message) == fake::main_image() + t::data::knock_message_table) {
        namespace m = t::knock_message;
        one.knock = true, one.damage = fake::get<float>(message + m::damage);
        one.x = fake::get<float>(message + m::velocity), one.y = fake::get<float>(message + m::velocity + 4), one.z = fake::get<float>(message + m::velocity + 8);
    } else {
        namespace m = t::damage_message;
        one.amount = fake::get<float>(message + m::amount), one.attack = fake::get<uint8_t>(message + m::attack);
        one.riding = fake::get<uint8_t>(message + m::riding_applies);
        one.x = fake::get<float>(message + m::origin), one.y = fake::get<float>(message + m::origin + 4), one.z = fake::get<float>(message + m::origin + 8);
    }
    posted.push_back(one);
    fake::engine_free(nullptr, memory); // taken or not, the game frees it in the end
    return game_takes_messages;
}
// Gives an actor the part that keeps its life.
uint64_t give_life(uint64_t which, float life, float full) {
    const uint64_t part = fake::object(u"Life", life_class, 0x80);
    fake::put(part + t::scene_component::owner, which);
    fake::put(part + t::life_component::current, life), fake::put(part + t::life_component::max, full);
    fake::put(part + t::life_component::extra, 5.0f), fake::put(part + t::life_component::max_extra, 20.0f);
    fake::put(which + t::game_actor::life, part);
    return part;
}

struct HeardDrop {
    bool defeated;
    uint64_t actor, attacker;
    float before, after;
};
std::vector<HeardDrop> heard_drops;
uint64_t hit_again_part = 0;
void on_damaged(void*, const TSUCHI_CombatDamaged* event) {
    heard_drops.push_back({false, event->actor.id, event->attacker.id, event->previous, event->life});
    if (hit_again_part) game_hits(std::exchange(hit_again_part, 0), 0, 0, 1);
}
void on_defeated(void*, const TSUCHI_CombatDefeated* event) { heard_drops.push_back({true, event->actor.id, event->attacker.id, event->previous, event->life}); }

struct HeardDeath {
    uint64_t actor, killer;
    std::string kind, type;
    uint32_t cause;
    float x, z;
};
std::vector<HeardDeath> heard_deaths;
uint64_t die_again = 0;
uint64_t game_report_kill(uint64_t, uint64_t, uint64_t, uint64_t) { return 77; } // the game's own answer, whatever it means
void on_pikmin_died(void*, const TSUCHI_CombatPikminDied* event) {
    heard_deaths.push_back({event->pikmin.id, event->attacker.id, event->kind, event->pikmin_type, uint32_t(event->cause), event->location.x, event->location.z});
    if (die_again) thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::pikmin_report_kill), std::exchange(die_again, 0), 0, 7);
}
struct HeardMessage {
    uint64_t actor, sender;
    uint32_t kind, attack;
    float amount, x, y, z;
};
std::vector<HeardMessage> heard_messages;
void on_intercepted(void*, const TSUCHI_CombatIntercepted* event) {
    heard_messages.push_back({event->actor.id, event->attacker.id, uint32_t(event->kind), event->attack, event->amount, event->vector.x, event->vector.y, event->vector.z});
}

void install_life() {
    const uint64_t main = fake::main_image();
    TSUCHI_Mod* game = tsuchi::mod_create("the.game.life");
    uint64_t id = 0;
    void* unused = nullptr;
    for (unsigned site : {unsigned(t::code_site::life_apply_damage), unsigned(t::code_site::life_set), unsigned(t::code_site::pikmin_report_kill)}) {
        const t::CodeSite& info = t::code_sites[site];
        fake::put<uint32_t>(main + info.address, 0x14000000u | uint32_t((info.entry - info.address) >> 2)); // as the patch leaves it
    }
    CHECK(tsuchi::hook_code_site(game, t::code_site::life_apply_damage, reinterpret_cast<void*>(&game_apply_damage), &id, &unused) == TSUCHI_OK);
    CHECK(tsuchi::hook_code_site(game, t::code_site::life_set, reinterpret_cast<void*>(&game_set_life), &id, &unused) == TSUCHI_OK);
    CHECK(tsuchi::hook_code_site(game, t::code_site::pikmin_report_kill, reinterpret_cast<void*>(&game_report_kill), &id, &unused) == TSUCHI_OK);
    tsuchi::life_events_start(tsuchi::mod_create("tsuchi.self.life"));
    CHECK(tsuchi::life_event_counts().sites == 3);
    life_class = fake::object(u"LifeComponent", w.root);
    t::fn::set_life = call_set_life, t::fn::damage_message = make_damage_message, t::fn::knock_message = make_knock_message;
    t::fn::post_message = take_message;
}

void fighting() {
    const TSUCHI_CombatService& fight = *tsuchi::host.combat;
    install_life();
    const uint64_t ogre = tabled(table_a(), u"Ogre", u"Ogre", 0, 30), knight = tabled(table_a(), u"Knight", u"Knight", 0, 0);
    const uint64_t ogre_life = give_life(ogre, 50, 80), knight_life = give_life(knight, 100, 100);
    give_life(w.bulborb, 50, 60);

    // How much life an actor has; and those with none.
    TSUCHI_Life life{};
    CHECK(fight.life(mod, handle(ogre), &life) == TSUCHI_OK && life.life == 50 && life.max_life == 80 && life.extra == 5 && life.max_extra == 20);
    CHECK(fight.life(mod, handle(w.rock), &life) == TSUCHI_NOT_FOUND && fight.life(mod, handle(ogre_life), &life) == TSUCHI_NOT_FOUND && life.life == 50);
    const uint64_t husk = tabled(table_a(), u"Husk", u"Husk", 0, 0);
    fake::put(husk + t::game_actor::life, fake::object(u"NotLife", w.component_class, 0x80)); // something else where the life part would be
    CHECK(fight.life(mod, handle(husk), &life) == TSUCHI_NOT_FOUND);

    // Set outright, through the game's own setter: the gauge jumps unless told to glide.
    CHECK(fight.set_life(mod, handle(ogre), 30, 0) == TSUCHI_OK && life_log == "set30jump " && fight.life(mod, handle(ogre), &life) == TSUCHI_OK && life.life == 30);
    life_log.clear();
    CHECK(fight.set_life(mod, handle(ogre), 80, TSUCHI_LIFE_FLAGS_SMOOTH_GAUGE) == TSUCHI_OK && life_log == "set80glide ");
    life_log.clear();
    CHECK(fight.set_life(mod, handle(ogre), -1, 0) == TSUCHI_INVALID && fight.set_life(mod, handle(ogre), 80.5f, 0) == TSUCHI_INVALID &&
          fight.set_life(mod, handle(w.rock), 1, 0) == TSUCHI_NOT_FOUND && life_log.empty());
    t::fn::set_life = nullptr;
    CHECK(fight.set_life(mod, handle(ogre), 10, 0) == TSUCHI_NOT_READY);
    t::fn::set_life = call_set_life;
    CHECK(fight.set_life(mod, handle(ogre), 50, 0) == TSUCHI_OK);

    // Nobody listening: drops in life are not kept.
    game_hits(ogre_life, knight, 0, 5);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::life_event_counts().drops == 0 && fake::get<float>(ogre_life + t::life_component::current) == 45);

    // A hit by the game: told at the start of the next frame, with who did it.
    TSUCHI_Subscription damaged{}, defeated{};
    CHECK(fight.on_damaged(mod, on_damaged, nullptr, &damaged) == TSUCHI_OK && fight.on_defeated(mod, on_defeated, nullptr, &defeated) == TSUCHI_OK);
    life_log.clear();
    game_hits(ogre_life, knight, 3, 10);
    CHECK(heard_drops.empty() && life_log == "hit3 " && tsuchi::life_event_counts().waiting == 1);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1 && !heard_drops[0].defeated && heard_drops[0].actor == to_handle(ogre) && heard_drops[0].attacker == to_handle(knight) &&
          heard_drops[0].before == 45 && heard_drops[0].after == 35 && tsuchi::life_event_counts().waiting == 0);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1);
    // A hit that takes nothing, life that is raised, and a hit on a part that is nobody's life: not told.
    game_hits(ogre_life, knight, 0, 0);
    CHECK(fight.set_life(mod, handle(ogre), 60, 0) == TSUCHI_OK);
    const uint64_t stray = fake::object(u"Stray", life_class, 0x80);
    fake::put(stray + t::life_component::current, 9.0f), fake::put(stray + t::scene_component::owner, ogre); // says it is the ogre's; the ogre says otherwise
    game_hits(stray, knight, 0, 4);
    const uint64_t orphan = fake::object(u"Orphan", life_class, 0x80);
    fake::put(orphan + t::life_component::current, 9.0f);
    game_hits(orphan, knight, 0, 4);
    // Something that is no actor, with life where an actor would keep it: its life is not read, and a hit on it is not told.
    const uint64_t pretender = fake::object(u"Pretender", w.component_class, 0x100), pretend_life = fake::object(u"Life", life_class, 0x80);
    fake::put(pretender + t::game_actor::life, pretend_life), fake::put(pretend_life + t::scene_component::owner, pretender);
    fake::put(pretend_life + t::life_component::current, 9.0f);
    game_hits(pretend_life, knight, 0, 4);
    CHECK(fight.life(mod, handle(pretender), &life) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1);
    // Several, in the order they happened. One that takes life to nothing is a defeat as well.
    heard_drops.clear();
    game_hits(knight_life, 0, 0, 20), game_hits(ogre_life, knight, 0, 100), game_hits(knight_life, ogre, 0, 5);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 4 && heard_drops[0].actor == to_handle(knight) && heard_drops[0].attacker == 0 && heard_drops[0].after == 80 &&
          heard_drops[1].actor == to_handle(ogre) && !heard_drops[1].defeated && heard_drops[1].before == 60 && heard_drops[1].after == 0 &&
          heard_drops[2].defeated && heard_drops[2].actor == to_handle(ogre) && heard_drops[2].attacker == to_handle(knight) &&
          heard_drops[3].actor == to_handle(knight) && heard_drops[3].attacker == to_handle(ogre) && heard_drops[3].after == 75);
    // With no life left there is nothing to take.
    heard_drops.clear();
    game_hits(ogre_life, knight, 0, 10);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.empty());
    // Should the game ever take life below nothing, that is a drop, and not a second defeat.
    call_set_life(reinterpret_cast<void*>(ogre_life), true, 0xFFFFFFFFull, -5);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1 && !heard_drops[0].defeated && heard_drops[0].before == 0 && heard_drops[0].after == -5);
    call_set_life(reinterpret_cast<void*>(ogre_life), true, 0xFFFFFFFFull, 0);
    heard_drops.clear();
    // Life set lower is a drop like any other, by nobody; set to nothing, a defeat.
    CHECK(fight.set_life(mod, handle(ogre), 40, 0) == TSUCHI_OK && fight.set_life(mod, handle(ogre), 25, 0) == TSUCHI_OK && fight.set_life(mod, handle(ogre), 0, 0) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 3 && heard_drops[0].before == 40 && heard_drops[0].after == 25 && heard_drops[0].attacker == 0 && !heard_drops[1].defeated &&
          heard_drops[2].defeated && heard_drops[2].before == 25 && heard_drops[2].after == 0);
    // An attacker the game names that is gone by then, or whose place another object has taken, is nobody.
    CHECK(fight.set_life(mod, handle(ogre), 50, 0) == TSUCHI_OK);
    heard_drops.clear();
    const uint64_t ghost = tabled(table_a(), u"Specter", u"Specter", 0, 0);
    next_hit = 1;
    const uint64_t ghost_item = fake::item_at(fake::get<int32_t>(ghost + t::object::index));
    const uint64_t stale = uint64_t(uint32_t(fake::get<int32_t>(ghost + t::object::index))) | uint64_t(uint32_t(fake::get<int32_t>(ghost_item + t::object_item::serial)) + 1) << 32;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::life_apply_damage), ogre_life, stale, 0);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1 && heard_drops[0].attacker == 0 && heard_drops[0].after == 49);
    // A handler that causes another hit: that one is told on the frame after.
    heard_drops.clear();
    hit_again_part = knight_life;
    game_hits(ogre_life, knight, 0, 1);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1 && heard_drops[0].actor == to_handle(ogre));
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 2 && heard_drops[1].actor == to_handle(knight) && heard_drops[1].after == 74);
    // Many at once, more than the list first has room for.
    heard_drops.clear();
    CHECK(fight.set_life(mod, handle(knight), 100, 0) == TSUCHI_OK);
    for (int i = 0; i < 90; ++i) game_hits(knight_life, ogre, 0, 1);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 90 && heard_drops[0].before == 100 && heard_drops[0].after == 99 && heard_drops[89].before == 11 && heard_drops[89].after == 10);
    CHECK(tsuchi::host.cancel(mod, damaged) == TSUCHI_OK);
    // Listening only for defeats.
    heard_drops.clear();
    game_hits(knight_life, ogre, 0, 3), game_hits(knight_life, ogre, 0, 30);
    tsuchi::frame(0.03f);
    CHECK(heard_drops.size() == 1 && heard_drops[0].defeated && heard_drops[0].before == 7);
    CHECK(tsuchi::host.cancel(mod, defeated) == TSUCHI_OK);
    CHECK(fight.set_life(mod, handle(knight), 100, 0) == TSUCHI_OK);

    // A hit sent the way the game sends its own: made by the game, filled in, and handed to the victim.
    const int freed = fake::engine().freed;
    CHECK(fight.damage(mod, handle(ogre), 12.5f, TSUCHI_ATTACK_KIND_FIRE, handle(knight)) == TSUCHI_OK && posted.size() == 1 && fake::engine().freed == freed + 1);
    CHECK(posted[0].victim == ogre && posted[0].attacker == knight && !posted[0].knock && posted[0].amount == 12.5f && posted[0].attack == 3 &&
          posted[0].riding == 1 && posted[0].x == 0 && posted[0].y == 0);
    // Nobody's hit comes from where the victim stands.
    CHECK(fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_OK && posted.size() == 2 && posted[1].attacker == 0 && posted[1].x == 30 &&
          posted[1].attack == 0);
    // What cannot be: no life to take, an attacker that is gone, less than nothing.
    const TSUCHI_Actor gone = handle(ghost);
    fake::destroy(ghost);
    CHECK(fight.damage(mod, handle(w.rock), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, gone) == TSUCHI_NOT_FOUND &&
          fight.damage(mod, gone, 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && fight.damage(mod, handle(ogre), -1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_INVALID);
    CHECK(posted.size() == 2);
    // The victim will not take it, there is no memory for it, or the game cannot be asked.
    game_takes_messages = false;
    CHECK(fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NOT_READY && posted.size() == 3);
    game_takes_messages = true;
    fake::engine().out_of_memory = true;
    CHECK(fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NO_MEMORY && posted.size() == 3);
    const TSUCHI_Vec3 thrown{100, -50, 300};
    CHECK(fight.knockback(mod, handle(knight), &thrown, 0, TSUCHI_Actor{}) == TSUCHI_NO_MEMORY);
    fake::engine().out_of_memory = false;
    t::fn::damage_message = nullptr;
    CHECK(fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NOT_READY);
    t::fn::damage_message = make_damage_message, t::fn::post_message = nullptr;
    CHECK(fight.damage(mod, handle(ogre), 1, TSUCHI_ATTACK_KIND_NORMAL, TSUCHI_Actor{}) == TSUCHI_NOT_READY && fight.knockback(mod, handle(knight), &thrown, 0, TSUCHI_Actor{}) == TSUCHI_NOT_READY);
    t::fn::post_message = take_message, t::fn::knock_message = nullptr;
    CHECK(fight.knockback(mod, handle(knight), &thrown, 0, TSUCHI_Actor{}) == TSUCHI_NOT_READY && posted.size() == 3);
    t::fn::knock_message = make_knock_message;

    // A Pikmin's death, as the game counts it: told at the start of the next frame, with what
    // killed it and who is blamed. The game's own answer is passed back untouched.
    const auto game_counts_death = [](uint64_t pikmin, uint64_t killer, int cause) {
        return thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::pikmin_report_kill), pikmin, killer, uint64_t(cause));
    };
    CHECK(game_counts_death(w.red, knight, 3) == 77 && tsuchi::life_event_counts().deaths == 0); // nobody listening
    TSUCHI_Subscription died{};
    CHECK(fight.on_pikmin_died(mod, on_pikmin_died, nullptr, &died) == TSUCHI_OK);
    CHECK(game_counts_death(w.red, knight, 3) == 77 && heard_deaths.empty());
    tsuchi::frame(0.03f);
    TSUCHI_Pose red_at{};
    CHECK(api.pose(mod, handle(w.red), &red_at) == TSUCHI_OK);
    CHECK(heard_deaths.size() == 1 && heard_deaths[0].actor == to_handle(w.red) && heard_deaths[0].kind == "PikminRed" && heard_deaths[0].killer == to_handle(knight) &&
          heard_deaths[0].cause == TSUCHI_DEATH_CAUSE_FIRE && heard_deaths[0].x == red_at.location.x && heard_deaths[0].z == red_at.location.z && red_at.location.z != 0);
    // The type comes from where pikmin.info reads it, and is empty when that read fails.
    TSUCHI_PikminInfo red_info{};
    const bool red_described = tsuchi::host.pikmin->info(mod, handle(w.red), &red_info) == TSUCHI_OK;
    CHECK(heard_deaths[0].type == (red_described ? std::string(red_info.kind) : std::string()));
    tsuchi::frame(0.03f);
    CHECK(heard_deaths.size() == 1);
    // Nobody to blame, a killer that is no actor, a cause the framework has no name for, and what is no actor at all.
    // A cause with no name is told as the game's number for it: it is not folded into another cause.
    heard_deaths.clear();
    game_counts_death(w.red, 0, 13), game_counts_death(w.red, ogre_life, 0x100 | 8), game_counts_death(w.red, knight, 14), game_counts_death(ogre_life, knight, 1);
    tsuchi::frame(0.03f);
    CHECK(heard_deaths.size() == 3 && heard_deaths[0].killer == 0 && heard_deaths[0].cause == TSUCHI_DEATH_CAUSE_LEFT_BEHIND && heard_deaths[1].killer == 0 &&
          heard_deaths[1].cause == TSUCHI_DEATH_CAUSE_DROWNED && heard_deaths[2].cause == 14);
    // Many at once, and one caused by a handler, which waits for the frame after.
    heard_deaths.clear();
    for (int i = 0; i < 150; ++i) game_counts_death(w.red, 0, i % 14);
    die_again = w.red;
    tsuchi::frame(0.03f);
    CHECK(heard_deaths.size() == 150 && heard_deaths[149].cause == 149 % 14 && heard_deaths[0].cause == 0 && heard_deaths[1].cause == 1 &&
          heard_deaths[63].cause == 63 % 14 && heard_deaths[5].kind == "PikminRed" && heard_deaths[5].actor == to_handle(w.red));
    tsuchi::frame(0.03f);
    CHECK(heard_deaths.size() == 151 && heard_deaths[150].cause == TSUCHI_DEATH_CAUSE_BOMB);
    CHECK(tsuchi::host.cancel(mod, died) == TSUCHI_OK && tsuchi::life_event_counts().deaths == 155);

    // What a suspended actor is sent: told as each is taken, to whoever drives it.
    tsuchi::combat_start(); // the one listener for taken messages is the service's again
    const uint64_t sentry = tabled(table_a(), u"Sentry", u"Sentry", 0, 0), sentry_brain = fake::object(u"Brain", w.component_class, 0x80);
    registers(sentry);
    fake::put(sentry_brain, fake::get<uint64_t>(brain)), fake::put(sentry_brain + t::ai_component::messages, fake::get<uint64_t>(brain + t::ai_component::messages));
    fake::put(sentry + t::game_actor::ai, sentry_brain);
    TSUCHI_Subscription caught{};
    CHECK(fight.on_intercepted(mod, on_intercepted, nullptr, &caught) == TSUCHI_OK && api.hold_suspended(mod, handle(sentry), TSUCHI_ENTITY_PARTS_LOGIC) == TSUCHI_OK);
    const uint64_t blow = message(t::data::damage_message_table, knight);
    fake::put(blow + t::damage_message::amount, 12.5f), fake::put<uint8_t>(blow + t::damage_message::attack, 4);
    fake::put(blow + t::damage_message::origin, 1.0f), fake::put(blow + t::damage_message::origin + 8, 3.0f);
    const uint64_t shove = message(t::data::knock_message_table, 0);
    fake::put(shove + t::knock_message::damage, 2.0f), fake::put(shove + t::knock_message::velocity + 4, 80.0f);
    for (uint64_t each : {blow, shove, message(t::data::whistle_message_table, w.captain), message(0x3500, 0)}) switches.waiting.push_back(each);
    tsuchi::frame(0.03f);
    CHECK(heard_messages.size() == 4 && heard_messages[0].actor == to_handle(sentry) && heard_messages[0].sender == to_handle(knight) &&
          heard_messages[0].kind == TSUCHI_MESSAGE_KIND_HIT && heard_messages[0].amount == 12.5f && heard_messages[0].attack == 4 && heard_messages[0].x == 1 &&
          heard_messages[0].z == 3);
    CHECK(heard_messages.size() == 4 && heard_messages[1].kind == TSUCHI_MESSAGE_KIND_KNOCK && heard_messages[1].sender == 0 && heard_messages[1].amount == 2 &&
          heard_messages[1].y == 80 && heard_messages[2].kind == TSUCHI_MESSAGE_KIND_WHISTLE && heard_messages[2].sender == to_handle(w.captain) &&
          heard_messages[3].kind == TSUCHI_MESSAGE_KIND_OTHER);
    // Given back, the actor answers for itself again and nothing is taken; nor is anything told with nobody listening.
    CHECK(api.release_suspended(mod, handle(sentry), TSUCHI_ENTITY_PARTS_LOGIC) == TSUCHI_OK);
    switches.waiting.push_back(message(t::data::whistle_message_table, w.captain));
    tsuchi::frame(0.03f);
    CHECK(heard_messages.size() == 4 && switches.waiting.size() == 1);
    CHECK(tsuchi::host.cancel(mod, caught) == TSUCHI_OK && api.hold_suspended(mod, handle(sentry), TSUCHI_ENTITY_PARTS_LOGIC) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_messages.size() == 4 && switches.waiting.empty());
    CHECK(api.release_suspended(mod, handle(sentry), TSUCHI_ENTITY_PARTS_LOGIC) == TSUCHI_OK);

    // An actor removed for all to see: not quietly, with a cause and someone to blame.
    const uint64_t doomed = tabled(table_a(), u"Doomed", u"Doomed", 0, 0);
    CHECK(remove_actor(doomed, false, 7, knight) == TSUCHI_OK && spawner.last_quiet == 0 && spawner.last_cause == 7 && spawner.last_killer == knight);
    const uint64_t hushed = tabled(table_a(), u"Hushed", u"Hushed", 0, 0);
    CHECK(remove_actor(hushed) == TSUCHI_OK && spawner.last_quiet == 1 && spawner.last_cause == 0 && spawner.last_killer == 0);

    // A knock: thrown this way, and losing this much. Any actor can be sent one.
    CHECK(fight.knockback(mod, handle(knight), &thrown, 7, handle(ogre)) == TSUCHI_OK && posted.size() == 4 && posted[3].knock && posted[3].victim == knight &&
          posted[3].attacker == ogre && posted[3].x == 100 && posted[3].y == -50 && posted[3].z == 300 && posted[3].damage == 7);
    CHECK(fight.knockback(mod, handle(w.rock), &thrown, 0, TSUCHI_Actor{}) == TSUCHI_OK && posted.size() == 5 && posted[4].victim == w.rock);
    CHECK(fight.knockback(mod, gone, &thrown, 0, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && fight.knockback(mod, handle(knight), &thrown, 0, gone) == TSUCHI_NOT_FOUND &&
          fight.knockback(mod, handle(knight), &thrown, -1, TSUCHI_Actor{}) == TSUCHI_INVALID && posted.size() == 5);
    game_takes_messages = false;
    CHECK(fight.knockback(mod, handle(knight), &thrown, 0, TSUCHI_Actor{}) == TSUCHI_NOT_READY);
    game_takes_messages = true;
}

// ---- Where the game is ------------------------------------------------------------
//
// The made-up game keeps its rule and its stages in one place, its pauses in
// another, and the loaded world's name in a third.

struct Running {
    uint64_t context = 0, flow = 0, instance = 0, scenes = 0, pauses = 0, world = 0;
    int counts[32] = {};  // how many holds are on each kind of pause
    std::string log;
} running;
void game_pause_change(void* manager, uint32_t kinds, bool add) {
    uint32_t active = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if (kinds >> bit & 1) running.counts[bit] += add ? 1 : -1;
        if (running.counts[bit] > 0) active |= 1u << bit;
    }
    fake::put(reinterpret_cast<uint64_t>(manager) + t::pause_manager::active, active);
    running.log += (add ? "pause+" : "pause-") + std::to_string(kinds) + " ";
}
void game_return_to_title(bool title) { running.log += title ? "title " : "exit "; }
// Puts the game in these stages, one within another.
void in_stages(std::initializer_list<const char16_t*> names) {
    uint64_t parent = running.flow;
    for (const char16_t* name : names) {
        const uint64_t stage = fake::memory(t::flow_stage::size);
        fake::put(stage + t::flow_stage::name, fake::name(name));
        fake::put(parent + t::flow_stage::child, stage);
        parent = stage;
    }
    fake::put<uint64_t>(parent + t::flow_stage::child, 0);
}
void in_world(const char16_t* name) {
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, name ? 1 : 0);
    if (name) fake::put(running.world + t::world::level_name, fake::name(name));
}

struct GameHeard {
    std::string log;
} game_heard;
void on_mode_changed(void*, const TSUCHI_GameModeChanged* e) { game_heard.log += "mode" + std::to_string(e->previous) + ">" + std::to_string(e->mode) + " "; }
void on_stage_changed(void*, const TSUCHI_GameStageChanged* e) { game_heard.log += std::string("stage:") + e->previous + ">" + e->stage + " "; }
void on_world_loaded(void*, const TSUCHI_GameWorldLoaded* e) { game_heard.log += std::string("loaded:") + e->world + " "; }
void on_world_unloaded(void*, const TSUCHI_GameWorldUnloaded* e) { game_heard.log += std::string("unloaded:") + e->world + " "; }
void on_pause_changed(void*, const TSUCHI_GamePauseChanged* e) { game_heard.log += "paused" + std::to_string(e->previous) + ">" + std::to_string(e->paused) + " "; }
void on_control_changed(void*, const TSUCHI_GameControlChanged* e) { game_heard.log += e->control ? "control+ " : "control- "; }

void running_game() {
    const TSUCHI_GameService& game_api = *tsuchi::host.game;
    const uint64_t main = fake::main_image();
    const auto text_of = [&](TSUCHI_Result (*read)(TSUCHI_Mod*, char*, uint64_t, uint64_t*)) -> std::string {
        char small[2] = "z";
        uint64_t needed = 0;
        const TSUCHI_Result first = read(mod, small, sizeof small, &needed);
        if (first == TSUCHI_OK) return small;
        if (first != TSUCHI_CAPACITY || std::string(small) != "z") return "<failed " + std::to_string(first) + ">";
        std::string text(size_t(needed), '?');
        if (read(mod, text.data(), needed, &needed) != TSUCHI_OK) return "<failed>";
        text.pop_back();
        return text;
    };
    TSUCHI_Mode mode = TSUCHI_MODE_OTHER;
    int32_t rule = 99;
    TSUCHI_Pause kinds = 99;
    bool control = true;
    TSUCHI_PauseHold hold{7};

    // Before the game has set itself up: no mode, no stages, no world, and nothing to ask.
    CHECK(game_api.mode(mod, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_NONE && game_api.rule(mod, &rule) == TSUCHI_NOT_READY && rule == 99);
    CHECK(text_of(game_api.stage) == "" && text_of(game_api.world) == "" && game_api.paused(mod, &kinds) == TSUCHI_NOT_READY);
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && !control && game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER, &hold) == TSUCHI_NOT_READY && hold.id == 7);
    CHECK(game_api.return_to_title(mod) == TSUCHI_NOT_READY);

    // Something where the game's context should be that cannot be read as one.
    fake::put<uint64_t>(main + t::data::game_context, 0x7FFF00000000ull);
    CHECK(game_api.mode(mod, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_NONE && game_api.rule(mod, &rule) == TSUCHI_NOT_READY && text_of(game_api.stage) == "");
    running.context = fake::memory(std::max<uint64_t>(0x80, t::game_context::night_start + 1)), running.flow = fake::memory(t::flow_stage::size);
    // The title-screen story mode also reads NightAreaStart, beyond the earlier context fields.
    fake::put<uint8_t>(running.context + t::game_context::night_start, 0);
    CHECK(readable(running.context + t::game_context::night_start, 1));
    running.instance = fake::memory(0x40), running.scenes = fake::memory(0x20), running.pauses = fake::memory(t::pause_manager::size);
    running.world = fake::memory(0x20);
    fake::put(main + t::data::game_context, running.context), fake::put(running.context + t::game_context::flow, running.flow);
    fake::put(main + t::data::game_instance, running.instance), fake::put(main + t::data::world, running.world);
    fake::put(running.instance + t::game_instance::scene_manager, running.scenes), fake::put(running.instance + t::game_instance::pause_manager, running.pauses);
    fake::put(running.pauses, main + t::data::pause_manager_table);
    t::fn::pause_change = game_pause_change, t::fn::return_to_title = game_return_to_title;

    // Its rule, its stages from the outermost in, and the mode the outermost stands for.
    fake::put<uint8_t>(running.context + t::game_context::rule, 6);
    CHECK(game_api.rule(mod, &rule) == TSUCHI_OK && rule == 6 && text_of(game_api.stage) == "" && game_api.mode(mod, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_NONE);
    in_stages({u"Story", u"StoryInGame"});
    CHECK(text_of(game_api.stage) == "Story/StoryInGame" && game_api.mode(mod, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_STORY);
    const std::pair<const char16_t*, TSUCHI_Mode> modes[] = {{u"Title", TSUCHI_MODE_TITLE}, {u"HeroStory", TSUCHI_MODE_OLIMAR}, {u"Cave", TSUCHI_MODE_CHALLENGE},
                                                           {u"VS", TSUCHI_MODE_BATTLE}, {u"Credits", TSUCHI_MODE_OTHER}, {u"story", TSUCHI_MODE_OTHER}};
    for (const auto& [name, expected] : modes) {
        in_stages({name});
        CHECK(game_api.mode(mod, &mode) == TSUCHI_OK && mode == expected);
    }
    in_stages({u"Story", u"StoryInGame", std::u16string(300, u'S').c_str()});
    CHECK(text_of(game_api.stage) == "Story/StoryInGame/" + std::string(300, 'S'));
    // Stages that lead back into themselves cannot be read.
    const uint64_t first_stage = fake::get<uint64_t>(running.flow + t::flow_stage::child);
    fake::put(fake::get<uint64_t>(first_stage + t::flow_stage::child) + t::flow_stage::child, first_stage);
    char text[8] = "same";
    uint64_t needed = 0;
    CHECK(game_api.stage(mod, text, sizeof text, &needed) == TSUCHI_NOT_READY && std::string(text) == "same" && game_api.mode(mod, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_NONE);
    in_stages({u"Story", u"StoryInGame"});

    // The loaded world: named once it has had its first update.
    fake::put(running.world + t::world::level_name, fake::name(u"Area001_P"));
    CHECK(text_of(game_api.world) == "");
    in_world(u"Area001_P");
    CHECK(text_of(game_api.world) == "Area001_P");

    // What is paused: the kinds a mod may hold, and not the game's own reasons kept above them.
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0x400002);
    CHECK(game_api.paused(mod, &kinds) == TSUCHI_OK && kinds == TSUCHI_PAUSE_ACTORS);
    fake::put(running.pauses, main + 0x3500); // something else where the pause manager should be
    CHECK(game_api.paused(mod, &kinds) == TSUCHI_NOT_READY && game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER, &hold) == TSUCHI_NOT_READY);
    fake::put(running.pauses, main + t::data::pause_manager_table);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);

    // Control: a world, a stage of play, and nothing paused that stops the captain.
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && control);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 4); // one of the two kinds that stop them, in the made-up game
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && !control);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 2);
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && control);
    in_stages({u"Story", u"DayEnd"});
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && !control);
    in_stages({u"VS", u"VsGame"});
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && control);
    in_world(nullptr);
    CHECK(game_api.has_control(mod, &control) == TSUCHI_OK && !control);
    in_world(u"Area001_P");
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    in_stages({u"Story", u"StoryInGame"});

    // Pauses a mod holds: counted with the game's own, one count for each hold.
    const uint32_t owned = tsuchi::owned_count(mod);
    TSUCHI_PauseHold first_hold{}, second_hold{};
    refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // the first room for sixteen holds
    CHECK(game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER, &hold) == TSUCHI_NO_MEMORY && running.log.empty());
    refused_size = 0;
    CHECK(game_api.hold_pause(mod, 0, &hold) == TSUCHI_INVALID);
    CHECK(game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS, &first_hold) == TSUCHI_OK && running.log == "pause+3 " && first_hold.id != 0 &&
          tsuchi::owned_count(mod) == owned + 1 && tsuchi::game_pause_holds() == 1);
    CHECK(game_api.hold_pause(mod, TSUCHI_PAUSE_ACTORS, &second_hold) == TSUCHI_OK && second_hold.id != first_hold.id && game_api.paused(mod, &kinds) == TSUCHI_OK && kinds == 3);
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.clockmaker");
    CHECK(game_api.release_pause(rival, first_hold) == TSUCHI_NOT_FOUND && game_api.release_pause(mod, {999}) == TSUCHI_NOT_FOUND);
    running.log.clear();
    CHECK(game_api.release_pause(mod, first_hold) == TSUCHI_OK && running.log == "pause-3 " && game_api.paused(mod, &kinds) == TSUCHI_OK && kinds == TSUCHI_PAUSE_ACTORS);
    CHECK(game_api.release_pause(mod, first_hold) == TSUCHI_NOT_FOUND && tsuchi::game_pause_holds() == 1);
    CHECK(game_api.release_pause(mod, second_hold) == TSUCHI_OK && game_api.paused(mod, &kinds) == TSUCHI_OK && kinds == 0 && tsuchi::owned_count(mod) == owned);
    // No memory to record it against the mod: nothing is paused.
    running.log.clear();
    refused_multiple = 40;
    CHECK(game_api.hold_pause(rival, TSUCHI_PAUSE_HUD, &hold) == TSUCHI_NO_MEMORY && running.log.empty() && tsuchi::game_pause_holds() == 0);
    refused_multiple = 0;
    // A mod that stops has its pauses ended.
    CHECK(game_api.hold_pause(rival, TSUCHI_PAUSE_HUD, &hold) == TSUCHI_OK && game_api.hold_pause(rival, TSUCHI_PAUSE_UI, &hold) == TSUCHI_OK);
    running.log.clear();
    tsuchi::mod_stop(rival);
    CHECK((running.log == "pause-8192 pause-16384 " || running.log == "pause-16384 pause-8192 ") && tsuchi::game_pause_holds() == 0 &&
          game_api.paused(mod, &kinds) == TSUCHI_OK && kinds == 0);
    t::fn::pause_change = nullptr;
    CHECK(game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER, &hold) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    t::fn::pause_change = game_pause_change;

    // Back to the title: only from the story, and not while the game saves, loads, or changes scene.
    running.log.clear();
    CHECK(game_api.return_to_title(mod) == TSUCHI_NOT_READY); // the rule is 6
    fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
    for (uint64_t busy : {t::data::save_async_state, t::data::save_mounted, t::data::engine_save_mounted}) {
        fake::put<uint8_t>(main + busy, 1);
        CHECK(game_api.return_to_title(mod) == TSUCHI_BUSY);
        fake::put<uint8_t>(main + busy, 0);
    }
    fake::put<uint64_t>(running.scenes + t::scene_manager::changing, 1);
    CHECK(game_api.return_to_title(mod) == TSUCHI_BUSY && running.log.empty());
    fake::put<uint64_t>(running.scenes + t::scene_manager::changing, 0);
    t::fn::return_to_title = nullptr;
    CHECK(game_api.return_to_title(mod) == TSUCHI_NOT_READY);
    t::fn::return_to_title = game_return_to_title;
    CHECK(game_api.return_to_title(mod) == TSUCHI_OK && running.log == "title ");

    // The random stream that play draws on.
    TSUCHI_RandomState random{};
    for (int i = 0; i < 4; ++i) fake::put<uint32_t>(main + t::data::random_stream + 4 * i, 100 + uint32_t(i));
    CHECK(game_api.random(mod, &random) == TSUCHI_OK && random.a == 100 && random.b == 101 && random.c == 102 && random.d == 103);
    const TSUCHI_RandomState none{0, 0, 0, 0}, last{0, 0, 0, 9}, some{5, 6, 7, 8};
    CHECK(game_api.set_random(mod, &none) == TSUCHI_INVALID && game_api.set_random(mod, nullptr) == TSUCHI_INVALID && fake::get<uint32_t>(main + t::data::random_stream) == 100);
    CHECK(game_api.set_random(mod, &last) == TSUCHI_OK && game_api.random(mod, &random) == TSUCHI_OK && random.a == 0 && random.d == 9);
    CHECK(game_api.set_random(mod, &some) == TSUCHI_OK && fake::get<uint32_t>(main + t::data::random_stream + 4) == 6 &&
          fake::get<uint32_t>(main + t::data::random_stream + 8) == 7);

    // Told when it changes: nothing for where the game is when listening starts.
    TSUCHI_Subscription subscriptions[6]{};
    CHECK(game_api.on_mode_changed(mod, on_mode_changed, nullptr, &subscriptions[0]) == TSUCHI_OK &&
          game_api.on_stage_changed(mod, on_stage_changed, nullptr, &subscriptions[1]) == TSUCHI_OK &&
          game_api.on_world_loaded(mod, on_world_loaded, nullptr, &subscriptions[2]) == TSUCHI_OK &&
          game_api.on_world_unloaded(mod, on_world_unloaded, nullptr, &subscriptions[3]) == TSUCHI_OK &&
          game_api.on_pause_changed(mod, on_pause_changed, nullptr, &subscriptions[4]) == TSUCHI_OK &&
          game_api.on_control_changed(mod, on_control_changed, nullptr, &subscriptions[5]) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    tsuchi::frame(0.03f);
    CHECK(game_heard.log.empty());
    // A stage within the same mode.
    in_stages({u"Story", u"DayEnd"});
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "stage:Story/StoryInGame>Story/DayEnd control- ");
    // Into more stages than before: the room their names are kept in grows, and the old room goes back.
    const int rooms_of_13 = blocks_of(13), rooms_of_18 = blocks_of(18);
    in_stages({u"Story", u"StoryInGame", u"Deeper"});
    tsuchi::frame(0.03f);
    in_stages({u"Story", u"DayEnd"});
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "stage:Story/StoryInGame>Story/DayEnd control- stage:Story/DayEnd>Story/StoryInGame/Deeper stage:Story/StoryInGame/Deeper>Story/DayEnd ");
    CHECK(blocks_of(13) == rooms_of_13 - 1 && blocks_of(18) == rooms_of_18 - 1);
    // Something is paused, and then not.
    game_heard.log.clear();
    CHECK(game_api.hold_pause(mod, TSUCHI_PAUSE_TIMER, &hold) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(game_api.release_pause(mod, hold) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "paused0>1 paused1>0 ");
    // One of the game's own reasons for a pause, kept above the kinds: not a change in what is paused.
    game_heard.log.clear();
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0x400000);
    tsuchi::frame(0.03f);
    CHECK(game_heard.log.empty());
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    // To another mode and another world, all between two frames: the old world goes first, the new one comes last.
    const int rooms_of_ten = blocks_of(10); // the room the old world's name is kept in
    game_heard.log.clear();
    in_stages({u"VS", u"VsGame"});
    in_world(u"Battle003_P");
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "unloaded:Area001_P mode2>5 stage:Story/DayEnd>VS/VsGame loaded:Battle003_P control+ ");
    CHECK(blocks_of(10) == rooms_of_ten - 1); // the longer name got new room, and the old room went back
    // The world goes away, and a while later another has begun.
    game_heard.log.clear();
    in_world(nullptr);
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "unloaded:Battle003_P control- ");
    tsuchi::frame(0.03f);
    in_world(u"Area002_P");
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "unloaded:Battle003_P control- loaded:Area002_P control+ ");
    // With nobody listening, nothing is kept: a later listener starts afresh.
    for (TSUCHI_Subscription& each : subscriptions) CHECK(tsuchi::host.cancel(mod, each) == TSUCHI_OK);
    game_heard.log.clear();
    tsuchi::frame(0.03f);
    in_stages({u"Title"});
    CHECK(game_api.on_stage_changed(mod, on_stage_changed, nullptr, &subscriptions[1]) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    in_stages({u"Title", u"Menu"});
    tsuchi::frame(0.03f);
    CHECK(game_heard.log == "stage:Title>Title/Menu ");
    CHECK(tsuchi::host.cancel(mod, subscriptions[1]) == TSUCHI_OK);
    tsuchi::frame(0.03f); // a frame with nobody listening: what was seen is let go
    in_stages({u"Story", u"StoryInGame"});
    in_world(u"Area001_P");
}

// ---- The people playing ---------------------------------------------------------------

struct People {
    uint64_t block = 0, config = 0, story = 0, pads = 0, events = 0;
    std::map<int32_t, uint64_t> steered; // by team: who the game says is steered
    std::map<int32_t, std::pair<uint64_t, uint64_t>> pairs; // by team: captain and dog
    bool game_switches = true;
    int asked = 0; // how often the game was asked who is steered
    std::string log;
} people;
void* game_controlled_actor(void*, int32_t team) {
    ++people.asked;
    return reinterpret_cast<void*>(people.steered[team]);
}
bool game_request_switch(void*, int32_t team, bool force, bool at_once) {
    people.log += "switch" + std::to_string(team) + (force ? "force" : "") + (at_once ? "now " : " ");
    if (!people.game_switches) return false;
    const auto& [captain, dog] = people.pairs[team];
    people.steered[team] = people.steered[team] == captain ? dog : captain;
    return true;
}
void list_actors(uint32_t list_offset, std::initializer_list<uint64_t> actors) {
    const uint64_t data = fake::memory(8 * (actors.size() + 1));
    int32_t count = 0;
    for (uint64_t each : actors) fake::put(data + 8 * uint64_t(count++), each);
    fake::put(people.block + list_offset + t::array::data, data), fake::put(people.block + list_offset + t::array::count, count);
}
uint32_t pad_in(uint32_t player, bool second_set = false) {
    return fake::get<uint32_t>(people.pads + (second_set ? t::controller_manager::slots_too : t::controller_manager::slots) + player * t::controller_slot::size +
                               t::controller_slot::pad);
}
struct HeardSteer {
    uint32_t player;
    uint64_t actor, previous;
};
std::vector<HeardSteer> heard_steers;
void on_controlled_changed(void*, const TSUCHI_PlayerCharacterChanged* e) { heard_steers.push_back({e->player, e->character.id, e->previous.id}); }

void playing() {
    const TSUCHI_PlayerService& players_api = *tsuchi::host.player;
    const uint64_t main = fake::main_image();
    uint32_t number = 99;
    TSUCHI_Actor who{};

    // No world, and nothing set up: nobody is playing.
    people.block = running.context + t::game_context::players;
    CHECK(players_api.count(mod, &number) == TSUCHI_OK && number == 0 && players_api.captain(mod, 0, &who) == TSUCHI_NOT_FOUND && players_api.character(mod, 0, &who) == TSUCHI_NOT_READY);
    CHECK(players_api.pad(mod, 0, &number) == TSUCHI_NOT_READY && players_api.hold_pad(mod, 0, 1) == TSUCHI_NOT_READY);

    people.config = fake::memory(0x20), people.story = fake::memory(0x20), people.pads = fake::memory(0x80), people.events = fake::memory(0x20);
    fake::put(running.instance + t::game_instance::config_save, people.config), fake::put(running.instance + t::game_instance::story_save, people.story);
    fake::put(running.instance + t::game_instance::controllers, people.pads), fake::put(w.framework + t::game_framework::events, people.events);
    fake::put<uint8_t>(people.story + t::story_save::switch_unlocked, 1);
    t::fn::controlled_actor = game_controlled_actor, t::fn::request_control_switch = game_request_switch;
    const auto person = [](const char16_t* name, int32_t team) {
        const uint64_t made = actor(name, w.actor_class, name, team, 0, component(0, 0, 0, 0));
        fake::put(made + t::game_actor::stick, fake::memory(0x60)), fake::put(made + t::game_actor::player, fake::memory(0x60));
        return made;
    };
    const int32_t sideless = int32_t(t::constant::team_story);

    // The story: one player, whose captain and dog are on no side.
    // The game's list of dogs begins with one of another team, which is not theirs.
    const uint64_t hero = person(u"Hero", sideless), hound = person(u"Hound", sideless), steed = person(u"Steed", sideless + 1);
    list_actors(t::player_block::captains, {hero}), list_actors(t::player_block::dogs, {steed, hound});
    people.pairs[sideless] = {hero, hound}, people.steered[sideless] = hero;
    CHECK(players_api.count(mod, &number) == TSUCHI_OK && number == 1);
    CHECK(players_api.captain(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == hero && players_api.dog(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == hound);
    CHECK(players_api.character(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == hero);
    const int asked = people.asked;
    CHECK(players_api.captain(mod, 1, &who) == TSUCHI_NOT_FOUND && players_api.dog(mod, 1, &who) == TSUCHI_NOT_FOUND && players_api.character(mod, 1, &who) == TSUCHI_NOT_FOUND &&
          players_api.captain(mod, 4, &who) == TSUCHI_NOT_FOUND && from_handle(who.id) == hero && people.asked == asked); // the game is not asked about nobody
    // The first of those on no side is the one; what is listed and is no actor is passed over; and the game has four places, whatever a team is numbered.
    const uint64_t understudy = person(u"Understudy", sideless), far_side = person(u"FarSide", 5), former = person(u"Former", sideless);
    fake::destroy(former);
    list_actors(t::player_block::captains, {former, hero, understudy, far_side});
    CHECK(players_api.captain(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == hero && players_api.captain(mod, 5, &who) == TSUCHI_NOT_FOUND);
    list_actors(t::player_block::captains, {hero});

    // A battle: two players, each on a side; the second has no dog.
    const uint64_t red_captain = person(u"RedCaptain", 0), red_dog = person(u"RedDog", 0), blue_captain = person(u"BlueCaptain", 1);
    list_actors(t::player_block::captains, {0, blue_captain, red_captain}), list_actors(t::player_block::dogs, {red_dog});
    people.pairs[0] = {red_captain, red_dog}, people.steered[0] = red_dog, people.pairs[1] = {blue_captain, 0}, people.steered[1] = blue_captain;
    CHECK(players_api.count(mod, &number) == TSUCHI_OK && number == 2);
    CHECK(players_api.captain(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == red_captain && players_api.captain(mod, 1, &who) == TSUCHI_OK && from_handle(who.id) == blue_captain);
    CHECK(players_api.dog(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == red_dog && players_api.dog(mod, 1, &who) == TSUCHI_OK && !who.id); // a player without a dog has none
    CHECK(players_api.character(mod, 0, &who) == TSUCHI_OK && from_handle(who.id) == red_dog && players_api.character(mod, 1, &who) == TSUCHI_OK && from_handle(who.id) == blue_captain);
    // A captain that is gone is nobody's, and a dog without a captain is no player's.
    list_actors(t::player_block::captains, {blue_captain}), list_actors(t::player_block::dogs, {person(u"Stray", -1), red_dog});
    CHECK(players_api.count(mod, &number) == TSUCHI_OK && number == 1 && players_api.dog(mod, 0, &who) == TSUCHI_NOT_FOUND);
    people.steered[1] = main; // the game names something that is no actor
    CHECK(players_api.character(mod, 1, &who) == TSUCHI_NOT_FOUND);
    people.steered[1] = blue_captain;
    t::fn::controlled_actor = nullptr;
    CHECK(players_api.character(mod, 1, &who) == TSUCHI_NOT_READY);
    t::fn::controlled_actor = game_controlled_actor;

    // Back in the story. Switching: through the game's own switch, when its rules allow.
    list_actors(t::player_block::captains, {hero}), list_actors(t::player_block::dogs, {steed, hound});
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_OK && people.log.empty()); // steered already
    CHECK(players_api.set_character(mod, 0, handle(hound)) == TSUCHI_OK && people.log == "switch" + std::to_string(sideless) + " " && people.steered[sideless] == hound);
    people.log.clear();
    CHECK(players_api.set_character(mod, 0, handle(red_captain)) == TSUCHI_INVALID && players_api.set_character(mod, 1, handle(hero)) == TSUCHI_NOT_FOUND &&
          players_api.set_character(mod, 0, handle(running.world)) == TSUCHI_INVALID);
    const uint64_t passerby = person(u"Passerby", sideless);
    const TSUCHI_Actor gone = handle(passerby);
    fake::destroy(passerby);
    CHECK(players_api.set_character(mod, 0, gone) == TSUCHI_NOT_FOUND && people.log.empty());
    // A switch already fading in; two people playing the story; switching not yet unlocked; the
    // character on its way out; the captain riding; the game saying no; the game not to be asked.
    fake::put<uint8_t>(people.block + t::player_block::switch_pending, 1);
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_BUSY);
    fake::put<uint8_t>(people.block + t::player_block::switch_pending, 0);
    fake::put<uint8_t>(people.config + t::config_save::play_mode, uint8_t(t::constant::play_mode_two));
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(people.config + t::config_save::play_mode, 0);
    fake::put<uint8_t>(people.story + t::story_save::switch_unlocked, 0);
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(people.story + t::story_save::switch_unlocked, 1);
    fake::put<uint8_t>(hero + t::game_actor::removed, 1);
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(hero + t::game_actor::removed, 0);
    const uint64_t grip = fake::get<uint64_t>(hero + t::game_actor::stick), seat = fake::get<uint64_t>(hero + t::game_actor::player);
    fake::put(grip + t::stick_component::target, hound), fake::put(seat + t::player_component::ridden_dog, hound);
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY);
    fake::put(grip + t::stick_component::target, red_dog); // holding on to something that is not its mount
    people.game_switches = false;
    CHECK(people.log.empty() && players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY && people.log == "switch" + std::to_string(sideless) + " ");
    people.game_switches = true;
    fake::put<uint64_t>(grip + t::stick_component::target, 0);
    t::fn::request_control_switch = nullptr;
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY);
    t::fn::request_control_switch = game_request_switch;
    people.log.clear();

    // Held on the dog: the player's own switch, fading in, is dropped; one the game has made is
    // undone, asked for now and then, at once, and not while an event has the game's attention.
    const uint32_t owned = tsuchi::owned_count(mod);
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.puppeteer");
    CHECK(players_api.hold_character(mod, 0, handle(hero)) == TSUCHI_NOT_READY && players_api.hold_character(mod, 1, handle(hound)) == TSUCHI_NOT_FOUND &&
          players_api.hold_character(mod, 0, gone) == TSUCHI_NOT_FOUND && tsuchi::owned_count(mod) == owned);
    CHECK(players_api.hold_character(mod, 0, handle(hound)) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    CHECK(players_api.hold_character(rival, 0, handle(hound)) == TSUCHI_BUSY && players_api.release_character(rival, 0) == TSUCHI_NOT_FOUND &&
          players_api.release_character(mod, 1) == TSUCHI_NOT_FOUND && players_api.release_character(mod, 9) == TSUCHI_NOT_FOUND);
    fake::put<uint8_t>(people.block + t::player_block::switch_pending, 1);
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(people.block + t::player_block::switch_pending) == 0 && people.log.empty());
    people.steered[sideless] = hero;
    for (int i = 0; i < 14; ++i) tsuchi::frame(0.03f);
    CHECK(people.log.empty());
    fake::put<uint8_t>(people.events + t::event_manager::busy_too, 1);
    tsuchi::frame(0.03f);
    CHECK(people.log.empty());
    fake::put<uint8_t>(people.events + t::event_manager::busy_too, 0);
    for (int i = 0; i < 15; ++i) tsuchi::frame(0.03f);
    CHECK(people.log == "switch" + std::to_string(sideless) + "now " && people.steered[sideless] == hound);
    for (int i = 0; i < 40; ++i) tsuchi::frame(0.03f);
    CHECK(people.log == "switch" + std::to_string(sideless) + "now ");
    // A switch away that the game takes back by itself before long does not count towards the next.
    people.log.clear();
    people.steered[sideless] = hero;
    for (int i = 0; i < 14; ++i) tsuchi::frame(0.03f);
    people.steered[sideless] = hound;
    tsuchi::frame(0.03f);
    people.steered[sideless] = hero;
    for (int i = 0; i < 14; ++i) tsuchi::frame(0.03f);
    CHECK(people.log.empty());
    tsuchi::frame(0.03f);
    CHECK(people.log == "switch" + std::to_string(sideless) + "now " && people.steered[sideless] == hound);
    // Released: the game's switches stand.
    CHECK(players_api.release_character(mod, 0) == TSUCHI_OK && tsuchi::owned_count(mod) == owned && players_api.release_character(mod, 0) == TSUCHI_NOT_FOUND);
    people.steered[sideless] = hero, people.log.clear();
    for (int i = 0; i < 40; ++i) tsuchi::frame(0.03f);
    CHECK(people.log.empty());
    // A hold on a character that leaves ends by itself; so does a stopped mod's.
    CHECK(players_api.hold_character(mod, 0, handle(hero)) == TSUCHI_OK);
    list_actors(t::player_block::captains, {blue_captain}); // the hero is no longer anyone's captain
    tsuchi::frame(0.03f);
    CHECK(tsuchi::owned_count(mod) == owned);
    list_actors(t::player_block::captains, {hero});
    CHECK(players_api.hold_character(rival, 0, handle(hero)) == TSUCHI_OK && players_api.hold_character(mod, 0, handle(hero)) == TSUCHI_BUSY);
    tsuchi::mod_stop(rival);
    CHECK(players_api.hold_character(mod, 0, handle(hero)) == TSUCHI_OK && players_api.release_character(mod, 0) == TSUCHI_OK);

    // Controllers: which one feeds each player, and one held in place of it.
    for (uint32_t player = 0; player < 4; ++player)
        for (uint64_t set : {uint64_t(t::controller_manager::slots), uint64_t(t::controller_manager::slots_too)})
            fake::put<uint32_t>(people.pads + set + player * t::controller_slot::size + t::controller_slot::pad, player);
    CHECK(players_api.pad(mod, 0, &number) == TSUCHI_OK && number == 0 && players_api.pad(mod, 3, &number) == TSUCHI_OK && number == 3 && players_api.pad(mod, 4, &number) == TSUCHI_INVALID);
    CHECK(!player_pad(4, number) && number == 3); // four places, and no fifth
    CHECK(players_api.hold_pad(mod, 4, 0) == TSUCHI_INVALID && players_api.hold_pad(mod, 1, 8) == TSUCHI_INVALID && players_api.hold_pad(mod, 1, 0x21) == TSUCHI_INVALID && pad_in(1) == 1);
    CHECK(players_api.hold_pad(mod, 1, 0x20) == TSUCHI_OK && pad_in(1) == 0x20 && pad_in(1, true) == 0x20 && pad_in(0) == 0 && pad_in(2) == 2 && tsuchi::owned_count(mod) == owned + 1);
    rival = tsuchi::mod_create("actor.operator");
    CHECK(players_api.hold_pad(rival, 1, 2) == TSUCHI_BUSY && players_api.release_pad(rival, 1) == TSUCHI_NOT_FOUND && pad_in(1) == 0x20);
    // The game sets its own again: put back on the next frame.
    fake::put<uint32_t>(people.pads + t::controller_manager::slots + t::controller_slot::size + t::controller_slot::pad, 1);
    tsuchi::frame(0.03f);
    CHECK(pad_in(1) == 0x20);
    // Held again by the same mod, it is another controller; released, the player has the one they had.
    CHECK(players_api.hold_pad(mod, 1, uint32_t(t::constant::pad_none)) == TSUCHI_OK && pad_in(1) == t::constant::pad_none && tsuchi::owned_count(mod) == owned + 1);
    CHECK(players_api.release_pad(mod, 1) == TSUCHI_OK && pad_in(1) == 1 && pad_in(1, true) == 1 && tsuchi::owned_count(mod) == owned && players_api.release_pad(mod, 1) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(pad_in(1) == 1);
    CHECK(players_api.hold_pad(rival, 0, 3) == TSUCHI_OK && pad_in(0) == 3);
    tsuchi::mod_stop(rival);
    CHECK(pad_in(0) == 0 && pad_in(0, true) == 0);
    fake::put<uint64_t>(running.instance + t::game_instance::controllers, 0x7FFF00000000ull); // not to be read
    CHECK(players_api.pad(mod, 0, &number) == TSUCHI_NOT_READY && players_api.hold_pad(mod, 0, 1) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    fake::put(running.instance + t::game_instance::controllers, people.pads);

    // Told when a player steers their other character; nothing for who steers what when listening starts.
    TSUCHI_Subscription steering{};
    CHECK(players_api.on_character_changed(mod, on_controlled_changed, nullptr, &steering) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    tsuchi::frame(0.03f);
    CHECK(heard_steers.empty());
    people.steered[sideless] = hound;
    tsuchi::frame(0.03f);
    CHECK(heard_steers.size() == 1 && heard_steers[0].player == 0 && heard_steers[0].actor == to_handle(hound) && heard_steers[0].previous == to_handle(hero));
    people.steered[sideless] = 0; // steering nothing for a moment
    tsuchi::frame(0.03f);
    people.steered[sideless] = hero;
    tsuchi::frame(0.03f);
    tsuchi::frame(0.03f);
    CHECK(heard_steers.size() == 3 && heard_steers[1].actor == 0 && heard_steers[1].previous == to_handle(hound) && heard_steers[2].actor == to_handle(hero) &&
          heard_steers[2].previous == 0);
    CHECK(tsuchi::host.cancel(mod, steering) == TSUCHI_OK);
    people.steered[sideless] = hound;
    tsuchi::frame(0.03f);
    CHECK(players_api.on_character_changed(mod, on_controlled_changed, nullptr, &steering) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    people.steered[sideless] = hero;
    tsuchi::frame(0.03f);
    CHECK(heard_steers.size() == 4 && heard_steers[3].previous == to_handle(hound));
    CHECK(tsuchi::host.cancel(mod, steering) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    // The game switches at once, and one that begins to listen and asks for the switch before a frame passes is told of it.
    CHECK(players_api.on_character_changed(mod, on_controlled_changed, nullptr, &steering) == TSUCHI_OK && players_api.set_character(mod, 0, handle(hound)) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_steers.size() == 5 && heard_steers[4].actor == to_handle(hound) && heard_steers[4].previous == to_handle(hero));
    CHECK(players_api.set_character(mod, 0, handle(hero)) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard_steers.size() == 6 && heard_steers[5].actor == to_handle(hero));
    CHECK(tsuchi::host.cancel(mod, steering) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    people.log.clear();
    w.hero = hero, w.hound = hound;
}

// ---- Captains -------------------------------------------------------------------------

struct Leading {
    uint64_t cursor = 0, squad = 0, thrown = 0, got_off = 0;
    int32_t picked = 0; // the type the squad has picked
    bool picks = true, allows = true;
    std::map<uint64_t, uint64_t> leaders; // by Pikmin
    std::string log;
} leading;
void game_start_whistle(void* part, bool riding) {
    const uint64_t at = reinterpret_cast<uint64_t>(part), whistle = fake::get<uint64_t>(at + t::avatar_component::whistle);
    leading.log += riding ? "whistle-riding " : "whistle ";
    fake::put<uint8_t>(at + t::avatar_component::whistling, 7); // any nonzero
    fake::put(whistle + t::whistle_part::count, fake::get<uint32_t>(whistle + t::whistle_part::count) + 1);
}
void game_end_whistle(void* part) {
    leading.log += "hush ";
    fake::put<uint8_t>(reinterpret_cast<uint64_t>(part) + t::avatar_component::whistling, 0);
}
void* game_captain_cursor(void*) { return reinterpret_cast<void*>(leading.cursor); }
uint64_t game_captain_squad(void*) { return leading.squad; }
int32_t game_squad_selected(void* squad) { return reinterpret_cast<uint64_t>(squad) == leading.squad ? leading.picked : -1; }
void game_squad_select_of(void*, void* pikmin) {
    leading.log += "pick ";
    if (leading.picks) leading.picked = fake::get<uint8_t>(reinterpret_cast<uint64_t>(pikmin) + t::game_actor::color);
}
void game_start_throw(void*, void* pikmin, void* nothing) {
    leading.log += nothing ? "throw! " : "throw ";
    leading.thrown = reinterpret_cast<uint64_t>(pikmin);
}
void* game_pikmin_leader(void* pikmin) { return reinterpret_cast<void*>(leading.leaders[reinterpret_cast<uint64_t>(pikmin)]); }
bool game_ride_allowed(void* dog, void* captain, bool sure) {
    leading.log += sure ? "may " : "may? ";
    return leading.allows && dog && captain;
}
void game_request_ride(void* part) {
    leading.log += "ride ";
    fake::put<uint8_t>(reinterpret_cast<uint64_t>(part) + t::avatar_component::ride_asked, 1);
}
void game_request_get_off(void* steering) {
    leading.log += "off ";
    leading.got_off = reinterpret_cast<uint64_t>(steering);
}

void leading_captains() {
    const TSUCHI_CaptainService& lead = *tsuchi::host.captain;
    namespace a = t::avatar_component;
    TSUCHI_Whistle sound{};
    TSUCHI_Vec3 point{};
    TSUCHI_Actor who{};
    const uint32_t owned = tsuchi::owned_count(mod);
    const auto log_is = [](const char* expected) {
        const bool same = leading.log == expected;
        if (!same) std::printf("  log: [%s]\n", leading.log.c_str());
        leading.log.clear();
        return same;
    };
    const auto close = [](float a_, float b_) { return std::fabs(a_ - b_) < 0.001f; };

    fake::code().push_back(reinterpret_cast<uint64_t>(&game_captain_squad));
    const uint64_t avatar_class = fake::object(u"AvatarComponent", w.root), avatar_table = fake::memory(0x60);
    fake::put(avatar_table + t::avatar_table::squad, &game_captain_squad);
    t::fn::start_whistle = game_start_whistle, t::fn::end_whistle = game_end_whistle, t::fn::captain_cursor = game_captain_cursor;
    t::fn::squad_selected = game_squad_selected, t::fn::squad_select_of = game_squad_select_of, t::fn::start_throw = game_start_throw;
    t::fn::pikmin_leader = game_pikmin_leader, t::fn::ride_allowed = game_ride_allowed, t::fn::request_ride = game_request_ride;
    t::fn::request_get_off = game_request_get_off;

    // A captain is an actor whose deciding part is a captain's. One that leaves play, with a function table, and its dog.
    const auto captain = [&](const std::u16string& name, float x, float y) {
        const uint64_t made = tabled(table_a(), name, u"Captain", 0, x);
        fake::put(fake::get<uint64_t>(made + t::game_actor::root) + t::scene_component::location + 4, y);
        const uint64_t part = fake::object(u"Deciding", avatar_class, 0xC0), whistle = fake::object(u"Whistle", w.component_class, 0xA0);
        fake::put(part, avatar_table), fake::put(part + a::whistle, whistle), fake::put(part + a::ride_reach, 150.0f);
        fake::put(whistle + t::whistle_part::radius, 80.0f), fake::put(whistle + t::whistle_part::count, uint32_t(3));
        const float center[3] = {10, 20, 30};
        for (int i = 0; i < 3; ++i) fake::put(whistle + t::whistle_part::center + 4 * i, center[i]);
        fake::put(made + t::game_actor::ai, part);
        fake::put(made + t::game_actor::stick, fake::memory(0x60)), fake::put(made + t::game_actor::player, fake::memory(0x60));
        registers(made);
        return made;
    };
    const uint64_t chief = captain(u"Chief", 100, 200), part = fake::get<uint64_t>(chief + t::game_actor::ai);
    const uint64_t whistle = fake::get<uint64_t>(part + a::whistle);
    const uint64_t thinker = actor(u"Thinker", w.actor_class, u"Thinker", 3, 0, component(0, 0, 0, 0)); // decides, and is no captain
    fake::put(thinker + t::game_actor::ai, fake::object(u"Deciding", w.component_class, 0xC0));
    const uint64_t ghost = actor(u"Ghost", w.actor_class, u"Ghost", 3, 0, component(0, 0, 0, 0));
    const TSUCHI_Actor gone = handle(ghost);
    fake::destroy(ghost);

    // The whistle, read.
    CHECK(lead.whistle(mod, handle(chief), &sound) == TSUCHI_OK && !sound.active && sound.radius == 80.0f && sound.center.x == 10 && sound.center.y == 20 &&
          sound.center.z == 30 && sound.count == 3);
    CHECK(lead.whistle(mod, handle(thinker), &sound) == TSUCHI_NOT_FOUND && lead.whistle(mod, handle(w.rock), &sound) == TSUCHI_NOT_FOUND &&
          lead.whistle(mod, gone, &sound) == TSUCHI_NOT_FOUND);
    refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // no memory for the first holds: nothing is held, and the game is not asked
    CHECK(lead.hold_whistle(mod, handle(chief)) == TSUCHI_NO_MEMORY && log_is("") && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    fake::put<uint64_t>(part + a::whistle, 0);
    CHECK(lead.whistle(mod, handle(chief), &sound) == TSUCHI_NOT_FOUND && lead.hold_whistle(mod, handle(chief)) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    fake::put(part + a::whistle, fake::object(u"Whistle", w.component_class, 0xA0));
    fake::destroy(fake::get<uint64_t>(part + a::whistle)); // its whistle is being destroyed
    CHECK(lead.whistle(mod, handle(chief), &sound) == TSUCHI_NOT_FOUND);
    fake::put(part + a::whistle, whistle);

    // Held: begun at once, begun again when the game has ended it, and ended with the hold.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.piper");
    CHECK(lead.hold_whistle(mod, handle(thinker)) == TSUCHI_NOT_FOUND && lead.hold_whistle(mod, gone) == TSUCHI_NOT_FOUND && lead.release_whistle(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    CHECK(lead.hold_whistle(mod, handle(chief)) == TSUCHI_OK && log_is("whistle ") && tsuchi::owned_count(mod) == owned + 1);
    CHECK(lead.whistle(mod, handle(chief), &sound) == TSUCHI_OK && sound.active && sound.count == 4);
    CHECK(lead.hold_whistle(mod, handle(chief)) == TSUCHI_OK && lead.hold_whistle(rival, handle(chief)) == TSUCHI_BUSY && lead.release_whistle(rival, handle(chief)) == TSUCHI_NOT_FOUND &&
          log_is("") && tsuchi::owned_count(mod) == owned + 1);
    tsuchi::frame(0.03f);
    CHECK(log_is("")); // it still sounds
    fake::put<uint8_t>(part + a::whistling, 0); // a steered captain ends it by itself
    tsuchi::frame(0.03f);
    CHECK(log_is("whistle "));
    CHECK(lead.release_whistle(mod, handle(chief)) == TSUCHI_OK && log_is("hush ") && tsuchi::owned_count(mod) == owned && lead.release_whistle(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(log_is(""));
    // The game cannot be asked: nothing is held.
    t::fn::start_whistle = nullptr;
    CHECK(lead.hold_whistle(mod, handle(chief)) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned && lead.release_whistle(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    t::fn::start_whistle = game_start_whistle;
    const uint64_t second = captain(u"Second", 0, 0);
    // It ends with the mod, and with the captain.
    CHECK(lead.hold_whistle(rival, handle(chief)) == TSUCHI_OK && log_is("whistle "));
    tsuchi::mod_stop(rival);
    CHECK(log_is("hush ") && lead.hold_whistle(mod, handle(chief)) == TSUCHI_OK && log_is("whistle ") && lead.hold_whistle(mod, handle(second)) == TSUCHI_OK && log_is("whistle "));
    const TSUCHI_Actor second_handle = handle(second);
    pool_return(kPool, second);
    CHECK(tsuchi::owned_count(mod) == owned + 1 && lead.release_whistle(mod, second_handle) == TSUCHI_NOT_FOUND);
    tsuchi::frame(0.03f);
    CHECK(log_is("") && lead.release_whistle(mod, handle(chief)) == TSUCHI_OK && log_is("hush ") && tsuchi::owned_count(mod) == owned);

    // The cursor, read: the one the game says is in use.
    CHECK(lead.cursor(mod, handle(chief), &point) == TSUCHI_NOT_READY && lead.cursor(mod, handle(thinker), &point) == TSUCHI_NOT_FOUND);
    leading.cursor = fake::memory(0x40);
    const auto cursor_put = [](uint32_t offset, float x, float y, float z) {
        fake::put(leading.cursor + offset, x), fake::put(leading.cursor + offset + 4, y), fake::put(leading.cursor + offset + 8, z);
    };
    const auto cursor_is = [&](uint32_t offset, float x, float y, float z) {
        return close(fake::get<float>(leading.cursor + offset), x) && close(fake::get<float>(leading.cursor + offset + 4), y) && close(fake::get<float>(leading.cursor + offset + 8), z);
    };
    cursor_put(t::cursor::position, 1, 2, 3), cursor_put(t::cursor::offset, 9, 9, 9);
    CHECK(lead.cursor(mod, handle(chief), &point) == TSUCHI_OK && point.x == 1 && point.y == 2 && point.z == 3);
    t::fn::captain_cursor = nullptr;
    CHECK(lead.cursor(mod, handle(chief), &point) == TSUCHI_NOT_READY);
    t::fn::captain_cursor = game_captain_cursor;
    const uint64_t real_cursor = leading.cursor;
    leading.cursor = 0x40; // nowhere
    CHECK(lead.cursor(mod, handle(chief), &point) == TSUCHI_NOT_READY && lead.hold_cursor(mod, handle(chief), &point) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    leading.cursor = real_cursor;

    // Held on a point: kept as a step across the ground from the captain, and put back when the game has moved it.
    const TSUCHI_Vec3 spot{400, 150, 25}, other{-50, 0, 5};
    rival = tsuchi::mod_create("actor.aimer");
    CHECK(lead.hold_cursor(mod, handle(thinker), &spot) == TSUCHI_NOT_FOUND && lead.hold_cursor(mod, gone, &spot) == TSUCHI_NOT_FOUND && lead.release_cursor(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    refused_size = 16 * (sizeof(tsuchi::Hold) + 24);
    CHECK(lead.hold_cursor(mod, handle(chief), &spot) == TSUCHI_NO_MEMORY && tsuchi::owned_count(mod) == owned && lead.release_cursor(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    refused_size = 0;
    CHECK(lead.hold_cursor(mod, handle(chief), &spot) == TSUCHI_OK && cursor_is(t::cursor::position, 400, 150, 25) && cursor_is(t::cursor::offset, 300, -50, 0) &&
          tsuchi::owned_count(mod) == owned + 1);
    cursor_put(t::cursor::position, 0, 0, 0), cursor_put(t::cursor::offset, 5, 5, 5);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 400, 150, 25) && cursor_is(t::cursor::offset, 300, -50, 0));
    CHECK(lead.hold_cursor(rival, handle(chief), &other) == TSUCHI_BUSY && lead.hold_cursor_on(rival, handle(chief), handle(w.rock)) == TSUCHI_BUSY &&
          lead.release_cursor(rival, handle(chief)) == TSUCHI_NOT_FOUND && cursor_is(t::cursor::position, 400, 150, 25));
    CHECK(lead.hold_cursor(mod, handle(chief), &other) == TSUCHI_OK && cursor_is(t::cursor::position, -50, 0, 5) && cursor_is(t::cursor::offset, -150, -200, 0) &&
          tsuchi::owned_count(mod) == owned + 1); // the hold moved
    // On an actor: followed as it moves; the same hold; over when the target is gone.
    const uint64_t prey = actor(u"Prey", w.actor_class, u"Prey", 3, 0, component(700, 800, 90, 0));
    CHECK(lead.hold_cursor_on(mod, handle(chief), gone) == TSUCHI_NOT_FOUND && cursor_is(t::cursor::position, -50, 0, 5));
    const uint64_t nowhere = actor(u"Nowhere", w.actor_class, u"Nowhere", 3, 0, 0);
    CHECK(lead.hold_cursor_on(mod, handle(chief), handle(nowhere)) == TSUCHI_NOT_READY && cursor_is(t::cursor::position, -50, 0, 5) && tsuchi::owned_count(mod) == owned + 1);
    CHECK(lead.release_cursor(mod, handle(chief)) == TSUCHI_OK);
    CHECK(lead.hold_cursor_on(mod, handle(chief), handle(nowhere)) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned && lead.release_cursor(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.squinter");
    refused_multiple = 40; // no room to note whose the hold is: there is no hold
    CHECK(lead.hold_cursor(poor, handle(chief), &other) == TSUCHI_NO_MEMORY && lead.hold_whistle(poor, handle(chief)) == TSUCHI_NO_MEMORY && log_is(""));
    refused_multiple = 0;
    cursor_put(t::cursor::position, 3, 3, 3);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 3, 3, 3) && lead.release_cursor(poor, handle(chief)) == TSUCHI_NOT_FOUND && lead.release_whistle(poor, handle(chief)) == TSUCHI_NOT_FOUND && log_is(""));
    tsuchi::mod_stop(poor);
    CHECK(lead.hold_cursor(mod, handle(chief), &other) == TSUCHI_OK);
    CHECK(lead.hold_cursor_on(mod, handle(chief), handle(prey)) == TSUCHI_OK && cursor_is(t::cursor::position, 700, 800, 90) && cursor_is(t::cursor::offset, 600, 600, 0) &&
          tsuchi::owned_count(mod) == owned + 1);
    fake::put(fake::get<uint64_t>(prey + t::game_actor::root) + t::scene_component::location, 710.0f);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 710, 800, 90) && cursor_is(t::cursor::offset, 610, 600, 0));
    fake::destroy(prey);
    cursor_put(t::cursor::position, 1, 1, 1);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 1, 1, 1) && tsuchi::owned_count(mod) == owned && lead.release_cursor(mod, handle(chief)) == TSUCHI_NOT_FOUND);
    // No cursor, or no place: nothing is held.
    leading.cursor = 0;
    CHECK(lead.hold_cursor(mod, handle(chief), &spot) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    leading.cursor = fake::memory(0x40);
    const uint64_t chief_root = fake::get<uint64_t>(chief + t::game_actor::root);
    fake::put<uint64_t>(chief + t::game_actor::root, 0);
    CHECK(lead.hold_cursor(mod, handle(chief), &spot) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    fake::put(chief + t::game_actor::root, chief_root);
    // Released, it is left where it is; and it ends with the mod and with the captain.
    CHECK(lead.hold_cursor(mod, handle(chief), &spot) == TSUCHI_OK && lead.release_cursor(mod, handle(chief)) == TSUCHI_OK && tsuchi::owned_count(mod) == owned);
    cursor_put(t::cursor::position, 2, 2, 2);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 2, 2, 2));
    CHECK(lead.hold_cursor(rival, handle(chief), &spot) == TSUCHI_OK);
    tsuchi::mod_stop(rival);
    cursor_put(t::cursor::position, 2, 2, 2);
    tsuchi::frame(0.03f);
    CHECK(cursor_is(t::cursor::position, 2, 2, 2));
    const uint64_t third = captain(u"Third", 0, 0);
    const TSUCHI_Actor third_handle = handle(third);
    CHECK(lead.hold_cursor(mod, third_handle, &spot) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    pool_return(kPool, third);
    CHECK(tsuchi::owned_count(mod) == owned && lead.release_cursor(mod, third_handle) == TSUCHI_NOT_FOUND);
    // A captain that is gone without its leaving having been seen: what was held on it ends on the next frame.
    const uint64_t vanished = captain(u"Vanished", 0, 0);
    CHECK(lead.hold_cursor(mod, handle(vanished), &spot) == TSUCHI_OK && lead.hold_whistle(mod, handle(vanished)) == TSUCHI_OK && log_is("whistle ") &&
          tsuchi::owned_count(mod) == owned + 2);
    fake::destroy(vanished);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::owned_count(mod) == owned && log_is(""));

    // Throwing: a Pikmin that follows the captain, of the type the captain has picked or can pick.
    const auto pikmin = [&](const std::u16string& name, uint8_t color, uint64_t leader) {
        const uint64_t made = actor(name, w.actor_class, u"Pikmin", 3, 0, component(0, 0, 0, 0));
        fake::put(made + t::game_actor::pikmin, fake::object(u"PikminPart", w.component_class, 0x40));
        fake::put<uint8_t>(made + t::game_actor::color, color);
        leading.leaders[made] = leader;
        return made;
    };
    const uint64_t mutt = actor(u"Mutt", w.actor_class, u"Mutt", 3, 0, component(180, 200, 0, 0)), mutt_seat = fake::object(u"Seat", w.component_class, 0x60);
    fake::put(mutt + t::game_actor::player, mutt_seat);
    const uint64_t red = pikmin(u"Red", 0, chief), blue = pikmin(u"Blue", 1, chief), stray = pikmin(u"Stray", 0, 0), theirs = pikmin(u"Theirs", 0, second);
    const uint64_t rider = pikmin(u"Rider", 2, mutt);
    leading.squad = fake::memory(0x20), leading.picked = 0;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_OK && log_is("throw ") && leading.thrown == red); // picked already
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(blue)) == TSUCHI_OK && log_is("pick throw ") && leading.thrown == blue && leading.picked == 1);
    leading.picks = false;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_BUSY && log_is("pick "));
    leading.picks = true;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(stray)) == TSUCHI_INVALID && lead.throw_pikmin(mod, handle(chief), handle(theirs)) == TSUCHI_INVALID &&
          lead.throw_pikmin(mod, handle(chief), handle(rider)) == TSUCHI_INVALID && log_is("")); // the dog is not its dog yet
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(w.rock)) == TSUCHI_NOT_FOUND && lead.throw_pikmin(mod, handle(chief), gone) == TSUCHI_NOT_FOUND &&
          lead.throw_pikmin(mod, handle(thinker), handle(red)) == TSUCHI_NOT_FOUND && lead.throw_pikmin(mod, gone, handle(red)) == TSUCHI_NOT_FOUND && log_is(""));
    fake::put<uint8_t>(red + t::game_actor::removed, 1);
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_FOUND);
    fake::put<uint8_t>(red + t::game_actor::removed, 0), fake::put<uint8_t>(red + t::game_actor::kill_queued, 1);
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_FOUND);
    fake::put<uint8_t>(red + t::game_actor::kill_queued, 0);
    const uint64_t red_part = fake::get<uint64_t>(red + t::game_actor::pikmin);
    fake::put<uint64_t>(red + t::game_actor::pikmin, 0);
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_FOUND);
    fake::put(red + t::game_actor::pikmin, red_part);
    fake::put(red + t::game_actor::pikmin, fake::object(u"PikminPart", w.component_class, 0x40));
    fake::destroy(fake::get<uint64_t>(red + t::game_actor::pikmin)); // the part that makes it one is being destroyed
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_FOUND);
    fake::put(red + t::game_actor::pikmin, red_part);
    const uint64_t decoy = fake::object(u"Decoy", w.component_class, 0x100); // no actor, with a Pikmin's part where one keeps it
    fake::put(decoy + t::game_actor::pikmin, red_part);
    leading.leaders[decoy] = chief;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(decoy)) == TSUCHI_NOT_FOUND && !is_pikmin(decoy) && is_pikmin(red) && log_is(""));
    leading.leaders[blue] = decoy; // the game names something that is no actor
    CHECK(pikmin_leader(blue) == 0 && pikmin_leader(red) == chief && lead.throw_pikmin(mod, handle(chief), handle(blue)) == TSUCHI_INVALID);
    leading.leaders[blue] = chief;
    const uint64_t squad = leading.squad;
    leading.squad = 0;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_READY && log_is(""));
    leading.squad = squad;
    fake::put<uint64_t>(avatar_table + t::avatar_table::squad, 0x1234); // not code
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_READY && log_is(""));
    fake::put(avatar_table + t::avatar_table::squad, &game_captain_squad);
    t::fn::start_throw = nullptr;
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(red)) == TSUCHI_NOT_READY && log_is(""));
    t::fn::start_throw = game_start_throw;

    // Riding: beside its dog, when the game allows; the game's note of the asking is cleared once it rides.
    const uint64_t grip = fake::get<uint64_t>(chief + t::game_actor::stick), seat = fake::get<uint64_t>(chief + t::game_actor::player);
    refused_size = 16 * sizeof(uint64_t); // no memory to remember an asking: not asked
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_NO_MEMORY);
    refused_size = 0;
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_NOT_READY && lead.riding(mod, handle(chief), &who) == TSUCHI_OK && !who.id && lead.dismount(mod, handle(chief)) == TSUCHI_NOT_READY &&
          log_is("")); // no dog
    fake::put(seat + t::player_component::ridden_dog, decoy);
    CHECK(partner_dog(chief) == 0 && lead.ride(mod, handle(chief)) == TSUCHI_NOT_READY && log_is(""));
    fake::put(seat + t::player_component::ridden_dog, mutt);
    CHECK(lead.throw_pikmin(mod, handle(chief), handle(rider)) == TSUCHI_OK && log_is("pick throw ")); // its dog's follower is its own to throw
    fake::put(fake::get<uint64_t>(mutt + t::game_actor::root) + t::scene_component::location, 260.0f); // 160 away, and it reaches 150
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_NOT_READY && log_is(""));
    fake::put(fake::get<uint64_t>(mutt + t::game_actor::root) + t::scene_component::location, 180.0f);
    fake::put(fake::get<uint64_t>(mutt + t::game_actor::root) + t::scene_component::location + 8, 900.0f); // height does not count
    leading.allows = false;
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_NOT_READY && log_is("may ") && !fake::get<uint8_t>(part + a::ride_asked));
    leading.allows = true;
    CHECK(lead.ride(mod, handle(thinker)) == TSUCHI_NOT_FOUND && lead.ride(mod, gone) == TSUCHI_NOT_FOUND && lead.riding(mod, handle(thinker), &who) == TSUCHI_NOT_FOUND &&
          lead.dismount(mod, handle(thinker)) == TSUCHI_NOT_FOUND && log_is(""));
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_OK && log_is("may ride ") && fake::get<uint8_t>(part + a::ride_asked) == 1);
    fake::put<uint8_t>(part + a::ride_asked, 0); // the game drops the asking by itself: it is forgotten here too
    tsuchi::frame(0.03f);
    fake::put<uint8_t>(part + a::ride_asked, 1), fake::put(grip + t::stick_component::target, mutt); // later the game seats it on its own account
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(part + a::ride_asked) == 1);
    fake::put<uint8_t>(part + a::ride_asked, 0), fake::put<uint64_t>(grip + t::stick_component::target, 0);
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_OK && log_is("may ride ") && fake::get<uint8_t>(part + a::ride_asked) == 1);
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_BUSY && log_is("") && lead.riding(mod, handle(chief), &who) == TSUCHI_OK && !who.id);
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(part + a::ride_asked) == 1); // not on yet
    fake::put(grip + t::stick_component::target, mutt);
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(part + a::ride_asked) == 0 && lead.riding(mod, handle(chief), &who) == TSUCHI_OK && from_handle(who.id) == mutt);
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_BUSY && log_is(""));
    fake::put<uint8_t>(part + a::ride_asked, 1); // the game's own asking, which nothing here settles each frame
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(part + a::ride_asked) == 1);
    CHECK(lead.dismount(mod, handle(chief)) == TSUCHI_OK && log_is("off ") && leading.got_off == mutt_seat && fake::get<uint8_t>(part + a::ride_asked) == 0);
    fake::put<uint64_t>(mutt + t::game_actor::player, 0);
    CHECK(lead.dismount(mod, handle(chief)) == TSUCHI_NOT_READY && log_is(""));
    fake::put(mutt + t::game_actor::player, fake::object(u"Seat", w.component_class, 0x60));
    fake::destroy(fake::get<uint64_t>(mutt + t::game_actor::player));
    CHECK(lead.dismount(mod, handle(chief)) == TSUCHI_NOT_READY && log_is(""));
    fake::put(mutt + t::game_actor::player, mutt_seat);
    fake::put<uint64_t>(grip + t::stick_component::target, 0);
    CHECK(lead.dismount(mod, handle(chief)) == TSUCHI_NOT_READY && lead.riding(mod, handle(chief), &who) == TSUCHI_OK && !who.id && log_is(""));
    t::fn::request_ride = nullptr;
    CHECK(lead.ride(mod, handle(chief)) == TSUCHI_NOT_READY && log_is(""));
    t::fn::request_ride = game_request_ride;
    // A captain asked to ride that leaves play is forgotten.
    const uint64_t fourth = captain(u"Fourth", 180, 0);
    fake::put(fake::get<uint64_t>(fourth + t::game_actor::player) + t::player_component::ridden_dog, mutt);
    fake::put(fake::get<uint64_t>(mutt + t::game_actor::root) + t::scene_component::location + 4, 0.0f);
    CHECK(lead.ride(mod, handle(fourth)) == TSUCHI_OK && log_is("may ride "));
    pool_return(kPool, fourth);
    fake::put(fake::get<uint64_t>(fourth + t::game_actor::stick) + t::stick_component::target, mutt);
    tsuchi::frame(0.03f);
    CHECK(fake::get<uint8_t>(fake::get<uint64_t>(fourth + t::game_actor::ai) + a::ride_asked) == 1);
    CHECK(tsuchi::owned_count(mod) == owned);
    w.chief = chief, w.mutt = mutt, w.thrown_red = red, w.thrown_blue = blue;
    leading.picked = 0;
}

// ---- Pikmin and squads ------------------------------------------------------------------

struct Herding {
    std::string log;
    uint64_t dismissed = 0, dog_settings = 0;
    uint8_t keep_type = 0, sound = 0, motion = 0;
    int32_t keep_special = -1;
    uint16_t filter = 9;
} herd;
void game_eject_pikmin(void* leader, void* transform, int32_t type, int32_t leaf, int32_t count, uint64_t sublevel, bool not_counted, int32_t decoration) {
    const auto* f = static_cast<const float*>(transform);
    char line[256];
    std::snprintf(line, sizeof line, "eject type%d leaf%d x%d part%llx%s deco%d turn%g,%g,%g,%g at%g,%g,%g scale%g,%g,%g ", type, leaf, count,
                  static_cast<unsigned long long>(sublevel), not_counted ? " apart" : "", decoration, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[8], f[9], f[10]);
    herd.log += line;
    herd.dismissed = reinterpret_cast<uint64_t>(leader);
    if (reinterpret_cast<uint64_t>(transform) % 16) herd.log += "crooked ";
}
void game_pikmin_join(void* pikmin, void* leader, uint32_t a, uint32_t b, uint32_t now) {
    herd.log += "join" + std::to_string(a) + std::to_string(b) + std::to_string(now) + " ";
    leading.leaders[reinterpret_cast<uint64_t>(pikmin)] = reinterpret_cast<uint64_t>(leader);
}
void game_pikmin_free(void* pikmin) {
    herd.log += "free ";
    leading.leaders[reinterpret_cast<uint64_t>(pikmin)] = 0;
}
void read_order(void* order) {
    namespace o = t::dismiss_order;
    const auto at = reinterpret_cast<uint64_t>(order);
    herd.dismissed = from_weak_value(fake::get<uint64_t>(at + o::leader));
    herd.keep_type = fake::get<uint8_t>(at + o::keep_type), herd.keep_special = fake::get<int32_t>(at + o::keep_special);
    herd.sound = fake::get<uint8_t>(at + o::sound), herd.motion = fake::get<uint8_t>(at + o::motion), herd.filter = fake::get<uint16_t>(at + o::filter);
}
void game_dismiss_squad(void* order) {
    herd.log += "dismiss ";
    read_order(order);
}
void game_dismiss_dog_squad(void* settings, void* order) {
    herd.log += "dismiss-dog ";
    herd.dog_settings = reinterpret_cast<uint64_t>(settings);
    read_order(order);
}
// The game's ring of Pikmin, through a link that is part of the object that runs the game.
void ring_pikmin(std::initializer_list<uint64_t> actors) {
    const uint64_t head = w.framework + t::game_framework::pikmin_list;
    uint64_t last = head;
    for (uint64_t each : actors) {
        const uint64_t node = fake::memory(t::actor_node::size);
        fake::put(node + t::actor_node::actor, each);
        fake::put(last + t::actor_node::next, node);
        last = node;
    }
    fake::put(last + t::actor_node::next, head);
}

struct Ordered {
    uint64_t pikmin = 0, sender = 0, leader = 0, target = 0, table = 0, self = 0, other = 0, order = 0;
    float place[3] = {};
    uint8_t flag = 9, ready = 0;
    uint32_t references = 0;
    bool takes = true;
    int made = 0, posted = 0;
} ordered;
// The check world's carryable object's carry component: a work order on its actor makes the Pikmin a carrier,
// as the game registers one when it starts carrying.
uint64_t check_carry_part = 0, check_carry_actor = 0;
void game_charge_order(void* order, void* sender) {
    std::memset(order, 0xEE, t::charge_order::size); // the game fills in what it wants; the rest is not zero
    fake::put(reinterpret_cast<uint64_t>(order) + 8, sender);
    ++ordered.made;
}
// The game's test of a carried target, which the charge stores in its order's flag; asked with true.
uint64_t carry_tested = 0, carry_full = 0;
bool carry_asked = false;
bool game_charge_carry_test(void* target, bool asked) {
    carry_tested = reinterpret_cast<uint64_t>(target), carry_asked = asked;
    return carry_tested == carry_full;
}
bool game_post_order(void* pikmin, void* memory) {
    namespace c = t::charge_order;
    const auto order = reinterpret_cast<uint64_t>(memory);
    ordered.pikmin = reinterpret_cast<uint64_t>(pikmin), ordered.sender = fake::get<uint64_t>(order + 8);
    ordered.leader = fake::get<uint64_t>(order + c::leader), ordered.target = fake::get<uint64_t>(order + c::target);
    for (int i = 0; i < 3; ++i) ordered.place[i] = fake::get<float>(order + c::place + 4 * i);
    ordered.flag = fake::get<uint8_t>(order + c::flag);
    // What an order the framework makes itself must hold, as the game's constructor writes it.
    ordered.order = order, ordered.table = fake::get<uint64_t>(order), ordered.self = fake::get<uint64_t>(order + t::message::self);
    ordered.other = fake::get<uint64_t>(order + t::message::other), ordered.references = fake::get<uint32_t>(order + t::message::references);
    ordered.ready = fake::get<uint8_t>(order + t::message::ready);
    ++ordered.posted;
    if (check_carry_actor && from_weak_value(ordered.target) == check_carry_actor) {
        // As the game does, a carrier that left play is no longer registered.
        const uint64_t at = check_carry_part + t::carry_component::carriers, data = fake::get<uint64_t>(at + t::array::data);
        int32_t count = 0;
        for (int32_t i = 0; i < fake::get<int32_t>(at + t::array::count); ++i) {
            const uint64_t carrier = fake::get<uint64_t>(data + uint64_t(i) * 8);
            if (tsuchi::game::live(carrier)) fake::put(data + uint64_t(count++) * 8, carrier);
        }
        if (count < fake::get<int32_t>(at + t::array::max)) fake::put(data + uint64_t(count++) * 8, ordered.pikmin);
        fake::put<int32_t>(at + t::array::count, count);
    }
    fake::engine_free(nullptr, memory); // taken or not, the game frees it in the end
    return ordered.takes;
}

void herding() {
    const TSUCHI_PikminService& flock = *tsuchi::host.pikmin;
    // Registered types include active content, not inactive saved allocations or live actors.
    uint32_t types=0;
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==9);
    auto* provider=tsuchi::mod_create("test.type-count"); CHECK(provider);
    CHECK(tsuchi::id_add(tsuchi::pikmin::PikminType_vocabulary,provider,"orange",8)==TSUCHI_OK);
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==10);
    CHECK(tsuchi::id_add(tsuchi::pikmin::PikminType_vocabulary,provider,"brown",10)==TSUCHI_OK);
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==11);
    CHECK(tsuchi::id_remove(tsuchi::pikmin::PikminType_vocabulary,provider,"orange")==TSUCHI_OK);
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==10);
    uint32_t reserved=0;
    CHECK(tsuchi::id_allocate(tsuchi::pikmin::PikminType_vocabulary,provider,"reserved",11,12,&reserved)==TSUCHI_OK);
    CHECK(tsuchi::id_remove(tsuchi::pikmin::PikminType_vocabulary,provider,"reserved")==TSUCHI_OK);
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==10);
    tsuchi::mod_stop(provider);
    CHECK(flock.type_count(mod,&types)==TSUCHI_OK && types==9);
    const TSUCHI_SquadService& band = *tsuchi::host.squad;
    const TSUCHI_CombatService& fight = *tsuchi::host.combat;
    const uint64_t main = fake::main_image(), chief = w.chief, mutt = w.mutt;
    TSUCHI_Actor found[8]{}, who{};
    TSUCHI_PikminInfo info{};
    uint64_t count = 99;
    const auto log_is = [](const char* expected) {
        const bool same = herd.log == expected;
        if (!same) std::printf("  log: [%s]\n", herd.log.c_str());
        herd.log.clear();
        return same;
    };
    const auto pikmin = [&](const std::u16string& name, uint8_t type, uint8_t leaf, uint64_t leader) {
        const uint64_t made = actor(name, w.actor_class, u"Pikmin", 3, 0, component(0, 0, 0, 0)), part = fake::object(u"PikminPart", w.component_class, 0x40);
        fake::put(made + t::game_actor::pikmin, part);
        fake::put<uint8_t>(made + t::game_actor::color, type), fake::put<uint8_t>(part + t::pikmin_component::leaf, leaf);
        leading.leaders[made] = leader;
        return made;
    };
    t::fn::eject_pikmin = game_eject_pikmin, t::fn::pikmin_join = game_pikmin_join, t::fn::pikmin_free = game_pikmin_free;
    t::fn::dismiss_squad = game_dismiss_squad, t::fn::dismiss_dog_squad = game_dismiss_dog_squad;
    fake::put<uint8_t>(mutt + t::game_actor::sort, uint8_t(t::constant::sort_dog | 1)); // a dog is marked as one
    const uint64_t mutt_seat = fake::get<uint64_t>(mutt + t::game_actor::player);

    // The game's list of Pikmin holds the dying, and here also what is no Pikmin and what is gone.
    const uint64_t sprig = pikmin(u"Sprig", 0, 0, chief), bud = pikmin(u"Bud", 1, 1, mutt), dying = pikmin(u"Dying", 2, 0, chief), loner = pikmin(u"Loner", 9, 2, 0);
    const uint64_t queued = pikmin(u"Queued", 3, 0, chief), ghost = pikmin(u"Ghost", 0, 0, chief);
    const TSUCHI_Actor gone = handle(ghost);
    fake::put<uint8_t>(dying + t::game_actor::removed, 1), fake::put<uint8_t>(queued + t::game_actor::kill_queued, 1);
    fake::put<uint8_t>(fake::get<uint64_t>(bud + t::game_actor::pikmin) + t::pikmin_component::in_army, 5);
    fake::destroy(ghost);
    CHECK(flock.list(mod, found, 8, &count) == TSUCHI_OK && count == 0); // an empty ring
    ring_pikmin({sprig, w.rock, dying, bud, ghost, 0, queued, loner});
    CHECK(flock.list(mod, found, 8, &count) == TSUCHI_OK && count == 3 && from_handle(found[0].id) == sprig && from_handle(found[1].id) == bud &&
          from_handle(found[2].id) == loner);
    found[1] = {};
    CHECK(flock.list(mod, found, 2, &count) == TSUCHI_CAPACITY && count == 3 && found[1].id == 0); // whole or not at all
    fake::put<uint64_t>(main + t::data::game_framework, 0);
    CHECK(flock.list(mod, found, 8, &count) == TSUCHI_NOT_READY && band.members(mod, handle(chief), found, 8, &count) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(main + t::data::game_framework, w.framework);

    // What a Pikmin is.
    const auto is_type = [&](const char* id) { return std::string(info.kind) == id; };
    CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type(TSUCHI_PIKMIN_TYPE_RED) && is_type("red") && info.leaf == TSUCHI_LEAF_LEAF && info.team == 3 && !info.in_army);
    CHECK(flock.info(mod, handle(bud), &info) == TSUCHI_OK && is_type(TSUCHI_PIKMIN_TYPE_BLUE) && info.leaf == TSUCHI_LEAF_BUD && info.in_army);
    CHECK(flock.info(mod, handle(loner), &info) == TSUCHI_OK && is_type(TSUCHI_PIKMIN_TYPE_GLOW) && info.leaf == TSUCHI_LEAF_FLOWER);
    CHECK(flock.info(mod, handle(w.rock), &info) == TSUCHI_NOT_FOUND && flock.info(mod, handle(dying), &info) == TSUCHI_NOT_FOUND &&
          flock.info(mod, handle(queued), &info) == TSUCHI_NOT_FOUND && flock.info(mod, gone, &info) == TSUCHI_NOT_FOUND && flock.info(mod, handle(chief), &info) == TSUCHI_NOT_FOUND);
    // A type the framework has no name for is still told: by its number's placeholder, which stays the same and
    // tells it from every other. A leaf with no name is the game's number as it is. Neither is a failure.
    info.team = 77;
    CHECK(flock.info(mod, handle(w.rock), &info) == TSUCHI_NOT_FOUND && info.team == 77 && is_type(TSUCHI_PIKMIN_TYPE_GLOW)); // a failed call changes nothing
    fake::put<uint8_t>(sprig + t::game_actor::color, 8);
    CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type("#8") && info.leaf == TSUCHI_LEAF_LEAF && info.team == 3);
    fake::put<uint8_t>(sprig + t::game_actor::color, 255);
    CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type("#255"));
    fake::put<uint8_t>(sprig + t::game_actor::color, 7);
    CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type(TSUCHI_PIKMIN_TYPE_ICE));
    fake::put<uint8_t>(sprig + t::game_actor::color, 0), fake::put<uint8_t>(fake::get<uint64_t>(sprig + t::game_actor::pikmin) + t::pikmin_component::leaf, 3);
    CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type("red") && info.leaf == 3);
    fake::put<uint8_t>(fake::get<uint64_t>(sprig + t::game_actor::pikmin) + t::pikmin_component::leaf, 0);

    // New Pikmin, as an Onion gives them out: beside the leader, in its part of the level, counted.
    fake::put<uint64_t>(chief + t::game_actor::sublevel, 0xABCD);
    CHECK(flock.add(mod, handle(chief), TSUCHI_PIKMIN_TYPE_ICE, TSUCHI_LEAF_FLOWER, 5) == TSUCHI_OK &&
          log_is("eject type7 leaf2 x5 partabcd deco17 turn0,0,0,1 at100,200,0 scale1,1,1 ") && herd.dismissed == chief);
    CHECK(flock.add(mod, handle(mutt), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_OK && log_is("eject type0 leaf0 x1 part0 deco17 turn0,0,0,1 at180,0,900 scale1,1,1 ") &&
          herd.dismissed == mutt);
    CHECK(flock.add(mod, handle(chief), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 0) == TSUCHI_INVALID && flock.add(mod, handle(w.rock), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND &&
          flock.add(mod, handle(sprig), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && flock.add(mod, gone, TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && log_is(""));
    // A type is named by its ID. The game's own are matched as the engine matches names, whatever the case of the letters.
    CHECK(flock.add(mod, handle(chief), "Glow", TSUCHI_LEAF_LEAF, 2) == TSUCHI_OK && log_is("eject type9 leaf0 x2 partabcd deco17 turn0,0,0,1 at100,200,0 scale1,1,1 "));
    // One rule for an ID that is not known: text that is no ID is invalid, and an ID that names no type is not found.
    CHECK(flock.add(mod, handle(chief), "no such", TSUCHI_LEAF_LEAF, 1) == TSUCHI_INVALID && flock.add(mod, handle(chief), "", TSUCHI_LEAF_LEAF, 1) == TSUCHI_INVALID &&
          flock.add(mod, handle(chief), nullptr, TSUCHI_LEAF_LEAF, 1) == TSUCHI_INVALID && flock.add(mod, handle(chief), "example.bulbmin:Bulbmin", TSUCHI_LEAF_LEAF, 1) == TSUCHI_INVALID);
    CHECK(flock.add(mod, handle(chief), "plaid", TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && flock.add(mod, handle(chief), "example.bulbmin:bulbmin", TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND &&
          flock.add(mod, handle(chief), "#256", TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && log_is(""));
    // A type nobody has named is a type, and its placeholder is taken; the game is not asked for Pikmin nobody vouches for.
    CHECK(flock.add(mod, handle(chief), "#8", TSUCHI_LEAF_LEAF, 1) == TSUCHI_UNSUPPORTED && log_is(""));
    // A placeholder for a number that has a name is that type.
    CHECK(flock.add(mod, handle(chief), "#1", TSUCHI_LEAF_BUD, 1) == TSUCHI_OK && log_is("eject type1 leaf1 x1 partabcd deco17 turn0,0,0,1 at100,200,0 scale1,1,1 "));
    {
        // What a mod adds: the service that adds a type gives it a number and names it, and from then on the type is
        // told and taken by its ID, for as long as the mod runs.
        TSUCHI_Mod* maker = tsuchi::mod_create("example.bulbmin");
        CHECK(tsuchi::id_add(tsuchi::pikmin::PikminType_vocabulary, maker, "bulbmin", 12) == TSUCHI_OK);
        CHECK(flock.add(mod, handle(chief), "example.bulbmin:bulbmin", TSUCHI_LEAF_LEAF, 1) == TSUCHI_OK &&
              log_is("eject type12 leaf0 x1 partabcd deco17 turn0,0,0,1 at100,200,0 scale1,1,1 "));
        fake::put<uint8_t>(sprig + t::game_actor::color, 12);
        CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type("example.bulbmin:bulbmin"));
        tsuchi::mod_stop(maker);
        CHECK(flock.info(mod, handle(sprig), &info) == TSUCHI_OK && is_type("#12"));
        CHECK(flock.add(mod, handle(chief), "example.bulbmin:bulbmin", TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && log_is(""));
        fake::put<uint8_t>(sprig + t::game_actor::color, 0);
    }
    const uint64_t chief_root = fake::get<uint64_t>(chief + t::game_actor::root);
    fake::put<uint64_t>(chief + t::game_actor::root, 0);
    CHECK(flock.add(mod, handle(chief), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_READY && log_is(""));
    fake::put(chief + t::game_actor::root, chief_root);
    t::fn::eject_pikmin = nullptr;
    CHECK(flock.add(mod, handle(chief), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_READY);
    t::fn::eject_pikmin = game_eject_pikmin;
    // A dog is one that is marked as a dog and has the part by which it is steered.
    fake::put<uint8_t>(mutt + t::game_actor::sort, 1);
    CHECK(flock.add(mod, handle(mutt), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND && !is_dog(mutt) && !is_leader(mutt) && is_leader(chief) && !is_dog(chief));
    fake::put<uint8_t>(mutt + t::game_actor::sort, uint8_t(t::constant::sort_dog));
    fake::put(mutt + t::game_actor::player, fake::object(u"Seat", w.component_class, 0x60));
    fake::destroy(fake::get<uint64_t>(mutt + t::game_actor::player));
    CHECK(!is_dog(mutt) && flock.add(mod, handle(mutt), TSUCHI_PIKMIN_TYPE_RED, TSUCHI_LEAF_LEAF, 1) == TSUCHI_NOT_FOUND);
    fake::put(mutt + t::game_actor::player, mutt_seat);
    const uint64_t decoy = fake::object(u"Decoy", w.component_class, 0x100); // no actor, marked as a dog
    fake::put<uint8_t>(decoy + t::game_actor::sort, uint8_t(t::constant::sort_dog)), fake::put(decoy + t::game_actor::player, mutt_seat);
    CHECK(is_dog(mutt) && !is_dog(decoy) && !is_leader(decoy) && !is_leader(0) && log_is(""));

    // Whom a Pikmin follows, and who follows a leader.
    CHECK(band.leader(mod, handle(sprig), &who) == TSUCHI_OK && from_handle(who.id) == chief && band.leader(mod, handle(bud), &who) == TSUCHI_OK && from_handle(who.id) == mutt);
    CHECK(band.leader(mod, handle(loner), &who) == TSUCHI_OK && !who.id && band.leader(mod, handle(w.rock), &who) == TSUCHI_NOT_FOUND && band.leader(mod, handle(dying), &who) == TSUCHI_NOT_FOUND &&
          band.leader(mod, gone, &who) == TSUCHI_NOT_FOUND);
    CHECK(band.members(mod, handle(chief), found, 8, &count) == TSUCHI_OK && count == 1 && from_handle(found[0].id) == sprig); // the dying are no members
    CHECK(band.members(mod, handle(mutt), found, 8, &count) == TSUCHI_OK && count == 1 && from_handle(found[0].id) == bud);
    CHECK(band.members(mod, handle(w.rock), found, 8, &count) == TSUCHI_NOT_FOUND && band.members(mod, handle(sprig), found, 8, &count) == TSUCHI_NOT_FOUND &&
          band.members(mod, gone, found, 8, &count) == TSUCHI_NOT_FOUND);
    t::fn::pikmin_leader = nullptr;
    CHECK(band.leader(mod, handle(sprig), &who) == TSUCHI_NOT_READY && band.members(mod, handle(chief), found, 8, &count) == TSUCHI_NOT_READY);
    t::fn::pikmin_leader = game_pikmin_leader;

    // Joining and leaving: the game's own, at once.
    CHECK(band.join(mod, handle(loner), handle(chief)) == TSUCHI_OK && log_is("join001 ") && band.leader(mod, handle(loner), &who) == TSUCHI_OK && from_handle(who.id) == chief);
    CHECK(band.members(mod, handle(chief), found, 8, &count) == TSUCHI_OK && count == 2 && from_handle(found[0].id) == sprig && from_handle(found[1].id) == loner);
    CHECK(band.members(mod, handle(chief), found, 1, &count) == TSUCHI_CAPACITY && count == 2);
    CHECK(band.join(mod, handle(sprig), handle(mutt)) == TSUCHI_OK && log_is("join001 ") && band.members(mod, handle(mutt), found, 8, &count) == TSUCHI_OK && count == 2);
    CHECK(band.join(mod, handle(w.rock), handle(chief)) == TSUCHI_NOT_FOUND && band.join(mod, handle(loner), handle(w.rock)) == TSUCHI_NOT_FOUND &&
          band.join(mod, handle(loner), handle(sprig)) == TSUCHI_NOT_FOUND && band.join(mod, handle(dying), handle(chief)) == TSUCHI_NOT_FOUND &&
          band.join(mod, gone, handle(chief)) == TSUCHI_NOT_FOUND && band.join(mod, handle(loner), gone) == TSUCHI_NOT_FOUND && log_is(""));
    t::fn::pikmin_join = nullptr;
    CHECK(band.join(mod, handle(loner), handle(chief)) == TSUCHI_NOT_READY);
    t::fn::pikmin_join = game_pikmin_join;
    CHECK(band.leave(mod, handle(loner)) == TSUCHI_OK && log_is("free ") && band.leader(mod, handle(loner), &who) == TSUCHI_OK && !who.id);
    CHECK(band.leave(mod, handle(w.rock)) == TSUCHI_NOT_FOUND && band.leave(mod, handle(dying)) == TSUCHI_NOT_FOUND && band.leave(mod, gone) == TSUCHI_NOT_FOUND && log_is(""));
    t::fn::pikmin_free = nullptr;
    CHECK(band.leave(mod, handle(sprig)) == TSUCHI_NOT_READY);
    t::fn::pikmin_free = game_pikmin_free;

    // Work: the order a captain's charge gives one Pikmin, in its leader's name, or in its own when it follows nobody.
    {
        const auto old_post = t::fn::post_message;
        t::fn::post_message = game_post_order, t::fn::charge_order = game_charge_order;
        Transform there{};
        get_transform(w.rock, there);
        // The rock has physics: the order goes where its physics put it, here where its root is.
        for (int i = 0; i < 3; ++i) fake::put<float>(w.rock + t::game_actor::physics_place + 4 * i, there.location[i]);
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_OK && ordered.made == 1 && ordered.posted == 1 && ordered.pikmin == sprig && ordered.sender == mutt &&
              from_weak_value(ordered.leader) == mutt && from_weak_value(ordered.target) == w.rock && ordered.place[0] == there.location[0] &&
              ordered.place[1] == there.location[1] && ordered.place[2] == there.location[2] && ordered.flag == 0);
        CHECK(flock.work_on(mod, handle(loner), handle(w.rock)) == TSUCHI_OK && ordered.pikmin == loner && ordered.sender == loner && ordered.leader == 0xFFFFFFFFull);
        ordered.takes = false; // it would not take it: asleep, or paused
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_NOT_READY && ordered.posted == 3);
        ordered.takes = true;
        const uint64_t nowhere = actor(u"Nowhere", w.actor_class, u"Nowhere", 3, 0, 0);
        CHECK(flock.work_on(mod, handle(w.rock), handle(w.rock)) == TSUCHI_NOT_FOUND && flock.work_on(mod, handle(dying), handle(w.rock)) == TSUCHI_NOT_FOUND &&
              flock.work_on(mod, handle(sprig), gone) == TSUCHI_NOT_FOUND && flock.work_on(mod, handle(sprig), TSUCHI_Actor{to_handle(fake::get<uint64_t>(w.rock + t::game_actor::root))}) == TSUCHI_NOT_FOUND &&
              flock.work_on(mod, handle(sprig), handle(nowhere)) == TSUCHI_NOT_READY && ordered.made == 3 && ordered.posted == 3);
        fake::engine().out_of_memory = true;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_NO_MEMORY && ordered.made == 3);
        fake::engine().out_of_memory = false;
        // A build that inlines the constructor: the framework makes the order from the table and the message's words.
        // The flag is the game's test of the target, as the charge asks it.
        t::fn::charge_carry_test = game_charge_carry_test;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_OK && ordered.flag == 0 && carry_tested == w.rock && carry_asked);
        carry_full = w.rock;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_OK && ordered.flag == 1 && ordered.made == 5 && ordered.posted == 5);
        t::fn::charge_carry_test = nullptr, carry_full = 0;
        t::fn::charge_order = nullptr;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_OK && ordered.made == 5 && ordered.posted == 6 &&
              ordered.table == fake::main_image() + t::data::charge_order_table && ordered.self == ordered.order &&
              from_weak_value(ordered.sender) == mutt && ordered.other == 0xFFFFFFFFull && ordered.references == 1 &&
              ordered.ready == 1 && from_weak_value(ordered.leader) == mutt && from_weak_value(ordered.target) == w.rock &&
              ordered.flag == 0);
        // And one with neither cannot send a Pikmin to work.
        const uint64_t table_was = t::data::charge_order_table;
        t::cells[t::data::charge_order_table.index] = t::absent_value;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_UNSUPPORTED && ordered.posted == 6);
        t::cells[t::data::charge_order_table.index] = table_was;
        // Nor one without the order's own layout, whichever way the order would be made.
        const uint64_t size_was = t::charge_order::size;
        t::cells[t::charge_order::size.index] = t::absent_value;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_UNSUPPORTED && ordered.posted == 6);
        t::fn::charge_order = game_charge_order;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_UNSUPPORTED && ordered.made == 5 && ordered.posted == 6);
        t::cells[t::charge_order::size.index] = size_was;
        // A target with physics is sent to where its physics put it, as the game's charge sends Pikmin.
        const uint64_t physics_was = fake::get<uint64_t>(w.rock + t::game_actor::physics);
        fake::put<uint64_t>(w.rock + t::game_actor::physics, fake::memory(0x10));
        for (int i = 0; i < 3; ++i) fake::put<float>(w.rock + t::game_actor::physics_place + 4 * i, 100.0f + float(i));
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_OK && ordered.posted == 7 && ordered.place[0] == 100.0f &&
              ordered.place[1] == 101.0f && ordered.place[2] == 102.0f);
        fake::put<uint64_t>(w.rock + t::game_actor::physics, physics_was);
        t::fn::post_message = nullptr;
        CHECK(flock.work_on(mod, handle(sprig), handle(w.rock)) == TSUCHI_NOT_READY && ordered.made == 6 && ordered.posted == 7);
        t::fn::post_message = old_post;
    }

    // Dismissing: the game's order, naming the leader, with nobody kept back; a dog's goes through the dog.
    CHECK(band.dismiss(mod, handle(chief)) == TSUCHI_OK && log_is("dismiss ") && herd.dismissed == chief && herd.keep_type == t::constant::dismiss_keep_none &&
          herd.keep_special == 0 && herd.sound == 1 && herd.motion == 1 && herd.filter == 0);
    herd.keep_type = 0, herd.sound = herd.motion = 0, herd.keep_special = -1, herd.filter = 9;
    CHECK(band.dismiss(mod, handle(mutt)) == TSUCHI_OK && log_is("dismiss-dog ") && herd.dismissed == mutt && herd.dog_settings == mutt_seat + t::dog_component::dismiss_settings &&
          herd.keep_type == t::constant::dismiss_keep_none && herd.keep_special == 0 && herd.sound == 1 && herd.motion == 1 && herd.filter == 0);
    CHECK(band.dismiss(mod, handle(w.rock)) == TSUCHI_NOT_FOUND && band.dismiss(mod, handle(sprig)) == TSUCHI_NOT_FOUND && band.dismiss(mod, gone) == TSUCHI_NOT_FOUND && log_is(""));
    t::fn::dismiss_squad = nullptr, t::fn::dismiss_dog_squad = nullptr;
    CHECK(band.dismiss(mod, handle(chief)) == TSUCHI_NOT_READY && band.dismiss(mod, handle(mutt)) == TSUCHI_NOT_READY);
    t::fn::dismiss_squad = game_dismiss_squad, t::fn::dismiss_dog_squad = game_dismiss_dog_squad;
    CHECK(to_weak_value(0) == 0xFFFFFFFFull && to_weak_value(ghost) == 0xFFFFFFFFull && from_weak_value(to_weak_value(chief)) == chief);

    // A Pikmin killed the game's way: loudly, of a cause, by someone.
    const int kills = spawner.kills;
    CHECK(fight.kill_pikmin(mod, handle(w.rock), TSUCHI_DEATH_CAUSE_FIRE, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && fight.kill_pikmin(mod, handle(dying), TSUCHI_DEATH_CAUSE_FIRE, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND &&
          fight.kill_pikmin(mod, gone, TSUCHI_DEATH_CAUSE_FIRE, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && fight.kill_pikmin(mod, handle(bud), TSUCHI_DEATH_CAUSE_FIRE, gone) == TSUCHI_NOT_FOUND &&
          spawner.kills == kills);
    CHECK(fight.kill_pikmin(mod, handle(bud), TSUCHI_DEATH_CAUSE_FIRE, handle(chief)) == TSUCHI_OK && spawner.kills == kills + 1 && spawner.last_quiet == 0 &&
          spawner.last_cause == TSUCHI_DEATH_CAUSE_FIRE && spawner.last_killer == chief);
    CHECK(fight.kill_pikmin(mod, handle(bud), TSUCHI_DEATH_CAUSE_FIRE, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND && spawner.kills == kills + 1); // on its way out
    const uint64_t last = pikmin(u"Last", 4, 0, 0);
    CHECK(fight.kill_pikmin(mod, handle(last), TSUCHI_DEATH_CAUSE_OTHER, TSUCHI_Actor{}) == TSUCHI_OK && spawner.kills == kills + 2 && spawner.last_quiet == 0 &&
          spawner.last_cause == 0 && spawner.last_killer == 0);
    spawner.queued.clear();
    w.sprig = sprig, w.loner = loner;
}

// ---- Creatures --------------------------------------------------------------------------

struct Revived {
    uint64_t creature = 0, sender = 0, held = 0, table = 0, self = 0, request = 0;
    int made = 0, posted = 0;
    bool takes = true;
} revived;
void game_revive_request(void* request, void* sender) {
    std::memset(request, 0xEE, t::revive_request::size);
    fake::put(reinterpret_cast<uint64_t>(request) + 8, sender);
    ++revived.made;
}
bool game_is_held_or_carried(void* actor) { return reinterpret_cast<uint64_t>(actor) == revived.held; }
bool game_post_revive(void* creature, void* request) {
    const uint64_t at = reinterpret_cast<uint64_t>(request);
    revived.creature = reinterpret_cast<uint64_t>(creature), revived.sender = fake::get<uint64_t>(at + 8), revived.request = at;
    revived.table = fake::get<uint64_t>(at), revived.self = fake::get<uint64_t>(at + t::message::self);
    if (revived.table == fake::main_image() + t::data::revive_request_table) revived.sender = fake::get<uint64_t>(at + t::message::sender);
    ++revived.posted;
    fake::engine_free(nullptr, request); // taken or not, the game frees it in the end
    return revived.takes;
}

void roaming() {
    const TSUCHI_CreatureService& wild = *tsuchi::host.creature;
    const uint64_t main = fake::main_image();
    static TSUCHI_Actor found[512];
    bool down = true;
    uint64_t count = 99;
    const auto teki = fake::name(u"Teki"), gimmick = fake::name(u"Gimmick"); // each made once: a name is its place in the game's table
    const auto grouped = [](const std::u16string& name, decltype(teki) group) {
        const uint64_t made = actor(name, w.actor_class, name, 3, 0, component(0, 0, 0, 0));
        fake::put(made + t::game_actor::group, group);
        return made;
    };
    // Until the game has named the group of its enemies, nothing is a creature: not even what has no group.
    CHECK(wild.list(mod, found, 512, &count) == TSUCHI_OK && count == 0 && wild.defeated(mod, handle(w.rock), &down) == TSUCHI_NOT_FOUND);
    fake::put(main + t::data::creature_group, teki);
    CHECK(wild.list(mod, found, 512, &count) == TSUCHI_OK && count == 0);

    const uint64_t beast = grouped(u"Beast", teki), prop = grouped(u"Prop", gimmick), corpse = grouped(u"Corpse", teki), sleeper = grouped(u"Sleeper", teki);
    const uint64_t numbered = grouped(u"Numbered", teki), ghost = grouped(u"Ghost", teki);
    fake::put<uint32_t>(numbered + t::game_actor::group + 4, 3); // the same name with a number is another name
    const TSUCHI_Actor gone = handle(ghost);
    fake::destroy(ghost);
    fake::put<uint8_t>(corpse + t::game_actor::removed, 7); // any nonzero
    CHECK(wild.list(mod, found, 512, &count) == TSUCHI_OK && count == 3 && from_handle(found[0].id) == beast && from_handle(found[1].id) == corpse &&
          from_handle(found[2].id) == sleeper);
    found[1] = {};
    CHECK(wild.list(mod, found, 2, &count) == TSUCHI_CAPACITY && count == 3 && found[1].id == 0);
    fake::put<uint64_t>(main + t::data::game_framework, 0);
    CHECK(wild.list(mod, found, 512, &count) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(main + t::data::game_framework, w.framework);

    CHECK(wild.defeated(mod, handle(beast), &down) == TSUCHI_OK && !down && wild.defeated(mod, handle(corpse), &down) == TSUCHI_OK && down);
    CHECK(wild.defeated(mod, handle(sleeper), &down) == TSUCHI_OK && !down);
    // A carcass: the game's own mark, among others it keeps beside it.
    bool lying = true;
    fake::put<uint32_t>(beast + t::game_actor::state_flags, 0x100 | 0x1), fake::put<uint32_t>(corpse + t::game_actor::state_flags, t::constant::state_carcass | 0x100);
    CHECK(wild.carcass(mod, handle(beast), &lying) == TSUCHI_OK && !lying && wild.carcass(mod, handle(corpse), &lying) == TSUCHI_OK && lying);
    CHECK(wild.carcass(mod, handle(prop), &lying) == TSUCHI_NOT_FOUND && wild.carcass(mod, handle(w.rock), &lying) == TSUCHI_NOT_FOUND && wild.carcass(mod, gone, &lying) == TSUCHI_NOT_FOUND);
    CHECK(wild.defeated(mod, handle(prop), &down) == TSUCHI_NOT_FOUND && wild.defeated(mod, handle(numbered), &down) == TSUCHI_NOT_FOUND &&
          wild.defeated(mod, handle(w.rock), &down) == TSUCHI_NOT_FOUND && wild.defeated(mod, gone, &down) == TSUCHI_NOT_FOUND);
    const uint64_t decoy = fake::object(u"Decoy", w.component_class, 0x100); // no actor, in the enemies' group
    fake::put(decoy + t::game_actor::group, teki);
    CHECK(wild.defeated(mod, handle(decoy), &down) == TSUCHI_NOT_FOUND);

    // A defeated creature brought back: the game's own request, sent by the creature to itself.
    {
        const auto old_post = t::fn::post_message;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_NOT_READY); // the game's functions for it are not known yet
        t::fn::revive_request = game_revive_request, t::fn::is_held_or_carried = game_is_held_or_carried, t::fn::post_message = game_post_revive;
        CHECK(wild.revive(mod, handle(beast)) == TSUCHI_NOT_READY && wild.revive(mod, handle(prop)) == TSUCHI_NOT_FOUND && wild.revive(mod, handle(w.rock)) == TSUCHI_NOT_FOUND &&
              wild.revive(mod, gone) == TSUCHI_NOT_FOUND && revived.made == 0); // not defeated, and no creature
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_OK && revived.made == 1 && revived.posted == 1 && revived.creature == corpse && revived.sender == corpse);
        revived.held = corpse; // being carried off
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_BUSY && revived.made == 1);
        revived.held = 0;
        fake::put<uint8_t>(corpse + t::game_actor::kill_queued, 1); // on its way out
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_BUSY && revived.made == 1);
        fake::put<uint8_t>(corpse + t::game_actor::kill_queued, 0);
        revived.takes = false; // it would not take it: dormant
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_NOT_READY && revived.posted == 2);
        revived.takes = true;
        fake::engine().out_of_memory = true;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_NO_MEMORY && revived.made == 2);
        fake::engine().out_of_memory = false;
        t::fn::is_held_or_carried = nullptr;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_NOT_READY && revived.made == 2);
        t::fn::is_held_or_carried = game_is_held_or_carried, t::fn::post_message = nullptr;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_NOT_READY && revived.made == 2);
        // A build that inlines the request: it is made from the request's table and the message's words.
        t::fn::post_message = game_post_revive, t::fn::revive_request = nullptr;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_OK && revived.made == 2 && revived.posted == 3 &&
              revived.table == fake::main_image() + t::data::revive_request_table && revived.self == revived.request &&
              from_weak_value(revived.sender) == corpse);
        // And one with neither cannot revive anything.
        const uint64_t table_was = t::data::revive_request_table;
        t::cells[t::data::revive_request_table.index] = t::absent_value;
        CHECK(wild.revive(mod, handle(corpse)) == TSUCHI_UNSUPPORTED && revived.posted == 3);
        t::cells[t::data::revive_request_table.index] = table_was;
        t::fn::revive_request = game_revive_request, t::fn::post_message = old_post;
    }
    w.beast = beast, w.corpse = corpse;
}

// ---- Assets -----------------------------------------------------------------------------

struct Shelf {
    std::map<uint64_t, uint64_t> assets; // by the name of the path
    int loads = 0;
    bool well_formed = true;
} shelf;
void* game_load_soft_object(void* reference) {
    const auto at = reinterpret_cast<uint64_t>(reference);
    ++shelf.loads;
    shelf.well_formed = shelf.well_formed && fake::get<int32_t>(at + t::soft_object::index) == -1 && at % 8 == 0;
    for (uint32_t i = 0; i < t::soft_object::size; ++i) // nothing else in it
        if ((i < t::soft_object::index || i >= t::soft_object::index + 4) && (i < t::soft_object::path || i >= t::soft_object::path + 8))
            shelf.well_formed = shelf.well_formed && fake::get<uint8_t>(at + i) == 0;
    const auto found = shelf.assets.find(fake::get<uint64_t>(at + t::soft_object::path));
    return reinterpret_cast<void*>(found == shelf.assets.end() ? 0 : found->second);
}

void stocking() {
    const TSUCHI_AssetService& store = *tsuchi::host.asset;
    const auto object = [](uint64_t address) { return TSUCHI_Object{to_handle(address)}; };
    const auto inside = [](const std::u16string& name, uint64_t outer) {
        const uint64_t made = fake::object(name, w.component_class, 0x40);
        fake::put(made + t::object::outer, outer);
        return made;
    };
    const auto shelve = [](std::u16string path, uint64_t asset) { shelf.assets[fake::engine_name(path.data(), path.size(), 1)] = asset; };
    TSUCHI_Object got{};
    char text[64] = "untouched";
    uint64_t needed = 0;
    const uint32_t owned = tsuchi::owned_count(mod);

    const uint64_t package = inside(u"/G/Idol", 0), idol = inside(u"Idol", package), lod = inside(u"Lod0", idol), section = inside(u"Sec", lod);
    const uint64_t relic = inside(u"Relic", inside(u"/G/Relic", 0));
    shelve(u"/G/Idol.Idol", idol), shelve(u"/G/Relic.Relic", relic), shelve(u"/G/Gone.Gone", section);
    t::fn::load_soft_object = game_load_soft_object;

    // A path: the package, the object in it, and what is inside that.
    CHECK(tsuchi::host.reflection->path(mod, object(section), text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "/G/Idol.Idol:Lod0.Sec") && needed == 22);
    CHECK(tsuchi::host.reflection->path(mod, object(lod), text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "/G/Idol.Idol:Lod0"));
    CHECK(tsuchi::host.reflection->path(mod, object(idol), text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "/G/Idol.Idol") && needed == 13);
    CHECK(tsuchi::host.reflection->path(mod, object(package), text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "/G/Idol") && needed == 8);
    std::snprintf(text, sizeof text, "untouched");
    CHECK(tsuchi::host.reflection->path(mod, object(idol), text, 12, &needed) == TSUCHI_CAPACITY && needed == 13 && !std::strcmp(text, "untouched"));
    CHECK(tsuchi::host.reflection->path(mod, object(idol), text, 13, &needed) == TSUCHI_OK && !std::strcmp(text, "/G/Idol.Idol"));
    fake::put(package + t::object::outer, section); // outers that lead back into themselves
    CHECK(tsuchi::host.reflection->path(mod, object(idol), text, sizeof text, &needed) == TSUCHI_NOT_FOUND);
    fake::put<uint64_t>(package + t::object::outer, 0);
    const uint64_t ghost = inside(u"Ghost", package);
    const TSUCHI_Object gone = object(ghost);
    fake::destroy(ghost);
    CHECK(tsuchi::host.reflection->path(mod, gone, text, sizeof text, &needed) == TSUCHI_NOT_FOUND);

    // Loading: what the path names, kept for the mod.
    CHECK(store.load(mod, "G/Idol.Idol", &got) == TSUCHI_INVALID && store.load(mod, "", &got) == TSUCHI_INVALID && store.load(mod, "/\xff", &got) == TSUCHI_INVALID &&
          store.load(mod, "/G/A2345678901234567.B", &got) == TSUCHI_INVALID && shelf.loads == 0); // 22 units, and the made-up game stores 21
    CHECK(store.load(mod, "/G/Nope.Nope", &got) == TSUCHI_NOT_FOUND && shelf.loads == 1 && shelf.well_formed && tsuchi::owned_count(mod) == owned);
    fake::destroy(section);
    CHECK(store.load(mod, "/G/Gone.Gone", &got) == TSUCHI_NOT_FOUND && tsuchi::owned_count(mod) == owned); // the game names something that is being destroyed
    t::fn::load_soft_object = nullptr;
    CHECK(store.load(mod, "/G/Idol.Idol", &got) == TSUCHI_NOT_READY);
    t::fn::load_soft_object = game_load_soft_object;
    refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // no memory for the first holds
    CHECK(store.load(mod, "/G/Idol.Idol", &got) == TSUCHI_NO_MEMORY && !object_kept(idol) && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    CHECK(!object_kept(idol) && store.load(mod, "/G/Idol.Idol", &got) == TSUCHI_OK && from_handle(got.id) == idol && object_kept(idol) && tsuchi::owned_count(mod) == owned + 1);
    CHECK(store.load(mod, "/G/Idol.Idol", &got) == TSUCHI_OK && from_handle(got.id) == idol && tsuchi::owned_count(mod) == owned + 1); // the same hold
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.pauper2");
    refused_multiple = 40; // no room to note whose the hold is
    CHECK(store.load(poor, "/G/Relic.Relic", &got) == TSUCHI_NO_MEMORY && !object_kept(relic) && store.release(poor, object(relic)) == TSUCHI_NOT_FOUND);
    refused_multiple = 0;
    tsuchi::mod_stop(poor);

    // Two mods hold it: the engine keeps it until the second lets go.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.collector");
    CHECK(store.release(rival, object(idol)) == TSUCHI_NOT_FOUND && store.load(rival, "/G/Idol.Idol", &got) == TSUCHI_OK && tsuchi::owned_count(rival) == 1);
    CHECK(store.release(mod, object(idol)) == TSUCHI_OK && object_kept(idol) && tsuchi::owned_count(mod) == owned && store.release(mod, object(idol)) == TSUCHI_NOT_FOUND);
    CHECK(store.release(rival, object(idol)) == TSUCHI_OK && !object_kept(idol) && tsuchi::owned_count(rival) == 0);
    // It ends with the mod.
    CHECK(store.load(rival, "/G/Idol.Idol", &got) == TSUCHI_OK && store.load(mod, "/G/Idol.Idol", &got) == TSUCHI_OK && object_kept(idol));
    tsuchi::mod_stop(rival);
    CHECK(object_kept(idol) && store.release(mod, object(idol)) == TSUCHI_OK && !object_kept(idol));
    // What the game itself keeps is left kept.
    CHECK(keep_object(relic, true) && !keep_object(relic, true) && object_kept(relic));
    CHECK(store.load(mod, "/G/Relic.Relic", &got) == TSUCHI_OK && from_handle(got.id) == relic && store.release(mod, object(relic)) == TSUCHI_OK && object_kept(relic));
    CHECK(keep_object(relic, false) && !keep_object(relic, false) && !object_kept(relic) && !keep_object(ghost, true) && !object_kept(ghost));
    // Renderer and asset holds share retention even when the asset lets go first.
    const uint64_t render_asset = fake::engine_object(u"RetainedMaterial", material_class);
    const uint64_t render_target = fake::engine_object(u"RetainedMesh", mesh_class, 0x100);
    slots_of[render_target] = 1;
    const uint64_t render_owner = tabled(table_a(), u"RetentionOwner", u"RetentionOwner", 0, 0);
    registers(render_owner);
    fake::put(render_target + t::scene_component::owner, render_owner);
    parts_of[render_owner] = {render_target};
    shelve(u"/G/R.R", render_asset);
    CHECK(store.load(mod, "/G/R.R", &got) == TSUCHI_OK);
    CHECK(tsuchi::hold_material(nullptr, render_target, 0, render_asset) == TSUCHI_OK && object_kept(render_asset));
    CHECK(store.release(mod, got) == TSUCHI_OK && object_kept(render_asset));
    CHECK(tsuchi::restore_material(nullptr, render_target, 0) == TSUCHI_OK && !object_kept(render_asset));
    // A root present before either hold remains when both holders finish.
    CHECK(keep_object(render_asset, true));
    CHECK(tsuchi::hold_material(nullptr, render_target, 0, render_asset) == TSUCHI_OK);
    CHECK(store.load(mod, "/G/R.R", &got) == TSUCHI_OK);
    CHECK(tsuchi::restore_material(nullptr, render_target, 0) == TSUCHI_OK && object_kept(render_asset));
    CHECK(store.release(mod, got) == TSUCHI_OK && object_kept(render_asset));
    CHECK(keep_object(render_asset, false));
    fake::destroy(render_target), fake::destroy(render_asset), fake::destroy(render_owner);
    parts_of.erase(render_owner);
    slots_of.erase(render_target);

    // An asset the game destroyed all the same: the hold is forgotten quietly.
    const uint64_t doomed = inside(u"Doomed", inside(u"/G/Doomed", 0));
    shelve(u"/G/Doomed.Doomed", doomed);
    CHECK(store.load(mod, "/G/Doomed.Doomed", &got) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    fake::destroy(doomed);
    CHECK(store.release(mod, got) == TSUCHI_OK && tsuchi::owned_count(mod) == owned && store.release(mod, got) == TSUCHI_NOT_FOUND);
    // Left held, such a hold ends by itself on the next frame, and the mod's record of it with it.
    const uint64_t fleeting = inside(u"Fleeting", inside(u"/G/Fleeting", 0));
    shelve(u"/G/Fleeting.Fleeting", fleeting);
    CHECK(store.load(mod, "/G/Fleeting.Fleeting", &got) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    fake::destroy(fleeting);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::owned_count(mod) == owned && store.release(mod, got) == TSUCHI_NOT_FOUND);
    w.idol_asset = idol;
}

// ---- Data tables ------------------------------------------------------------------------

struct Tabled {
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> rows; // by table and name
    bool adds = true;
    int added = 0;
} sheets;
constexpr size_t kRowBytes = 0x80;
void* game_find_table_row(void* table, uint64_t name) {
    const auto found = sheets.rows.find({reinterpret_cast<uint64_t>(table), name});
    return reinterpret_cast<void*>(found == sheets.rows.end() ? 0 : found->second);
}
void game_add_table_row(void* table, uint64_t name, void* from) {
    ++sheets.added;
    if (!sheets.adds) return;
    const uint64_t copy = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(copy), from, kRowBytes);
    sheets.rows[{reinterpret_cast<uint64_t>(table), name}] = copy;
}
void game_remove_table_row(void*, uint64_t) {}
void game_add_table_row_internal(void*, uint64_t, void*) {}
uint64_t row_name(std::u16string text) { return fake::engine_name(text.data(), text.size(), 1); }

void tabling() {
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    const auto object = [](uint64_t address) { return TSUCHI_Object{to_handle(address)}; };
    const auto number = [](double v) {
        TSUCHI_Value value{TSUCHI_VALUE_NUMBER, 0, {}};
        value.as.number = v;
        return value;
    };
    const auto integer = [](int64_t v) {
        TSUCHI_Value value{TSUCHI_VALUE_INTEGER, 0, {}};
        value.as.integer = v;
        return value;
    };
    const auto reference = [](uint64_t address) {
        TSUCHI_Value value{TSUCHI_VALUE_OBJECT, 0, {}};
        value.as.object.id = to_handle(address);
        return value;
    };
    const TSUCHI_Value nothing{TSUCHI_VALUE_NONE, 0, {}};
    TSUCHI_Value got{};
    char text[64] = "";
    uint64_t needed = 0;
    const uint32_t owned = tsuchi::owned_count(mod);
    namespace f = t::data_table_table;

    // A table: an object of the engine's table class, with a type for its rows and the engine's own functions for them.
    const uint64_t table_class = fake::object(u"DataTable", w.root), functions = fake::memory(0x40);
    const uint64_t vector = fake::memory(0x80), row_type = fake::engine_object(u"CreatureRow", w.root);
    fake::put<uint64_t>(vector, fake::engine().vtable);
    for (int i = 0; i < 3; ++i) fake::field(vector, std::u16string(1, char16_t(u'X' + i)), u"FloatProperty", 4 * i, 4);
    fake::field(row_type, u"MaxLife", u"FloatProperty", 0x0, 4);
    fake::field(row_type, u"Kira", u"IntProperty", 0x4, 4);
    fake::field(row_type, u"Species", u"NameProperty", 0x8, 8);
    fake::field(row_type, u"Title", u"StrProperty", 0x10, 0x10);
    fake::field(row_type, u"Mesh", u"SoftObjectProperty", 0x20, 0x30, w.component_class);
    fake::field(row_type, u"Drop", u"StructProperty", 0x50, 12, vector);
    fake::field(row_type, u"Scores", u"IntProperty", 0x5C, 4, 0, 3);
    t::fn::find_table_row = game_find_table_row, t::fn::add_table_row = game_add_table_row;
    t::fn::remove_table_row = game_remove_table_row, t::fn::add_table_row_internal = game_add_table_row_internal;
    fake::put(functions + f::add_row, &game_add_table_row), fake::put(functions + f::remove_row, &game_remove_table_row);
    fake::put(functions + f::add_row_internal, &game_add_table_row_internal);
    const auto table_of = [&](const std::u16string& name) {
        const uint64_t made = fake::object(name, table_class, 0x60);
        fake::put(made, functions), fake::put(made + t::data_table::row_struct, row_type);
        return made;
    };
    const auto row_in = [&](uint64_t table, const std::u16string& name, float life, int32_t kira) {
        const uint64_t row = fake::memory(kRowBytes);
        fake::put(row + 0x0, life), fake::put(row + 0x4, kira), fake::put(row + 0x8, row_name(name));
        for (int i = 0; i < 3; ++i) fake::put(row + 0x50 + 4 * i, float(i + 1)), fake::put<int32_t>(row + 0x5C + 4 * i, 10 * (i + 1));
        fake::put<int32_t>(row + 0x20 + t::soft_object::index, -1), fake::put(row + 0x20 + t::soft_object::path, row_name(u"/G/Old.Old"));
        sheets.rows[{table, row_name(name)}] = row;
        return row;
    };
    const uint64_t creatures = table_of(u"Creatures"), dwarf = row_in(creatures, u"KOCHAPPY", 100, 5), big = row_in(creatures, u"CHAPPY", 750, 12);
    const TSUCHI_Object sheet_of = object(creatures);
    const auto life = [&](uint64_t row) { return fake::get<float>(row); };
    const auto text_of = [&](const char* row, const char* path) {
        char buffer[64] = "<failed>";
        uint64_t bytes = 0;
        sheet.get_text(mod, sheet_of, row, path, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };

    // Reading: by the row's name and the field's path.
    CHECK(sheet.get(mod, sheet_of, "KOCHAPPY", "MaxLife", &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_NUMBER && got.as.number == 100);
    CHECK(sheet.get(mod, sheet_of, "CHAPPY", "Kira", &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_INTEGER && got.as.integer == 12);
    CHECK(sheet.get(mod, sheet_of, "KOCHAPPY", "Drop.Y", &got) == TSUCHI_OK && got.as.number == 2 && sheet.get(mod, sheet_of, "KOCHAPPY", "Scores[2]", &got) == TSUCHI_OK &&
          got.as.integer == 30);
    CHECK(sheet.get(mod, sheet_of, "KOCHAPPY", "Drop", &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_STRUCT && sheet.get(mod, sheet_of, "KOCHAPPY", "Species", &got) == TSUCHI_OK &&
          got.kind == TSUCHI_VALUE_TEXT && got.count == 9);
    CHECK(text_of("KOCHAPPY", "Species") == "KOCHAPPY" && text_of("CHAPPY", "species") == "CHAPPY" && text_of("KOCHAPPY", "Mesh") == "/G/Old.Old");
    CHECK(sheet.get_text(mod, sheet_of, "KOCHAPPY", "Species", text, 8, &needed) == TSUCHI_CAPACITY && needed == 9 &&
          sheet.get_text(mod, sheet_of, "KOCHAPPY", "MaxLife", text, sizeof text, &needed) == TSUCHI_INVALID);
    CHECK(sheet.field_at(mod, sheet_of, "KOCHAPPY", "", 0, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "MaxLife") &&
          sheet.field_at(mod, sheet_of, "KOCHAPPY", "", 6, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Scores") &&
          sheet.field_at(mod, sheet_of, "KOCHAPPY", "", 7, text, sizeof text, &needed) == TSUCHI_NOT_FOUND &&
          sheet.field_at(mod, sheet_of, "KOCHAPPY", "Drop", 1, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Y") &&
          sheet.field_at(mod, sheet_of, "KOCHAPPY", "Kira", 0, text, sizeof text, &needed) == TSUCHI_INVALID);
    // What is no row, no table and no path.
    const uint64_t ghost = table_of(u"Ghost"), bare = table_of(u"Bare");
    const TSUCHI_Object gone = object(ghost);
    fake::destroy(ghost);
    fake::put<uint64_t>(bare + t::data_table::row_struct, 0);
    row_in(bare, u"KOCHAPPY", 1, 1);
    CHECK(sheet.get(mod, sheet_of, "NOBODY", "MaxLife", &got) == TSUCHI_NOT_FOUND && sheet.get(mod, sheet_of, "", "MaxLife", &got) == TSUCHI_NOT_FOUND &&
          sheet.get(mod, sheet_of, "KOCHAPPY", "Weight", &got) == TSUCHI_NOT_FOUND && sheet.get(mod, sheet_of, "KOCHAPPY", "Scores[", &got) == TSUCHI_INVALID &&
          sheet.get(mod, sheet_of, "\xff", "MaxLife", &got) == TSUCHI_INVALID && sheet.get(mod, sheet_of, "a row name of 22 chars", "MaxLife", &got) == TSUCHI_NOT_FOUND);
    CHECK(sheet.get(mod, object(w.idol_asset), "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_FOUND && sheet.get(mod, gone, "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_FOUND &&
          sheet.get(mod, object(bare), "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_FOUND && sheet.get_text(mod, gone, "KOCHAPPY", "Species", text, sizeof text, &needed) == TSUCHI_NOT_FOUND &&
          sheet.field_at(mod, gone, "KOCHAPPY", "", 0, text, sizeof text, &needed) == TSUCHI_NOT_FOUND);
    t::fn::find_table_row = nullptr;
    CHECK(sheet.get(mod, sheet_of, "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_READY);
    t::fn::find_table_row = game_find_table_row;
    const uint64_t pretender = fake::object(u"Pretender", w.component_class, 0x60), stale = table_of(u"Stale");
    const uint64_t dead_type = fake::engine_object(u"DeadRow", w.root);
    fake::field(dead_type, u"MaxLife", u"FloatProperty", 0x0, 4);
    fake::put(pretender + t::data_table::row_struct, row_type), fake::put(stale + t::data_table::row_struct, dead_type);
    row_in(pretender, u"KOCHAPPY", 1, 1), row_in(stale, u"KOCHAPPY", 1, 1);
    CHECK(sheet.get(mod, object(stale), "KOCHAPPY", "MaxLife", &got) == TSUCHI_OK);
    fake::destroy(dead_type);
    CHECK(sheet.get(mod, object(pretender), "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_FOUND && sheet.get(mod, object(stale), "KOCHAPPY", "MaxLife", &got) == TSUCHI_NOT_FOUND);

    // A change is the mod's: what the field held comes back when the mod restores it.
    const TSUCHI_Value more = number(200), most = integer(300);
    refused_size = 16 * 88; // no memory for the first changes: nothing is changed
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_NO_MEMORY && life(dwarf) == 100 && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_OK && life(dwarf) == 200 && life(big) == 750 && tsuchi::owned_count(mod) == owned + 1 &&
          tsuchi::table_changes() == 1);
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "maxlife", &most) == TSUCHI_OK && life(dwarf) == 300 && tsuchi::owned_count(mod) == owned + 1 && tsuchi::table_changes() == 1);
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "MAXLIFE") == TSUCHI_OK && life(dwarf) == 100 && tsuchi::owned_count(mod) == owned && tsuchi::table_changes() == 0 &&
          sheet.restore(mod, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_NOT_FOUND);
    // A copy the game keeps of a row gets each field written into the row, and what the row holds again when the
    // change is taken back. A copy of another row, or of a row of another table, is left alone.
    static uint64_t copied_row = 0, copied_table = 0, copied_name = 0;
    copied_row = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(copied_row), reinterpret_cast<void*>(dwarf), kRowBytes);
    copied_table = creatures, copied_name = row_name(u"KOCHAPPY");
    tsuchi::add_row_copies([](uint64_t table, uint64_t row) { return table == copied_table && row == copied_name ? copied_row : uint64_t(0); });
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_OK && life(dwarf) == 200 && life(copied_row) == 200);
    CHECK(sheet.override(mod, sheet_of, "CHAPPY", "MaxLife", &most) == TSUCHI_OK && life(big) == 300 && life(copied_row) == 200);
    CHECK(sheet.override_text(mod, sheet_of, "KOCHAPPY", "Species", "OOCHAPPY") == TSUCHI_OK && fake::get<uint64_t>(copied_row + 0x8) == row_name(u"OOCHAPPY"));
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_OK && sheet.restore(mod, sheet_of, "KOCHAPPY", "Species") == TSUCHI_OK &&
          sheet.restore(mod, sheet_of, "CHAPPY", "MaxLife") == TSUCHI_OK && life(copied_row) == 100 && life(big) == 750 &&
          fake::get<uint64_t>(copied_row + 0x8) == row_name(u"KOCHAPPY") && tsuchi::table_changes() == 0 && tsuchi::owned_count(mod) == owned);
    // Text of any length reaches the copy whole, and so does what the row holds again.
    const auto copied_title = [] {
        const uint64_t data = fake::get<uint64_t>(copied_row + 0x10 + t::array::data);
        const int32_t count = fake::get<int32_t>(copied_row + 0x10 + t::array::count);
        std::string units;
        for (int32_t i = 0; i + 1 < count; ++i) units += char(fake::get<char16_t>(data + uint64_t(i) * 2));
        return units;
    };
    const std::string long_title(600, 't');
    TSUCHI_Mod* titler = tsuchi::mod_create("actor.titler");
    CHECK(sheet.override_text(mod, sheet_of, "KOCHAPPY", "Title", "Short") == TSUCHI_OK && copied_title() == "Short" &&
          sheet.override_text(mod, sheet_of, "KOCHAPPY", "Title", long_title.c_str()) == TSUCHI_OK && copied_title() == long_title);
    CHECK(sheet.override_text(titler, sheet_of, "KOCHAPPY", "Title", "Short") == TSUCHI_OK && copied_title() == "Short" &&
          sheet.restore(titler, sheet_of, "KOCHAPPY", "Title") == TSUCHI_OK && copied_title() == long_title);
    tsuchi::mod_stop(titler);
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "Title") == TSUCHI_OK && copied_title().empty() && tsuchi::table_changes() == 0 &&
          tsuchi::owned_count(mod) == owned);
    copied_table = 0; // the checks below change the table with no copy of it
    // A change that cannot be made leaves nothing behind.
    TSUCHI_Value flag{TSUCHI_VALUE_BOOLEAN, 0, {}};
    flag.as.boolean = true;
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &flag) == TSUCHI_INVALID && sheet.override(mod, sheet_of, "KOCHAPPY", "Drop", &more) == TSUCHI_UNSUPPORTED &&
          sheet.override(mod, sheet_of, "KOCHAPPY", "Scores", &more) == TSUCHI_UNSUPPORTED && sheet.override(mod, sheet_of, "KOCHAPPY", "Weight", &more) == TSUCHI_NOT_FOUND &&
          sheet.override(mod, sheet_of, "NOBODY", "MaxLife", &more) == TSUCHI_NOT_FOUND && sheet.override(mod, gone, "KOCHAPPY", "MaxLife", &more) == TSUCHI_NOT_FOUND &&
          sheet.override_text(mod, sheet_of, "KOCHAPPY", "MaxLife", "many") == TSUCHI_INVALID && sheet.override_text(mod, sheet_of, "KOCHAPPY", "Species", "\xff") == TSUCHI_INVALID &&
          life(dwarf) == 100 && tsuchi::owned_count(mod) == owned && tsuchi::table_changes() == 0);
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.clerk");
    refused_multiple = 40; // no room to note whose the change is
    CHECK(sheet.override(poor, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_NO_MEMORY && life(dwarf) == 100 && tsuchi::table_changes() == 0);
    refused_multiple = 0;
    tsuchi::mod_stop(poor);
    // Parts of a struct and of values side by side, each its own change.
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "Drop.X", &more) == TSUCHI_OK && sheet.override(mod, sheet_of, "KOCHAPPY", "Scores[1]", &most) == TSUCHI_OK &&
          fake::get<float>(dwarf + 0x50) == 200 && fake::get<int32_t>(dwarf + 0x60) == 300 && tsuchi::owned_count(mod) == owned + 2);
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "Drop.X") == TSUCHI_OK && sheet.restore(mod, sheet_of, "KOCHAPPY", "Scores[1]") == TSUCHI_OK &&
          fake::get<float>(dwarf + 0x50) == 1 && fake::get<int32_t>(dwarf + 0x60) == 20 && tsuchi::owned_count(mod) == owned);
    // Text: a name, and the path in a reference to an asset, which also takes an object.
    CHECK(sheet.override_text(mod, sheet_of, "KOCHAPPY", "Species", "MYCHAPPY") == TSUCHI_OK && text_of("KOCHAPPY", "Species") == "MYCHAPPY" &&
          sheet.override_text(mod, sheet_of, "KOCHAPPY", "Species", "OTHER") == TSUCHI_OK && sheet.restore(mod, sheet_of, "KOCHAPPY", "Species") == TSUCHI_OK &&
          text_of("KOCHAPPY", "Species") == "KOCHAPPY");
    const TSUCHI_Value part = reference(fake::get<uint64_t>(w.rock + t::game_actor::root)), idol = reference(w.idol_asset);
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "Mesh", &idol) == TSUCHI_OK && text_of("KOCHAPPY", "Mesh") == "/G/Idol.Idol" &&
          sheet.override_text(mod, sheet_of, "KOCHAPPY", "Mesh", "/G/New.New") == TSUCHI_OK && text_of("KOCHAPPY", "Mesh") == "/G/New.New" &&
          sheet.override(mod, sheet_of, "KOCHAPPY", "Mesh", &nothing) == TSUCHI_OK && fake::get<uint64_t>(dwarf + 0x20 + t::soft_object::path) == 0 &&
          sheet.restore(mod, sheet_of, "KOCHAPPY", "Mesh") == TSUCHI_OK && text_of("KOCHAPPY", "Mesh") == "/G/Old.Old" && tsuchi::owned_count(mod) == owned);
    CHECK(part.as.object.id && sheet.override(mod, sheet_of, "KOCHAPPY", "Mesh", &part) == TSUCHI_OK && sheet.restore(mod, sheet_of, "KOCHAPPY", "Mesh") == TSUCHI_OK);
    const TSUCHI_Value actor_value = reference(w.rock); // an actor is no part
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "Mesh", &actor_value) == TSUCHI_INVALID && text_of("KOCHAPPY", "Mesh") == "/G/Old.Old" && tsuchi::owned_count(mod) == owned);

    refused_size = 9; // no memory to remember "KOCHAPPY": the name stays
    CHECK(sheet.override_text(mod, sheet_of, "KOCHAPPY", "Species", "MYCHAPPY") == TSUCHI_NO_MEMORY && text_of("KOCHAPPY", "Species") == "KOCHAPPY" &&
          tsuchi::owned_count(mod) == owned && tsuchi::table_changes() == 0);
    refused_size = 0;

    // Several mods can each have a say on a field. The say of the mod started last is in force; when it is taken
    // back the next one's is, as that one stands now; and a say ends with its mod.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.editor");
    const uint64_t second = table_of(u"Second"), second_row = row_in(second, u"KOCHAPPY", 60, 2);
    const TSUCHI_Value fewer = integer(150);
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_OK && life(dwarf) == 200 &&
          sheet.restore(rival, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_NOT_FOUND && life(dwarf) == 200 && tsuchi::table_changes() == 1);
    CHECK(sheet.override(rival, sheet_of, "KOCHAPPY", "maxlife", &most) == TSUCHI_OK && life(dwarf) == 300 && tsuchi::table_changes() == 2); // started later: its say is in force
    CHECK(sheet.override_text(rival, sheet_of, "KOCHAPPY", "maxlife", "x") != TSUCHI_OK && life(dwarf) == 300 && tsuchi::table_changes() == 2); // a say that does not fit leaves things as they were
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &fewer) == TSUCHI_OK && life(dwarf) == 300 && tsuchi::table_changes() == 2); // the earlier mod thinks again: still not in force
    CHECK(sheet.restore(rival, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_OK && life(dwarf) == 150 && tsuchi::table_changes() == 1); // taken back: the earlier mod's say, as it is now
    CHECK(sheet.override(rival, sheet_of, "KOCHAPPY", "MaxLife", &most) == TSUCHI_OK && life(dwarf) == 300);
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_OK && life(dwarf) == 300 && tsuchi::table_changes() == 1); // the earlier one's is taken back: nothing changes
    CHECK(sheet.override(mod, sheet_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_OK && life(dwarf) == 300 && tsuchi::table_changes() == 2);
    CHECK(sheet.override(rival, sheet_of, "CHAPPY", "MaxLife", &most) == TSUCHI_OK && sheet.override(rival, sheet_of, "KOCHAPPY", "Kira", &most) == TSUCHI_OK && life(big) == 300 &&
          fake::get<int32_t>(dwarf + 4) == 300 && tsuchi::table_changes() == 4);
    CHECK(sheet.override(rival, object(second), "KOCHAPPY", "MaxLife", &most) == TSUCHI_OK && life(second_row) == 300 && tsuchi::table_changes() == 5); // another table's
    tsuchi::mod_stop(rival);
    CHECK(life(big) == 750 && fake::get<int32_t>(dwarf + 4) == 5 && life(dwarf) == 200 && life(second_row) == 60 && tsuchi::table_changes() == 1);
    CHECK(sheet.restore(mod, sheet_of, "KOCHAPPY", "MaxLife") == TSUCHI_OK && life(dwarf) == 100);
    // The game takes the row or the table away meanwhile: the change is forgotten quietly.
    const uint64_t passing = table_of(u"Passing"), passing_row = row_in(passing, u"KOCHAPPY", 40, 1);
    const TSUCHI_Object passing_of = object(passing);
    CHECK(sheet.override(mod, passing_of, "KOCHAPPY", "MaxLife", &more) == TSUCHI_OK && sheet.override(mod, passing_of, "KOCHAPPY", "Kira", &most) == TSUCHI_OK);
    sheets.rows.erase({passing, row_name(u"KOCHAPPY")});
    CHECK(sheet.restore(mod, passing_of, "KOCHAPPY", "MaxLife") == TSUCHI_OK && life(passing_row) == 200 && tsuchi::owned_count(mod) == owned + 1);
    fake::destroy(passing);
    CHECK(sheet.restore(mod, passing_of, "KOCHAPPY", "Kira") == TSUCHI_OK && fake::get<int32_t>(passing_row + 4) == 300 && tsuchi::owned_count(mod) == owned &&
          tsuchi::table_changes() == 0);

    // A row copied under a new name. It stays.
    CHECK(sheet.add_row(mod, sheet_of, "MYCHAPPY", "KOCHAPPY") == TSUCHI_OK && sheets.added == 1 && sheet.get(mod, sheet_of, "MYCHAPPY", "Kira", &got) == TSUCHI_OK &&
          got.as.integer == 5 && text_of("MYCHAPPY", "Species") == "KOCHAPPY");
    CHECK(sheet.add_row(mod, sheet_of, "MYCHAPPY", "KOCHAPPY") == TSUCHI_INVALID && sheet.add_row(mod, sheet_of, "CHAPPY", "KOCHAPPY") == TSUCHI_INVALID &&
          sheet.add_row(mod, sheet_of, "", "KOCHAPPY") == TSUCHI_INVALID && sheet.add_row(mod, sheet_of, "\xff", "KOCHAPPY") == TSUCHI_INVALID &&
          sheet.add_row(mod, sheet_of, "a row name of 22 chars", "KOCHAPPY") == TSUCHI_INVALID && sheet.add_row(mod, sheet_of, "NEW", "NOBODY") == TSUCHI_NOT_FOUND &&
          sheet.add_row(mod, gone, "NEW", "KOCHAPPY") == TSUCHI_NOT_FOUND && sheet.add_row(mod, object(w.idol_asset), "NEW", "KOCHAPPY") == TSUCHI_NOT_FOUND && sheets.added == 1);
    for (uint32_t slot : {uint32_t(f::add_row), uint32_t(f::remove_row), uint32_t(f::add_row_internal)}) { // a table that keeps rows its own way
        const uint64_t usual = fake::get<uint64_t>(functions + slot);
        fake::put<uint64_t>(functions + slot, usual + 4);
        CHECK(sheet.add_row(mod, sheet_of, "NEW", "KOCHAPPY") == TSUCHI_UNSUPPORTED && sheets.added == 1);
        fake::put(functions + slot, usual);
    }
    t::fn::remove_table_row = nullptr;
    CHECK(sheet.add_row(mod, sheet_of, "NEW", "KOCHAPPY") == TSUCHI_UNSUPPORTED && sheets.added == 1);
    t::fn::remove_table_row = game_remove_table_row;
    sheets.adds = false; // the game adds nothing
    CHECK(sheet.add_row(mod, sheet_of, "NEW", "KOCHAPPY") == TSUCHI_NOT_READY && sheets.added == 2);
    sheets.adds = true;
    CHECK(tsuchi::owned_count(mod) == owned && life(dwarf) == 100);
    w.creature_table = creatures;
    #include "table_atomic_checks.h"
}

// ---- The game's text --------------------------------------------------------------------

struct Words {
    std::vector<std::u16string> keys;                                   // what keys were made from
    std::map<std::pair<std::u16string, std::u16string>, uint64_t> strings; // by namespace and key: the string every use shares
    uint64_t manager = 0;
    int32_t holders = 2; // on what a lookup hands out: the table's own hold, and the lookup's
    int locks = 0, unlocks = 0, registered = 0, objects_destroyed = 0, counters_destroyed = 0;
    bool locked = false, well_formed = true;
    std::vector<uint64_t> counters;
} words;
std::u16string string_of(const void* header) {
    const auto at = reinterpret_cast<uint64_t>(header);
    const int32_t count = fake::get<int32_t>(at + t::array::count);
    words.well_formed = words.well_formed && count >= 1 && fake::get<int32_t>(at + t::array::max) == count;
    const auto* data = reinterpret_cast<const char16_t*>(fake::get<uint64_t>(at + t::array::data));
    words.well_formed = words.well_formed && data[count - 1] == 0;
    return std::u16string(data, size_t(count - 1));
}
void game_text_key(void* key, void* string) {
    words.keys.push_back(string_of(string));
    fake::put<uint64_t>(reinterpret_cast<uint64_t>(key), words.keys.size()); // one more than its place
}
void counter_destroy_object(void*) { ++words.objects_destroyed; }
void counter_destroy_self(void*) { ++words.counters_destroyed; }
bool game_find_text(void* name_space, void* key, void* out, void* source) {
    const auto space_at = fake::get<uint64_t>(reinterpret_cast<uint64_t>(name_space)), key_at = fake::get<uint64_t>(reinterpret_cast<uint64_t>(key));
    words.well_formed = words.well_formed && !source && space_at && key_at;
    const auto found = words.strings.find({words.keys[space_at - 1], words.keys[key_at - 1]});
    if (found == words.strings.end()) return false;
    const uint64_t counter = fake::memory(t::shared_count::size + 0x20), functions = fake::memory(0x40);
    fake::put(functions + t::shared_count_table::destroy_object, &counter_destroy_object);
    fake::put(functions + t::shared_count_table::destroy_self, &counter_destroy_self);
    fake::put(counter, functions), fake::put<int32_t>(counter + t::shared_count::holders, words.holders), fake::put<int32_t>(counter + t::shared_count::watchers, 1);
    words.counters.push_back(counter);
    fake::put(reinterpret_cast<uint64_t>(out), found->second), fake::put(reinterpret_cast<uint64_t>(out) + t::text_value::counter, counter);
    return true;
}
void* game_text_display_string(void* text) { return reinterpret_cast<void*>(fake::get<uint64_t>(reinterpret_cast<uint64_t>(text))); }
uint64_t shared_string(const std::u16string& text) {
    const uint64_t header = fake::memory(t::array::size);
    auto* data = static_cast<char16_t*>(tsuchi::game::engine_memory((text.size() + 1) * 2));
    std::memcpy(data, text.c_str(), (text.size() + 1) * 2);
    fake::put(header + t::array::data, data), fake::put<int32_t>(header + t::array::count, int32_t(text.size() + 1));
    fake::put<int32_t>(header + t::array::max, int32_t(text.size() + 1));
    return header;
}
void game_register_text(void* manager, void* entry, int32_t count) {
    namespace p = t::polyglot_text;
    const auto at = reinterpret_cast<uint64_t>(entry);
    ++words.registered;
    words.well_formed = words.well_formed && reinterpret_cast<uint64_t>(manager) == words.manager && count == 1 && at % 16 == 0 &&
                        fake::get<int32_t>(at + p::max_bits) == int32_t(t::constant::polyglot_max_bits) && fake::get<int32_t>(at + p::first_free) == -1;
    words.strings[{string_of(reinterpret_cast<void*>(at + p::name_space)), string_of(reinterpret_cast<void*>(at + p::key))}] =
        shared_string(string_of(reinterpret_cast<void*>(at + p::text)));
}
void game_lock_mutex(void* lock) {
    ++words.locks;
    words.well_formed = words.well_formed && !words.locked && reinterpret_cast<uint64_t>(lock) == words.manager + t::text_manager::lock;
    words.locked = true;
}
void game_unlock_mutex(void* lock) {
    ++words.unlocks;
    words.well_formed = words.well_formed && words.locked && reinterpret_cast<uint64_t>(lock) == words.manager + t::text_manager::lock;
    words.locked = false;
}

void wording() {
    const TSUCHI_TextService& lines = *tsuchi::host.text;
    const uint64_t main = fake::main_image();
    char text[64] = "untouched";
    uint64_t needed = 0;
    const uint32_t owned = tsuchi::owned_count(mod);
    auto& engine = fake::engine();
    const auto read = [&](const char* name_space, const char* key) {
        char buffer[128] = "<failed>";
        uint64_t bytes = 0;
        lines.get(mod, name_space, key, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };
    for (auto function : {reinterpret_cast<uint64_t>(&counter_destroy_object), reinterpret_cast<uint64_t>(&counter_destroy_self)}) fake::code().push_back(function);
    words.strings[{u"GenseiName", u"CHAPPY"}] = shared_string(u"Bulborb");
    words.strings[{u"GenseiName", u"OLIMAR"}] = shared_string(u"Olimar \u00E9\U0001F331");
    words.manager = fake::memory(0x40);

    // Until the engine's keeper of text is there and has read the text, nothing can be asked.
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY && lines.override(mod, "GenseiName", "CHAPPY", "x") == TSUCHI_NOT_READY);
    t::fn::text_key_from_string = game_text_key, t::fn::find_text = game_find_text, t::fn::text_display_string = game_text_display_string;
    t::fn::register_text = game_register_text, t::fn::lock_mutex = game_lock_mutex, t::fn::unlock_mutex = game_unlock_mutex;
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY);
    fake::put(main + t::data::text_manager, words.manager);
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(main + t::data::text_manager_ready, 1);
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY && lines.override(mod, "GenseiName", "CHAPPY", "x") == TSUCHI_NOT_READY);
    fake::put<uint8_t>(words.manager + t::text_manager::ready, 1);
    fake::put<uint8_t>(main + t::data::text_manager_ready, 2); // the pointer is not yet said to be good
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(main + t::data::text_manager_ready, 3);
    t::fn::unlock_mutex = nullptr;
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_READY && tsuchi::owned_count(mod) == owned);
    t::fn::unlock_mutex = game_unlock_mutex;

    // Read: by namespace and key. What the lookup handed out is let go of at once.
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Bulborb") && needed == 8 && words.well_formed);
    CHECK(fake::get<int32_t>(words.counters.back() + t::shared_count::holders) == 1 && words.objects_destroyed == 0);
    CHECK(read("GenseiName", "OLIMAR") == "Olimar \xC3\xA9\xF0\x9F\x8C\xB1");
    std::snprintf(text, sizeof text, "untouched");
    CHECK(lines.get(mod, "GenseiName", "CHAPPY", text, 7, &needed) == TSUCHI_CAPACITY && needed == 8 && !std::strcmp(text, "untouched"));
    CHECK(lines.get(mod, "GenseiName", "NOBODY", text, sizeof text, &needed) == TSUCHI_NOT_FOUND && lines.get(mod, "Nowhere", "CHAPPY", text, sizeof text, &needed) == TSUCHI_NOT_FOUND);
    CHECK(lines.get(mod, "", "CHAPPY", text, sizeof text, &needed) == TSUCHI_INVALID && lines.get(mod, "GenseiName", "", text, sizeof text, &needed) == TSUCHI_INVALID &&
          lines.get(mod, "\xff", "CHAPPY", text, sizeof text, &needed) == TSUCHI_INVALID && lines.get(mod, "GenseiName", "\xff", text, sizeof text, &needed) == TSUCHI_INVALID);
    words.holders = 1; // nobody else holds what the lookup hands out: it goes, and its counter with it
    CHECK(read("GenseiName", "CHAPPY") == "Bulborb" && words.objects_destroyed == 1 && words.counters_destroyed == 1);
    words.holders = 2;

    // Replaced: the same string gets new text under the keeper's lock; the game's own comes back.
    refused_size = 16 * 88; // no memory for the first replacements
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_NO_MEMORY && read("GenseiName", "CHAPPY") == "Bulborb" && tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    int freed = engine.freed;
    const uint64_t shared = words.strings[std::make_pair(std::u16string(u"GenseiName"), std::u16string(u"CHAPPY"))];
    const auto shared_now = [&] { return words.strings[std::make_pair(std::u16string(u"GenseiName"), std::u16string(u"CHAPPY"))]; };
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Grub-dog" && shared_now() == shared &&
          fake::get<int32_t>(shared + t::array::count) == 9 && fake::get<int32_t>(shared + t::array::max) == 9 && engine.freed == freed + 1 && words.locks == 1 &&
          words.unlocks == 1 && words.well_formed && tsuchi::owned_count(mod) == owned + 1 && tsuchi::text_replacements() == 1);
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "") == TSUCHI_OK && read("GenseiName", "CHAPPY").empty() && fake::get<int32_t>(shared + t::array::count) == 1 &&
          tsuchi::owned_count(mod) == owned + 1 && tsuchi::text_replacements() == 1);
    CHECK(lines.restore(mod, "GenseiName", "CHAPPY") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Bulborb" && tsuchi::owned_count(mod) == owned &&
          tsuchi::text_replacements() == 0 && lines.restore(mod, "GenseiName", "CHAPPY") == TSUCHI_NOT_FOUND && words.locks == words.unlocks && words.locks == 3);
    CHECK(lines.override(mod, "", "CHAPPY", "x") == TSUCHI_INVALID && lines.override(mod, "GenseiName", "", "x") == TSUCHI_INVALID && lines.override(mod, "GenseiName", "CHAPPY", "\xff") == TSUCHI_INVALID &&
          lines.override(mod, "\xff", "CHAPPY", "x") == TSUCHI_INVALID && read("GenseiName", "CHAPPY") == "Bulborb" && tsuchi::owned_count(mod) == owned);
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.scribe");
    refused_multiple = 40; // no room to note whose the replacement is
    CHECK(lines.override(poor, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_NO_MEMORY && read("GenseiName", "CHAPPY") == "Bulborb" && tsuchi::text_replacements() == 0);
    refused_multiple = 0;
    tsuchi::mod_stop(poor);
    engine.out_of_memory = true; // the engine has no memory for the new text
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_NO_MEMORY && tsuchi::owned_count(mod) == owned && tsuchi::text_replacements() == 0);
    engine.out_of_memory = false;
    CHECK(read("GenseiName", "CHAPPY") == "Bulborb");

    // Several mods can each replace a text. The replacement of the mod started last is shown; when it is taken back
    // the next one's is, as that one stands now; and a replacement ends with its mod.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.poet");
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_OK && lines.restore(rival, "GenseiName", "CHAPPY") == TSUCHI_NOT_FOUND &&
          read("GenseiName", "CHAPPY") == "Grub-dog");
    const unsigned replaced = tsuchi::text_replacements();
    CHECK(lines.override(rival, "GenseiName", "CHAPPY", "Spotty") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Spotty" && tsuchi::text_replacements() == replaced + 1);
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grubber") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Spotty"); // the earlier mod thinks again: not shown
    CHECK(lines.restore(rival, "GenseiName", "CHAPPY") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Grubber" && tsuchi::text_replacements() == replaced);
    CHECK(lines.override(rival, "GenseiName", "CHAPPY", "Spotty") == TSUCHI_OK && lines.restore(mod, "GenseiName", "CHAPPY") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Spotty");
    CHECK(lines.restore(rival, "GenseiName", "CHAPPY") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Bulborb"); // the game's own, kept through both
    CHECK(lines.override(mod, "GenseiName", "CHAPPY", "Grub-dog") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Grub-dog" && tsuchi::text_replacements() == replaced);
    CHECK(lines.override(rival, "GenseiName", "OLIMAR", "Captain") == TSUCHI_OK && lines.override(rival, "GenseiName", "OLIMAR", "The Captain") == TSUCHI_OK &&
          read("GenseiName", "OLIMAR") == "The Captain" && tsuchi::text_replacements() == 2);
    words.strings[std::make_pair(std::u16string(u"GenseiDesc"), std::u16string(u"CHAPPY"))] = shared_string(u"It sleeps by day.");
    CHECK(lines.override(rival, "GenseiDesc", "CHAPPY", "It hunts by night.") == TSUCHI_OK && read("GenseiDesc", "CHAPPY") == "It hunts by night." &&
          read("GenseiName", "CHAPPY") == "Grub-dog" && tsuchi::text_replacements() == 3); // the same key under another namespace is another text
    tsuchi::mod_stop(rival);
    CHECK(read("GenseiDesc", "CHAPPY") == "It sleeps by day." && tsuchi::text_replacements() == 1);
    CHECK(read("GenseiName", "OLIMAR") == "Olimar \xC3\xA9\xF0\x9F\x8C\xB1" && read("GenseiName", "CHAPPY") == "Grub-dog" && tsuchi::text_replacements() == 1);
    CHECK(lines.restore(mod, "GenseiName", "CHAPPY") == TSUCHI_OK && read("GenseiName", "CHAPPY") == "Bulborb");

    // A key the game does not have: the text is handed to the keeper, and stays when the mod lets go.
    const int locks = words.locks;
    CHECK(lines.override(mod, "GenseiName", "GROINK", "Gatling Groink") == TSUCHI_OK && words.registered == 1 && words.well_formed && words.locks == locks &&
          read("GenseiName", "GROINK") == "Gatling Groink" && tsuchi::owned_count(mod) == owned + 1);
    CHECK(lines.override(mod, "GenseiName", "GROINK", "Groink") == TSUCHI_OK && words.registered == 1 && read("GenseiName", "GROINK") == "Groink"); // it has the key now
    CHECK(lines.restore(mod, "GenseiName", "GROINK") == TSUCHI_OK && read("GenseiName", "GROINK") == "Groink" && tsuchi::owned_count(mod) == owned &&
          tsuchi::text_replacements() == 0 && words.well_formed && words.locks == words.unlocks);
}

// ---- The banner, and sounds ---------------------------------------------------------------

struct Announced {
    std::string log;
    std::u16string text;
    uint64_t counter = 0, sound_actor = 0;
    bool has_banner = true, attach_none = false;
    std::vector<std::u16string> made;
    std::map<std::string, bool> sounds;
} announced;
bool game_ensure_banner(void*) {
    announced.log += "ensure ";
    return announced.has_banner;
}
tsuchi::game::EngineText game_text_from_string(const void* string) {
    tsuchi::game::EngineText made;
    announced.made.push_back(string_of(string));
    announced.counter = fake::memory(t::shared_count::size + 0x20);
    const uint64_t functions = fake::memory(0x40), index = announced.made.size();
    fake::put(functions + t::shared_count_table::destroy_object, &counter_destroy_object);
    fake::put(functions + t::shared_count_table::destroy_self, &counter_destroy_self);
    fake::put(announced.counter, functions), fake::put<int32_t>(announced.counter + t::shared_count::holders, 1);
    fake::put<int32_t>(announced.counter + t::shared_count::watchers, 1);
    std::memcpy(made.bytes, &index, 8), std::memcpy(made.bytes + t::text_value::counter, &announced.counter, 8);
    return made;
}
void game_banner_set_text(void*, void* text) {
    const auto at = reinterpret_cast<uint64_t>(text);
    announced.text = announced.made[fake::get<uint64_t>(at) - 1];
    announced.log += "text ";
    const uint64_t counter = fake::get<uint64_t>(at + t::text_value::counter); // the banner keeps a share of its own
    fake::put<int32_t>(counter + t::shared_count::holders, fake::get<int32_t>(counter + t::shared_count::holders) + 1);
}
void game_banner_place(void*) { announced.log += "place "; }
void game_banner_play(void*) { announced.log += "play "; }
void game_widget_set_visibility(void*, uint32_t how) { announced.log += "show" + std::to_string(how) + " "; }
bool game_play_sound_on(void* actor, void* key, void* attach, bool flag) {
    char text[64] = "";
    tsuchi::game::name_text(*static_cast<const tsuchi::game::Name*>(key), text, sizeof text);
    announced.sound_actor = reinterpret_cast<uint64_t>(actor);
    announced.attach_none = fake::get<uint64_t>(reinterpret_cast<uint64_t>(attach)) == 0 && flag;
    announced.log += std::string("sound:") + text + " ";
    return announced.sounds[text];
}

void announcing() {
    const TSUCHI_HudService& board = *tsuchi::host.hud;
    const TSUCHI_AudioService& sound = *tsuchi::host.audio;
    const uint64_t main = fake::main_image();
    namespace a = t::announce_widget;
    const auto log_is = [](const char* expected) {
        const bool same = announced.log == expected;
        if (!same) std::printf("  log: [%s]\n", announced.log.c_str());
        announced.log.clear();
        return same;
    };
    const int destroyed = words.objects_destroyed;
    fake::code().push_back(reinterpret_cast<uint64_t>(&game_widget_set_visibility));
    const uint64_t widget_table = fake::memory(0x40);
    fake::put(widget_table + t::widget_table::set_visibility, &game_widget_set_visibility);
    const auto part = [&](const std::u16string& name) {
        const uint64_t made = fake::object(name, w.component_class, 0x60);
        fake::put(made, widget_table);
        return made;
    };
    const uint64_t ui = part(u"Ui"), announcer = part(u"Announcer"), widget = part(u"Banner"), clock = part(u"Clock");
    fake::put(clock + a::wait_time, 2.5f), fake::put<int32_t>(clock + a::state, 0);

    // No banner to be had: nothing that keeps screens, nothing that announces, none made, or one without its clock.
    t::fn::ensure_banner = game_ensure_banner, t::fn::banner_set_text = game_banner_set_text, t::fn::banner_place = game_banner_place;
    t::fn::banner_play = game_banner_play;
    t::fn::text_from_string = &game_text_from_string;
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is(""));
    fake::put(w.framework + t::game_framework::ui, ui);
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is(""));
    fake::put(ui + t::ui_manager::announcer, announcer);
    announced.has_banner = false;
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is("ensure "));
    announced.has_banner = true;
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is("ensure ")); // it says it has one, and has none
    fake::put(announcer + t::announcer::banner, widget);
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is("ensure "));
    fake::put(widget + t::banner_widget::clock, clock);
    t::fn::banner_play = nullptr;
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is(""));
    t::fn::banner_play = game_banner_play;
    fake::put<uint64_t>(widget_table + t::widget_table::set_visibility, 0x1234); // not code
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is("ensure ") && fake::get<float>(clock + a::wait_time) == 2.5f);
    fake::put(widget_table + t::widget_table::set_visibility, &game_widget_set_visibility);
    CHECK(board.banner(mod, "Hello", 0) == TSUCHI_INVALID && board.banner(mod, "Hello", -1) == TSUCHI_INVALID && board.banner(mod, "\xff", 5) == TSUCHI_INVALID && log_is(""));
    announced.has_banner = false; // the game says it has no banner, whatever is left where one was
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is("ensure "));
    announced.has_banner = true;
    const uint64_t dying = part(u"Announcer"); // what announces is being destroyed
    fake::put(dying + t::announcer::banner, widget), fake::put(ui + t::ui_manager::announcer, dying);
    fake::destroy(dying);
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NOT_READY && log_is(""));
    fake::put(ui + t::ui_manager::announcer, announcer);
    refused_size = 12; // no memory for the text
    CHECK(board.banner(mod, "Hello", 5) == TSUCHI_NO_MEMORY && log_is("ensure ") && fake::get<float>(clock + a::wait_time) == 2.5f);
    refused_size = 0;

    // Shown: for as long as asked, with the text, and the engine's text let go of afterward.
    CHECK(board.banner(mod, "Hello \xC3\xA9", 6) == TSUCHI_OK && log_is("ensure show5 text place play ") && announced.text == u"Hello \u00E9" &&
          fake::get<float>(clock + a::wait_time) == 6 && fake::get<int32_t>(announced.counter + t::shared_count::holders) == 1 && words.objects_destroyed == destroyed);
    // The game's own time comes back once the banner has taken ours: not while it still slides in.
    fake::put<int32_t>(clock + a::state, int32_t(t::constant::announce_sliding_in));
    tsuchi::frame(0.03f);
    CHECK(fake::get<float>(clock + a::wait_time) == 6);
    CHECK(board.banner(mod, "Again", 9) == TSUCHI_OK && log_is("ensure show5 text place play ") && fake::get<float>(clock + a::wait_time) == 9);
    tsuchi::frame(0.03f);
    fake::put<int32_t>(clock + a::state, 2);
    tsuchi::frame(0.03f);
    CHECK(fake::get<float>(clock + a::wait_time) == 2.5f); // the game's, not the first banner's
    fake::put(clock + a::wait_time, 7.0f);
    tsuchi::frame(0.03f);
    CHECK(fake::get<float>(clock + a::wait_time) == 7); // and then it is left alone
    // The banner goes with its level before that: nothing is written to what replaced it.
    CHECK(board.banner(mod, "Bye", 3) == TSUCHI_OK && log_is("ensure show5 text place play ") && fake::get<float>(clock + a::wait_time) == 3);
    const uint64_t next_clock = part(u"Clock");
    fake::put(next_clock + a::wait_time, 1.5f), fake::put(widget + t::banner_widget::clock, next_clock);
    tsuchi::frame(0.03f);
    CHECK(fake::get<float>(next_clock + a::wait_time) == 1.5f && fake::get<float>(clock + a::wait_time) == 3);
    fake::put(clock + a::wait_time, 2.5f), fake::put(widget + t::banner_widget::clock, clock), fake::put<int32_t>(clock + a::state, 0);

    // A sound of an actor's, by its key.
    const uint64_t sounds = fake::memory(0x20), ghost = actor(u"Ghost", w.actor_class, u"Ghost", 3, 0, component(0, 0, 0, 0));
    const TSUCHI_Actor gone = handle(ghost);
    fake::destroy(ghost);
    announced.sounds["Whistle"] = true;
    t::fn::play_sound_on = game_play_sound_on;
    CHECK(sound.play_on(mod, handle(w.chief), "Whistle") == TSUCHI_NOT_READY);
    fake::put(main + t::data::sound_system, sounds);
    fake::put<uint8_t>(sounds + t::sound_system::started, 1);
    CHECK(sound.play_on(mod, handle(w.chief), "Whistle") == TSUCHI_NOT_READY);
    fake::put<uint8_t>(sounds + t::sound_system::started, 0), fake::put<uint8_t>(sounds + t::sound_system::ready, 1);
    CHECK(sound.play_on(mod, handle(w.chief), "Whistle") == TSUCHI_NOT_READY && log_is(""));
    fake::put<uint8_t>(sounds + t::sound_system::started, 1);
    t::fn::play_sound_on = nullptr;
    CHECK(sound.play_on(mod, handle(w.chief), "Whistle") == TSUCHI_NOT_READY);
    t::fn::play_sound_on = game_play_sound_on;
    CHECK(sound.play_on(mod, handle(w.chief), "Whistle") == TSUCHI_OK && log_is("sound:Whistle ") && announced.sound_actor == w.chief && announced.attach_none);
    CHECK(sound.play_on(mod, handle(w.chief), "Roar") == TSUCHI_NOT_FOUND && log_is("sound:Roar "));
    CHECK(sound.play_on(mod, gone, "Whistle") == TSUCHI_NOT_FOUND && sound.play_on(mod, TSUCHI_Actor{to_handle(w.idol_asset)}, "Whistle") == TSUCHI_NOT_FOUND &&
          sound.play_on(mod, handle(w.chief), "") == TSUCHI_INVALID && sound.play_on(mod, handle(w.chief), "\xff") == TSUCHI_INVALID &&
          sound.play_on(mod, handle(w.chief), "a sound key of 22 chrs") == TSUCHI_INVALID && log_is(""));
}

// ---- The story: its day, its clock, and travel ----------------------------------------------

struct Journey {
    std::vector<uint64_t> posted; // requests handed to the story
    std::string log;
    uint64_t portal_part = 0, asked_class = 0;
} journey;
void game_clock_set(void* clock, float part, bool saved_too) {
    char line[64];
    std::snprintf(line, sizeof line, "clock%g%s ", part, saved_too ? " saved" : "");
    journey.log += line;
    fake::put(reinterpret_cast<uint64_t>(clock) + t::clock::progress, part);
}
void game_push_request(void*, void* request) { journey.posted.push_back(reinterpret_cast<uint64_t>(request)); }
void game_travel_request(void* request, uint64_t source) {
    std::memset(request, 0xEE, t::travel_request::size); // the game fills in what it wants; the rest is not zero
    fake::put<uint32_t>(reinterpret_cast<uint64_t>(request) + t::request::source, uint32_t(source));
}
void* game_find_component(void*, void* type) {
    journey.asked_class = reinterpret_cast<uint64_t>(type);
    return reinterpret_cast<void*>(journey.portal_part);
}

struct Hoard {
    std::map<uint64_t, uint64_t> rows;    // by a treasure's name: its row
    std::map<uint64_t, uint64_t> records; // by name: the story's entry, which begins with its count
    uint64_t story = 0, context = 0;
    std::string log;
    bool well_formed = true;
} hoard;
void* game_find_treasure_row(void* name) {
    const auto found = hoard.rows.find(fake::get<uint64_t>(reinterpret_cast<uint64_t>(name)));
    return reinterpret_cast<void*>(found == hoard.rows.end() ? 0 : found->second);
}
void* game_find_story_record(void* story, void* records, void* name) {
    hoard.well_formed = hoard.well_formed && reinterpret_cast<uint64_t>(story) == hoard.story &&
                        reinterpret_cast<uint64_t>(records) == hoard.story + t::story_save::treasure_records;
    const auto found = hoard.records.find(fake::get<uint64_t>(reinterpret_cast<uint64_t>(name)));
    return reinterpret_cast<void*>(found == hoard.records.end() ? 0 : found->second);
}
void game_collect_treasure(uint64_t name, int32_t sparklium, int32_t poko) {
    char text[64] = "";
    Name unpacked;
    std::memcpy(&unpacked, &name, sizeof unpacked);
    name_text(unpacked, text, sizeof text);
    hoard.log += std::string("collect:") + text + " " + std::to_string(sparklium) + " " + std::to_string(poko) + " ";
    uint64_t& record = hoard.records[name];
    if (!record) record = fake::memory(0x10);
    fake::put<int32_t>(record, fake::get<int32_t>(record) + 1);
}
void game_add_sparklium(void* context, int32_t amount, bool shown) {
    hoard.well_formed = hoard.well_formed && reinterpret_cast<uint64_t>(context) == hoard.context;
    hoard.log += "sparklium:" + std::to_string(amount) + (shown ? "+shown " : " ");
}

void journeying() {
    const TSUCHI_StoryService& tale = *tsuchi::host.story;
    const uint64_t main = fake::main_image();
    namespace f = t::flow_stage;
    uint32_t number = 0;
    float part = -1;
    for (auto function : {reinterpret_cast<uint64_t>(&game_clock_set), reinterpret_cast<uint64_t>(&game_push_request), reinterpret_cast<uint64_t>(&game_find_component)})
        fake::code().push_back(function);

    // The day: the story's own count.
    const uint64_t old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save);
    CHECK(tale.day(mod, &number) == TSUCHI_NOT_READY && tale.clock(mod, &part) == TSUCHI_NOT_READY && tale.set_clock(mod, 0.5f) == TSUCHI_NOT_READY); // no story that is a live object
    const uint64_t story = fake::object(u"StorySave", w.component_class, 0x60);
    fake::put<uint32_t>(story + t::story_save::day, 7);
    fake::put(running.instance + t::game_instance::story_save, story);
    CHECK(tale.day(mod, &number) == TSUCHI_OK && number == 7);

    // The clock: the level's, known by its function table.
    const uint64_t clock = fake::memory(0x20), old_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
    fake::put(clock + t::clock::progress, 0.25f);
    CHECK(tale.clock(mod, &part) == TSUCHI_NOT_READY && tale.set_clock(mod, 0.5f) == TSUCHI_NOT_READY);
    fake::put(w.framework + t::game_framework::clock, clock);
    CHECK(tale.clock(mod, &part) == TSUCHI_NOT_READY); // something else where the clock would be
    fake::put<uint64_t>(clock, main + t::data::clock_table);
    CHECK(tale.clock(mod, &part) == TSUCHI_OK && part == 0.25f);
    CHECK(tale.set_clock(mod, 0.5f) == TSUCHI_NOT_READY && journey.log.empty()); // its table has no such function yet
    fake::put(main + t::data::clock_table + t::clock_table::set_progress, &game_clock_set);
    CHECK(tale.set_clock(mod, 0.5f) == TSUCHI_OK && journey.log == "clock0.5 " && tale.clock(mod, &part) == TSUCHI_OK && part == 0.5f);
    CHECK(tale.set_clock(mod, 0) == TSUCHI_OK && tale.set_clock(mod, 1) == TSUCHI_OK && tale.set_clock(mod, -0.01f) == TSUCHI_INVALID && tale.set_clock(mod, 1.01f) == TSUCHI_INVALID &&
          journey.log == "clock0.5 clock0 clock1 ");
    fake::put<uint64_t>(running.instance + t::game_instance::story_save, old_story);
    CHECK(tale.clock(mod, &part) == TSUCHI_NOT_READY && tale.set_clock(mod, 0.5f) == TSUCHI_NOT_READY); // a clock and no story
    fake::put(running.instance + t::game_instance::story_save, story);

    // Travel: the story in its stage of plain play, by day, with nothing else under way.
    const uint64_t context = running.context, flow_root = fake::get<uint64_t>(context + t::game_context::flow);
    const uint64_t old_top = fake::get<uint64_t>(flow_root + f::child), old_requests = fake::get<uint64_t>(context + t::game_context::requests);
    const uint8_t old_rule = fake::get<uint8_t>(context + t::game_context::rule), old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
    const uint32_t old_paused = fake::get<uint32_t>(running.pauses + t::pause_manager::active);
    const uint64_t story_stage = fake::memory(f::size), play = fake::memory(f::size), inner = fake::memory(f::size);
    fake::put(story_stage, main + t::data::story_flow_table), fake::put(story_stage + f::name, fake::name(u"Story")), fake::put(story_stage + f::child, play);
    fake::put(play, main + t::data::story_play_table), fake::put(play + f::name, fake::name(u"StoryInGame")), fake::put<int32_t>(play + f::status, 1);
    fake::put(flow_root + f::child, story_stage);
    fake::put<uint8_t>(context + t::game_context::rule, uint8_t(t::constant::rule_story));
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1), fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    const uint64_t queue = fake::memory(0x20), queue_table = fake::memory(0x20), mode = fake::memory(0x10);
    fake::put(queue, queue_table);
    fake::put(main + t::data::game_mode, mode);
    in_world(u"Area001_P");
    const auto travelled = [&] { return int(journey.posted.size()); };
    const auto settle = [] { // ten seconds with no journey: the request is taken to have come to nothing
        for (int i = 0; i < 300; ++i) tsuchi::frame(0.03f);
    };
    CHECK(tale.travel(mod, "Area012_P", -1) == TSUCHI_NOT_READY && travelled() == 0); // nowhere to hand a request in
    fake::put(context + t::game_context::requests, queue);
    CHECK(tale.travel(mod, "Area012_P", -1) == TSUCHI_NOT_READY && travelled() == 0); // a queue without the function for it
    fake::put(queue_table + t::request_queue_table::push, &game_push_request);
    CHECK(tale.travel(mod, "Area012_P", -1) == TSUCHI_NOT_READY && travelled() == 0); // the game's own maker of the request is not known
    t::fn::travel_request = game_travel_request;

    // Each thing that keeps the game from taking it.
    const auto refused = [&](TSUCHI_Result with, const char* why) {
        const bool right = tale.travel(mod, "Area012_P", -1) == with && tale.leave_cave(mod) == (with == TSUCHI_BUSY ? TSUCHI_BUSY : TSUCHI_NOT_READY) && travelled() == 0;
        if (!right) std::printf("  not refused as expected: %s\n", why);
        return right;
    };
    fake::put<uint8_t>(context + t::game_context::rule, uint8_t(t::constant::rule_story + 1));
    CHECK(refused(TSUCHI_NOT_READY, "another rule"));
    fake::put<uint8_t>(context + t::game_context::rule, uint8_t(t::constant::rule_story));
    fake::put<uint64_t>(story_stage, main + t::data::story_play_table);
    CHECK(refused(TSUCHI_NOT_READY, "the outer stage is not the story's"));
    fake::put(story_stage, main + t::data::story_flow_table);
    fake::put<uint64_t>(play, main + t::data::story_flow_table);
    CHECK(refused(TSUCHI_BUSY, "the stage within is not plain play"));
    fake::put(play, main + t::data::story_play_table);
    fake::put<uint64_t>(story_stage + f::child, 0);
    CHECK(refused(TSUCHI_BUSY, "no stage within"));
    fake::put(story_stage + f::child, play);
    fake::put(play + f::child, inner), fake::put(inner + f::name, fake::name(u"Menu"));
    CHECK(refused(TSUCHI_BUSY, "a stage within plain play"));
    fake::put<uint64_t>(play + f::child, 0), fake::put<uint64_t>(play + f::pending, inner);
    CHECK(refused(TSUCHI_BUSY, "a stage waiting"));
    fake::put<uint64_t>(play + f::pending, 0), fake::put<int32_t>(play + f::status, 0);
    CHECK(refused(TSUCHI_BUSY, "plain play not running"));
    fake::put<int32_t>(play + f::status, 1), fake::put<int32_t>(play + f::stopped, 1);
    CHECK(refused(TSUCHI_BUSY, "plain play stopped"));
    fake::put<int32_t>(play + f::stopped, 0);
    fake::put<uint8_t>(main + t::data::save_mounted, 1);
    CHECK(refused(TSUCHI_BUSY, "a save under way"));
    fake::put<uint8_t>(main + t::data::save_mounted, 0);
    fake::put<uint64_t>(running.instance + t::game_instance::story_save, old_story);
    CHECK(refused(TSUCHI_NOT_READY, "no story"));
    fake::put(running.instance + t::game_instance::story_save, story);
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
    CHECK(refused(TSUCHI_NOT_READY, "the world has not begun"));
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0x4);
    CHECK(refused(TSUCHI_BUSY, "paused in a way that blocks travel"));
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    fake::put<uint8_t>(clock + t::clock::night, 1);
    CHECK(refused(TSUCHI_NOT_READY, "night"));
    fake::put<uint8_t>(clock + t::clock::night, 0), fake::put<uint8_t>(clock + t::clock::time_up, 1);
    CHECK(refused(TSUCHI_BUSY, "the day has run out"));
    fake::put<uint8_t>(clock + t::clock::time_up, 0);
    fake::put<uint64_t>(w.framework + t::game_framework::clock, 0);
    CHECK(refused(TSUCHI_NOT_READY, "no clock"));
    fake::put(w.framework + t::game_framework::clock, clock);
    fake::put<uint8_t>(mode + t::game_mode::skip_story, 1);
    CHECK(refused(TSUCHI_NOT_READY, "started with no story around the level"));
    fake::put<uint8_t>(mode + t::game_mode::skip_story, 0);
    CHECK(tale.leave_cave(mod) == TSUCHI_NOT_READY && travelled() == 0); // above ground the way out of a cave ends the day
    // An area is named by its world's ID. Text that is no ID is invalid, and an ID that names no area is not found:
    // a number past the game's three digits, a cave's floor, a mod's map while no mod has added one, a placeholder.
    CHECK(tale.travel(mod, "no such area", -1) == TSUCHI_INVALID && tale.travel(mod, "", -1) == TSUCHI_INVALID && tale.travel(mod, nullptr, -1) == TSUCHI_INVALID);
    CHECK(tale.travel(mod, "Area1000_P", -1) == TSUCHI_NOT_FOUND && tale.travel(mod, "Area01_P", -1) == TSUCHI_NOT_FOUND && tale.travel(mod, "Area012", -1) == TSUCHI_NOT_FOUND &&
          tale.travel(mod, "Cave000_F00", -1) == TSUCHI_NOT_FOUND && tale.travel(mod, "example.cave:mine", -1) == TSUCHI_NOT_FOUND && tale.travel(mod, "#12", -1) == TSUCHI_NOT_FOUND &&
          tale.travel(mod, "Area0x2_P", -1) == TSUCHI_NOT_FOUND && travelled() == 0);
    fake::engine().out_of_memory = true;
    CHECK(tale.travel(mod, "Area012_P", -1) == TSUCHI_NO_MEMORY && travelled() == 0);
    fake::engine().out_of_memory = false;

    // Asked: the game's own request, filled in with who travels and where to. Paused in a way that does not matter, it is still taken.
    namespace r = t::travel_request;
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0x1);
    CHECK(tale.travel(mod, "Area012_P", 2) == TSUCHI_OK && travelled() == 1);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    const uint64_t first = journey.posted[0];
    char level[32] = "";
    name_text(fake::get<Name>(first + r::area), level, sizeof level);
    CHECK(fake::get<uint32_t>(first + t::request::source) == t::constant::request_from_menu && from_weak_value(fake::get<uint64_t>(first + r::captain)) == w.hero &&
          !std::strcmp(level, "Area012_P") && fake::get<int32_t>(first + r::base) == 2);
    // Until the journey has come and gone, another request would land on top of it.
    CHECK(tale.travel(mod, "Area003_P", -1) == TSUCHI_BUSY && travelled() == 1);
    for (int i = 0; i < 299; ++i) tsuchi::frame(0.03f);
    CHECK(tale.travel(mod, "Area003_P", -1) == TSUCHI_BUSY && travelled() == 1);
    tsuchi::frame(0.03f); // ten seconds, and no journey began
    CHECK(tale.travel(mod, "Area003_P", -1) == TSUCHI_OK && travelled() == 2 && fake::get<int32_t>(journey.posted[1] + r::base) == -1);
    name_text(fake::get<Name>(journey.posted[1] + r::area), level, sizeof level);
    CHECK(!std::strcmp(level, "Area003_P"));
    fake::put(play + f::child, inner), fake::put(inner + f::name, fake::name(u"FastTravel")); // the journey begins
    for (int i = 0; i < 400; ++i) tsuchi::frame(0.03f);
    fake::put<uint64_t>(play + f::child, 0);
    CHECK(tale.travel(mod, "Area003_P", -1) == TSUCHI_BUSY && travelled() == 2); // it has ended, and this has not looked yet
    tsuchi::frame(0.03f);
    CHECK(tale.travel(mod, "Area999_P", -1) == TSUCHI_OK && travelled() == 3);
    settle();
    // Nobody playing: the request names no one.
    list_actors(t::player_block::captains, {});
    CHECK(tale.travel(mod, "Area001_P", -1) == TSUCHI_OK && fake::get<uint64_t>(journey.posted.back() + r::captain) == 0xFFFFFFFFull);
    list_actors(t::player_block::captains, {w.hero});
    settle();

    // Out of a cave: the plain request, when the level is a cave. From a cave no area is travelled to.
    const int before = travelled();
    in_world(u"Cave004_F01");
    CHECK(tale.travel(mod, "Area001_P", -1) == TSUCHI_NOT_READY && travelled() == before);
    CHECK(tale.leave_cave(mod) == TSUCHI_OK && travelled() == before + 1 && tale.leave_cave(mod) == TSUCHI_BUSY);
    const uint64_t leave = journey.posted.back();
    bool rest_zero = true;
    for (uint32_t i = 8; i < t::leave_request::size; ++i)
        rest_zero = rest_zero && (fake::get<uint8_t>(leave + i) == 0 || (i >= t::request::self && i < t::request::self + 8) || (i >= t::request::id && i < t::request::id + 4) ||
                                  (i >= t::request::source && i < t::request::source + 4));
    CHECK(fake::get<uint64_t>(leave) == main + t::data::leave_request_table && fake::get<uint64_t>(leave + t::request::self) == leave &&
          fake::get<uint32_t>(leave + t::request::id) == t::constant::leave_request_id && fake::get<uint32_t>(leave + t::request::source) == t::constant::request_from_menu && rest_zero);
    settle();
    fake::engine().out_of_memory = true;
    CHECK(tale.leave_cave(mod) == TSUCHI_NO_MEMORY && travelled() == before + 1);
    fake::engine().out_of_memory = false;
    in_world(u"Area001_P");

    // Through a portal: an actor with the part that leads somewhere.
    const uint64_t mouth = tabled(table_a(), u"Mouth", u"Mouth", 0, 0), part_class = fake::object(u"PortalPart", w.root), leads = fake::memory(0x60);
    const uint64_t ghost = tabled(table_a(), u"Ghost", u"Ghost", 0, 0);
    const TSUCHI_Actor gone = handle(ghost);
    fake::destroy(ghost);
    fake::put<int32_t>(leads + t::portal_part::leads_to, 3);
    CHECK(tale.enter_portal(mod, gone) == TSUCHI_NOT_FOUND && tale.enter_portal(mod, TSUCHI_Actor{to_handle(w.idol_asset)}) == TSUCHI_NOT_FOUND);
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_NOT_READY); // the class of that part is not known yet, nor how to ask an actor for it
    fake::put(main + t::data::portal_part_class, part_class);
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_NOT_READY);
    fake::put(table_a() + t::actor_table::find_component, &game_find_component);
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_INVALID && journey.asked_class == part_class); // it has no such part
    journey.portal_part = leads;
    fake::put<int32_t>(leads + t::portal_part::leads_to, -1);
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_INVALID && travelled() == before + 1); // it leads nowhere
    fake::put<int32_t>(leads + t::portal_part::leads_to, 0);
    fake::put<uint8_t>(clock + t::clock::night, 1);
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_NOT_READY && travelled() == before + 1);
    fake::put<uint8_t>(clock + t::clock::night, 0);
    fake::engine().out_of_memory = true;
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_NO_MEMORY && travelled() == before + 1);
    fake::engine().out_of_memory = false;
    CHECK(tale.enter_portal(mod, handle(mouth)) == TSUCHI_OK && travelled() == before + 2 && tale.enter_portal(mod, handle(mouth)) == TSUCHI_BUSY);
    const uint64_t through = journey.posted.back();
    CHECK(fake::get<uint64_t>(through) == main + t::data::portal_request_table && fake::get<uint64_t>(through + t::request::self) == through &&
          fake::get<uint32_t>(through + t::request::id) == t::constant::portal_request_id && fake::get<uint32_t>(through + t::request::source) == t::constant::request_from_menu &&
          from_weak_value(fake::get<uint64_t>(through + t::portal_request::captain)) == w.hero && from_weak_value(fake::get<uint64_t>(through + t::portal_request::portal)) == mouth);
    settle();

    // Treasures: how many of one the story has collected, and one recorded as collected, as its delivery does.
    {
        uint32_t count = 99;
        const auto log_is = [](const char* expected) {
            const bool same = hoard.log == expected;
            if (!same) std::printf("  log: [%s]\n", hoard.log.c_str());
            hoard.log.clear();
            return same;
        };
        const auto row_of = [](const char16_t* name, int32_t sparklium, int32_t poko) {
            const uint64_t row = fake::memory(0x20);
            fake::put(row + t::treasure_row::sparklium, sparklium), fake::put(row + t::treasure_row::poko, poko);
            const std::u16string text(name);
            hoard.rows[fake::engine_name(const_cast<char16_t*>(text.data()), text.size(), 1)] = row;
        };
        hoard.story = story, hoard.context = context;
        row_of(u"KEY", 30, 5), row_of(u"DUD", 0, 2);
        const std::u16string bad(u"BAD"); // the game answers with something that is not memory
        hoard.rows[fake::engine_name(const_cast<char16_t*>(bad.data()), bad.size(), 1)] = 0x40;
        CHECK(tale.treasure_count(mod, "KEY", &count) == TSUCHI_NOT_READY && tale.give_treasure(mod, "KEY", true) == TSUCHI_NOT_READY); // the game's functions are not known yet
        t::fn::find_treasure_row = game_find_treasure_row, t::fn::find_story_record = game_find_story_record;
        t::fn::collect_treasure = game_collect_treasure, t::fn::add_sparklium = game_add_sparklium;
        CHECK(tale.treasure_count(mod, "KEY", &count) == TSUCHI_OK && count == 0 && hoard.well_formed); // never collected: it has no record
        CHECK(tale.treasure_count(mod, "BAD", &count) == TSUCHI_NOT_FOUND && tale.give_treasure(mod, "BAD", true) == TSUCHI_NOT_FOUND && log_is(""));
        CHECK(tale.treasure_count(mod, "NOPE", &count) == TSUCHI_NOT_FOUND && tale.treasure_count(mod, "", &count) == TSUCHI_INVALID &&
              tale.treasure_count(mod, "\xff", &count) == TSUCHI_INVALID && tale.give_treasure(mod, "NOPE", true) == TSUCHI_NOT_FOUND &&
              tale.give_treasure(mod, "", true) == TSUCHI_INVALID && log_is(""));
        // A treasure is named by an ID, with the one rule for what is not known: text that is no ID is invalid, and an
        // ID that names no treasure is not found, a mod's and a placeholder among them.
        CHECK(tale.treasure_count(mod, "no such", &count) == TSUCHI_INVALID && tale.give_treasure(mod, "a:b:c", true) == TSUCHI_INVALID &&
              tale.treasure_count(mod, "example.gems:ruby", &count) == TSUCHI_NOT_FOUND && tale.give_treasure(mod, "example.gems:ruby", true) == TSUCHI_NOT_FOUND &&
              tale.treasure_count(mod, "#3", &count) == TSUCHI_NOT_FOUND && tale.give_treasure(mod, "#3", true) == TSUCHI_NOT_FOUND && log_is(""));
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_OK && log_is("collect:KEY 30 5 sparklium:30+shown ") && tale.treasure_count(mod, "KEY", &count) == TSUCHI_OK && count == 1);
        CHECK(tale.give_treasure(mod, "KEY", false) == TSUCHI_OK && log_is("collect:KEY 30 5 sparklium:30 ") && tale.treasure_count(mod, "KEY", &count) == TSUCHI_OK && count == 2);
        CHECK(tale.give_treasure(mod, "DUD", true) == TSUCHI_OK && log_is("collect:DUD 0 2 ") && hoard.well_formed); // worth no Sparklium: none is added
        // Only in the rules that keep the story's stores, with a story and a level.
        fake::put<uint8_t>(context + t::game_context::rule, 4);
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_NOT_READY && log_is(""));
        fake::put<uint8_t>(context + t::game_context::rule, 6);
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_OK && log_is("collect:KEY 30 5 sparklium:30+shown "));
        fake::put<uint8_t>(context + t::game_context::rule, uint8_t(t::constant::rule_story));
        fake::put<uint64_t>(running.instance + t::game_instance::story_save, old_story);
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_NOT_READY && tale.treasure_count(mod, "KEY", &count) == TSUCHI_NOT_READY && log_is(""));
        fake::put(running.instance + t::game_instance::story_save, story);
        fake::put<uint64_t>(main + t::data::game_framework, 0);
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_NOT_READY && tale.treasure_count(mod, "KEY", &count) == TSUCHI_NOT_READY && log_is(""));
        fake::put<uint64_t>(main + t::data::game_framework, w.framework);
        t::fn::add_sparklium = nullptr;
        CHECK(tale.give_treasure(mod, "KEY", true) == TSUCHI_NOT_READY && log_is(""));
        t::fn::add_sparklium = game_add_sparklium;
    }

    // As it was, for what comes after.
    fake::put(flow_root + f::child, old_top), fake::put(context + t::game_context::requests, old_requests);
    fake::put<uint8_t>(context + t::game_context::rule, old_rule), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, old_paused);
    fake::put(running.instance + t::game_instance::story_save, old_story), fake::put(w.framework + t::game_framework::clock, old_clock);
}

// ---- Saves ------------------------------------------------------------------------------

struct Saved {
    std::string log;
    bool succeeds = true;
} saved;
std::string narrow(uint64_t name) {
    std::string text;
    if (name && name < 0x10000) return "<nowhere>"; // not memory
    for (const auto* at = reinterpret_cast<const char16_t*>(name); at && *at; ++at) text += char(*at);
    return text;
}
uint64_t game_save_write(uint64_t, uint64_t with_screen, uint64_t name, uint64_t user) {
    saved.log += "write:" + narrow(name) + ((with_screen & 0xFF) ? "+screen" : "") + "/" + std::to_string(user & 0xFFFFFFFF) + " ";
    return saved.succeeds;
}
uint64_t game_save_delete(uint64_t, uint64_t, uint64_t name, uint64_t) {
    saved.log += "delete:" + narrow(name) + " ";
    return saved.succeeds;
}

void saving() {
    const TSUCHI_SaveService& vault = *tsuchi::host.save;
    const uint64_t main = fake::main_image();
    const t::Slot& write = t::slot::save_write;
    const t::Slot& remove = t::slot::save_delete;
    const uint64_t write_entry = main + write.table + write.offset, delete_entry = main + remove.table + remove.offset;
    uint32_t number = 99;
    uint64_t id = 0;
    void* unused = nullptr;
    const auto log_is = [](const char* expected) {
        const bool same = saved.log == expected;
        if (!same) std::printf("  log: [%s]\n", saved.log.c_str());
        saved.log.clear();
        return same;
    };
    // A file's name in the game's memory, as the game would hand it over.
    const auto in_game = [](const char16_t* name) -> uint64_t {
        if (!name) return 0;
        const std::u16string text(name);
        const uint64_t at = fake::memory((text.size() + 1) * 2);
        std::memcpy(reinterpret_cast<void*>(at), text.c_str(), (text.size() + 1) * 2);
        return at;
    };
    const auto game_writes = [&](const char16_t* name, bool with_screen = false) {
        return (thunks::call(fake::get<uint64_t>(write_entry), 0x5A5E, with_screen, in_game(name), 7) & 0xFF) != 0;
    };
    const auto game_deletes = [&](const char16_t* name) { return (thunks::call(fake::get<uint64_t>(delete_entry), 0x5A5E, 0, in_game(name), 7) & 0xFF) != 0; };

    // The slot in use, from the game's settings.
    const uint64_t config = fake::get<uint64_t>(running.instance + t::game_instance::config_save);
    CHECK(config != 0);
    fake::put<int32_t>(config + t::config_save::slot, 2);
    TSUCHI_Mod* self = tsuchi::mod_create("tsuchi.self.save");
    // The game's two functions, with the test's own at the far end standing in for them. First with one of them
    // unfamiliar, then with the other: either way both are left exactly as they were.
    for (uint64_t address : {main + write.stock, main + write.stock + 8, main + remove.stock, main + remove.stock + 8}) fake::code().push_back(address);
    fake::put(write_entry, main + write.stock + 8), fake::put(delete_entry, main + remove.stock);
    tsuchi::saves_start(self);
    CHECK(vault.hold(mod) == TSUCHI_NOT_READY && fake::get<uint64_t>(write_entry) == main + write.stock + 8 && fake::get<uint64_t>(delete_entry) == main + remove.stock);
    fake::put(write_entry, main + write.stock), fake::put(delete_entry, main + remove.stock + 8);
    TSUCHI_Mod* game = tsuchi::mod_create("the.save");
    CHECK(tsuchi::hook_wrap(game, write_entry, main + write.stock, reinterpret_cast<void*>(&game_save_write), 0, &id, &unused) == TSUCHI_OK);
    tsuchi::saves_start(self);
    CHECK(vault.slot(mod, &number) == TSUCHI_OK && number == 2);
    CHECK(vault.hold(mod) == TSUCHI_NOT_READY && vault.release(mod) == TSUCHI_NOT_FOUND && fake::get<uint64_t>(delete_entry) == main + remove.stock + 8);
    CHECK(game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 ") && tsuchi::save_counts().passed == 0); // not wrapped after all: it goes straight to the game
    fake::put<int32_t>(config + t::config_save::slot, -1);
    CHECK(vault.slot(mod, &number) == TSUCHI_NOT_FOUND);
    fake::put<int32_t>(config + t::config_save::slot, int32_t(t::constant::save_slots));
    CHECK(vault.slot(mod, &number) == TSUCHI_NOT_FOUND);
    fake::put<int32_t>(config + t::config_save::slot, int32_t(t::constant::save_slots) - 1);
    CHECK(vault.slot(mod, &number) == TSUCHI_OK && number == t::constant::save_slots - 1);
    fake::put<uint64_t>(running.instance + t::game_instance::config_save, 0);
    CHECK(vault.slot(mod, &number) == TSUCHI_NOT_READY);
    fake::put(running.instance + t::game_instance::config_save, config);

    // Both the game's own: wrapped.
    fake::put(delete_entry, main + remove.stock);
    CHECK(tsuchi::hook_wrap(game, delete_entry, main + remove.stock, reinterpret_cast<void*>(&game_save_delete), 0, &id, &unused) == TSUCHI_OK);
    tsuchi::saves_start(self);
    const uint32_t owned = tsuchi::owned_count(mod);
    // With nobody holding, everything goes on to the game, with what the game gave.
    CHECK(game_writes(u"A/single.sav", true) && log_is("write:A/single.sav+screen/7 ") && game_deletes(u"B/hero.sav") && log_is("delete:B/hero.sav ") &&
          tsuchi::save_counts().passed == 2 && tsuchi::save_counts().held_back == 0);
    saved.succeeds = false;
    CHECK(!game_writes(u"A/single.sav") && !game_deletes(u"A/single.sav") && log_is("write:A/single.sav/7 delete:A/single.sav "));
    saved.succeeds = true;

    // Held: files in a slot are not written or deleted, and the game is told they were. Its settings still go through.
    refused_size = 16 * (sizeof(tsuchi::Hold) + 8); // no memory for the first holders
    CHECK(vault.hold(mod) == TSUCHI_NO_MEMORY && tsuchi::owned_count(mod) == owned && game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 "));
    refused_size = 0;
    CHECK(vault.hold(mod) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1 && vault.hold(mod) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    const uint32_t passed = tsuchi::save_counts().passed;
    saved.succeeds = false; // the game would have failed; it is not asked
    CHECK(game_writes(u"A/single.sav") && game_deletes(u"F/hero/001.sav") && log_is("") && tsuchi::save_counts().held_back == 2 &&
          !std::strcmp(tsuchi::save_counts().last, "F/hero/001.sav") && tsuchi::save_counts().passed == passed);
    saved.succeeds = true;
    CHECK(game_writes(u"config.sav") && game_writes(u"a/single.sav") && game_writes(u"AB/single.sav") && game_writes(u"/single.sav") && game_writes(nullptr) &&
          game_writes(u"1/single.sav") && game_deletes(u"GameUserSettings.ini") &&
          log_is("write:config.sav/7 write:a/single.sav/7 write:AB/single.sav/7 write:/single.sav/7 write:/7 write:1/single.sav/7 delete:GameUserSettings.ini ") &&
          tsuchi::save_counts().passed == passed + 7 && tsuchi::save_counts().held_back == 2);
    // A name that is not in memory goes on to the game untouched.
    CHECK((thunks::call(fake::get<uint64_t>(write_entry), 0x5A5E, 0, 0x40, 7) & 0xFF) != 0 && log_is("write:<nowhere>/7 ") && tsuchi::save_counts().held_back == 2);
    // Two hold: held until the second lets go. A hold ends with its mod.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.guest"), *poor = tsuchi::mod_create("actor.guest2");
    refused_multiple = 40; // no room to note whose the hold is
    CHECK(vault.hold(poor) == TSUCHI_NO_MEMORY && vault.release(poor) == TSUCHI_NOT_FOUND);
    refused_multiple = 0;
    tsuchi::mod_stop(poor);
    CHECK(vault.release(rival) == TSUCHI_NOT_FOUND && vault.hold(rival) == TSUCHI_OK && vault.release(mod) == TSUCHI_OK && tsuchi::owned_count(mod) == owned &&
          vault.release(mod) == TSUCHI_NOT_FOUND);
    CHECK(game_writes(u"A/single.sav") && log_is("") && tsuchi::save_counts().held_back == 3);
    // A hold let go by its mod ends there: with nobody else holding, the game writes at once.
    in_stages({u"Story", u"StoryInGame"});
    CHECK(vault.release(rival) == TSUCHI_OK && game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 ") && tsuchi::save_counts().held_back == 3);
    // A hold that ends because its mod stopped, in the middle of a story, leaves the slots closed:
    // what the mod was keeping out of the save is not written the moment the mod is gone.
    CHECK(vault.hold(rival) == TSUCHI_OK);
    tsuchi::mod_stop(rival);
    CHECK(game_writes(u"A/single.sav") && game_deletes(u"A/single.sav") && log_is("") && tsuchi::save_counts().held_back == 5);
    CHECK(game_writes(u"config.sav") && log_is("write:config.sav/7 ")); // the settings are no slot's
    in_stages({u"Story", u"DayEnd"});
    CHECK(game_writes(u"A/single.sav") && log_is("") && tsuchi::save_counts().held_back == 6);
    // At the title screen the game has let go of what it had loaded, and the slots are open again from then on.
    in_stages({u"Title", u"MenuTitle"});
    CHECK(game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 ") && tsuchi::save_counts().held_back == 6);
    in_stages({u"Story", u"StoryInGame"});
    CHECK(game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 ") && tsuchi::save_counts().held_back == 6);
    // A mod that stops at the title screen while holding leaves nothing closed.
    TSUCHI_Mod* late = tsuchi::mod_create("actor.guest3");
    in_stages({u"Title"});
    CHECK(vault.hold(late) == TSUCHI_OK);
    tsuchi::mod_stop(late);
    in_stages({u"Story", u"StoryInGame"});
    CHECK(game_writes(u"A/single.sav") && log_is("write:A/single.sav/7 ") && tsuchi::save_counts().held_back == 6);
}

// ---- A clip held on a skeletal mesh ---------------------------------------------------------

void animating() {
    namespace a = t::game_actor;
    const TSUCHI_AnimationService& motion = *tsuchi::host.animation;
    const auto part = [](uint64_t object) { return TSUCHI_Component{to_handle(object)}; };
    const auto object = [](uint64_t address) { return TSUCHI_Object{to_handle(address)}; };
    const uint8_t by_blueprint = uint8_t(t::constant::animation_by_blueprint), single = uint8_t(t::constant::animation_single);
    const auto skeletal_part = [&](const std::u16string& name, uint64_t parent, uint64_t owner) {
        const uint64_t mesh = fake::engine_object(name, skeletal_class, 0x100);
        fake::put(mesh + t::scene_component::owner, owner);
        fake::put<uint8_t>(mesh + kAnimationModeAt, by_blueprint);
        saved_by_default(mesh);
        attach_under(parent, mesh);
        parts_of[owner].push_back(mesh);
        return mesh;
    };
    const auto mode_of = [](uint64_t mesh) { return fake::get<uint8_t>(mesh + kAnimationModeAt); };
    const auto log_is = [](const std::string& expected) {
        const bool same = part_log == expected;
        if (!same) std::printf("  log: [%s]\n", part_log.c_str());
        part_log.clear();
        return same;
    };
    const uint32_t owned = tsuchi::owned_count(mod);
    const std::string back = "mode" + std::to_string(by_blueprint) + ":";
    TSUCHI_Clip told{};

    // A dancer with a body that has a skeleton, a hat that has none, and a few clips.
    const uint64_t dancer = tabled(table_a(), u"Dancer", u"Dancer", 0, 0);
    registers(dancer);
    const uint64_t dancer_root = fake::get<uint64_t>(dancer + a::root);
    fake::put(dancer_root + t::scene_component::owner, dancer);
    parts_of[dancer].push_back(dancer_root);
    const uint64_t body = skeletal_part(u"Body", dancer_root, dancer), tail = skeletal_part(u"Tail", dancer_root, dancer);
    const uint64_t hat = fake::engine_object(u"Hat", mesh_class, 0x100);
    fake::put<uint8_t>(hat + kAnimationModeAt, by_blueprint); // whatever a mesh without a skeleton has there
    walk_clip = fake::engine_object(u"Walk", clip_class);
    const uint64_t bite = fake::engine_object(u"Bite", clip_class), passing = fake::engine_object(u"Passing", clip_class);
    dancer_mesh = tail;
    part_log.clear();

    // What cannot have a clip played on it, and what is no clip.
    const TSUCHI_Component gone_part{to_handle(body) ^ 1};
    fake::destroy(passing);
    CHECK(motion.hold_clip(mod, gone_part, object(walk_clip), false, 1) == TSUCHI_NOT_FOUND && motion.hold_clip(mod, part(body), TSUCHI_Object{to_handle(walk_clip) ^ 1}, false, 1) == TSUCHI_NOT_FOUND &&
          motion.hold_clip(mod, part(dancer), object(walk_clip), false, 1) == TSUCHI_NOT_FOUND);
    CHECK(motion.hold_clip(mod, part(hat), object(walk_clip), false, 1) == TSUCHI_UNSUPPORTED && motion.hold_clip(mod, part(body), object(red_material), false, 1) == TSUCHI_INVALID &&
          motion.hold_clip(mod, part(body), object(walk_clip), false, std::nanf("")) == TSUCHI_INVALID && log_is(""));
    for (uint8_t other : {single, uint8_t(single + 1)}) { // a mesh the game plays one clip on itself, or animates some other way
        fake::put<uint8_t>(body + kAnimationModeAt, other);
        CHECK(motion.hold_clip(mod, part(body), object(walk_clip), false, 1) == TSUCHI_UNSUPPORTED && log_is("") && tsuchi::clip_holds() == 0);
    }
    fake::put<uint8_t>(body + kAnimationModeAt, by_blueprint);
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && !told.clip.id && motion.release_clip(mod, part(body)) == TSUCHI_NOT_FOUND && motion.seek_clip(mod, part(body), 1) == TSUCHI_NOT_FOUND &&
          motion.set_clip_rate(mod, part(body), 1) == TSUCHI_NOT_FOUND && tsuchi::owned_count(mod) == owned);

    refused_size = 16 * (sizeof(tsuchi::Hold) + 16); // no memory for the first holds
    CHECK(motion.hold_clip(mod, part(body), object(walk_clip), true, 1) == TSUCHI_NO_MEMORY && log_is("") && mode_of(body) == by_blueprint && tsuchi::clip_holds() == 0 &&
          tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    // Played, and held: the mesh is animated by the one clip, at the rate asked for.
    CHECK(motion.hold_clip(mod, part(body), object(walk_clip), true, 1) == TSUCHI_OK && log_is("play:Walk+loop rate:1 ") && mode_of(body) == single && tsuchi::clip_holds() == 1 &&
          tsuchi::owned_count(mod) == owned + 1);
    motions[body].position = 0.5f;
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == walk_clip && told.looping && told.playing && told.position == 0.5f && told.rate == 1);
    // Another clip on the same mesh changes what the mod holds there.
    CHECK(motion.hold_clip(mod, part(body), object(bite), false, 2) == TSUCHI_OK && log_is("play:Bite rate:2 ") && tsuchi::clip_holds() == 1 && tsuchi::owned_count(mod) == owned + 1);
    motions[body].playing = false;
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == bite && !told.looping && !told.playing && told.position == 0 && told.rate == 2);
    // Its rate, and a place in it.
    CHECK(motion.set_clip_rate(mod, part(body), 0) == TSUCHI_OK && motion.set_clip_rate(mod, part(body), -0.5f) == TSUCHI_OK && log_is("rate:0 rate:-0.5 "));
    CHECK(motion.seek_clip(mod, part(body), 1.25f) == TSUCHI_OK && motion.seek_clip(mod, part(body), 0) == TSUCHI_OK && log_is("seek:1.25 seek:0 ") &&
          motion.seek_clip(mod, part(body), -0.01f) == TSUCHI_INVALID && motion.seek_clip(mod, part(body), std::nanf("")) == TSUCHI_INVALID && log_is(""));
    // The engine registers the mesh anew and starts its clip again from what the mesh kept: the held rate, place and
    // looping stay, so a pose held still stays still.
    CHECK(motion.set_clip_rate(mod, part(body), 0) == TSUCHI_OK && motion.seek_clip(mod, part(body), 0.75f) == TSUCHI_OK && log_is("rate:0 seek:0.75 "));
    registers_anew(body);
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == bite && told.rate == 0 && told.position == 0.75f && !motions[body].looping);
    // One mod at a time holds a mesh. Anyone can ask what is held on it.
    TSUCHI_Mod* rival = tsuchi::mod_create("actor.guest7"), *poor = tsuchi::mod_create("actor.guest8");
    CHECK(motion.hold_clip(rival, part(body), object(walk_clip), false, 1) == TSUCHI_BUSY && motion.release_clip(rival, part(body)) == TSUCHI_NOT_FOUND &&
          motion.seek_clip(rival, part(body), 1) == TSUCHI_NOT_FOUND && motion.set_clip_rate(rival, part(body), 1) == TSUCHI_NOT_FOUND && log_is("") &&
          motion.clip(rival, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == bite);
    // The engine cannot be asked: nothing is changed, and what is held stays held.
    fake::engine().out_of_memory = true;
    CHECK(motion.set_clip_rate(mod, part(body), 3) == TSUCHI_NOT_READY && motion.seek_clip(mod, part(body), 3) == TSUCHI_NOT_READY && motion.clip(mod, part(body), &told) == TSUCHI_NOT_READY &&
          motion.hold_clip(mod, part(body), object(walk_clip), true, 1) == TSUCHI_NOT_READY && motion.hold_clip(mod, part(tail), object(walk_clip), true, 1) == TSUCHI_NOT_READY &&
          log_is("") && tsuchi::clip_holds() == 1 && tsuchi::owned_count(mod) == owned + 1 && mode_of(tail) == by_blueprint);
    fake::engine().out_of_memory = false;
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == bite && !told.looping);
    // The engine runs out of memory once a clip begins. The rate was made ready before, so a new hold and a
    // change of what is held both play the clip at its rate, and the hold says the clip that plays.
    engine_starves_after_playing = true;
    CHECK(motion.hold_clip(mod, part(tail), object(walk_clip), true, 1) == TSUCHI_OK && motions[tail].clip == walk_clip && motions[tail].rate == 1 &&
          tsuchi::clip_holds() == 2);
    fake::engine().out_of_memory = false;
    CHECK(motion.hold_clip(mod, part(body), object(walk_clip), true, 3) == TSUCHI_OK && motions[body].clip == walk_clip && motions[body].rate == 3);
    fake::engine().out_of_memory = false;
    CHECK(motion.clip(mod, part(body), &told) == TSUCHI_OK && from_handle(told.clip.id) == walk_clip && told.looping && told.rate == 3);
    engine_starves_after_playing = false;
    CHECK(motion.release_clip(mod, part(tail)) == TSUCHI_OK && tsuchi::clip_holds() == 1 && tsuchi::owned_count(mod) == owned + 1);
    fake::put<uint8_t>(tail + kAnimationModeAt, by_blueprint), motions.erase(tail);
    part_log.clear();
    // Stopped: the mesh's blueprint animates it again.
    CHECK(motion.release_clip(mod, part(body)) == TSUCHI_OK && log_is(back + "Body ") && mode_of(body) == by_blueprint && tsuchi::clip_holds() == 0 && tsuchi::owned_count(mod) == owned &&
          motion.release_clip(mod, part(body)) == TSUCHI_NOT_FOUND && motion.clip(mod, part(body), &told) == TSUCHI_OK && !told.clip.id);

    // No memory for the hold: nothing is played.
    const size_t out = blocks_out.size();
    refused_multiple = 40; // nor to note whose it is
    CHECK(motion.hold_clip(poor, part(body), object(walk_clip), false, 1) == TSUCHI_NO_MEMORY && log_is("") && tsuchi::clip_holds() == 0 && blocks_out.size() == out);
    refused_multiple = 0;
    // Two meshes, held apart. One hold ends with its mod, the other when the actor leaves play.
    CHECK(motion.hold_clip(mod, part(body), object(walk_clip), false, 1) == TSUCHI_OK && motion.hold_clip(rival, part(tail), object(bite), true, 0) == TSUCHI_OK &&
          log_is("play:Walk rate:1 play:Bite+loop rate:0 ") && tsuchi::clip_holds() == 2);
    tsuchi::mod_stop(rival);
    CHECK(log_is(back + "Tail ") && mode_of(tail) == by_blueprint && mode_of(body) == single && tsuchi::clip_holds() == 1);
    CHECK(motion.hold_clip(mod, part(tail), object(bite), true, 0) == TSUCHI_OK && tsuchi::clip_holds() == 2 && tsuchi::owned_count(mod) == owned + 2);
    // Another actor leaving play changes nothing here.
    const uint64_t bystander = tabled(table_a(), u"Bystander", u"Bystander", 0, 0);
    registers(bystander);
    part_log.clear();
    ends_play(bystander);
    CHECK(log_is("") && tsuchi::clip_holds() == 2 && mode_of(body) == single && mode_of(tail) == single);
    ends_play(dancer);
    CHECK(mode_of(body) == by_blueprint && mode_of(tail) == by_blueprint && tsuchi::clip_holds() == 0 && tsuchi::owned_count(mod) == owned && part_log.find(back + "Body ") != std::string::npos &&
          part_log.find(back + "Tail ") != std::string::npos);
    // A mesh that is destroyed while held: the hold ends at the next frame, and nothing is asked of what is gone.
    registers(dancer);
    const uint64_t wing = skeletal_part(u"Wing", dancer_root, dancer);
    CHECK(motion.hold_clip(mod, part(wing), object(walk_clip), true, 1) == TSUCHI_OK && tsuchi::clip_holds() == 1);
    part_log.clear();
    fake::destroy(wing);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::clip_holds() == 0 && tsuchi::owned_count(mod) == owned && log_is(""));
}

// ---- A slot's files, and a stand-in for a slot ---------------------------------------------

// The made-up game's save: files by where the system's file functions find them, once the game has opened it.
struct Disk {
    std::map<std::string, std::string> files;
    std::string log;
    int sessions = 0, open_files = 0, open_folders = 0, most_folders = 0;
    bool mounts = true, well_formed = true, read_fails = false, size_fails = false, backward = false, short_reads = false;
    uint32_t open_error = t::constant::fs_no_such_path;
    std::string unreadable; // a file whose path has this in it cannot be opened, though it is there
    std::string short_in, failing_in; // and one of which the system gives less than was asked for, or nothing
    std::vector<int32_t> modes;       // by a file's number, less one: how it was opened
    std::set<std::string> folders;    // folders that were made, or had a file taken out of them
    std::string changes;              // what was changed in the save, in order
    std::string refuse;               // a kind of change the system refuses
    int commits = 0;
    std::vector<std::string> opened;                                        // by a file's number, less one
    std::vector<std::vector<std::pair<std::string, int64_t>>> listings;     // by a folder's number, less one: names, and sizes or -1 for a folder
    std::vector<size_t> next;
} disk;
bool game_save_begin(int32_t user, bool wait, bool section) {
    disk.well_formed = disk.well_formed && user == 0 && !wait && !section && disk.sessions == 0; // a second opening is an error on the screen
    if (!disk.mounts) return false;
    ++disk.sessions;
    disk.log += "begin ";
    return true;
}
void game_save_end() {
    disk.well_formed = disk.well_formed && disk.sessions == 1 && !disk.open_files && !disk.open_folders;
    --disk.sessions;
    disk.log += "end ";
}
uint32_t game_fs_open_file(void* out, void* path, int32_t mode) {
    const bool saved = !std::strncmp(static_cast<const char*>(path), "save:", 5); // the save has to be open; what the game ships with always is
    disk.well_formed = disk.well_formed && disk.sessions == (saved ? 1 : 0) && (mode == int32_t(t::constant::fs_read) || (saved && mode == int32_t(t::constant::fs_write)));
    const auto found = disk.files.find(static_cast<const char*>(path));
    if (found == disk.files.end()) return disk.open_error;
    if (!disk.unreadable.empty() && found->first.find(disk.unreadable) != std::string::npos) return 0x4321;
    disk.opened.push_back(found->first), disk.modes.push_back(mode);
    ++disk.open_files;
    *static_cast<uint64_t*>(out) = disk.opened.size();
    return 0;
}
void game_fs_close_file(void*) { --disk.open_files; }
uint32_t game_fs_file_size(void* out, void* file) {
    if (disk.size_fails) return 5;
    *static_cast<int64_t*>(out) = int64_t(disk.files[disk.opened[reinterpret_cast<uint64_t>(file) - 1]].size());
    return 0;
}
uint32_t game_fs_read_file(void* got, void* file, int64_t offset, void* buffer, uint64_t size) {
    if (disk.read_fails) return 9;
    const std::string& path = disk.opened[reinterpret_cast<uint64_t>(file) - 1];
    if (!disk.failing_in.empty() && path.find(disk.failing_in) != std::string::npos) return 9;
    const std::string& bytes = disk.files[path];
    disk.well_formed = disk.well_formed && offset >= 0 && uint64_t(offset) + size <= bytes.size() && size > 0; // the system refuses a read past the end
    if (disk.short_reads || (!disk.short_in.empty() && path.find(disk.short_in) != std::string::npos)) --size; // the system gave less than was asked for
    std::memcpy(buffer, bytes.data() + offset, size);
    *static_cast<uint64_t*>(got) = size;
    disk.log += "read" + std::to_string(offset) + "+" + std::to_string(size) + " ";
    return 0;
}
uint32_t game_fs_open_folder(void* out, void* path, int32_t mode) {
    const bool saved = !std::strncmp(static_cast<const char*>(path), "save:", 5);
    disk.well_formed = disk.well_formed && disk.sessions == (saved ? 1 : 0) && mode == int32_t(t::constant::fs_list_all);
    const std::string prefix = std::string(static_cast<const char*>(path)) + "/";
    std::vector<std::pair<std::string, int64_t>> entries;
    for (const auto& [where, bytes] : disk.files) {
        if (where.compare(0, prefix.size(), prefix)) continue;
        const std::string rest = where.substr(prefix.size());
        const size_t slash = rest.find('/');
        const std::pair<std::string, int64_t> entry{rest.substr(0, slash), slash == std::string::npos ? int64_t(bytes.size()) : -1};
        if (std::find(entries.begin(), entries.end(), entry) == entries.end()) entries.push_back(entry);
    }
    if (entries.empty() && !disk.folders.count(static_cast<const char*>(path))) return t::constant::fs_no_such_path;
    if (disk.backward) std::reverse(entries.begin(), entries.end());
    disk.listings.push_back(entries), disk.next.push_back(0);
    disk.most_folders = std::max(disk.most_folders, ++disk.open_folders);
    *static_cast<uint64_t*>(out) = disk.listings.size();
    return 0;
}
uint32_t game_fs_read_folder(void* got, void* entries, void* folder, int64_t room) {
    const size_t which = reinterpret_cast<uint64_t>(folder) - 1;
    disk.well_formed = disk.well_formed && room == 1;
    *static_cast<int64_t*>(got) = 0;
    if (disk.next[which] >= disk.listings[which].size()) return 0;
    const auto& [name, bytes] = disk.listings[which][disk.next[which]++];
    auto* entry = static_cast<uint8_t*>(entries);
    std::memset(entry, 0xEE, t::fs_entry::size); // nothing but what is asked after is to be counted on
    std::memcpy(entry + t::fs_entry::name, name.c_str(), name.size() + 1);
    entry[t::fs_entry::kind] = uint8_t(bytes < 0 ? t::constant::fs_entry_folder : t::constant::fs_entry_folder + 1);
    std::memcpy(entry + t::fs_entry::bytes, &bytes, 8);
    *static_cast<int64_t*>(got) = 1;
    return 0;
}
void game_fs_close_folder(void*) { --disk.open_folders; }

// The made-up game's own answers about its saves, and its own reading of one.
uint64_t game_save_exists(uint64_t, uint64_t name, uint64_t user) {
    saved.log += "exists:" + narrow(name) + "/" + std::to_string(user & 0xFFFFFFFF) + " ";
    return saved.succeeds;
}
uint64_t game_save_exists_result(uint64_t, uint64_t name, uint64_t) {
    saved.log += "result:" + narrow(name) + " ";
    return 3;
}
uint64_t game_save_read(uint64_t, uint64_t with_screen, uint64_t name, uint64_t, uint64_t out) {
    saved.log += "read:" + narrow(name) + ((with_screen & 0xFF) ? "+screen" : "") + (out ? " " : "? ");
    return saved.succeeds;
}

void filing() {
    const TSUCHI_SaveService& vault = *tsuchi::host.save;
    const uint64_t main = fake::main_image();
    char buffer[4096];
    uint64_t needed = 77, size = 77, id = 0;
    void* unused = nullptr;
    const auto log_is = [](std::string& log, const char* expected) {
        const bool same = log == expected;
        if (!same) std::printf("  log: [%s]\n", log.c_str());
        log.clear();
        return same;
    };
    const auto closed = [] { // nothing left open, and nothing done the system would refuse
        const bool right = disk.well_formed && !disk.sessions && !disk.open_files && !disk.open_folders && disk.most_folders <= 1;
        if (!right) std::printf("  the save was not left as it was found\n");
        return right;
    };
    const auto listed = [&](uint32_t slot) { // the paths, in the order they came
        std::vector<std::string> paths;
        std::vector<char> big(1 << 16);
        if (vault.list(mod, slot, big.data(), big.size(), &needed) != TSUCHI_OK) return paths;
        const char* at = big.data();
        for (; *at; at += std::strlen(at) + 1) paths.push_back(at);
        if (uint64_t(at - big.data()) + 1 != needed) paths.push_back("<the size is wrong>");
        return paths;
    };

    // Looking into a slot: not before the game's functions for it are known, not while it is busy with a save, not a slot there is none of.
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_NOT_READY && vault.size(mod, 1, "single.sav", &size) == TSUCHI_NOT_READY &&
          vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_NOT_READY);
    t::fn::save_begin_session = game_save_begin, t::fn::save_end_session = game_save_end, t::fn::fs_open_file = game_fs_open_file;
    t::fn::fs_close_file = game_fs_close_file, t::fn::fs_file_size = game_fs_file_size, t::fn::fs_read_file = game_fs_read_file;
    t::fn::fs_open_folder = game_fs_open_folder, t::fn::fs_read_folder = game_fs_read_folder;
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_NOT_READY && log_is(disk.log, "")); // one still missing
    t::fn::fs_close_folder = game_fs_close_folder;
    const uint32_t slots = t::constant::save_slots;
    CHECK(vault.list(mod, slots, buffer, sizeof buffer, &needed) == TSUCHI_INVALID && vault.size(mod, slots, "single.sav", &size) == TSUCHI_INVALID &&
          vault.read(mod, slots, "single.sav", 0, buffer, 4, &size) == TSUCHI_INVALID && log_is(disk.log, ""));
    disk.files = {{"save:/B/single.sav", "0123456789"}, {"save:/B/hero/001/fow/Area001_P.sav", "fog"}, {"save:/B/hero/note.txt", ""},
                  {"save:/C/single.sav", "another slot's"}, {"save:/config.sav", "no slot's"}};
    fake::put<uint8_t>(main + t::data::save_mounted, 1);
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_BUSY && vault.size(mod, 1, "single.sav", &size) == TSUCHI_BUSY &&
          vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_BUSY && log_is(disk.log, ""));
    fake::put<uint8_t>(main + t::data::save_mounted, 0);
    disk.mounts = false; // the save cannot be opened
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_IO && vault.size(mod, 1, "single.sav", &size) == TSUCHI_IO &&
          vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_IO && log_is(disk.log, "") && closed());
    disk.mounts = true;

    // The files of a slot, those in its folders too, each by its path within the slot. One folder is open at a time.
    const size_t out = blocks_out.size();
    std::memset(buffer, 'x', sizeof buffer);
    const std::string three("single.sav\0hero/note.txt\0hero/001/fow/Area001_P.sav\0", 52);
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_OK && needed == three.size() + 1 && std::string(buffer, needed) == three + std::string(1, '\0') &&
          buffer[needed] == 'x' && log_is(disk.log, "begin end ") && closed() && blocks_out.size() == out);
    std::memset(buffer, 'x', sizeof buffer);
    needed = 0;
    CHECK(vault.list(mod, 1, buffer, three.size(), &needed) == TSUCHI_CAPACITY && needed == three.size() + 1 && buffer[0] == 'x' && closed() && blocks_out.size() == out);
    CHECK(vault.list(mod, 1, buffer, three.size() + 1, &needed) == TSUCHI_OK && buffer[three.size() + 1] == 'x');
    CHECK(listed(2) == std::vector<std::string>{"single.sav"} && vault.list(mod, 0, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND && closed()); // a slot with nothing in it
    disk.log.clear();
    refused_size = t::fs_entry::size; // no memory to read an entry into
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && log_is(disk.log, "begin end ") && closed() && blocks_out.size() == out);
    refused_size = 256; // nor for the paths
    CHECK(vault.list(mod, 1, buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && closed() && blocks_out.size() == out);
    refused_size = 0;
    // Many, in folders within folders with long names: every one is found, once.
    std::vector<std::string> many;
    for (int i = 0; i < 60; ++i) {
        const std::string path = std::string(i % 3 ? "deep/deeper/and_a_folder_with_a_long_name_" : "wide_") + std::to_string(i % 7) + "/file_number_" + std::to_string(i) + ".sav";
        disk.files["save:/D/" + path] = std::string(size_t(i), 'd');
        many.push_back(path);
    }
    std::vector<std::string> found = listed(3);
    std::sort(found.begin(), found.end()), std::sort(many.begin(), many.end());
    CHECK(found == many && closed() && blocks_out.size() == out);
    refused_size = 1024; // no memory when the list of them has to grow
    CHECK(vault.list(mod, 3, buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && closed() && blocks_out.size() == out);
    refused_size = 0;
    disk.log.clear();

    // One file's size, and a part of it.
    CHECK(vault.size(mod, 1, "single.sav", &size) == TSUCHI_OK && size == 10 && log_is(disk.log, "begin end ") && closed());
    bool there_is = false;
    CHECK(vault.exists(mod, 1, "single.sav", &there_is) == TSUCHI_OK && there_is && vault.exists(mod, 1, "none.sav", &there_is) == TSUCHI_OK && !there_is &&
          vault.exists(mod, 1, "../single.sav", &there_is) == TSUCHI_INVALID && closed());
    disk.log.clear();
    CHECK(vault.size(mod, 1, "hero/note.txt", &size) == TSUCHI_OK && size == 0 && vault.size(mod, 1, "hero/001/fow/Area001_P.sav", &size) == TSUCHI_OK && size == 3);
    CHECK(vault.size(mod, 1, "missing.sav", &size) == TSUCHI_NOT_FOUND && vault.size(mod, 1, "hero", &size) == TSUCHI_NOT_FOUND &&
          vault.size(mod, 0, "single.sav", &size) == TSUCHI_NOT_FOUND && closed());
    for (const char* path : {"", "/single.sav", "..", "../C/single.sav", "hero/../../C/single.sav", "hero/..", "save:/C/single.sav", "hero\\note.txt"})
        CHECK(vault.size(mod, 1, path, &size) == TSUCHI_INVALID && vault.read(mod, 1, path, 0, buffer, 4, &size) == TSUCHI_INVALID);
    disk.files["save:/B/..hidden/a..b"] = "dots";
    CHECK(log_is(disk.log, "begin end begin end begin end begin end begin end ") && vault.size(mod, 1, "..hidden/a..b", &size) == TSUCHI_OK && size == 4); // dots that step back out of nothing
    disk.files.erase("save:/B/..hidden/a..b");
    disk.open_error = 0x1234;
    CHECK(vault.size(mod, 1, "missing.sav", &size) == TSUCHI_IO && closed());
    disk.open_error = t::constant::fs_no_such_path, disk.size_fails = true;
    CHECK(vault.size(mod, 1, "single.sav", &size) == TSUCHI_IO && vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_IO && closed());
    disk.size_fails = false;
    refused_size = 256; // no memory for where the file is
    CHECK(vault.size(mod, 1, "single.sav", &size) == TSUCHI_NO_MEMORY && closed() && blocks_out.size() == out);
    refused_size = 0;
    disk.log.clear();
    std::memset(buffer, 'x', sizeof buffer);
    CHECK(vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_OK && size == 4 && !std::memcmp(buffer, "0123x", 5) && log_is(disk.log, "begin read0+4 end ") && closed());
    CHECK(vault.read(mod, 1, "single.sav", 8, buffer, 4, &size) == TSUCHI_OK && size == 2 && !std::memcmp(buffer, "8923x", 5) && log_is(disk.log, "begin read8+2 end "));
    CHECK(vault.read(mod, 1, "single.sav", 3, buffer, 100, &size) == TSUCHI_OK && size == 7 && !std::memcmp(buffer, "3456789", 7) && log_is(disk.log, "begin read3+7 end "));
    size = 77;
    CHECK(vault.read(mod, 1, "single.sav", 10, buffer, 4, &size) == TSUCHI_OK && size == 0 && log_is(disk.log, "begin end ")); // at the end, and past it
    size = 77;
    CHECK(vault.read(mod, 1, "single.sav", 99, buffer, 4, &size) == TSUCHI_OK && size == 0 && vault.read(mod, 1, "single.sav", 2, nullptr, 0, &size) == TSUCHI_OK && size == 0 &&
          log_is(disk.log, "begin end begin end "));
    CHECK(vault.read(mod, 1, "single.sav", 2, nullptr, 4, &size) == TSUCHI_INVALID && vault.read(mod, 1, "missing.sav", 0, buffer, 4, &size) == TSUCHI_NOT_FOUND);
    disk.read_fails = true;
    CHECK(vault.read(mod, 1, "single.sav", 0, buffer, 4, &size) == TSUCHI_IO && closed());
    disk.read_fails = false;
    disk.log.clear();

    // A stand-in for a slot. Its files are kept from the first on; the game sees none of them until it is put to use.
    const t::Slot& exists = t::slot::save_exists;
    const t::Slot& result = t::slot::save_exists_result;
    const t::Slot& read = t::slot::save_read;
    const t::Slot& write = t::slot::save_write;
    const uint64_t exists_entry = main + exists.table + exists.offset, result_entry = main + result.table + result.offset, read_entry = main + read.table + read.offset;
    const uint64_t write_entry = main + write.table + write.offset;
    const auto in_game = [](const char16_t* name) -> uint64_t { // a file's name in the game's memory, as the game would hand it over
        const std::u16string text(name);
        const uint64_t at = fake::memory((text.size() + 1) * 2 + 2);
        std::memcpy(reinterpret_cast<void*>(at), text.c_str(), (text.size() + 1) * 2);
        return at;
    };
    const auto there = [&](const char16_t* name) { return (thunks::call(fake::get<uint64_t>(exists_entry), 0x5A5E, in_game(name), 7) & 0xFF) != 0; };
    const auto answer = [&](const char16_t* name) { return uint32_t(thunks::call(fake::get<uint64_t>(result_entry), 0x5A5E, in_game(name), 7)); };
    // The game reads a file into an array of bytes, which may hold something already and have room for more.
    const auto array = [](const char* held, int32_t room) {
        const uint64_t at = fake::memory(t::array::size), length = std::strlen(held);
        const uint64_t data = room ? reinterpret_cast<uint64_t>(fake::engine_allocate(nullptr, uint64_t(room), 0)) : 0;
        if (length) std::memcpy(reinterpret_cast<void*>(data), held, length);
        fake::put(at + t::array::data, data), fake::put<int32_t>(at + t::array::count, int32_t(length)), fake::put<int32_t>(at + t::array::max, room);
        return at;
    };
    const auto holds = [](uint64_t at) {
        return std::string(reinterpret_cast<const char*>(fake::get<uint64_t>(at + t::array::data)), size_t(fake::get<int32_t>(at + t::array::count))) + "/" +
               std::to_string(fake::get<int32_t>(at + t::array::max));
    };
    const auto reads = [&](const char16_t* name, uint64_t into) { return (thunks::call(fake::get<uint64_t>(read_entry), 0x5A5E, 1, in_game(name), 7, into) & 0xFF) != 0; };
    TSUCHI_Mod* self = tsuchi::mod_create("tsuchi.self.file"), *game = tsuchi::mod_create("the.file"), *rival = tsuchi::mod_create("actor.guest5"), *poor = tsuchi::mod_create("actor.guest6");
    const uint32_t owned = tsuchi::owned_count(mod), self_owned = tsuchi::owned_count(self);
    CHECK(vault.use_stand_in(mod) == TSUCHI_NOT_FOUND && vault.end_stand_in(mod) == TSUCHI_NOT_FOUND);
    CHECK(vault.add_stand_in_file(mod, 1, "single.sav", "HOST", 4) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1);
    CHECK(vault.use_stand_in(mod) == TSUCHI_NOT_READY); // the game's functions for reading are not wrapped: first one unfamiliar, then all the game's own
    for (const t::Slot* slot : {&exists, &result, &read}) fake::code().push_back(main + slot->stock), fake::put(main + slot->table + slot->offset, main + slot->stock);
    fake::code().push_back(main + read.stock + 8), fake::put(read_entry, main + read.stock + 8);
    tsuchi::saves_start(self);
    CHECK(vault.use_stand_in(mod) == TSUCHI_NOT_READY && tsuchi::owned_count(self) == self_owned && fake::get<uint64_t>(read_entry) == main + read.stock + 8);
    fake::put(read_entry, main + read.stock);
    CHECK(tsuchi::hook_wrap(game, exists_entry, main + exists.stock, reinterpret_cast<void*>(&game_save_exists), 0, &id, &unused) == TSUCHI_OK &&
          tsuchi::hook_wrap(game, result_entry, main + result.stock, reinterpret_cast<void*>(&game_save_exists_result), 0, &id, &unused) == TSUCHI_OK &&
          tsuchi::hook_wrap(game, read_entry, main + read.stock, reinterpret_cast<void*>(&game_save_read), 0, &id, &unused) == TSUCHI_OK);
    tsuchi::saves_start(self);
    tsuchi::saves_start(self); // and once more changes nothing
    CHECK(tsuchi::owned_count(self) == self_owned + 3);
    saved.log.clear();
    // Not in use yet: the game is asked, as ever.
    CHECK(there(u"B/single.sav") && answer(u"B/single.sav") == 3 && reads(u"B/single.sav", array("", 0)) &&
          log_is(saved.log, "exists:B/single.sav/7 result:B/single.sav read:B/single.sav+screen "));
    // More files; one given again is replaced. What cannot be one, and whose the stand-in is.
    const size_t kept = blocks_out.size();
    CHECK(vault.add_stand_in_file(mod, 1, "hero/001/fow/Area001_P.sav", "fog", 3) == TSUCHI_OK && vault.add_stand_in_file(mod, 1, "empty.sav", nullptr, 0) == TSUCHI_OK &&
          vault.add_stand_in_file(mod, 1, "single.sav", "HOSTDATA", 8) == TSUCHI_OK && tsuchi::owned_count(mod) == owned + 1 && blocks_out.size() == kept + 4);
    CHECK(vault.add_stand_in_file(mod, slots, "single.sav", "x", 1) == TSUCHI_INVALID && vault.add_stand_in_file(mod, 1, "../C/single.sav", "x", 1) == TSUCHI_INVALID &&
          vault.add_stand_in_file(mod, 1, "", "x", 1) == TSUCHI_INVALID && vault.add_stand_in_file(mod, 1, "\xff.sav", "x", 1) == TSUCHI_INVALID &&
          vault.add_stand_in_file(mod, 1, "single.sav", nullptr, 1) == TSUCHI_INVALID);
    CHECK(vault.add_stand_in_file(rival, 1, "single.sav", "x", 1) == TSUCHI_BUSY && vault.add_stand_in_file(mod, 2, "single.sav", "x", 1) == TSUCHI_BUSY &&
          vault.use_stand_in(rival) == TSUCHI_NOT_FOUND && vault.end_stand_in(rival) == TSUCHI_NOT_FOUND && blocks_out.size() == kept + 4);
    refused_size = 12; // no memory for the name
    CHECK(vault.add_stand_in_file(mod, 1, "a.sav", "x", 1) == TSUCHI_NO_MEMORY);
    refused_size = 5; // nor for what it holds
    CHECK(vault.add_stand_in_file(mod, 1, "a.sav", "12345", 5) == TSUCHI_NO_MEMORY && vault.add_stand_in_file(mod, 1, "single.sav", "12345", 5) == TSUCHI_NO_MEMORY && blocks_out.size() == kept + 4);
    refused_size = 0;

    // In use: the slot holds exactly the stand-in's files, as far as the game can tell. Other slots, and what is no slot's, are the game's.
    CHECK(vault.use_stand_in(mod) == TSUCHI_OK && vault.use_stand_in(mod) == TSUCHI_OK && log_is(saved.log, ""));
    CHECK(there(u"B/single.sav") && there(u"B/hero/001/fow/Area001_P.sav") && there(u"B/empty.sav") && !there(u"B/missing.sav") && !there(u"B/single.sav2") &&
          !there(u"B/single.sa") && !there(u"B/") && !there(u"B/hero") && log_is(saved.log, ""));
    CHECK(answer(u"B/single.sav") == t::constant::save_there && answer(u"B/missing.sav") == t::constant::save_missing && log_is(saved.log, ""));
    saved.succeeds = false;
    CHECK(!there(u"C/single.sav") && answer(u"C/single.sav") == 3 && !there(u"config.sav") && !reads(u"A/single.sav", array("", 0)) &&
          log_is(saved.log, "exists:C/single.sav/7 result:C/single.sav exists:config.sav/7 read:A/single.sav+screen "));
    CHECK((thunks::call(fake::get<uint64_t>(exists_entry), 0x5A5E, 0x40, 7) & 0xFF) == 0 && log_is(saved.log, "exists:<nowhere>/7 ")); // a name that is not in memory
    saved.succeeds = true;
    // Read: the file's bytes go after what the array holds, in more room when it has too little.
    const uint32_t read_before = tsuchi::save_counts().stand_in_reads;
    int freed = fake::engine().freed, allocated = fake::engine().allocated;
    uint64_t into = array("", 0);
    CHECK(reads(u"B/single.sav", into) && holds(into) == "HOSTDATA/8" && fake::engine().allocated == allocated + 1 && fake::engine().freed == freed && log_is(saved.log, ""));
    into = array("abc", 16), allocated = fake::engine().allocated;
    CHECK(reads(u"B/single.sav", into) && holds(into) == "abcHOSTDATA/16" && fake::engine().allocated == allocated && fake::engine().freed == freed);
    into = array("abc", 10), allocated = fake::engine().allocated;
    CHECK(reads(u"B/single.sav", into) && holds(into) == "abcHOSTDATA/11" && fake::engine().allocated == allocated + 1 && fake::engine().freed == freed + 1);
    into = array("abc", 11), allocated = fake::engine().allocated; // room for exactly that much
    CHECK(reads(u"B/single.sav", into) && holds(into) == "abcHOSTDATA/11" && fake::engine().allocated == allocated && fake::engine().freed == freed + 1);
    into = array("abc", 8), allocated = fake::engine().allocated;
    CHECK(reads(u"B/empty.sav", into) && holds(into) == "abc/8" && fake::engine().allocated == allocated && reads(u"B/hero/001/fow/Area001_P.sav", into) && holds(into) == "abcfog/8");
    CHECK(tsuchi::save_counts().stand_in_reads == read_before + 6);
    // Not read: a file the stand-in does not have, an array that is not there, and no memory for more room.
    CHECK(!reads(u"B/missing.sav", into) && holds(into) == "abcfog/8" && !reads(u"B/single.sav", 0x40) && !reads(u"B/single.sav", 0));
    fake::engine().out_of_memory = true;
    CHECK(!reads(u"B/single.sav", into) && holds(into) == "abcfog/8");
    fake::engine().out_of_memory = false;
    fake::put<int32_t>(into + t::array::count, -1);
    CHECK(!reads(u"B/single.sav", into));
    fake::put<int32_t>(into + t::array::count, 0x7FFFFFFA);
    CHECK(!reads(u"B/single.sav", into) && tsuchi::save_counts().stand_in_reads == read_before + 6 && log_is(saved.log, ""));
    // While it is in use nothing is written to the slots, as with a hold.
    const uint32_t held_back = tsuchi::save_counts().held_back;
    CHECK((thunks::call(fake::get<uint64_t>(write_entry), 0x5A5E, 0, in_game(u"B/single.sav"), 7) & 0xFF) != 0 && log_is(saved.log, "") &&
          tsuchi::save_counts().held_back == held_back + 1);
    // A file given while it is in use counts at once.
    CHECK(vault.add_stand_in_file(mod, 1, "late.sav", "late", 4) == TSUCHI_OK && there(u"B/late.sav"));
    // A name that begins as another does, and one as long as another, are files of their own.
    into = array("", 0);
    CHECK(vault.add_stand_in_file(mod, 1, "single.sa", "short", 5) == TSUCHI_OK && vault.add_stand_in_file(mod, 1, "simple.sav", "other", 5) == TSUCHI_OK && there(u"B/single.sa") &&
          there(u"B/simple.sav") && reads(u"B/single.sav", into) && holds(into) == "HOSTDATA/8" && blocks_out.size() == kept + 10);
    // Ended in the middle of a story: the game is asked about the slot's files again and the stand-in's are
    // forgotten, but what the game has loaded may still be the stand-in's, so the slots stay closed.
    in_stages({u"Story", u"StoryInGame"});
    CHECK(vault.end_stand_in(mod) == TSUCHI_OK && tsuchi::owned_count(mod) == owned && blocks_out.size() == kept - 3 && vault.end_stand_in(mod) == TSUCHI_NOT_FOUND &&
          vault.use_stand_in(mod) == TSUCHI_NOT_FOUND);
    CHECK(there(u"B/missing.sav") && (thunks::call(fake::get<uint64_t>(write_entry), 0x5A5E, 0, in_game(u"B/single.sav"), 7) & 0xFF) != 0 &&
          log_is(saved.log, "exists:B/missing.sav/7 ") && tsuchi::save_counts().held_back == held_back + 2);
    // At the title screen the game holds no story, and writes go through from then on.
    in_stages({u"Title"});
    CHECK((thunks::call(fake::get<uint64_t>(write_entry), 0x5A5E, 0, in_game(u"B/single.sav"), 7) & 0xFF) != 0 && log_is(saved.log, "write:B/single.sav/7 ") &&
          tsuchi::save_counts().held_back == held_back + 2);
    in_stages({u"Story", u"StoryInGame"});
    // A stand-in ends with its mod; another mod can have one then. No memory to make one: none is made.
    const size_t before = blocks_out.size();
    refused_multiple = 40; // no memory to note whose it is
    CHECK(vault.add_stand_in_file(poor, 0, "single.sav", "x", 1) == TSUCHI_NO_MEMORY && vault.use_stand_in(poor) == TSUCHI_NOT_FOUND && blocks_out.size() == before);
    refused_multiple = 0, refused_size = 16 * sizeof(void*) * 4; // nor for the list of its files
    CHECK(vault.add_stand_in_file(mod, 0, "single.sav", "x", 1) == TSUCHI_NO_MEMORY && vault.use_stand_in(mod) == TSUCHI_NOT_FOUND && blocks_out.size() == before &&
          tsuchi::owned_count(mod) == owned);
    refused_size = 0;
    CHECK(vault.add_stand_in_file(rival, 0, "single.sav", "RIVAL", 5) == TSUCHI_OK && vault.use_stand_in(rival) == TSUCHI_OK && there(u"A/single.sav") && !there(u"A/other.sav") &&
          vault.add_stand_in_file(mod, 0, "single.sav", "x", 1) == TSUCHI_BUSY && log_is(saved.log, ""));
    tsuchi::mod_stop(rival);
    CHECK(there(u"A/other.sav") && log_is(saved.log, "exists:A/other.sav/7 ") && vault.add_stand_in_file(mod, 2, "single.sav", "x", 1) == TSUCHI_OK && vault.end_stand_in(mod) == TSUCHI_OK);
    disk.log.clear();
}

// ---- The mods that are installed, and a mod's own files -------------------------------------

struct Logged {
    std::string lines;
} logged;
// Nothing left open, and nothing done the system would refuse.
bool closed() { return disk.well_formed && !disk.sessions && !disk.open_files && !disk.open_folders && disk.most_folders <= 1; }
void log_sink(const char* mod_id, const char* text) { logged.lines += std::string("[") + mod_id + "] " + text + "\n"; }

// ---- Mods written in Lua ----------------------------------------------------------------------

void scripting() {
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const auto file = [&](const std::string& id, const std::string& path, const std::string& text) { disk.files[mods_at + id + "/" + path] = text; };
    const auto mod_of = [&](const std::string& id, const std::string& main, const std::string& more = "") {
        file(id, "mod.json", "{\"id\": \"" + id + "\", \"name\": \"A script\", \"version\": \"1.0.0\"" + more + "}");
        file(id, "main.lua", main);
    };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::running ? "<running>" : "<ready>");
    };
    const auto log_is = [](const std::string& expected) {
        const bool same = logged.lines == expected;
        if (!same) std::printf("  logged: [%s]\n", logged.lines.c_str());
        logged.lines.clear();
        return same;
    };
    tsuchi::forget_mods();
    tsuchi::set_log_sink(log_sink);
    const size_t out = blocks_out.size();
    const auto other_files = disk.files; // what the checks around this one keep there
    disk.files.clear();
    // Until the framework is told of scripts, a main.lua is one more file.
    mod_of("script.one", "print('hello', 1, true, nil, 2.5)\n");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("script.one") == "<running>" && log_is(""));
    tsuchi_lua::register_script_mods();
    tsuchi_js::register_script_mods();

    // A script runs when its mod starts, with what it needs of the language and none of what reaches the machine.
    mod_of("script.one", R"(print('hello', 1, true, nil, 2.5)
print(mod.id(), mod.name(), mod.version())
print(type(string.format), type(math.floor), type(table.sort), type(coroutine.wrap), type(utf8.char), type(pcall), type(require))
print(io, os, package, debug, dofile, loadfile)
print(select('#', print()))
)");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("script.one") == "<running>" &&
          log_is("[script.one] hello\t1\ttrue\tnil\t2.5\n[script.one] script.one\tA script\t1.0.0\n[script.one] function\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction\n"
                 "[script.one] nil\tnil\tnil\tnil\tnil\tnil\n[script.one] \n[script.one] 0\n"));

    // require: a module among the mod's own files, run once, by a name with dots for folders.
    file("script.one", "helper.lua", "print('helper runs', ...)\nlocal M = {}\nfunction M.double(x) return x * 2 end\nreturn M\n");
    file("script.one", "lib/deep/thing.lua", "print('thing runs')\nthing_ran = (thing_ran or 0) + 1\n");
    file("script.one", "main.lua", R"(local helper = require('helper')
print(helper.double(21), require('helper') == helper)
print(require('lib.deep.thing'), require('lib.deep.thing'), thing_ran)
print(pcall(require, 'missing'))
print(pcall(require, '../script.two/secret'))
print(pcall(require))
)");
    file("script.two", "secret.lua", "print('never')\n");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(log_is("[script.one] helper runs\thelper\n[script.one] 42\ttrue\n[script.one] thing runs\n[script.one] true\ttrue\t1\n"
                 "[script.one] false\tmissing.lua: not_found\n[script.one] false\t///script/two/secret.lua: invalid\n"
                 "[script.one] false\tbad argument #1 to 'require' (string expected, got no value)\n"));

    // A script that fails as it starts: the mod is blocked with what went wrong and where, nothing of it is left,
    // and what needs it does not start. A mistake in how it is written is told the same way.
    mod_of("script.bad", "local x = 1\nprint('before')\nerror('on purpose')\nprint('after')\n");
    mod_of("script.needy", "print('needy runs')\n", R"(, "requires": {"script.bad": "*"})");
    mod_of("script.typo", "print('fine')\nlocal = 5\n");
    mod_of("script.table", "error({code = 7})\n");
    mod_of("script.later", "print('later runs')\n", R"(, "requires": {"script.one": "*"})");
    file("script.one", "main.lua", "print('one runs')\n");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("script.bad") == "main.lua: script.bad/main.lua:3: on purpose" && !tsuchi::find_mod("script.bad")->context &&
          problem("script.needy") == "it needs script.bad, which cannot run" && problem("script.one") == "<running>" && problem("script.later") == "<running>");
    CHECK(problem("script.typo").find("main.lua: script.typo/main.lua:2:") == 0 && problem("script.typo").find("near '='") != std::string::npos &&
          problem("script.table") == "main.lua: it raised an error that is no text");
    CHECK(log_is("[script.bad] before\n[script.one] one runs\n[script.later] later runs\n"));
    // Selected mods are prepared before script startup. The dependent gets a context too, then its provider's
    // failure stops it without running code. Only those known context tombstones may remain after forgetting.
    tsuchi::forget_mods();
    const size_t failed_once = blocks_out.size();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::prepare_mods();
    std::vector<TSUCHI_Mod*> prepared;
    for (uint32_t i = 0; i < tsuchi::mod_count(); ++i) {
        const auto& record = *tsuchi::mod_at(i);
        if (record.state != tsuchi::ModState::ready) { CHECK(!record.context); continue; }
        CHECK(record.prepared && tsuchi::mod_usable(record.context));
        prepared.push_back(record.context);
    }
    CHECK(prepared.size() == 6 && tsuchi::find_mod("script.needy")->context);
    tsuchi::start_mods();
    CHECK(!tsuchi::find_mod("script.needy")->context && problem("script.needy") == "it needs script.bad, which cannot run");
    tsuchi::forget_mods();
    for (TSUCHI_Mod* context : prepared)
        CHECK(blocks_out.count(context) == 1 && !tsuchi::mod_running(context) && !tsuchi::owned_count(context));
    CHECK(blocks_out.size() == failed_once + prepared.size() && log_is("[script.bad] before\n[script.one] one runs\n[script.later] later runs\n"));
    // A main.lua that is there and cannot be read: the mod is blocked, and is not taken for one with no code.
    disk.unreadable = "script.one/main.lua";
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    disk.unreadable.clear();
    CHECK(problem("script.one") == "main.lua: it could not be read" && problem("script.later") == "it needs script.one, which cannot run");
    logged.lines.clear();

    // A script listens like any mod: its handlers run each frame, and stop with it. A handler that fails is
    // written to the log and runs again the next time.
    mod_of("script.one", R"(local frames = 0
time.on_step(function(event)
    frames = frames + 1
    if frames == 2 then error('in a handler') end
    if frames > 4 then error('none left') end
    print('frame', frames)
    if frames == 4 then print(pcall(require, 'helper')) end
end)
)");
    disk.files.erase(mods_at + "script.bad/mod.json"), disk.files.erase(mods_at + "script.needy/mod.json"), disk.files.erase(mods_at + "script.typo/mod.json");
    disk.files.erase(mods_at + "script.table/mod.json"), disk.files.erase(mods_at + "script.later/mod.json");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    logged.lines.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(log_is("[script.one] frame\t1\n[script.one] error: script.one/main.lua:4: in a handler\n[script.one] frame\t3\n"));
    // A module the system gives less of than there is, asked for later: it is not run cut short.
    disk.short_reads = true;
    tsuchi::frame(0.03f);
    disk.short_reads = false;
    CHECK(log_is("[script.one] frame\t4\n[script.one] false\thelper.lua: io\n"));
    // With no memory to make the line, a handler's error is lost and nothing more, however often.
    allocations_left = 0;
    for (int i = 0; i < 64; ++i) tsuchi::frame(0.03f);
    allocations_left = -1;
    CHECK(log_is(""));
    tsuchi::frame(0.03f);
    CHECK(log_is("[script.one] error: script.one/main.lua:5: none left\n"));
    TSUCHI_Mod* one = tsuchi::find_mod("script.one")->context;
    CHECK(tsuchi::owned_count(one) == 2); // its state, and its one handler
    tsuchi::forget_mods();
    tsuchi::frame(0.03f);
    CHECK(log_is("") && !tsuchi::mod_usable(one));
    // Started and stopped once more: nothing of a script's state is left, only the mod's context.
    const size_t stopped = blocks_out.size();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(blocks_out.size() > stopped + 100);
    tsuchi::forget_mods();
    CHECK(blocks_out.size() == stopped + 1 && stopped > out);

    // No memory at any point while a script starts: the mod is blocked or runs, and nothing is left behind either way.
    mod_of("script.one", "local t = {} for i = 1, 200 do t[i] = tostring(i) end\nprint(#t, require('helper').double(4))\n");
    int needed = 0;
    for (;; ++needed) {
        const size_t before = blocks_out.size();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        allocations_left = needed;
        tsuchi::start_mods();
        allocations_left = -1;
        const bool ran = problem("script.one") == "<running>";
        const std::string why = problem("script.one");
        if (!ran) CHECK(why == "it could not be started" || why.find("main.lua: ") == 0 || why.empty()); // the reason, when there was memory to keep it
        if (ran) CHECK(logged.lines == "[script.one] helper runs\thelper\n[script.one] 200\t8\n"); // and when it runs, it has run
        logged.lines.clear();
        tsuchi::forget_mods();
        CHECK(blocks_out.size() <= before + 1); // its context, at most
        if (ran || needed > 20000) break;
    }
    if (needed <= 20 || needed > 20000) std::printf("  a script started with room for %d blocks: %s\n", needed, problem("script.one").c_str());
    CHECK(needed > 20 && needed <= 20000);

    // What a script may use is bounded. One that never comes back is stopped where it stands, as it starts or in
    // a handler; catching the error does not buy a longer turn; and one that takes memory without end gets Lua's
    // own memory error. `load` takes text only, and a number that does not fit an argument is refused.
    mod_of("script.loop", "while true do end\n");
    mod_of("script.hog", "local t = {}\nwhile true do t[#t + 1] = string.rep('x', 1000000) .. #t end\n");
    mod_of("script.sly", "print('caught', pcall(function() while true do end end))\nwhile true do pcall(function() while true do end end) end\n");
    mod_of("script.strict", "print('binary', load(string.dump(function() return 1 end)))\nprint('text', load('return 2')())\n"
                            "print('too big', pcall(save.exists, 4294967296, 'x'))\nprint('negative', pcall(save.exists, -1, 'x'))\n");
    mod_of("script.slow", "local n = 0\ntime.on_step(function() n = n + 1 print('frame', n) while true do end end)\n");
    // The limits change nothing of the language: a script that calls itself without end still gets Lua's own error.
    mod_of("script.deep", "local depth = 0 local function deep() depth = depth + 1 return deep() + 1 end\n"
                          "local ok, why = pcall(deep)\nprint('deep', ok, depth > 1000, why)\n");
    logged.lines.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    const auto has = [&](const std::string& part) { return logged.lines.find(part) != std::string::npos; };
    CHECK(problem("script.loop").find("it ran too long without giving the game its turn back") != std::string::npos && !tsuchi::find_mod("script.loop")->context);
    CHECK(problem("script.hog").find("not enough memory") != std::string::npos && !tsuchi::find_mod("script.hog")->context);
    CHECK(problem("script.sly").find("it ran too long") != std::string::npos && !has("[script.sly] caught")); // caught, and raised again before the script can do one thing more
    CHECK(problem("script.strict") == "<running>" && has("[script.strict] binary\tnil\tattempt to load a binary chunk") && has("[script.strict] text\t2\n") &&
          has("[script.strict] too big\tfalse") && has("[script.strict] negative\tfalse") && has("the number is out of range"));
    CHECK(problem("script.slow") == "<running>" && !has("[script.slow] frame"));
    CHECK(problem("script.deep") == "<running>" && has("[script.deep] deep\tfalse\ttrue\t") && has("stack overflow"));
    logged.lines.clear();
    tsuchi::frame(0.03f);
    CHECK(has("[script.slow] frame\t1\n") && has("[script.slow] error: ") && has("it ran too long"));
    logged.lines.clear();
    tsuchi::frame(0.03f);
    CHECK(!has("[script.slow]")); // the handler that did not come back is not called again
    tsuchi::forget_mods();
    logged.lines.clear();
    tsuchi::set_log_sink(nullptr);
    logged.lines.clear();
    disk.files = other_files;
    disk.log.clear();
}

// ---- Compiled mods: a made-up library of the system's, with a loader in it -----------------

using CompiledStart = TSUCHI_Result (*)(const TSUCHI_Host*, TSUCHI_Mod*);
struct MadeLoader {
    std::string log; // what was asked of it, in order
    uint32_t start_answer = 0, register_answer = 0, room_answer = 0, load_answer = 0, lookup_answer = 0;
    bool lookup_finds_nothing = false, well_formed = true;
    bool says_built_for = true;             // whether a module has the SDK's mark of the version it was built for
    uint32_t built_for = TSUCHI_API_VERSION;   // and what the mark says
    uint64_t mark = 0;                      // where the mark is kept, in the made-up game's memory
    std::map<void*, std::string> lists; // by the loader's record of a list: the hash the list vouches for
    std::map<void*, int> modules;       // by the loader's record of a module: which code it is
    std::vector<CompiledStart> code;
} made_loader;
struct CompiledRan {
    const TSUCHI_Host* host = nullptr;
    TSUCHI_Mod* mod = nullptr;
    int frames = 0, starts = 0;
    TSUCHI_Result answer = TSUCHI_OK;
} compiled_ran;

bool all_bytes(const void* at, uint64_t count, uint8_t value) {
    for (uint64_t i = 0; i < count; ++i)
        if (static_cast<const uint8_t*>(at)[i] != value) return false;
    return true;
}
uint32_t loader_start() {
    made_loader.log += "start ";
    return made_loader.start_answer;
}
uint32_t loader_register(void* registration, const void* list) {
    const auto* page = static_cast<const uint8_t*>(list);
    uint64_t program = 0;
    std::memcpy(&program, page + 0x330, 8);
    made_loader.well_formed = made_loader.well_formed && reinterpret_cast<uint64_t>(list) % 0x1000 == 0 && !std::memcmp(page, "NRR0", 4) && program == t::constant::program_id &&
                              all_bytes(registration, t::constant::loader_registration_bytes, 0);
    if (made_loader.register_answer) {
        made_loader.log += "register-refused ";
        return made_loader.register_answer;
    }
    made_loader.lists[registration] = std::string(reinterpret_cast<const char*>(page) + 0x350, 32);
    std::memset(registration, 0xC3, t::constant::loader_registration_bytes); // the loader's own record, as it keeps it
    made_loader.log += "register ";
    return 0;
}
void loader_unregister(void* registration) {
    made_loader.well_formed = made_loader.well_formed && made_loader.lists.erase(registration) == 1;
    made_loader.log += "unregister ";
}
uint32_t loader_room(uint64_t* out, const void* image) {
    if (made_loader.room_answer) {
        made_loader.log += "room-refused ";
        return made_loader.room_answer;
    }
    uint32_t room = 0;
    std::memcpy(&room, static_cast<const uint8_t*>(image) + 0x38, 4);
    *out = room;
    made_loader.log += "room ";
    return 0;
}
uint32_t loader_load(void* module, const void* image, void* buffer, uint64_t room, int32_t flags) {
    // The loader takes only a file some list it was given vouches for, by the hash of all of it.
    uint32_t bytes = 0;
    int32_t which = 0;
    std::memcpy(&bytes, static_cast<const uint8_t*>(image) + 0x18, 4), std::memcpy(&which, static_cast<const uint8_t*>(image) + 0x80, 4);
    uint8_t hash[tsuchi::kSha256Bytes];
    tsuchi::sha256(image, bytes, hash);
    bool vouched = false;
    for (const auto& [registration, listed] : made_loader.lists) vouched = vouched || !std::memcmp(listed.data(), hash, sizeof hash);
    made_loader.well_formed = made_loader.well_formed && vouched && reinterpret_cast<uint64_t>(image) % 0x1000 == 0 && flags == int32_t(t::constant::loader_bind_now) &&
                              all_bytes(module, t::constant::loader_module_bytes, 0) &&
                              (room ? reinterpret_cast<uint64_t>(buffer) % 0x1000 == 0 && buffer && all_bytes(buffer, room, 0) : !buffer);
    if (made_loader.load_answer) {
        made_loader.log += "load-refused ";
        return made_loader.load_answer;
    }
    made_loader.modules[module] = which;
    std::memset(module, 0xD4, t::constant::loader_module_bytes);
    if (room) std::memset(buffer, 0x77, room); // the module's own memory, in use from now on
    made_loader.log += "load ";
    return 0;
}
uint32_t loader_lookup(uint64_t* out, const void* module, const char* name) {
    if (!std::strcmp(name, tsuchi::kCompiledBuiltFor)) { // the SDK's mark, asked for after the start; not in the log of what was asked
        if (!made_loader.says_built_for) return 0x1016;
        if (!made_loader.mark) made_loader.mark = fake::memory(8);
        fake::put<uint32_t>(made_loader.mark, made_loader.built_for);
        *out = made_loader.mark;
        return 0;
    }
    made_loader.log += std::string("lookup:") + name + " ";
    const auto found = made_loader.modules.find(const_cast<void*>(module));
    made_loader.well_formed = made_loader.well_formed && found != made_loader.modules.end();
    *out = 0xDEAD0000; // nothing to be counted on when it fails
    if (made_loader.lookup_answer || found == made_loader.modules.end() || made_loader.lookup_finds_nothing || std::strcmp(name, "tsuchi_mod_start")) return made_loader.lookup_answer ? made_loader.lookup_answer : 0x1016;
    *out = reinterpret_cast<uint64_t>(made_loader.code[size_t(found->second)]);
    return 0;
}

void compiled_frame(void*, const TSUCHI_TimeStep*) { ++compiled_ran.frames; }
TSUCHI_Result compiled_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    compiled_ran.host = host, compiled_ran.mod = mod;
    ++compiled_ran.starts;
    host->mod->log(mod, "compiled runs");
    TSUCHI_Subscription subscription{};
    host->time->on_step(mod, compiled_frame, nullptr, &subscription);
    return compiled_ran.answer;
}
TSUCHI_Result compiled_second(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    host->mod->log(mod, "second runs");
    return TSUCHI_OK;
}
// A mod that hooks a function the game calls directly, which takes the framework's patch.
struct CompiledHooks {
    TSUCHI_Hook hook{};
    TSUCHI_Result wrapped = TSUCHI_OK;
    bool gives_up = false; // whether its start answers what the hook did, or carries on without the hook
    int calls = 0;
    int game_calls = 0; // times the game's own function ran
} compiled_hooks;
uint64_t game_update(uint64_t, uint64_t, uint64_t, uint64_t) { return uint64_t(++compiled_hooks.game_calls); }
uint64_t compiled_update(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    ++compiled_hooks.calls;
    return thunks::call(reinterpret_cast<uint64_t>(compiled_hooks.hook.original), a, b, c, d);
}
TSUCHI_Result compiled_hooks_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    TSUCHI_Subscription subscription{};
    host->time->on_step(mod, compiled_frame, nullptr, &subscription);
    compiled_hooks.hook = TSUCHI_Hook{};
    compiled_hooks.wrapped = host->hook->wrap_site(mod, "update", reinterpret_cast<void*>(&compiled_update), &compiled_hooks.hook);
    host->mod->log(mod, compiled_hooks.wrapped == TSUCHI_OK ? "hooked" : "not hooked");
    return compiled_hooks.gives_up ? compiled_hooks.wrapped : TSUCHI_OK;
}

// A module file: `pages` pages, with what the loader reads of its header, and which of the made-up code it is.
std::string module_image(int32_t which, uint32_t pages = 2, uint32_t room = 0x2000) {
    std::string image(pages * 0x1000, '\x5A');
    const auto put = [&](size_t at, uint32_t value) { std::memcpy(image.data() + at, &value, 4); };
    std::memcpy(image.data() + 0x10, "NRO0", 4);
    put(0x18, uint32_t(image.size()));
    put(0x20, 0), put(0x24, 0x1000), put(0x28, 0x1000), put(0x2C, 0), put(0x30, 0x1000), put(0x34, uint32_t(image.size()) - 0x1000);
    put(0x38, room), put(0x80, uint32_t(which));
    return image;
}

void compiling() {
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const auto file = [&](const std::string& id, const std::string& path, const std::string& text) { disk.files[mods_at + id + "/" + path] = text; };
    const auto mod_of = [&](const std::string& id, const std::string& image, const std::string& more = "") {
        file(id, "mod.json", "{\"id\": \"" + id + "\", \"name\": \"Built\", \"version\": \"1.0.0\"" + more + "}");
        file(id, "mod.nro", image);
    };
    const auto gone = [&](const std::string& id) { disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/mod.nro"); };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::running ? "<running>" : "<ready>");
    };
    const auto log_is = [](const std::string& expected) {
        const bool same = logged.lines == expected;
        if (!same) std::printf("  logged: [%s]\n", logged.lines.c_str());
        logged.lines.clear();
        return same;
    };
    const auto asked = [](const std::string& expected) {
        const bool same = made_loader.log == expected;
        if (!same) std::printf("  asked of the loader: [%s]\n", made_loader.log.c_str());
        made_loader.log.clear();
        return same;
    };
    const uint64_t state_at = fake::main_image() + t::data::loader_state, anchor_at = fake::main_image() + t::data::system_library_anchor;
    const tsuchi::CompiledCounts& counts = tsuchi::compiled_counts();
    // Forgets the mods there were, starts those that are installed now, and tells why `id` cannot run, or that it does.
    const auto started = [&](const char* id) {
        tsuchi::forget_mods();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::start_mods();
        return problem(id);
    };
    tsuchi::forget_mods();
    tsuchi::set_log_sink(log_sink);
    const auto other_files = disk.files;
    disk.files.clear();
    made_loader.code = {compiled_start, compiled_second};
    const std::string good = module_image(0);
    const auto with = [&](size_t at, uint32_t value) {
        std::string image = good;
        std::memcpy(image.data() + at, &value, 4);
        return image;
    };

    // Until the framework is told of compiled mods, a mod.nro is one more file.
    mod_of("built.one", good);
    CHECK(started("built.one") == "<running>" && log_is("") && asked(""));
    CHECK(tsuchi::register_compiled_mods());

    // With no library of the system's to be found, a compiled mod cannot run: nothing leads to one, or what does leads
    // to no module.
    const char* const no_loader = "mod.nro: the system's loader was not found";
    fake::put<uint64_t>(anchor_at, 0);
    tsuchi::forget_mods();
    size_t out = blocks_out.size();
    CHECK(started("built.one") == no_loader && asked("") && !tsuchi::find_mod("built.one")->context);
    fake::put<uint64_t>(anchor_at, fake::memory(0x3000) + 0x2100);
    CHECK(started("built.one") == no_loader && asked(""));
    const std::vector<fake::Offered> whole = {{"_ZN2nn2ro10InitializeEv", reinterpret_cast<uint64_t>(&loader_start)},
                                              {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", reinterpret_cast<uint64_t>(&loader_register)},
                                              {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", reinterpret_cast<uint64_t>(&loader_unregister)},
                                              {"_ZN2nn2ro13GetBufferSizeEPmPKv", reinterpret_cast<uint64_t>(&loader_room)},
                                              {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", reinterpret_cast<uint64_t>(&loader_load)},
                                              {"_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", reinterpret_cast<uint64_t>(&loader_lookup)}};
    // A library that lacks any one of the loader's functions has no loader.
    for (size_t missing = 0; missing < whole.size(); ++missing) {
        std::vector<fake::Offered> partial = whole;
        partial.erase(partial.begin() + long(missing));
        fake::put<uint64_t>(anchor_at, fake::loaded_module(partial) + 0x2100);
        CHECK(started("built.one") == no_loader && asked(""));
    }
    fake::put<uint64_t>(anchor_at, fake::loaded_module(whole) + 0x2100);
    tsuchi::forget_mods();
    // A context each time, and once the room in which the framework notes what code it has loaded.
    CHECK(blocks_out.size() == out + 2 + whole.size() + 1 && counts.loaded == 0 && counts.started == 0);

    // A file that is no module is never shown to the loader: not whole pages, less than one, another kind of file, a
    // size that is not its own, a part that reaches out of it. Nor is one that cannot be read whole.
    const char* const no_module = "mod.nro: it is not a module built for the game";
    for (const std::string& bad : {good.substr(0, 0x1800), good.substr(0, 0x100), good + "x", with(0x10, 0x3052524E), with(0x18, 0x3000), with(0x34, 0x1001), std::string("")}) {
        out = blocks_out.size();
        file("built.one", "mod.nro", bad);
        CHECK(started("built.one") == no_module && asked("") && !tsuchi::find_mod("built.one")->context);
        tsuchi::forget_mods();
        CHECK(blocks_out.size() == out + 1 && log_is(""));
    }
    file("built.one", "mod.nro", good);
    disk.short_in = "mod.nro";
    CHECK(started("built.one") == "mod.nro: it could not be read" && asked(""));
    disk.short_in.clear(), disk.failing_in = "mod.nro";
    CHECK(started("built.one") == "mod.nro: it could not be read" && asked(""));
    disk.failing_in.clear();

    // The loader will not start: the engine is not told it has, so that it is tried again.
    const char* const not_taken = "mod.nro: the system's loader would not take it";
    fake::put<uint8_t>(state_at, 0);
    made_loader.start_answer = 0x1234;
    CHECK(started("built.one") == "mod.nro: the system's loader would not start" && asked("start ") && fake::get<uint8_t>(state_at) == 0 && counts.refused == 0x1234);
    made_loader.start_answer = 0;
    // It will not take the list, cannot say what room the module needs, or will not take the file: the list it did take
    // is taken back, one it never had is not, and nothing is kept.
    tsuchi::forget_mods();
    out = blocks_out.size();
    made_loader.register_answer = 0x2345;
    CHECK(started("built.one") == not_taken && asked("start register-refused ") && fake::get<uint8_t>(state_at) == t::constant::loader_started && counts.refused == 0x2345);
    made_loader.register_answer = 0, made_loader.room_answer = 0x3456;
    CHECK(started("built.one") == not_taken && asked("register room-refused unregister ") && counts.refused == 0x3456);
    made_loader.room_answer = 0, made_loader.load_answer = 0x4567;
    CHECK(started("built.one") == not_taken && asked("register room load-refused unregister ") && counts.refused == 0x4567 && counts.loaded == 0 && counts.started == 0);
    made_loader.load_answer = 0;
    tsuchi::forget_mods();
    CHECK(blocks_out.size() == out + 3 && made_loader.well_formed && made_loader.lists.empty() && made_loader.modules.empty() && log_is(""));
    // No memory for any one of the things the loader is given: the same, with the reason.
    for (size_t refused : {size_t(0x3000), size_t(t::constant::loader_module_bytes + t::constant::loader_registration_bytes), size_t(0x2000)}) {
        refused_size = refused; // the file's pages and the room the module needs; the loader's records; the list
        CHECK(started("built.one") == "mod.nro: there is no memory to load it" && made_loader.lists.empty() && made_loader.modules.empty() && counts.loaded == 0);
        refused_size = 0;
    }
    made_loader.log.clear();
    // The room the module needs, when the file's pages were to be had: the list the loader took is taken back.
    mod_of("built.one", module_image(0, 4, 0x2000));
    refused_size = 0x3000;
    CHECK(started("built.one") == "mod.nro: there is no memory to load it" && asked("register room unregister ") && made_loader.lists.empty());
    refused_size = 0;
    mod_of("built.one", good);

    // A compiled mod is loaded when it starts: the loader is started, since the engine has not, then given a list that
    // vouches for the file, asked what room the module needs, and given the file; and the mod's own start is called
    // with everything the framework offers and its context.
    tsuchi::forget_mods();
    fake::put<uint8_t>(state_at, 0);
    out = blocks_out.size();
    CHECK(started("built.one") == "<running>" && asked("start register room load lookup:tsuchi_mod_start ") && log_is("[built.one] compiled runs\n"));
    CHECK(made_loader.well_formed && compiled_ran.host == &tsuchi::host && compiled_ran.mod == tsuchi::find_mod("built.one")->context && fake::get<uint8_t>(state_at) == t::constant::loader_started);
    CHECK(counts.loaded == 1 && counts.started == 1 && counts.refused == 0x4567 && made_loader.lists.size() == 1 && made_loader.modules.size() == 1);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(compiled_ran.frames == 2);
    // When the mod stops, what it made goes, and its code stays: the loader is asked nothing.
    tsuchi::forget_mods();
    tsuchi::frame(0.03f);
    CHECK(asked("") && compiled_ran.frames == 2 && made_loader.lists.size() == 1 && made_loader.modules.size() == 1 && counts.loaded == 1);
    const size_t kept = blocks_out.size() - out - 1; // besides the mod's context: the file's pages, the room, the list, and the loader's records
    CHECK(kept == 4);
    // Started again, the same code is: nothing is loaded, and nothing more is kept.
    out = blocks_out.size();
    CHECK(started("built.one") == "<running>" && asked("") && log_is("[built.one] compiled runs\n") && compiled_ran.starts == 2 && counts.loaded == 1 && counts.started == 2);
    CHECK(compiled_ran.mod == tsuchi::find_mod("built.one")->context);
    tsuchi::forget_mods();
    CHECK(blocks_out.size() == out + 1);
    // With another file in its place, the mod cannot run until the game is started again: its code is the old file's.
    file("built.one", "mod.nro", module_image(1));
    CHECK(started("built.one") == "mod.nro: its file is not the one its code was loaded from; the game has to be started again" && asked("") && log_is("") && counts.started == 2);
    file("built.one", "mod.nro", with(0x10, 0x3052524E));
    CHECK(started("built.one") == no_module && asked(""));
    file("built.one", "mod.nro", good);

    // The loader is started once: not again by the framework, and not at all when the engine has started it. Each
    // compiled mod has its own list and its own module, in the order mods start. One that needs no room is given none.
    fake::put<uint8_t>(state_at, 2);
    mod_of("built.two", module_image(1, 3, 0), ", \"requires\": {\"built.one\": \"1.0.0\"}");
    CHECK(started("built.two") == "<running>" && problem("built.one") == "<running>" && fake::get<uint8_t>(state_at) == 2);
    CHECK(asked("register room load lookup:tsuchi_mod_start ") && log_is("[built.one] compiled runs\n[built.two] second runs\n"));
    CHECK(made_loader.lists.size() == 2 && made_loader.modules.size() == 2 && counts.loaded == 2 && counts.started == 4 && made_loader.well_formed);
    gone("built.two");

    // The module has no start for the framework to call: the loader says so. The code is loaded all the same, and
    // stays; the mod cannot run, now or when it is tried again.
    mod_of("built.mute", module_image(0, 2, 0x1000));
    made_loader.lookup_answer = 0x5678;
    CHECK(started("built.mute") == "mod.nro: it has no tsuchi_mod_start" && asked("register room load lookup:tsuchi_mod_start ") && counts.refused == 0x4567 && counts.loaded == 3);
    made_loader.lookup_answer = 0;
    CHECK(started("built.mute") == "mod.nro: it has no tsuchi_mod_start" && asked("") && counts.loaded == 3 && counts.started == 6 && compiled_ran.starts == 5);
    gone("built.mute");
    mod_of("built.dumb", module_image(0, 2, 0x3000));
    made_loader.lookup_finds_nothing = true;
    CHECK(started("built.dumb") == "mod.nro: it has no tsuchi_mod_start" && asked("register room load lookup:tsuchi_mod_start ") && counts.loaded == 4);
    made_loader.lookup_finds_nothing = false;
    gone("built.dumb");
    CHECK(log_is("[built.one] compiled runs\n[built.one] compiled runs\n[built.one] compiled runs\n") && made_loader.well_formed);

    // A mod whose start refuses is blocked with what it answered. What it made before it refused is undone, and a mod
    // that needs it does not start.
    mod_of("built.two", module_image(1, 3, 0), ", \"requires\": {\"built.one\": \"1.0.0\"}");
    compiled_ran.answer = TSUCHI_NOT_READY, compiled_ran.frames = 0;
    tsuchi::forget_mods();
    out = blocks_out.size();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::prepare_mods();
    TSUCHI_Mod* const provider = tsuchi::find_mod("built.one")->context;
    TSUCHI_Mod* const dependent = tsuchi::find_mod("built.two")->context;
    CHECK(provider && dependent && provider != dependent && tsuchi::mod_usable(provider) && tsuchi::mod_usable(dependent));
    tsuchi::start_mods();
    CHECK(problem("built.one") == "mod.nro: its start answered not_ready" && problem("built.two") == "it needs built.one, which cannot run" &&
          !tsuchi::find_mod("built.two")->context);
    CHECK(asked("") && log_is("[built.one] compiled runs\n") && !tsuchi::find_mod("built.one")->context);
    tsuchi::frame(0.03f);
    CHECK(compiled_ran.frames == 0 && !tsuchi::mod_usable(compiled_ran.mod));
    tsuchi::forget_mods();
    CHECK(blocks_out.count(provider) == 1 && blocks_out.count(dependent) == 1 &&
          !tsuchi::mod_running(provider) && !tsuchi::mod_running(dependent) && !tsuchi::owned_count(provider) && !tsuchi::owned_count(dependent));
    CHECK(blocks_out.size() == out + 2); // the two explicitly captured contexts, with all startup ownership released
    compiled_ran.answer = TSUCHI_Result(77); // an answer that has no name
    CHECK(started("built.one") == "mod.nro: its start answered unknown");
    compiled_ran.answer = TSUCHI_OK;
    gone("built.two"), gone("built.one");
    made_loader.log.clear(), logged.lines.clear();

    // No memory at any point while a compiled mod is first started: it is blocked or it runs, the loader is left with
    // nothing it was not asked to take back, and nothing is left behind but code that was loaded. One mod after
    // another, so that the room in which the framework notes what it has loaded has to grow on the way.
    int needed = 0;
    for (int round = 0; round < 14; ++round)
    for (needed = 0;; ++needed) {
        const std::string late_id = "built.late" + std::to_string(round);
        const char* const late = late_id.c_str();
        if (!needed) gone("built.late" + std::to_string(round - 1)), mod_of(late_id, module_image(0, 5 + uint32_t(round), 0x1000));
        tsuchi::forget_mods();
        const size_t before = blocks_out.size(), had = made_loader.modules.size();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        allocations_left = needed;
        tsuchi::start_mods();
        allocations_left = -1;
        const bool ran = problem(late) == "<running>", is_loaded = made_loader.modules.size() == had + 1;
        CHECK(made_loader.lists.size() == made_loader.modules.size() && made_loader.well_formed && (is_loaded || made_loader.modules.size() == had));
        if (ran) CHECK(logged.lines == "[" + late_id + "] compiled runs\n" && is_loaded);
        logged.lines.clear();
        tsuchi::forget_mods();
        CHECK(blocks_out.size() <= before + 1 + (is_loaded ? kept + 1 : 0)); // and that room, when it grew
        if (is_loaded || needed > 2000) break;
    }
    CHECK(needed >= 6 && needed <= 2000 && counts.loaded == 18);
    gone("built.late13");

    // A mod that hooks a function the game calls directly needs the framework's patch, and the patch has to be the
    // one the framework was built with. With that patch the mod runs, and its function is in the game's way.
    tsuchi::forget_mods();
    logged.lines.clear();
    made_loader.code.push_back(compiled_hooks_start);
    const unsigned update = t::code_site::update;
    // The game reaches the site: true when the mod's function ran on the way to the game's own.
    const auto game_updates = [&] {
        const int before = compiled_hooks.calls, game_before = compiled_hooks.game_calls;
        thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | update), 1, 2, 3, 4);
        CHECK(compiled_hooks.game_calls == game_before + 1);
        return compiled_hooks.calls == before + 1;
    };
    const std::vector<uint32_t> code_as_it_was = [&] {
        std::vector<uint32_t> words;
        for (unsigned site = 0; site < t::code_site_count; ++site)
            words.push_back(t::present(t::code_sites[site].address) ? fake::get<uint32_t>(fake::main_image() + t::code_sites[site].address) : 0);
        return words;
    }();
    const auto code_back = [&] {
        for (unsigned site = 0; site < t::code_site_count; ++site)
            if (t::present(t::code_sites[site].address)) fake::put<uint32_t>(fake::main_image() + t::code_sites[site].address, code_as_it_was[site]);
        fake::patch_mark();
        fake::patch_site(update);
        return tsuchi::find_patch() == tsuchi::Patch::right;
    };
    CHECK(code_back());
    // The way back to the game is in the patch's block, which is not code here: the game's own function stands in.
    TSUCHI_Mod* const game_side = tsuchi::mod_create("the.game.update");
    uint64_t game_hook = 0;
    void* unused = nullptr;
    CHECK(tsuchi::hook_code_site(game_side, update, reinterpret_cast<void*>(&game_update), &game_hook, &unused) == TSUCHI_OK);
    mod_of("built.hook", module_image(2, 26, 0x1000));
    mod_of("built.one", good);
    mod_of("built.over", module_image(1, 3, 0), ", \"requires\": {\"built.hook\": \"1.0.0\"}");
    compiled_ran.frames = 0;
    CHECK(started("built.hook") == "<running>" && problem("built.one") == "<running>" && problem("built.over") == "<running>");
    CHECK(log_is("[built.hook] hooked\n[built.one] compiled runs\n[built.over] second runs\n") && compiled_hooks.wrapped == TSUCHI_OK && compiled_hooks.hook.id.id);
    compiled_hooks.calls = 0;
    CHECK(game_updates() && compiled_hooks.calls == 1);
    tsuchi::forget_mods();
    CHECK(!game_updates() && compiled_hooks.calls == 1); // its hook went with it

    // With no patch, or another build's, the mod is blocked and the mod list says which: for a mod that gives up when
    // its hook is refused, and for one that would carry on without it. What it made before it asked is undone, a mod
    // that needs it does not start, and a mod that hooks nothing of the kind runs as it always did.
    const char* const not_installed = "it hooks the game's code, and the framework's patch is not installed";
    const char* const another_version = "it hooks the game's code, and the framework's patch is from another version";
    for (const bool gives_up : {false, true}) {
        compiled_hooks.gives_up = gives_up;
        for (unsigned site = 0; site < t::code_site_count; ++site) fake::patch_site(site, false);
        fake::no_patch_mark();
        CHECK(tsuchi::find_patch() == tsuchi::Patch::missing);
        compiled_ran.frames = 0;
        CHECK(started("built.hook") == not_installed && problem("built.one") == "<running>" && problem("built.over") == "it needs built.hook, which cannot run");
        CHECK(log_is("[built.hook] not hooked\n[built.one] compiled runs\n") && compiled_hooks.wrapped == TSUCHI_NOT_READY && !compiled_hooks.hook.id.id && !tsuchi::find_mod("built.hook")->context);
        tsuchi::frame(0.03f);
        CHECK(compiled_ran.frames == 1 && !game_updates()); // built.one's, and not the blocked mod's

        for (unsigned site = 0; site < t::code_site_count; ++site) fake::patch_site(site);
        fake::patch_mark(t::stub::patch_hash + 1);
        CHECK(tsuchi::find_patch() == tsuchi::Patch::other);
        CHECK(started("built.hook") == another_version && problem("built.one") == "<running>" && problem("built.over") == "it needs built.hook, which cannot run");
        CHECK(log_is("[built.hook] not hooked\n[built.one] compiled runs\n") && compiled_hooks.wrapped == TSUCHI_NOT_READY && !game_updates());
    }
    // And with the right patch back, the same mod runs: the reason was that start's.
    CHECK(code_back());
    CHECK(started("built.hook") == "<running>" && problem("built.over") == "<running>" && game_updates() && compiled_hooks.calls == 2);
    CHECK(log_is("[built.hook] hooked\n[built.one] compiled runs\n[built.over] second runs\n"));
    compiled_hooks.gives_up = false;
    tsuchi::forget_mods();
    gone("built.hook"), gone("built.one"), gone("built.over");
    tsuchi::mod_stop(game_side);
    fake::put<uint32_t>(fake::main_image() + t::code_sites[update].address, code_as_it_was[update]);

    // Code of both kinds in one mod: each is started, in the order the kinds were added to the framework.
    tsuchi::forget_mods();
    logged.lines.clear();
    mod_of("built.both", module_image(0, 25, 0x1000));
    file("built.both", "main.lua", "print('script half runs')\n");
    CHECK(started("built.both") == "<running>" && log_is("[built.both] script half runs\n[built.both] compiled runs\n"));
    gone("built.both");
    disk.files.erase(mods_at + "built.both/main.lua");

    // A mod says which version of the framework's headers it was built against. One that does not say was not
    // built with the SDK, and one built for a newer framework may call what this one does not have: neither is
    // started. One built for this version or an older one is: the tables only grow at their ends.
    tsuchi::forget_mods();
    logged.lines.clear();
    const uint32_t started_before = tsuchi::compiled_counts().started;
    mod_of("built.unmarked", module_image(0, 21, 0x1000));
    made_loader.says_built_for = false;
    CHECK(started("built.unmarked") == "mod.nro: it does not say which version of the framework it was built for; build it with the SDK");
    made_loader.says_built_for = true;
    gone("built.unmarked");
    mod_of("built.newer", module_image(0, 22, 0x1000));
    made_loader.built_for = TSUCHI_API_VERSION + 1;
    CHECK(started("built.newer") == "mod.nro: it was built for a newer version of the framework than this one, and its manifest does not say it runs on this one");
    gone("built.newer");
    CHECK(tsuchi::compiled_counts().started == started_before && log_is(""));
    // With the lowest version it runs on in its manifest, its author has vouched for it, and it is started.
    mod_of("built.careful", module_image(0, 24, 0x1000), ", \"framework\": 0");
    CHECK(started("built.careful") == "<running>" && tsuchi::compiled_counts().started == started_before + 1);
    gone("built.careful");
    logged.lines.clear();
    mod_of("built.older", module_image(0, 23, 0x1000));
    made_loader.built_for = 0;
    CHECK(started("built.older") == "<running>" && tsuchi::compiled_counts().started == started_before + 2);
    made_loader.built_for = TSUCHI_API_VERSION;
    gone("built.older");
    tsuchi::forget_mods();
    tsuchi::set_log_sink(nullptr);
    made_loader.log.clear(), logged.lines.clear();
    disk.files = other_files;
    disk.log.clear();
}

// ---- Writing to the made-up disk -------------------------------------------------------------------

bool disk_folder_at(const std::string& path) {
    if (path == "save:" || disk.folders.count(path)) return true;
    for (const auto& [where, bytes] : disk.files)
        if (!where.compare(0, path.size() + 1, path + "/")) return true;
    return false;
}
std::string disk_folder_of(const std::string& path) { return path.substr(0, path.rfind('/')); }
// A change to the save: only while it is open, and refused when the check has the system refuse that step.
bool disk_changes(const char* step, const std::string& what) {
    disk.well_formed = disk.well_formed && disk.sessions == 1;
    if (disk.refuse == step) return false;
    disk.changes += std::string(step) + ":" + what + " ";
    return true;
}
uint32_t game_fs_entry_kind(void* out, void* path) {
    const std::string where = static_cast<const char*>(path);
    disk.well_formed = disk.well_formed && disk.sessions == 1 && where != "save:"; // the save's name alone is no path
    if (!disk.files.count(where) && !disk_folder_at(where)) return t::constant::fs_no_such_path;
    *static_cast<int32_t*>(out) = int32_t(disk.files.count(where) ? t::constant::fs_entry_folder + 1 : t::constant::fs_entry_folder);
    return 0;
}
uint32_t game_fs_create_file(void* path, int64_t size) {
    const std::string where = static_cast<const char*>(path);
    if (disk.files.count(where) || disk_folder_at(where) || !disk_folder_at(disk_folder_of(where)) || size < 0) return disk.well_formed = false, 0x402;
    if (!disk_changes("create", where + "=" + std::to_string(size))) return 0x99;
    disk.files[where] = std::string(size_t(size), '\0');
    return 0;
}
uint32_t game_fs_write_file(void* file, int64_t offset, void* data, uint64_t size, void* how) {
    const std::string& where = disk.opened[reinterpret_cast<uint64_t>(file) - 1];
    std::string& bytes = disk.files[where];
    // The file was opened to be written, the bytes fit in it, and the write is to reach it before the call returns.
    disk.well_formed = disk.well_formed && disk.modes[reinterpret_cast<uint64_t>(file) - 1] == int32_t(t::constant::fs_write) && offset >= 0 && size > 0 &&
                       uint64_t(offset) + size <= bytes.size() && *static_cast<uint32_t*>(how) == t::constant::fs_write_through;
    if (!disk_changes("write", where + "+" + std::to_string(size))) return 0x99;
    bytes.replace(size_t(offset), size_t(size), static_cast<const char*>(data), size_t(size));
    return 0;
}
uint32_t game_fs_delete_file(void* path) {
    const std::string where = static_cast<const char*>(path);
    if (!disk.files.count(where)) return disk.well_formed = false, t::constant::fs_no_such_path;
    if (!disk_changes("delete", where)) return 0x99;
    disk.folders.insert(disk_folder_of(where)); // the folder it was in stays
    disk.files.erase(where);
    return 0;
}
uint32_t game_fs_set_file_size(void* file, int64_t size) {
    const std::string& where = disk.opened[reinterpret_cast<uint64_t>(file) - 1];
    disk.well_formed = disk.well_formed && disk.modes[reinterpret_cast<uint64_t>(file) - 1] == int32_t(t::constant::fs_write) && size >= 0;
    if (!disk_changes("resize", where + "=" + std::to_string(size))) return 0x99;
    disk.files[where].resize(size_t(size), '\0');
    return 0;
}
uint32_t game_fs_create_folder(void* path) {
    const std::string where = static_cast<const char*>(path);
    if (disk.files.count(where) || disk_folder_at(where) || !disk_folder_at(disk_folder_of(where))) return disk.well_formed = false, 0x402;
    if (!disk_changes("folder", where)) return 0x99;
    disk.folders.insert(where);
    return 0;
}
uint32_t game_fs_commit(void* name) {
    disk.well_formed = disk.well_formed && !std::strcmp(static_cast<const char*>(name), "save") && !disk.open_files && !disk.open_folders;
    if (!disk_changes("keep", "")) return 0x99;
    ++disk.commits;
    return 0;
}

void keeping() {
    const TSUCHI_StorageService& kept = *tsuchi::host.storage;
    const uint64_t main = fake::main_image();
    char buffer[4096];
    uint64_t needed = 77, size = 77;
    const std::string mine = "save:/tsuchi/actor.test/data/";
    const auto changed = [](const std::string& expected) {
        const bool same = disk.changes == expected;
        if (!same) std::printf("  changed: [%s]\n", disk.changes.c_str());
        disk.changes.clear();
        return same;
    };
    const auto closed = [] { // nothing left open, and nothing done the system would refuse
        const bool right = disk.well_formed && !disk.sessions && !disk.open_files && !disk.open_folders;
        if (!right) std::printf("  the save was not left as it was found\n");
        return right;
    };
    const auto save = [&](TSUCHI_Mod* who, const char* name, const std::string& data) { return kept.write(who, name, data.data(), data.size()); };
    const auto listed = [&](TSUCHI_Mod* who) {
        std::vector<std::string> names;
        std::vector<char> big(1 << 16);
        if (kept.list(who, big.data(), big.size(), &needed) != TSUCHI_OK) return std::vector<std::string>{"<refused>"};
        const char* at = big.data();
        for (; *at; at += std::strlen(at) + 1) names.push_back(at);
        if (uint64_t(at - big.data()) + 1 != needed) names.push_back("<the size is wrong>");
        std::sort(names.begin(), names.end());
        return names;
    };
    const auto all = [&](const char* name) { // every call that names a piece
        return std::vector<TSUCHI_Result>{save(mod, name, "x"), kept.read(mod, name, 0, buffer, 4, &size), kept.size(mod, name, &size), kept.remove(mod, name)};
    };
    const auto each_is = [](const std::vector<TSUCHI_Result>& results, TSUCHI_Result expected) {
        for (TSUCHI_Result result : results)
            if (result != expected) return false;
        return true;
    };
    TSUCHI_Mod* other = tsuchi::mod_create("actor.keeper");
    const auto other_files = disk.files;
    disk.files = {{"save:/B/single.sav", "a slot's"}};
    disk.log.clear(), disk.changes.clear();

    // Not before the system's functions for changing files are known, every one of them.
    void** const writers[] = {reinterpret_cast<void**>(&t::fn::fs_entry_kind),  reinterpret_cast<void**>(&t::fn::fs_create_file), reinterpret_cast<void**>(&t::fn::fs_write_file),
                              reinterpret_cast<void**>(&t::fn::fs_delete_file), reinterpret_cast<void**>(&t::fn::fs_set_file_size), reinterpret_cast<void**>(&t::fn::fs_create_folder),
                              reinterpret_cast<void**>(&t::fn::fs_commit),      reinterpret_cast<void**>(&t::fn::save_begin_session),
                              reinterpret_cast<void**>(&t::fn::save_end_session)};
    t::fn::fs_entry_kind = game_fs_entry_kind, t::fn::fs_create_file = game_fs_create_file, t::fn::fs_write_file = game_fs_write_file, t::fn::fs_delete_file = game_fs_delete_file;
    t::fn::fs_set_file_size = game_fs_set_file_size, t::fn::fs_create_folder = game_fs_create_folder, t::fn::fs_commit = game_fs_commit;
    for (void** writer : writers) {
        void* const known = *writer;
        *writer = nullptr;
        CHECK(each_is(all("progress.json"), TSUCHI_NOT_READY) && kept.list(mod, buffer, sizeof buffer, &needed) == TSUCHI_NOT_READY);
        *writer = known;
    }
    CHECK(disk.log.empty() && changed(""));
    // Not while the game is busy with a save itself, and not when the save cannot be opened.
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 1);
    CHECK(each_is(all("progress.json"), TSUCHI_BUSY) && kept.list(mod, buffer, sizeof buffer, &needed) == TSUCHI_BUSY && disk.log.empty());
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 0);
    disk.mounts = false;
    CHECK(each_is(all("progress.json"), TSUCHI_IO) && kept.list(mod, buffer, sizeof buffer, &needed) == TSUCHI_IO && disk.log.empty() && changed(""));
    disk.mounts = true;
    // A name is a path within the mod's data that names a file.
    for (const char* name : {"", "/a", "..", "../other/data/a", "a/../../b", "a/..", "save:/a", "a\\b", "a/", "a//b", "/"}) CHECK(each_is(all(name), TSUCHI_INVALID));
    CHECK(disk.log.empty() && changed("") && closed());

    // Nothing is kept at first: nothing to list, read, measure or forget.
    CHECK(listed(mod).empty() && kept.read(mod, "progress.json", 0, buffer, 4, &size) == TSUCHI_NOT_FOUND && kept.size(mod, "progress.json", &size) == TSUCHI_NOT_FOUND &&
          kept.remove(mod, "progress.json") == TSUCHI_NOT_FOUND && changed("") && closed());
    disk.log.clear();

    // A piece is kept: the folders it needs are made, the file is made at its size and written from its start, and the
    // save is told to keep the change, all with the save open, and closed after.
    const size_t out = blocks_out.size();
    const std::string at = mine + "progress.json";
    CHECK(save(mod, "progress.json", "hello") == TSUCHI_OK && disk.files[at] == "hello" && closed() && blocks_out.size() == out);
    CHECK(changed("folder:save:/tsuchi folder:save:/tsuchi/actor.test folder:save:/tsuchi/actor.test/data create:" + at + "=5 write:" + at + "+5 keep: ") && disk.commits == 1 &&
          disk.log.find("begin ") == 0 && disk.log.rfind("end ") == disk.log.size() - 4);
    disk.log.clear();
    // Kept again: the file is given its new size and written over. A shorter one leaves nothing of the longer.
    CHECK(save(mod, "progress.json", "a longer one") == TSUCHI_OK && disk.files[at] == "a longer one" && closed());
    CHECK(changed("resize:" + at + "=12 write:" + at + "+12 keep: ") && disk.commits == 2);
    CHECK(save(mod, "progress.json", "short") == TSUCHI_OK && disk.files[at] == "short" && changed("resize:" + at + "=5 write:" + at + "+5 keep: "));
    // An empty one is a piece of no bytes, and nothing is written into it: made so, or emptied.
    CHECK(save(mod, "empty.bin", "") == TSUCHI_OK && disk.files.count(mine + "empty.bin") && disk.files[mine + "empty.bin"].empty() && changed("create:" + mine + "empty.bin=0 keep: "));
    CHECK(save(mod, "emptied", "full") == TSUCHI_OK && save(mod, "emptied", "") == TSUCHI_OK && disk.files[mine + "emptied"].empty() && kept.remove(mod, "emptied") == TSUCHI_OK && closed());
    CHECK(changed("create:" + mine + "emptied=4 write:" + mine + "emptied+4 keep: resize:" + mine + "emptied=0 keep: delete:" + mine + "emptied keep: "));
    // Bytes of any value, a zero among them, and a name with folders in it.
    const std::string bytes("\x00\xff\x01\x80 end", 8);
    CHECK(save(mod, "best/area1/times.bin", bytes) == TSUCHI_OK && disk.files[mine + "best/area1/times.bin"] == bytes && closed());
    CHECK(changed("folder:" + mine + "best folder:" + mine + "best/area1 create:" + mine + "best/area1/times.bin=8 write:" + mine + "best/area1/times.bin+8 keep: "));
    CHECK(save(mod, "best/area1/more.bin", "m") == TSUCHI_OK && disk.changes.find("folder:") == std::string::npos);
    disk.changes.clear(), disk.log.clear();

    // What is kept is read back, in parts, and measured.
    std::memset(buffer, 'x', sizeof buffer);
    CHECK(kept.size(mod, "progress.json", &size) == TSUCHI_OK && size == 5 && kept.size(mod, "empty.bin", &size) == TSUCHI_OK && size == 0 &&
          kept.size(mod, "best/area1/times.bin", &size) == TSUCHI_OK && size == 8 && closed());
    CHECK(kept.read(mod, "progress.json", 0, buffer, 100, &size) == TSUCHI_OK && size == 5 && !std::memcmp(buffer, "shortx", 6));
    CHECK(kept.read(mod, "progress.json", 2, buffer, 2, &size) == TSUCHI_OK && size == 2 && !std::memcmp(buffer, "orortx", 6));
    CHECK(kept.read(mod, "progress.json", 5, buffer, 4, &size) == TSUCHI_OK && size == 0 && kept.read(mod, "progress.json", 99, buffer, 4, &size) == TSUCHI_OK && size == 0);
    CHECK(kept.read(mod, "best/area1/times.bin", 0, buffer, 8, &size) == TSUCHI_OK && size == 8 && !std::memcmp(buffer, bytes.data(), 8));
    CHECK(kept.read(mod, "missing", 0, buffer, 4, &size) == TSUCHI_NOT_FOUND && kept.size(mod, "best", &size) == TSUCHI_NOT_FOUND && kept.size(mod, "best/area1", &size) == TSUCHI_NOT_FOUND);
    CHECK(changed("") && closed() && disk.commits == 9 && blocks_out.size() == out);

    // Every piece by its name, those in folders too. Each mod has its own: the same name is another piece.
    CHECK((listed(mod) == std::vector<std::string>{"best/area1/more.bin", "best/area1/times.bin", "empty.bin", "progress.json"}) && listed(other).empty() && closed());
    CHECK(save(other, "progress.json", "theirs") == TSUCHI_OK && disk.files["save:/tsuchi/actor.keeper/data/progress.json"] == "theirs" && disk.files[mine + "progress.json"] == "short");
    CHECK((listed(other) == std::vector<std::string>{"progress.json"}) && kept.read(other, "progress.json", 0, buffer, 6, &size) == TSUCHI_OK && !std::memcmp(buffer, "theirs", 6));
    CHECK(kept.size(other, "empty.bin", &size) == TSUCHI_NOT_FOUND && kept.remove(other, "empty.bin") == TSUCHI_NOT_FOUND && listed(mod).size() == 4 && closed());
    std::memset(buffer, 'x', sizeof buffer);
    CHECK(kept.list(other, buffer, 14, &needed) == TSUCHI_CAPACITY && needed == 15 && buffer[0] == 'x' && kept.list(other, buffer, 15, &needed) == TSUCHI_OK && buffer[15] == 'x');
    // Nothing of the slots is among a mod's data, and a mod's data is in no slot.
    CHECK(disk.files.count("save:/B/single.sav") && disk.files["save:/B/single.sav"] == "a slot's");
    disk.changes.clear();

    // A piece is forgotten, and the save told to keep that. Its folder stays, and holds no piece.
    CHECK(kept.remove(mod, "best/area1/more.bin") == TSUCHI_OK && changed("delete:" + mine + "best/area1/more.bin keep: ") && !disk.files.count(mine + "best/area1/more.bin") && closed());
    CHECK(kept.remove(mod, "best/area1/more.bin") == TSUCHI_NOT_FOUND && kept.remove(mod, "best/area1") == TSUCHI_NOT_FOUND && kept.remove(mod, "best/area1/times.bin") == TSUCHI_OK);
    // A name cannot be both a piece and a folder of pieces.
    CHECK((listed(mod) == std::vector<std::string>{"empty.bin", "progress.json"}) && save(mod, "best/area1", "now a piece") == TSUCHI_IO && closed());
    CHECK(save(mod, "empty.bin/inside", "x") == TSUCHI_IO && closed() && disk.files.count(mine + "empty.bin") && !disk.files.count(mine + "empty.bin/inside"));
    disk.changes.clear();

    // The system refuses a step: the call fails, and the save is not told to keep anything. A piece that was to be made
    // is not there afterward; of one that was to be replaced, whatever the save itself does with a change it was not
    // told to keep.
    const int commits = disk.commits;
    for (const char* step : {"create", "write"}) {
        disk.refuse = step;
        CHECK(save(mod, "fresh", "another") == TSUCHI_IO && !disk.files.count(mine + "fresh") && closed() && disk.changes.find("keep:") == std::string::npos);
        disk.refuse.clear(), disk.changes.clear();
    }
    for (const char* step : {"resize", "write"}) {
        disk.refuse = step;
        CHECK(save(mod, "progress.json", "another") == TSUCHI_IO && disk.files.count(at) && closed() && disk.changes.find("keep:") == std::string::npos);
        CHECK(disk.changes.find("delete:") == std::string::npos); // and it is not taken away
        disk.refuse.clear(), disk.changes.clear();
    }
    disk.files[at] = "short";
    CHECK(disk.commits == commits);
    disk.changes.clear();
    // No folder can be made, the save will not keep the change, or forgetting is refused.
    disk.refuse = "folder";
    CHECK(save(mod, "new/folder/piece", "x") == TSUCHI_IO && !disk.files.count(mine + "new/folder/piece") && closed());
    disk.refuse = "keep";
    CHECK(save(mod, "progress.json", "unkept") == TSUCHI_IO && kept.remove(mod, "empty.bin") == TSUCHI_IO && closed());
    disk.refuse = "delete";
    CHECK(kept.remove(mod, "progress.json") == TSUCHI_IO && disk.files.count(mine + "progress.json") && closed());
    disk.refuse.clear();
    disk.changes.clear();

    // No memory for where a piece is, or for the names: refused, with nothing changed and nothing left open.
    refused_size = 256;
    CHECK(each_is(all("progress.json"), TSUCHI_NO_MEMORY) && kept.list(mod, buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && changed("") && closed() && blocks_out.size() == out);
    refused_size = 0;

    tsuchi::mod_stop(other);
    disk.files = other_files;
    disk.folders.clear();
    disk.log.clear(), disk.changes.clear();
}

// ---- Settings ------------------------------------------------------------------------------------------

std::string settings_heard;
void heard_setting(void*, const TSUCHI_SettingsChanged* event) { settings_heard += std::string(event->mod) + ":" + event->key + " "; }

void configuring() {
    const TSUCHI_SettingsService& set = *tsuchi::host.settings;
    const uint64_t main = fake::main_image();
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const auto manifest = [&](const std::string& id, const std::string& settings, const std::string& more = "") {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"Set\", \"version\": \"1.0.0\"" + more + (settings.empty() ? "" : ", \"settings\": " + settings) + "}";
    };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::running ? "<running>" : "<ready>");
    };
    const auto started = [&](const char* id) {
        tsuchi::forget_mods();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::start_mods();
        return problem(id);
    };
    const auto context = [](const char* id) { return tsuchi::find_mod(id)->context; };
    const auto heard = [](const std::string& expected) {
        const bool same = settings_heard == expected;
        if (!same) std::printf("  heard: [%s]\n", settings_heard.c_str());
        settings_heard.clear();
        return same;
    };
    const auto kept_at = [](const char* id) { return std::string("save:/tsuchi/") + id + "/settings.json"; };
    const auto kept = [&](const char* id, const std::string& expected) {
        const auto found = disk.files.find(kept_at(id));
        const bool same = found != disk.files.end() && found->second == expected;
        if (!same) std::printf("  kept: [%s]\n", found == disk.files.end() ? "<no file>" : found->second.c_str());
        return same;
    };
    tsuchi::forget_mods();
    const auto other_files = disk.files;
    disk.files.clear(), disk.folders.clear();
    disk.log.clear(), disk.changes.clear();

    // What a manifest may declare, and what it may not. A setting declared wrongly blocks the mod, with what is wrong.
    const struct {
        const char* settings;
        const char* why;
    } wrong[] = {
        {R"([])", R"(mod.json: "settings" is an object, in braces: each setting by its key)"},
        {R"({"Loud": {"type": "toggle", "default": true}})", R"(mod.json: the setting "Loud" has a key that is not spelled as a mod's ID is)"},
        {R"({"a:b": {"type": "toggle", "default": true}})", R"(mod.json: the setting "a:b" has a key that is not spelled as a mod's ID is)"},
        {R"({"": {"type": "toggle", "default": true}})", R"(mod.json: the setting "" has a key that is not spelled as a mod's ID is)"},
        {R"({"a2345678901234567890123456789012345678901234567x": {"type": "toggle", "default": true}})",
         R"(mod.json: the setting "a2345678901234567890123456789012345678901234567x" has a key that is not spelled as a mod's ID is)"},
        {R"({"loud": true})", R"(mod.json: the setting "loud" is an object, in braces)"},
        {R"({"loud": {"type": "toggle", "default": true, "colour": 1}})", R"(mod.json: the setting "loud" has a key the framework does not know: colour)"},
        {R"({"loud": {"default": true}})", R"(mod.json: the setting "loud" needs a "type": toggle, integer, number, text, choice or set)"},
        {R"({"loud": {"type": "switch", "default": true}})", R"(mod.json: the setting "loud" needs a "type": toggle, integer, number, text, choice or set)"},
        {R"({"loud": {"type": 1, "default": true}})", R"(mod.json: the setting "loud" needs a "type": toggle, integer, number, text, choice or set)"},
        {R"({"loud": {"type": "toggle", "default": true, "name": 5}})", R"(mod.json: the setting "loud" has a name or a description that is not a text)"},
        {R"({"loud": {"type": "toggle", "default": true, "description": []}})", R"(mod.json: the setting "loud" has a name or a description that is not a text)"},
        {R"({"loud": {"type": "toggle", "default": true, "public": "yes"}})", R"(mod.json: the setting "loud" has a "public" that is not true or false)"},
        {R"({"loud": {"type": "toggle", "default": true, "min": 0}})", R"(mod.json: the setting "loud" has a "min" or a "max", which only an integer or a number has)"},
        {R"({"name": {"type": "text", "default": "", "max": 4}})", R"(mod.json: the setting "name" has a "min" or a "max", which only an integer or a number has)"},
        {R"({"count": {"type": "integer", "default": 1, "max_length": 4}})", R"(mod.json: the setting "count" has a "max_length", which only a text has)"},
        {R"({"speed": {"type": "number", "default": 1, "choices": ["a"]}})", R"(mod.json: the setting "speed" has "choices", which only a choice or a set has)"},
        {R"({"count": {"type": "integer", "default": 1, "min": "0"}})", R"(mod.json: the setting "count" has a "min" or a "max" that is not a number of its kind)"},
        {R"({"count": {"type": "integer", "default": 1, "max": 9.5}})", R"(mod.json: the setting "count" has a "min" or a "max" that is not a number of its kind)"},
        {R"({"speed": {"type": "number", "default": 1, "min": "0"}})", R"(mod.json: the setting "speed" has a "min" or a "max" that is not a number of its kind)"},
        {R"({"count": {"type": "integer", "default": 5, "min": 6, "max": 5}})", R"(mod.json: the setting "count" has a "min" above its "max")"},
        {R"({"count": {"type": "integer", "default": 9007199254740993, "min": 9007199254740993, "max": 9007199254740992}})", R"(mod.json: the setting "count" has a "min" above its "max")"},
        {R"({"speed": {"type": "number", "default": 1, "min": 1.5, "max": 1.25}})", R"(mod.json: the setting "speed" has a "min" above its "max")"},
        {R"({"name": {"type": "text", "default": "", "max_length": -1}})", R"(mod.json: the setting "name" has a "max_length" that is not a whole number of bytes)"},
        {R"({"name": {"type": "text", "default": "", "max_length": 2.5}})", R"(mod.json: the setting "name" has a "max_length" that is not a whole number of bytes)"},
        {R"({"mode": {"type": "choice", "default": "a"}})", R"(mod.json: the setting "mode" needs "choices": a list of words)"},
        {R"({"mode": {"type": "set", "default": [], "choices": []}})", R"(mod.json: the setting "mode" needs "choices": a list of words)"},
        {R"({"mode": {"type": "choice", "default": "a", "choices": "a"}})", R"(mod.json: the setting "mode" needs "choices": a list of words)"},
        {R"({"mode": {"type": "choice", "default": "a", "choices": {"a": "a"}}})", R"(mod.json: the setting "mode" needs "choices": a list of words)"},
        {R"({"mode": {"type": "choice", "default": "a", "choices": ["a", 2]}})", R"(mod.json: the setting "mode" has a choice that is not a word)"},
        {R"({"mode": {"type": "choice", "default": "a", "choices": ["a", ""]}})", R"(mod.json: the setting "mode" has a choice that is not a word)"},
        {R"({"mode": {"type": "choice", "default": "a", "choices": ["a", "b\u0000c"]}})", R"(mod.json: the setting "mode" has a choice that is not a word)"},
        {R"({"mode": {"type": "set", "default": [], "choices": ["a", "b", "a"]}})", R"(mod.json: the setting "mode" has the same choice twice: a)"},
    };
    for (const auto& each : wrong) {
        manifest("set.wrong", each.settings);
        const std::string why = started("set.wrong");
        if (why != each.why) std::printf("  %s\n  is blocked with: %s\n", each.settings, why.c_str());
        CHECK(why == each.why && !tsuchi::find_mod("set.wrong")->context);
    }
    // A default has to be there, of the setting's kind, and within what the setting allows.
    for (const char* settings : {R"({"a": {"type": "toggle"}})", R"({"a": {"type": "toggle", "default": 1}})", R"({"a": {"type": "integer", "default": 1.5}})",
                                 R"({"a": {"type": "integer", "default": true}})", R"({"a": {"type": "integer", "default": 3, "min": 4}})",
                                 R"({"a": {"type": "integer", "default": 9, "max": 8}})", R"({"a": {"type": "number", "default": "1"}})",
                                 R"({"a": {"type": "number", "default": 0.5, "min": 0.75}})", R"({"a": {"type": "number", "default": 2, "max": 1.5}})",
                                 R"({"a": {"type": "text", "default": 5}})", R"({"a": {"type": "text", "default": "four", "max_length": 3}})",
                                 R"({"a": {"type": "text", "default": "a\u0000b"}})", R"({"a": {"type": "choice", "default": "c", "choices": ["a", "b"]}})",
                                 R"({"a": {"type": "choice", "default": 1, "choices": ["a", "b"]}})", R"({"a": {"type": "set", "default": "a", "choices": ["a", "b"]}})",
                                 R"({"a": {"type": "set", "default": ["a", "c"], "choices": ["a", "b"]}})", R"({"a": {"type": "set", "default": [1], "choices": ["a", "b"]}})"}) {
        manifest("set.wrong", settings);
        const std::string why = started("set.wrong");
        if (why != R"(mod.json: the setting "a" needs a "default" its kind and its limits allow)") std::printf("  %s\n  is blocked with: %s\n", settings, why.c_str());
        CHECK(why == R"(mod.json: the setting "a" needs a "default" its kind and its limits allow)");
    }
    manifest("set.wrong", R"({"a234567890123456789012345678901234567890123456x": {"type": "toggle", "default": true}})");
    CHECK(started("set.wrong") == "<running>");
    disk.files.erase(mods_at + "set.wrong/mod.json");
    CHECK(disk.changes.empty());

    // One of each kind, as a mod finds them when nothing is kept yet: at their defaults.
    const std::string every = R"({
        "loud": {"type": "toggle", "default": true, "name": "Loud", "description": "Whether it is."},
        "count": {"type": "integer", "default": 3, "min": -2, "max": 10, "public": true},
        "any": {"type": "integer", "default": -9000000000},
        "speed": {"type": "number", "default": 1.5, "min": 0.25, "max": 4},
        "free": {"type": "number", "default": 0},
        "level": {"type": "number", "default": 1},
        "nick": {"type": "text", "default": "Olimar", "max_length": 8, "public": false},
        "note": {"type": "text", "default": ""},
        "mode": {"type": "choice", "default": "calm", "choices": ["calm", "wild", "odd one"], "public": true},
        "extras": {"type": "set", "default": ["hats", "capes"], "choices": ["hats", "boots", "capes"]}
    })";
    manifest("set.one", every);
    manifest("set.two", R"({"count": {"type": "integer", "default": 77}})", ", \"requires\": {\"set.one\": \"*\"}");
    manifest("set.none", "");
    size_t out = blocks_out.size();
    CHECK(started("set.one") == "<running>" && problem("set.two") == "<running>" && problem("set.none") == "<running>");
    TSUCHI_Mod* one = context("set.one");
    TSUCHI_Mod* two = context("set.two");
    TSUCHI_Mod* none = context("set.none");
    TSUCHI_Subscription listening{};
    CHECK(set.on_changed(two, heard_setting, nullptr, &listening) == TSUCHI_OK);
    bool flag = false, in = false;
    int64_t whole = 0;
    float amount = 0;
    char text[64];
    uint64_t needed = 0;
    CHECK(set.toggle(one, "loud", &flag) == TSUCHI_OK && flag && set.integer(one, "count", &whole) == TSUCHI_OK && whole == 3 && set.integer(one, "any", &whole) == TSUCHI_OK &&
          whole == -9000000000 && set.number(one, "speed", &amount) == TSUCHI_OK && amount == 1.5f && set.number(one, "free", &amount) == TSUCHI_OK && amount == 0);
    CHECK(set.text(one, "nick", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Olimar") && needed == 7 && set.text(one, "note", text, sizeof text, &needed) == TSUCHI_OK &&
          !text[0] && needed == 1 && set.text(one, "mode", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "calm"));
    CHECK(set.has(one, "extras", "hats", &in) == TSUCHI_OK && in && set.has(one, "extras", "boots", &in) == TSUCHI_OK && !in && set.has(one, "extras", "capes", &in) == TSUCHI_OK && in);
    std::memset(text, 'x', sizeof text);
    CHECK(set.text(one, "nick", text, 6, &needed) == TSUCHI_CAPACITY && needed == 7 && text[0] == 'x' && set.text(one, "nick", text, 7, &needed) == TSUCHI_OK && text[7] == 'x');
    // A setting there is none of, one of another kind, a word that is no choice of the set's.
    CHECK(set.toggle(one, "missing", &flag) == TSUCHI_NOT_FOUND && set.integer(one, "", &whole) == TSUCHI_NOT_FOUND && set.toggle(none, "loud", &flag) == TSUCHI_NOT_FOUND &&
          set.toggle(mod, "loud", &flag) == TSUCHI_NOT_FOUND && set.reset(none, "loud") == TSUCHI_NOT_FOUND && set.set_toggle(mod, "loud", true) == TSUCHI_NOT_FOUND);
    CHECK(set.toggle(one, "count", &flag) == TSUCHI_INVALID && set.integer(one, "loud", &whole) == TSUCHI_INVALID && set.integer(one, "speed", &whole) == TSUCHI_INVALID &&
          set.number(one, "count", &amount) == TSUCHI_INVALID && set.text(one, "extras", text, sizeof text, &needed) == TSUCHI_INVALID &&
          set.text(one, "loud", text, sizeof text, &needed) == TSUCHI_INVALID && set.has(one, "mode", "calm", &in) == TSUCHI_INVALID && set.has(one, "extras", "gloves", &in) == TSUCHI_INVALID);
    // Nothing was kept, so nothing was read but an absence, once for each mod that has settings; nothing is written.
    tsuchi::frame(0.03f);
    CHECK(disk.changes.empty() && heard("") && tsuchi::settings_counts().read == 0 && tsuchi::settings_counts().unread == 0 && !disk.files.count(kept_at("set.one")));

    // Changing them. A value its setting does not allow is refused, and so is one of another kind.
    CHECK(set.set_toggle(one, "loud", false) == TSUCHI_OK && set.toggle(one, "loud", &flag) == TSUCHI_OK && !flag && set.set_integer(one, "count", 10) == TSUCHI_OK &&
          set.set_integer(one, "count", -2) == TSUCHI_OK && set.integer(one, "count", &whole) == TSUCHI_OK && whole == -2 && set.set_integer(one, "any", INT64_MIN) == TSUCHI_OK &&
          set.integer(one, "any", &whole) == TSUCHI_OK && whole == INT64_MIN);
    CHECK(set.set_integer(one, "count", 11) == TSUCHI_INVALID && set.set_integer(one, "count", -3) == TSUCHI_INVALID && set.set_integer(one, "loud", 1) == TSUCHI_INVALID &&
          set.set_toggle(one, "count", true) == TSUCHI_INVALID && set.integer(one, "count", &whole) == TSUCHI_OK && whole == -2);
    CHECK(set.set_number(one, "speed", 0.25f) == TSUCHI_OK && set.set_number(one, "speed", 4) == TSUCHI_OK && set.number(one, "speed", &amount) == TSUCHI_OK && amount == 4 &&
          set.set_number(one, "speed", 4.0001f) == TSUCHI_INVALID && set.set_number(one, "speed", 0.2499f) == TSUCHI_INVALID && set.set_number(one, "count", 1) == TSUCHI_INVALID &&
          set.set_number(one, "free", NAN) == TSUCHI_INVALID && set.set_number(one, "free", -1e30f) == TSUCHI_OK && set.number(one, "free", &amount) == TSUCHI_OK && amount == -1e30f);
    CHECK(set.set_text(one, "nick", "Louie") == TSUCHI_OK && set.set_text(one, "nick", "12345678") == TSUCHI_OK && set.set_text(one, "nick", "123456789") == TSUCHI_INVALID &&
          set.text(one, "nick", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "12345678") && set.set_text(one, "note", std::string(5000, 'n').c_str()) == TSUCHI_OK &&
          set.set_text(one, "loud", "true") == TSUCHI_INVALID && set.set_text(one, "extras", "hats") == TSUCHI_INVALID);
    CHECK(set.set_text(one, "mode", "odd one") == TSUCHI_OK && set.text(one, "mode", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "odd one") &&
          set.set_text(one, "mode", "Calm") == TSUCHI_INVALID && set.set_text(one, "mode", "") == TSUCHI_INVALID);
    CHECK(set.set_has(one, "extras", "boots", true) == TSUCHI_OK && set.set_has(one, "extras", "hats", false) == TSUCHI_OK && set.has(one, "extras", "boots", &in) == TSUCHI_OK && in &&
          set.has(one, "extras", "hats", &in) == TSUCHI_OK && !in && set.has(one, "extras", "capes", &in) == TSUCHI_OK && in && set.set_has(one, "extras", "gloves", true) == TSUCHI_INVALID &&
          set.set_has(one, "mode", "calm", true) == TSUCHI_INVALID);
    // Each change is told once, at the start of the next frame, to whoever listens; a value set to what it is, is none.
    CHECK(heard(""));
    tsuchi::frame(0.03f);
    CHECK(heard("set.one:loud set.one:count set.one:count set.one:any set.one:speed set.one:speed set.one:free set.one:nick set.one:nick set.one:note set.one:mode "
                "set.one:extras set.one:extras "));
    CHECK(set.set_toggle(one, "loud", false) == TSUCHI_OK && set.set_integer(one, "count", -2) == TSUCHI_OK && set.set_number(one, "speed", 4) == TSUCHI_OK &&
          set.set_text(one, "nick", "12345678") == TSUCHI_OK && set.set_text(one, "mode", "odd one") == TSUCHI_OK && set.set_has(one, "extras", "boots", true) == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(heard(""));

    // The values that are not their defaults are kept in the save, in the frame after they changed.
    const std::string note(5000, 'n');
    CHECK(kept("set.one", "{\n  \"loud\": false,\n  \"count\": -2,\n  \"any\": -9223372036854775808,\n  \"speed\": 4,\n  \"free\": -1e30,\n  \"nick\": \"12345678\",\n  \"note\": \"" +
                              note + "\",\n  \"mode\": \"odd one\",\n  \"extras\": [\"boots\", \"capes\"]\n}\n"));
    CHECK(tsuchi::settings_counts().written == 1 && disk.commits > 0 && !disk.files.count(kept_at("set.two")) && !disk.files.count(kept_at("set.none")));
    // Back at its default, a setting is no longer in the file; with all of them back, the file holds none.
    CHECK(set.reset(one, "loud") == TSUCHI_OK && set.reset(one, "any") == TSUCHI_OK && set.reset(one, "free") == TSUCHI_OK && set.reset(one, "note") == TSUCHI_OK &&
          set.reset(one, "extras") == TSUCHI_OK && set.reset(one, "loud") == TSUCHI_OK && set.toggle(one, "loud", &flag) == TSUCHI_OK && flag &&
          set.has(one, "extras", "hats", &in) == TSUCHI_OK && in && set.has(one, "extras", "boots", &in) == TSUCHI_OK && !in);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("set.one:loud set.one:any set.one:free set.one:note set.one:extras ") && kept("set.one", "{\n  \"count\": -2,\n  \"speed\": 4,\n  \"nick\": \"12345678\",\n  \"mode\": \"odd one\"\n}\n"));
    CHECK(set.set_integer(one, "count", 3) == TSUCHI_OK && set.reset(one, "speed") == TSUCHI_OK && set.set_text(one, "nick", "Olimar") == TSUCHI_OK && set.set_text(one, "mode", "calm") == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(kept("set.one", "{}\n") && tsuchi::settings_counts().written == 3);
    settings_heard.clear();

    // Runtime integer ranges are owned metadata, not a replacement saved preference.
    const uint32_t range_owned=tsuchi::owned_count(one);
    CHECK(set.set_integer_range(one,"count",0,2)==TSUCHI_OK);
    CHECK(set.integer(one,"count",&whole)==TSUCHI_OK && whole==2);
    CHECK(set.integer(two,"set.one:count",&whole)==TSUCHI_OK && whole==2);
    CHECK(tsuchi::owned_count(one)==range_owned+1);
    CHECK(set.set_integer_range(one,"count",0,2)==TSUCHI_OK && tsuchi::owned_count(one)==range_owned+1);
    CHECK(set.set_integer(one,"count",3)==TSUCHI_INVALID);
    CHECK(set.set_integer_range(two,"set.one:count",0,1)==TSUCHI_DENIED);
    CHECK(set.set_integer_range(one,"loud",0,1)==TSUCHI_INVALID);
    CHECK(set.set_integer_range(one,"count",3,2)==TSUCHI_INVALID);
    CHECK(set.set_integer_range(one,"count",-3,2)==TSUCHI_INVALID);
    CHECK(set.set_integer_range(one,"count",0,11)==TSUCHI_INVALID);
    tsuchi::frame(0.03f);
    CHECK(heard("set.one:count ") && kept("set.one","{}\n"));
    CHECK(set.set_integer_range(one,"count",0,0)==TSUCHI_OK && set.reset(one,"count")==TSUCHI_OK);
    CHECK(set.integer(one,"count",&whole)==TSUCHI_OK && whole==0);
    CHECK(set.set_integer_range(one,"count",-2,10)==TSUCHI_OK);
    CHECK(set.integer(one,"count",&whole)==TSUCHI_OK && whole==3);
    tsuchi::frame(0.03f); settings_heard.clear();

    // Another mod's setting is read as `<mod id>:<key>` when it is public, and never changed.
    CHECK(set.integer(two, "set.one:count", &whole) == TSUCHI_OK && whole == 3 && set.text(two, "set.one:mode", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "calm") &&
          set.integer(two, "count", &whole) == TSUCHI_OK && whole == 77);
    CHECK(set.toggle(two, "set.one:loud", &flag) == TSUCHI_DENIED && set.text(two, "set.one:nick", text, sizeof text, &needed) == TSUCHI_DENIED &&
          set.has(two, "set.one:extras", "hats", &in) == TSUCHI_DENIED && set.set_integer(two, "set.one:count", 4) == TSUCHI_DENIED && set.reset(two, "set.one:count") == TSUCHI_DENIED &&
          set.set_toggle(two, "set.one:loud", false) == TSUCHI_DENIED);
    CHECK(set.integer(two, "set.one:missing", &whole) == TSUCHI_NOT_FOUND && set.integer(two, "set.absent:count", &whole) == TSUCHI_NOT_FOUND &&
          set.integer(two, (std::string(60, 'a') + ":count").c_str(), &whole) == TSUCHI_NOT_FOUND && set.integer(two, (std::string(9000, 'a') + ":count").c_str(), &whole) == TSUCHI_NOT_FOUND && set.integer(two, ":count", &whole) == TSUCHI_NOT_FOUND &&
          set.integer(mod, "set.one:count", &whole) == TSUCHI_OK && whole == 3);
    // A mod may name its own that way too.
    CHECK(set.toggle(one, "set.one:loud", &flag) == TSUCHI_OK && flag && set.set_integer(one, "set.one:count", 4) == TSUCHI_OK && set.integer(two, "set.one:count", &whole) == TSUCHI_OK && whole == 4);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("set.one:count ") && kept("set.one", "{\n  \"count\": 4\n}\n"));

    // Runtime dependency control is owned, attributed, and never a saved edit.
    TSUCHI_Value held_value{}; held_value.kind=TSUCHI_VALUE_INTEGER; held_value.as.integer=8;
    TSUCHI_SettingHold held_setting{}, other_setting{};
    CHECK(set.hold(two,"set.one:count",&held_value,&held_setting)==TSUCHI_OK);
    CHECK(set.integer(one,"count",&whole)==TSUCHI_OK && whole==8);
    CHECK(set.controllers(one,"count",text,sizeof text,&needed)==TSUCHI_OK && !std::strcmp(text,"set.two"));
    CHECK(set.set_integer(one,"count",5)==TSUCHI_DENIED && set.reset(one,"count")==TSUCHI_DENIED);
    CHECK(set.hold(one,"count",&held_value,&other_setting)==TSUCHI_BUSY);
    CHECK(set.update_hold(one,held_setting,&held_value)==TSUCHI_DENIED && set.release_hold(one,held_setting)==TSUCHI_DENIED);
    held_value.as.integer=11;
    CHECK(set.update_hold(two,held_setting,&held_value)==TSUCHI_INVALID && set.integer(one,"count",&whole)==TSUCHI_OK && whole==8);
    held_value.as.integer=6;
    CHECK(set.update_hold(two,held_setting,&held_value)==TSUCHI_OK && set.integer(one,"count",&whole)==TSUCHI_OK && whole==6);
    const auto isolation=tsuchi::isolate_ownership(two,tsuchi::ownership_mark(two));
    CHECK(set.update_hold(two,held_setting,&held_value)==TSUCHI_BUSY && set.release_hold(two,held_setting)==TSUCHI_BUSY);
    tsuchi::isolate_ownership(two,isolation);
    tsuchi::frame(0.03f);
    CHECK(kept("set.one","{\n  \"count\": 4\n}\n"));
    CHECK(set.release_hold(two,held_setting)==TSUCHI_OK && set.release_hold(two,held_setting)==TSUCHI_NOT_FOUND);
    CHECK(set.integer(one,"count",&whole)==TSUCHI_OK && whole==4);
    CHECK(set.hold(context("set.none"),"set.one:count",&held_value,&other_setting)==TSUCHI_DENIED);
    CHECK(set.controllers(one,"count",text,sizeof text,&needed)==TSUCHI_OK && !*text);
    tsuchi::frame(0.03f); settings_heard.clear();

    // Numbers are written the shortest way that reads back as the same value.
    const struct {
        float value;
        const char* written;
    } numbers[] = {{0.1f, "0.1"},   {-2.5f, "-2.5"},          {1e-7f, "1e-7"},       {0.000015f, "0.000015"}, {0.0000015f, "1.5e-6"},   {123456.79f, "123456.79"},
                   {16777216.f, "16777216"}, {1e15f, "1000000000000000"}, {1e16f, "1e16"}, {3.4028235e38f, "3.4028235e38"}, {1.17549435e-38f, "1.1754944e-38"}, {1e-45f, "1e-45"},
                   {100.f, "100"},  {0.3f, "0.3"},            {1.f / 3, "0.33333334"}};
    CHECK(set.set_number(one, "level", 0) == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(kept("set.one", "{\n  \"count\": 4,\n  \"level\": 0\n}\n") && set.reset(one, "level") == TSUCHI_OK);
    for (const auto& each : numbers) {
        CHECK(set.set_number(one, "free", each.value) == TSUCHI_OK);
        tsuchi::frame(0.03f), tsuchi::frame(0.03f);
        const std::string expected = std::string("{\n  \"count\": 4,\n  \"free\": ") + each.written + "\n}\n";
        if (disk.files[kept_at("set.one")] != expected) std::printf("  %.9g is written as: %s", each.value, disk.files[kept_at("set.one")].c_str());
        CHECK(disk.files[kept_at("set.one")] == expected);
        // And the mod finds the very same value when it starts again.
        CHECK(started("set.one") == "<running>" && set.number(context("set.one"), "free", &amount) == TSUCHI_OK && amount == each.value && set.integer(context("set.one"), "count", &whole) == TSUCHI_OK && whole == 4);
        one = context("set.one");
    }
    two = context("set.two");
    settings_heard.clear();
    CHECK(set.on_changed(two, heard_setting, nullptr, &listening) == TSUCHI_OK);

    // What the save holds is how a mod finds its settings when it starts: nothing is told, and nothing written.
    disk.files[kept_at("set.one")] = R"({"loud": false, "count": 9, "any": 12, "speed": 0.5, "nick": "Brittany", "note": "a \"note\"\n", "mode": "wild", "extras": ["boots"]})";
    disk.changes.clear();
    const uint32_t read_before = tsuchi::settings_counts().read;
    CHECK(started("set.one") == "<running>");
    one = context("set.one"), two = context("set.two");
    CHECK(set.on_changed(two, heard_setting, nullptr, &listening) == TSUCHI_OK);
    CHECK(set.toggle(one, "loud", &flag) == TSUCHI_OK && !flag && set.integer(one, "count", &whole) == TSUCHI_OK && whole == 9 && set.integer(one, "any", &whole) == TSUCHI_OK && whole == 12 &&
          set.number(one, "speed", &amount) == TSUCHI_OK && amount == 0.5f && set.text(one, "nick", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Brittany") &&
          set.text(one, "note", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "a \"note\"\n") && set.text(one, "mode", text, sizeof text, &needed) == TSUCHI_OK &&
          !std::strcmp(text, "wild") && set.has(one, "extras", "boots", &in) == TSUCHI_OK && in && set.has(one, "extras", "hats", &in) == TSUCHI_OK && !in);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("") && disk.changes.empty() && tsuchi::settings_counts().read == read_before + 1);
    // A text with what JSON writes with a backslash goes back the way it came.
    CHECK(set.set_text(one, "note", "tab\there \"quoted\" back\\slash \x01\x1f end") == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(disk.files[kept_at("set.one")].find(R"("note": "tab\u0009here \"quoted\" back\\slash \u0001\u001f end")") != std::string::npos);
    CHECK(started("set.one") == "<running>" && set.text(context("set.one"), "note", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "tab\there \"quoted\" back\\slash \x01\x1f end"));
    settings_heard.clear();

    // What the save holds that the manifest does not allow is passed over, one setting at a time: of another kind,
    // out of its limits, no choice of its, for a setting there is none of.
    disk.files[kept_at("set.one")] = R"({"loud": 1, "count": 11, "any": 1.5, "speed": 5, "free": "1", "nick": "123456789", "note": 5, "mode": "tame", "extras": ["gloves", "boots", 4],
                                        "gone": true, "count2": 1})";
    CHECK(started("set.one") == "<running>");
    one = context("set.one");
    CHECK(set.toggle(one, "loud", &flag) == TSUCHI_OK && flag && set.integer(one, "count", &whole) == TSUCHI_OK && whole == 3 && set.integer(one, "any", &whole) == TSUCHI_OK &&
          whole == -9000000000 && set.number(one, "speed", &amount) == TSUCHI_OK && amount == 1.5f && set.number(one, "free", &amount) == TSUCHI_OK && amount == 0 &&
          set.text(one, "nick", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Olimar") && set.text(one, "mode", text, sizeof text, &needed) == TSUCHI_OK &&
          !std::strcmp(text, "calm") && set.has(one, "extras", "boots", &in) == TSUCHI_OK && in && set.has(one, "extras", "hats", &in) == TSUCHI_OK && !in);
    disk.files[kept_at("set.one")] = R"({"extras": "boots", "level": "3", "speed": true})";
    CHECK(started("set.one") == "<running>");
    one = context("set.one");
    CHECK(set.has(one, "extras", "hats", &in) == TSUCHI_OK && in && set.has(one, "extras", "boots", &in) == TSUCHI_OK && !in && set.number(one, "level", &amount) == TSUCHI_OK && amount == 1 &&
          set.number(one, "speed", &amount) == TSUCHI_OK && amount == 1.5f);
    // A file that is no JSON, or no object, leaves every setting at its default, and is counted. Nothing of it is kept.
    const uint32_t unread_before = tsuchi::settings_counts().unread;
    tsuchi::forget_mods();
    out = blocks_out.size();
    for (const char* held : {"{\"count\": 9", "[1, 2]", ""}) {
        disk.files[kept_at("set.one")] = held;
        CHECK(started("set.one") == "<running>" && set.integer(context("set.one"), "count", &whole) == TSUCHI_OK && whole == 3);
    }
    tsuchi::forget_mods();
    CHECK(tsuchi::settings_counts().unread == unread_before + 3 && blocks_out.size() == out + 9); // a context for each of three mods, three times
    // The save cannot be opened at all: the same, and nothing is asked of it while it is shut.
    disk.files[kept_at("set.one")] = R"({"count": 9})";
    disk.mounts = false;
    CHECK(started("set.one") == "<running>" && set.integer(context("set.one"), "count", &whole) == TSUCHI_OK && whole == 3 && tsuchi::settings_counts().unread == unread_before + 5); // this mod and the one that needs it
    disk.mounts = true;
    CHECK(disk.well_formed);

    // The save cannot be reached when the mod starts: it starts with its defaults, and the values arrive when it can
    // be, each told as a change. One the mod has changed in the meantime stays as the mod set it.
    const char* const held_late = R"({"loud": false, "count": 9, "speed": 0.5, "level": 7, "nick": "Louie", "mode": "wild", "extras": ["boots"]})";
    disk.files[kept_at("set.one")] = held_late;
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 1);
    CHECK(started("set.one") == "<running>");
    one = context("set.one"), two = context("set.two");
    CHECK(set.on_changed(two, heard_setting, nullptr, &listening) == TSUCHI_OK);
    CHECK(set.integer(one, "count", &whole) == TSUCHI_OK && whole == 3 && set.toggle(one, "loud", &flag) == TSUCHI_OK && flag);
    // Set to what they already are, a toggle and a text are the mod's all the same; so are a word of a set and a reset.
    CHECK(set.set_integer(one, "count", 5) == TSUCHI_OK && set.set_number(one, "speed", 2) == TSUCHI_OK && set.set_toggle(one, "loud", true) == TSUCHI_OK &&
          set.set_text(one, "nick", "Olimar") == TSUCHI_OK && set.set_has(one, "extras", "hats", true) == TSUCHI_OK && set.reset(one, "level") == TSUCHI_OK);
    disk.changes.clear(), disk.log.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("set.one:count set.one:speed ") && disk.changes.empty() && disk.log.empty()); // told, and not yet written
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 0);
    tsuchi::frame(0.03f);
    CHECK(set.toggle(one, "loud", &flag) == TSUCHI_OK && flag && set.integer(one, "count", &whole) == TSUCHI_OK && whole == 5 && set.text(one, "mode", text, sizeof text, &needed) == TSUCHI_OK &&
          !std::strcmp(text, "wild") && set.number(one, "speed", &amount) == TSUCHI_OK && amount == 2 && set.number(one, "level", &amount) == TSUCHI_OK && amount == 1 &&
          set.text(one, "nick", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Olimar") && set.has(one, "extras", "hats", &in) == TSUCHI_OK && in &&
          set.has(one, "extras", "boots", &in) == TSUCHI_OK && !in);
    tsuchi::frame(0.03f);
    CHECK(heard("set.one:mode ") && kept("set.one", "{\n  \"count\": 5,\n  \"speed\": 2,\n  \"mode\": \"wild\"\n}\n"));
    // No memory to read the file when the mod starts: the same. Nothing is written over it before it has been read.
    disk.files[kept_at("set.one")] = std::string(R"({"loud": false, "count": 9})") + std::string(306, ' ');
    refused_size = 333;
    CHECK(started("set.one") == "<running>");
    one = context("set.one");
    CHECK(set.integer(one, "count", &whole) == TSUCHI_OK && whole == 3 && set.set_number(one, "speed", 3) == TSUCHI_OK);
    disk.changes.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(disk.changes.empty() && disk.files[kept_at("set.one")].size() == 333);
    refused_size = 0;
    tsuchi::frame(0.03f);
    CHECK(set.integer(one, "count", &whole) == TSUCHI_OK && whole == 9 && kept("set.one", "{\n  \"loud\": false,\n  \"count\": 9,\n  \"speed\": 3\n}\n"));
    // A change made while the game is busy with a save is written when it no longer is, and the save told to keep it.
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 1);
    CHECK(set.set_integer(one, "count", 8) == TSUCHI_OK);
    disk.changes.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(disk.changes.empty());
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 0);
    const int commits_before = disk.commits;
    tsuchi::frame(0.03f);
    CHECK(kept("set.one", "{\n  \"loud\": false,\n  \"count\": 8,\n  \"speed\": 3\n}\n") && disk.commits == commits_before + 1);
    // A text there is no memory to keep is no change: nothing is told, and nothing written.
    refused_size = 778;
    CHECK(set.set_text(one, "note", std::string(777, 'm').c_str()) == TSUCHI_NO_MEMORY);
    refused_size = 0;
    disk.changes.clear(), settings_heard.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("") && disk.changes.empty() && set.text(one, "note", text, sizeof text, &needed) == TSUCHI_OK && !text[0]);
    // The save cannot be opened to write: nothing is asked of it while it is shut, and the write waits like any other.
    disk.mounts = false;
    CHECK(set.set_integer(one, "count", 7) == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(disk.changes.empty() && disk.well_formed);
    disk.mounts = true;
    for (int i = 0; i < 310; ++i) tsuchi::frame(0.03f);
    CHECK(kept("set.one", "{\n  \"loud\": false,\n  \"count\": 7,\n  \"speed\": 3\n}\n"));
    disk.files[kept_at("set.one")] = "{\n  \"loud\": false,\n  \"count\": 5,\n  \"speed\": 2,\n  \"mode\": \"wild\"\n}\n";
    CHECK(started("set.one") == "<running>");
    one = context("set.one"), two = context("set.two");
    CHECK(set.on_changed(two, heard_setting, nullptr, &listening) == TSUCHI_OK);

    // The save will not take a write: it is tried again after a while, not every frame.
    const uint32_t unwritten_before = tsuchi::settings_counts().unwritten;
    disk.refuse = "write";
    CHECK(set.set_integer(one, "count", 6) == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    disk.changes.clear();
    for (int i = 0; i < 290; ++i) tsuchi::frame(0.03f);
    CHECK(disk.changes.empty() && tsuchi::settings_counts().unwritten == unwritten_before + 1);
    disk.refuse.clear();
    for (int i = 0; i < 20; ++i) tsuchi::frame(0.03f);
    CHECK(kept("set.one", "{\n  \"loud\": false,\n  \"count\": 6,\n  \"speed\": 2,\n  \"mode\": \"wild\"\n}\n"));
    settings_heard.clear();

    // A mod that is not running has no settings to read, and one with none of its own costs the save nothing.
    manifest("set.blocked", R"({"count": {"type": "integer", "default": 1, "public": true}})", ", \"requires\": {\"set.absent\": \"*\"}");
    disk.files["save:/tsuchi/set.blocked/settings.json"] = R"({"count": 5})";
    CHECK(started("set.blocked") != "<running>" && set.integer(context("set.one"), "set.blocked:count", &whole) == TSUCHI_NOT_FOUND);
    disk.log.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(disk.log.find("begin") == std::string::npos); // the save is not opened for a mod that does not run
    disk.files.erase(mods_at + "set.blocked/mod.json");
    disk.files.erase(mods_at + "set.one/mod.json"), disk.files.erase(mods_at + "set.two/mod.json");
    disk.log.clear();
    CHECK(started("set.none") == "<running>");
    for (int i = 0; i < 5; ++i) tsuchi::frame(0.03f);
    disk.changes.clear();
    CHECK(disk.log.find("begin") == std::string::npos && disk.changes.empty()); // nor for one that has no settings

    // From a script: the same calls, a setting's value as what it is, and a change heard as a table.
    disk.files.clear();
    manifest("set.one", every);
    manifest("set.script", every, ", \"requires\": {\"set.one\": \"*\"}");
    disk.files[mods_at + "set.script/main.lua"] = R"(print(settings.toggle("loud"), settings.integer("count"), settings.number("speed"), settings.text("nick"), settings.text("mode"), settings.has("extras", "hats"))
print(settings.set_integer("count", 7), settings.set_integer("count", 99))
print(settings.set_number("speed", 2.5), settings.set_text("mode", "wild"), settings.set_has("extras", "boots", true), settings.set_toggle("loud", false), settings.reset("nick"))
print(settings.integer("set.one:count"), settings.toggle("set.one:loud"))
print(settings.integer("count"), settings.number("speed"), settings.text("mode"), settings.has("extras", "boots"), settings.toggle("loud"))
print(pcall(settings.set_integer, "count", 1.5), pcall(settings.toggle))
local live_setting = assert(settings.hold("set.one:count", 8))
assert(settings.integer("set.one:count") == 8)
assert(settings.update_hold(live_setting, 6) and settings.integer("set.one:count") == 6)
assert(settings.release_hold(live_setting) and settings.integer("set.one:count") == 3)
local whole_float = assert(settings.hold("set.one:count", 4/2))
assert(settings.integer("set.one:count") == 2 and settings.release_hold(whole_float))
settings.on_changed(function(event) print("changed", event.mod, event.key) end)
)";
    tsuchi::set_log_sink(log_sink);
    logged.lines.clear();
    CHECK(started("set.script") == "<running>");
    CHECK(logged.lines.find("[set.script] true\t3\t1.5\tOlimar\tcalm\ttrue\n[set.script] true\tnil\tinvalid\n[set.script] true\ttrue\ttrue\ttrue\ttrue\n"
                            "[set.script] 3\tnil\tdenied\n[set.script] 7\t2.5\twild\ttrue\tfalse\n[set.script] false\t") == 0);
    logged.lines.clear();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(logged.lines == "[set.script] changed\tset.script\tcount\n[set.script] changed\tset.script\tspeed\n[set.script] changed\tset.script\tmode\n"
                          "[set.script] changed\tset.script\textras\n[set.script] changed\tset.script\tloud\n"
                          "[set.script] changed\tset.one\tcount\n[set.script] changed\tset.one\tcount\n[set.script] changed\tset.one\tcount\n"
                          "[set.script] changed\tset.one\tcount\n[set.script] changed\tset.one\tcount\n");
    CHECK(kept("set.script", "{\n  \"loud\": false,\n  \"count\": 7,\n  \"speed\": 2.5,\n  \"mode\": \"wild\",\n  \"extras\": [\"hats\", \"boots\", \"capes\"]\n}\n"));
    tsuchi::set_log_sink(nullptr);
    logged.lines.clear();
    settings_heard.clear();

    TSUCHI_Mod* range_script=context("set.script");
    CHECK(set.set_integer_range(range_script,"count",0,1)==TSUCHI_OK);
    auto* ranged_setting=tsuchi::find_setting(tsuchi::find_mod("set.script")->settings,"count");
    CHECK(ranged_setting && ranged_setting->ranged && ranged_setting->integer==7);
    held_value.as.integer=6;
    CHECK(set.hold(range_script,"set.one:count",&held_value,&other_setting)==TSUCHI_OK);
    CHECK(set.integer(context("set.one"),"count",&whole)==TSUCHI_OK && whole==6);
    tsuchi::mod_stop(range_script);
    CHECK(!ranged_setting->ranged && ranged_setting->integer==7);
    CHECK(set.integer(context("set.one"),"count",&whole)==TSUCHI_OK && whole==3);
    manifest("set.javascript", "", ", \"requires\": {\"set.one\": \"*\"}");
    disk.files[mods_at + "set.javascript/main.js"] = R"(
function check(value) { if (!value) throw new Error("runtime settings check"); }
const held = settings.hold("set.one:count", 8);
check(settings.integer("set.one:count") === 8n);
check(settings.update_hold(held, 6n) && settings.integer("set.one:count") === 6n);
check(settings.release_hold(held) && settings.integer("set.one:count") === 3n);
let refused = false;
try { settings.hold("set.one:count", 3.5); } catch (_) { refused = true; }
check(refused);
)";
    CHECK(started("set.javascript") == "<running>");
    // Nothing of a mod's settings is left when the mods are forgotten.
    tsuchi::forget_mods();
    tsuchi::frame(0.03f);
    disk.files.clear();
    manifest("set.one", every);
    disk.files[kept_at("set.one")] = R"({"count": 9, "nick": "Louie"})";
    out = blocks_out.size();
    CHECK(started("set.one") == "<running>" && set.set_text(context("set.one"), "note", "kept a while") == TSUCHI_OK && set.set_has(context("set.one"), "extras", "boots", true) == TSUCHI_OK &&
          set.set_number(context("set.one"), "speed", 2.75f) == TSUCHI_OK);
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    // No memory at any point while the values are written: they are written a little later, and nothing is left.
    for (int left = 0; left < 40; ++left) {
        CHECK(set.set_text(context("set.one"), "note", (std::string(3000, 'w') + std::to_string(left)).c_str()) == TSUCHI_OK);
        allocations_left = left;
        tsuchi::frame(0.03f);
        allocations_left = -1;
        tsuchi::frame(0.03f);
        CHECK(disk.files[kept_at("set.one")].find("www" + std::to_string(left) + "\"") != std::string::npos);
    }
    tsuchi::forget_mods();
    tsuchi::frame(0.03f);
    CHECK(blocks_out.size() == out + 1 && disk.well_formed);
    settings_heard.clear();
    // No memory for the settings as a manifest is read: the mod is blocked, and nothing is left. With a toggle first
    // among them, then a text, then a set.
    for (const char* first : {"", R"("a": {"type": "text", "default": "some text"}, )", R"("a": {"type": "set", "default": ["x"], "choices": ["x", "y"]}, )"})
    for (int left = 0;; ++left) {
        if (!left) manifest("set.one", "{" + std::string(first) + every.substr(1));
        tsuchi::forget_mods();
        out = blocks_out.size();
        allocations_left = left;
        const TSUCHI_Result found = tsuchi::find_mods();
        allocations_left = -1;
        const bool ready = found == TSUCHI_OK && tsuchi::find_mod("set.one") && tsuchi::find_mod("set.one")->state == tsuchi::ModState::ready;
        tsuchi::forget_mods();
        CHECK(blocks_out.size() == out);
        if (ready || left > 3000) break;
    }

    disk.files = other_files;
    disk.folders.clear();
    disk.log.clear(), disk.changes.clear();
}

// A navmesh of the made-up game's, and what is done to it.
struct NavScene {
    std::string log;
    int taken = 0, given = 0;
    uint32_t added = 0;
} nav_scene;
std::vector<uint8_t> navigation_fixture; // a navigation file for /G/Cave_P with one navmesh, Nav-Pikmin, of two tiles: `navigating` makes it
void* game_nav_allocate(int bytes, int) {
    ++nav_scene.taken;
    return std::malloc(size_t(bytes));
}
void game_nav_release(void* block) {
    ++nav_scene.given;
    std::free(block);
}
uint32_t game_nav_remove_tile(void* mesh, uint64_t reference, void*, void*) {
    const uint64_t at = reinterpret_cast<uint64_t>(mesh);
    const uint32_t poly_bits = fake::get<uint32_t>(at + t::navmesh::poly_bits), tile_bits = fake::get<uint32_t>(at + t::navmesh::tile_bits);
    const uint32_t index = uint32_t(reference >> poly_bits) & ((1u << tile_bits) - 1);
    nav_scene.log += "out:" + std::to_string(index) + "/" + std::to_string(reference >> (poly_bits + tile_bits)) + " ";
    fake::put<uint64_t>(fake::get<uint64_t>(at + t::navmesh::tiles) + index * t::navmesh_tile::size + t::navmesh_tile::header, 0);
    return 0;
}
uint32_t game_nav_add_tile(void* mesh, void* block, int32_t bytes, int32_t flags, uint64_t, void* result) {
    uint32_t magic = 0;
    std::memcpy(&magic, block, 4);
    nav_scene.log += "in:" + std::to_string(bytes) + (magic == tsuchi::nav::kTileMagic ? "" : "?magic") + (flags == 1 ? " " : "?flags ");
    const uint64_t reference = uint64_t(nav_scene.added++) << fake::get<uint32_t>(reinterpret_cast<uint64_t>(mesh) + t::navmesh::poly_bits);
    std::memcpy(result, &reference, 8);
    std::free(block); // the navmesh's from here on
    return 0;
}
void game_nav_invalidate_paths(void* actor, void* indices) {
    uint32_t* data = nullptr;
    int32_t count = 0;
    std::memcpy(&data, indices, 8), std::memcpy(&count, static_cast<char*>(indices) + 8, 4);
    char name[64] = "?";
    tsuchi::game::name_text(tsuchi::game::name_of(reinterpret_cast<uint64_t>(actor)), name, sizeof name);
    nav_scene.log += std::string("paths:") + name + ":";
    for (int32_t i = 0; i < count; ++i) nav_scene.log += (i ? "," : "") + std::to_string(data[i]);
    nav_scene.log += " ";
}

// The made-up game's generators: what makes a map's placed objects, and what is done to them.
struct Placing {
    std::string log;
    uint64_t world = 0;
    uint64_t short_of = 0; // the placement number of a generator the game makes one actor too few for
    uint64_t keeper = 0, grow_of = 0, moved_of = 0, bad_start_of = 0;
    int destroyed = 0;
    uint64_t destroyed_bytes = 0, destroyed_actors = 0;
    bool grow_refused = false, copied_independently = true;
    int started = 0;
    bool requests_good = true;
    std::vector<uint64_t> made;
    std::function<void(uint64_t)> start_callback;
} placing;
constexpr uint32_t kRowClassAt = 0x50, kRowPlaceAt = 0x80; // where a row names its class, and where the test keeps its place
constexpr uint32_t kRowBytesAt = 0x90; // one owned serializer byte array in the made-up row
uint64_t game_day(void*) { return 7; }
void game_generator_copy(void* to, void* from) {
    std::memcpy(to, from, t::generator::size);
    const uint64_t source = reinterpret_cast<uint64_t>(from), dest = reinterpret_cast<uint64_t>(to);
    for (uint32_t offset : {kRowBytesAt, uint32_t(t::generator::actors)}) {
        const uint64_t data = fake::get<uint64_t>(source + offset);
        const int32_t count = fake::get<int32_t>(source + offset + t::array::count);
        const uint64_t copy = count ? fake::memory(size_t(count) * (offset == t::generator::actors ? 8 : 1)) : 0;
        if (count) std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<void*>(data), size_t(count) * (offset == t::generator::actors ? 8 : 1));
        fake::put(dest + offset, copy), fake::put<int32_t>(dest + offset + t::array::max, count);
        placing.copied_independently = placing.copied_independently && (!count || copy != data);
    }
    placing.log += "copy ";
}
void game_generator_destroy(void* pointer) {
    const uint64_t row = reinterpret_cast<uint64_t>(pointer);
    placing.destroyed_bytes = fake::get<uint64_t>(row + kRowBytesAt);
    placing.destroyed_actors = fake::get<uint64_t>(row + t::generator::actors);
    for (uint32_t offset : {kRowBytesAt, uint32_t(t::generator::actors)}) {
        if (const uint64_t data = fake::get<uint64_t>(row + offset)) fake::unmap(data);
    }
    ++placing.destroyed;
}
// Room for the count the caller raised it to, and a little more.
void game_generator_array_grow(void* array_pointer) {
    const auto array = reinterpret_cast<uint64_t>(array_pointer);
    if (placing.grow_refused) { placing.log += "no-grow "; return; }
    const int32_t count = fake::get<int32_t>(array + t::array::count);
    const uint64_t old = fake::get<uint64_t>(array + t::array::data), grown = fake::memory(size_t(count + 2) * t::generator::size);
    if (old && count > 1) std::memcpy(reinterpret_cast<void*>(grown), reinterpret_cast<void*>(old), size_t(count - 1) * t::generator::size);
    fake::put(array + t::array::data, grown), fake::put<int32_t>(array + t::array::max, count + 2);
    if (old) { std::memset(reinterpret_cast<void*>(old), 0xDD, size_t(count - 1) * t::generator::size); fake::unmap(old); }
    placing.log += "grow ";
}
void game_generator_spawn(void* generator, void* world, uint32_t zero) {
    namespace g = t::generator;
    const auto row = reinterpret_cast<uint64_t>(generator);
    const uint64_t number = fake::get<uint64_t>(row + g::placement);
    char text[40];
    std::snprintf(text, sizeof text, "spawn:%llx%s ", static_cast<unsigned long long>(number), reinterpret_cast<uint64_t>(world) == placing.world && !zero ? "" : "?");
    placing.log += text;
    uint32_t count = fake::get<uint32_t>(row + g::count);
    if (number == placing.short_of) --count;
    const uint64_t list = fake::memory(8 * (count + 1));
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t made = actor(u"Placed", fake::get<uint64_t>(row + g::class_), u"Placed", 3, number, component(fake::get<float>(row + kRowPlaceAt) + float(i), 0, 0, 0));
        fake::put<uint64_t>(made, fake::engine().vtable); // native actors also expose the engine ProcessEvent entry
        fake::put(list + 8 * i, made);
        placing.made.push_back(made);
        if (number == placing.bad_start_of && i == 1) fake::put<uint64_t>(made + t::game_actor::root, 0);
    }
    fake::put(row + g::actors + t::array::data, list);
    fake::put<int32_t>(row + g::actors + t::array::count, int32_t(count)), fake::put<int32_t>(row + g::actors + t::array::max, int32_t(count + 1));
    if (number == placing.grow_of || number == placing.moved_of) {
        const uint64_t array = placing.keeper + t::generator_keeper::generators;
        const int32_t before = fake::get<int32_t>(array + t::array::count);
        fake::put<int32_t>(array + t::array::count, before + 1), fake::put<int32_t>(array + t::array::max, before);
        game_generator_array_grow(reinterpret_cast<void*>(array));
        const uint64_t data = fake::get<uint64_t>(array + t::array::data), foreign = data + uint64_t(before) * g::size;
        fake::put<uint64_t>(foreign + g::placement, 0xFA001), fake::put<int32_t>(foreign + g::index, before);
        if (number == placing.moved_of) {
            const uint64_t original = data + uint64_t(before - 1) * g::size;
            uint8_t bytes[g::most];
            std::memcpy(bytes, reinterpret_cast<void*>(original), g::size);
            std::memcpy(reinterpret_cast<void*>(original), reinterpret_cast<void*>(foreign), g::size);
            std::memcpy(reinterpret_cast<void*>(foreign), bytes, g::size);
            fake::put<int32_t>(original + g::index, before - 1), fake::put<int32_t>(foreign + g::index, before);
        }
    }
}
void game_actor_start_spawned(void* which, void* request_pointer) {
    namespace r = t::spawn_request;
    namespace c = t::constant;
    const auto made = reinterpret_cast<uint64_t>(which), request = reinterpret_cast<uint64_t>(request_pointer);
    const uint64_t root = fake::get<uint64_t>(made + t::game_actor::root);
    placing.requests_good = placing.requests_good && fake::get<uint64_t>(request + r::class_) == fake::get<uint64_t>(made + t::object::class_) &&
                            fake::get<float>(request + r::location) == fake::get<float>(root + t::scene_component::location) && fake::get<float>(request + r::scale) == 1 &&
                            fake::get<uint32_t>(request + r::object_flags) == c::spawn_object_flags && fake::get<int32_t>(request + r::team) == int32_t(c::spawn_team_default) &&
                            fake::get<int32_t>(request + r::user_value) == -1 && fake::get<uint64_t>(request + r::node_self) == request + r::node;
    ++placing.started;
    placing.log += "start ";
    if (placing.start_callback) placing.start_callback(made);
}

// The made-up game's AI parts: the values each has, as its serializer writes them, and what is done to them.
struct Minds {
    std::string log;
    std::map<uint64_t, std::vector<uint8_t>> set;
    bool calls_good = true;
} minds;
// A part's values: every group empty or zero, but for one number of its territory and, for a larva, its patrol.
std::vector<uint8_t> ai_territory(float radius) {
    std::vector<uint8_t> bytes(20);
    std::memcpy(bytes.data() + 12, &radius, 4);
    return bytes;
}
std::vector<uint8_t> ai_stream(uint32_t kind, float radius) {
    std::vector<uint8_t> bytes = ai_territory(radius);
    bytes.resize(kind == tsuchi::ai::kLarva ? 166 : 158);
    return bytes;
}
std::string ai_holds(uint64_t part) {
    const std::vector<uint8_t>& bytes = minds.set[part];
    float radius = 0;
    if (bytes.size() < 158) return "?";
    std::memcpy(&radius, bytes.data() + 12, 4);
    char text[48];
    if (bytes.size() == 166) std::snprintf(text, sizeof text, "%g/%d", radius, bytes[158]); // a larva's, with its patrol
    else std::snprintf(text, sizeof text, "%g", radius);
    return text;
}
void ai_bytes_into(uint64_t array, const std::vector<uint8_t>& bytes) {
    void* block = bytes.empty() ? nullptr : fake::engine_allocate(nullptr, bytes.size(), 0);
    if (block) std::memcpy(block, bytes.data(), bytes.size());
    fake::put(array + t::array::data, reinterpret_cast<uint64_t>(block));
    fake::put(array + t::array::count, int32_t(bytes.size())), fake::put(array + t::array::max, int32_t(bytes.size()));
}
void game_ai_values_write(void* part, void* values, void* before, uint64_t version, uint32_t zero) {
    const auto at = reinterpret_cast<uint64_t>(values);
    minds.calls_good = minds.calls_good && version == t::constant::ai_stream_version && !zero && all_bytes(before, t::ai_values::size, 0) && all_bytes(values, t::ai_values::size, 0);
    ai_bytes_into(at + t::ai_values::set, minds.set[reinterpret_cast<uint64_t>(part)]);
    ai_bytes_into(at + t::ai_values::state, {1, 2, 3});
    minds.log += "write ";
}
void game_ai_values_read(void* part, void* values, uint64_t from, uint64_t to) {
    const uint64_t set = reinterpret_cast<uint64_t>(values) + t::ai_values::set, state = reinterpret_cast<uint64_t>(values) + t::ai_values::state;
    const auto* data = reinterpret_cast<const uint8_t*>(fake::get<uint64_t>(set + t::array::data));
    const int32_t count = fake::get<int32_t>(set + t::array::count);
    // No state comes with the values, so the part keeps its own.
    minds.calls_good = minds.calls_good && from == t::constant::ai_stream_version && to == from && count > 0 && fake::get<int32_t>(set + t::array::max) == count &&
                       !fake::get<uint64_t>(state + t::array::data) && !fake::get<int32_t>(state + t::array::count);
    minds.set[reinterpret_cast<uint64_t>(part)].assign(data, data + count);
    minds.log += "read ";
}
TSUCHI_Mod* ai_native_mod = nullptr;
void (*ai_native_observe)(uint64_t) = nullptr;
uint64_t ai_native_observe_entry = 0; // host-only indirection for observing the model's actual native callee
uint64_t game_ai_part_start(uint64_t part, uint64_t, uint64_t, uint64_t) {
    minds.log += "start:" + ai_holds(part) + " ";
    if (ai_native_observe_entry)
        reinterpret_cast<thunks::AiEntry>(fake::get<uint64_t>(ai_native_observe_entry))(reinterpret_cast<void*>(part));
    else if (ai_native_observe) ai_native_observe(part);
    return 0;
}
// Native starts stay at the bottom of the chain when SDK fixtures install AI values
// before the installer regressions. Re-wrapping a native stand-in later would bypass
// the framework's persistent hook, unlike a native function in the real game.
void install_ai_native_starts() {
    if (ai_native_mod) return;
    ai_native_mod = tsuchi::mod_create("the.game.ai-start");
    const uint64_t main = fake::main_image();
    const auto& start = t::slot_group::ai_part_start;
    const auto& set = t::slot_group::ai_part_write_set;
    const auto& state = t::slot_group::ai_part_write_state;
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t table = main + start.tables[i];
        fake::put(table + start.offset, main + start.stock[i]);
        fake::put(table + set.offset, main + set.stock[i ? 1 : 0]);
        fake::put(table + state.offset, main + state.stock[0]);
        fake::code().push_back(main + start.stock[i]);
        uint64_t id = 0;
        void* next = nullptr;
        CHECK(tsuchi::hook_wrap(ai_native_mod, table + start.offset, 0, reinterpret_cast<void*>(&game_ai_part_start), 0, &id, &next) == TSUCHI_OK);
    }
}

void installing() {
    const TSUCHI_ModService& about = *tsuchi::host.mod;
    const TSUCHI_FileService& folder = *tsuchi::host.file;
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    char buffer[512];
    uint64_t needed = 0, size = 0;
    const auto manifest = [&](const std::string& id, const std::string& text) { disk.files[mods_at + id + "/mod.json"] = text; };
    const auto plain = [&](const std::string& id, const std::string& version, const std::string& more = "") {
        manifest(id, "{\"id\": \"" + id + "\", \"name\": \"The " + id + " mod\", \"version\": \"" + version + "\"" + more + "}");
    };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::ready ? "<ready>" : "<running>");
    };

    // A manifest on its own: what it has to say, and how each thing wrong with it is told.
    const auto read = [](const char* id, const std::string& text) {
        static tsuchi::ModRecord record;
        record.name.clear(), record.version.clear(), record.problem.clear();
        tsuchi::release_json(record.manifest);
        record = tsuchi::ModRecord{};
        if (!id) return std::string();
        std::memcpy(record.id, id, std::strlen(id) + 1);
        const bool ok = tsuchi::read_manifest(record, text.data(), text.size());
        if (ok != (record.state == tsuchi::ModState::ready) || ok != !record.problem.get()[0]) return std::string("<the answer and the record disagree>");
        return ok ? std::string("ok ") + record.name.get() + " " + record.version.get() + " " + std::to_string(record.parts[0]) + "/" + std::to_string(record.parts[1]) + "/" +
                        std::to_string(record.parts[2])
                  : std::string(record.problem.get());
    };
    const auto says = [&](const std::string& text, const std::string& word) {
        const std::string told = read("my.mod", text);
        const bool right = told.find(word) != std::string::npos && told.compare(0, 3, "ok ") != 0;
        if (!right) std::printf("  [%s]: %s\n", text.c_str(), told.c_str());
        return right;
    };
    const auto reads = [&](const std::string& text, const std::string& expected) {
        const std::string told = read("my.mod", text);
        if (told != expected) std::printf("  [%s]: %s\n", text.c_str(), told.c_str());
        return told == expected;
    };
    const size_t out = blocks_out.size();
    CHECK(reads(R"({"id": "my.mod", "name": "My Mod", "version": "1.20.3"})", "ok My Mod 1.20.3 1/20/3"));
    CHECK(reads(R"({"version": "0.0.0", "name": "caf\u00e9", "id": "my.mod", "framework": 0, "requires": {"a.b": "1.0.0", "c": "*"}, "optional": {}, "conflicts": ["x-y_z.0"]})",
                "ok caf\xC3\xA9 0.0.0 0/0/0"));
    CHECK(says("{\"id\": \"my.mod\",\n \"name\": }", "mod.json, line 2, column 10: ") && says("", "mod.json, line 1, column 1: ") && says("[]", "is an object") && says("7", "is an object"));
    CHECK(says(R"({"id": "my.mod", "name": "My Mod", "version": "1.0.0", "Name": "x"})", "does not know: Name") && says(R"({"id": "my.mod", "name": "N", "version": "1.0.0", "colour": []})", "does not know: colour"));
    CHECK(says(R"({"name": "My Mod", "version": "1.0.0"})", "\"id\" has to be the folder's name, my.mod") && says(R"({"id": "other", "name": "N", "version": "1.0.0"})", "\"id\"") &&
          says(R"({"id": 5, "name": "N", "version": "1.0.0"})", "\"id\"") && says(R"({"id": "my.mod.", "name": "N", "version": "1.0.0"})", "\"id\""));
    CHECK(says(R"({"id": "my.mod", "version": "1.0.0"})", "\"name\"") && says(R"({"id": "my.mod", "name": "", "version": "1.0.0"})", "\"name\"") &&
          says(R"({"id": "my.mod", "name": 5, "version": "1.0.0"})", "\"name\"") && says(R"({"id": "my.mod", "name": "a\u0000b", "version": "1.0.0"})", "\"name\""));
    for (const char* bad : {"\"1.0\"", "\"1.0.0.0\"", "\"01.0.0\"", "\"1.00.0\"", "\"a.b.c\"", "\"\"", "\"1.0.0 \"", "\"v1.0.0\"", "\"1..0\"", "\"1.0.\"", "\"1000000000.0.0\"", "\"*\"", "1", "null"})
        CHECK(says(std::string(R"({"id": "my.mod", "name": "N", "version": )") + bad + "}", "\"version\""));
    CHECK(says(R"({"id": "my.mod", "name": "N"})", "\"version\"") && read("my.mod", R"({"id": "my.mod", "name": "N", "version": "999999999.0.10"})") == "ok N 999999999.0.10 999999999/0/10");
    const std::string head = R"({"id": "my.mod", "name": "N", "version": "1.0.0", )";
    CHECK(says(head + "\"framework\": \"1\"}", "\"framework\"") && says(head + "\"framework\": 1.5}", "\"framework\"") && says(head + "\"framework\": 1e0}", "\"framework\"") &&
          says(head + "\"framework\": -1}", "\"framework\"") && says(head + "\"framework\": 99999999999}", "\"framework\"") &&
          says(head + "\"framework\": " + std::to_string(TSUCHI_API_VERSION + 1) + "}", "it needs version " + std::to_string(TSUCHI_API_VERSION + 1) + " of the framework, and this is version " + std::to_string(TSUCHI_API_VERSION)) &&
          reads(head + "\"framework\": " + std::to_string(TSUCHI_API_VERSION) + "}", "ok N 1.0.0 1/0/0"));
    // A mod made for a later framework hears that, whatever else it has that this framework does not know.
    CHECK(says(head + "\"later_key\": 1, \"framework\": " + std::to_string(TSUCHI_API_VERSION + 1) + "}", "it needs version "));
    // What the mod list shows of a mod, and an area of its own for a tool, which the framework reads past.
    CHECK(reads(head + R"("author": "A", "description": "What it does.", "url": "https://example.org", "meta": {"tool": [1, {"deep": null}]}})", "ok N 1.0.0 1/0/0") &&
          reads(head + R"("meta": 7})", "ok N 1.0.0 1/0/0") && says(head + "\"author\": 5}", "\"author\" is text") &&
          says(head + "\"description\": []}", "\"description\" is text") && says(head + "\"url\": {}}", "\"url\" is text"));
    // A mod for one version of the game says which, and does not start on another.
    CHECK(reads(head + "\"game\": \"" + std::string(static_cast<const char*>(t::version)) + "\"}", "ok N 1.0.0 1/0/0") && says(head + "\"game\": 1}", "\"game\" is the build of the game") &&
          says(head + "\"game\": \"0.0.1\"}", std::string("it is for version 0.0.1 of the game, and this is version ") + static_cast<const char*>(t::version)));
    CHECK(reads(head + "\"game\": [\"9.9\", \"" + std::string(static_cast<const char*>(t::build_key)) + "\"]}", "ok N 1.0.0 1/0/0") &&
          says(head + "\"game\": [\"9.9\", \"9.8\"]}", std::string("it is for version 9.9 of the game, and this is version ") + static_cast<const char*>(t::version)) &&
          says(head + "\"game\": []}", "\"game\" is ") && says(head + "\"game\": [1]}", "\"game\" is "));
    for (const char* key : {"requires", "optional"}) {
        const std::string start = head + "\"" + key + "\": ";
        CHECK(says(start + "{\"" + std::string(48, 'a') + "\": \"*\"}}", "no other mod's ID") && reads(head + "\"" + key + "\": {\"" + std::string(47, 'a') + "\": \"*\"}}", "ok N 1.0.0 1/0/0"));
        CHECK(says(start + "[\"a.b\"]}", "is an object of mod IDs") && says(start + "{\"A.b\": \"*\"}}", "no other mod's ID: A.b") && says(start + "{\"my.mod\": \"*\"}}", "no other mod's ID: my.mod") &&
              says(start + "{\"\": \"*\"}}", "no other mod's ID") && says(start + "{\"a.b\": \"1.0\"}}", "the version asked of a.b") && says(start + "{\"a.b\": 1}}", "the version asked of a.b") &&
              says(start + "{\"a.b\": \"1.0.0\", \"c\": \">=1.0.0\"}}", "the version asked of c"));
    }
    CHECK(says(head + "\"conflicts\": {\"a.b\": \"*\"}}", "\"conflicts\" is a list") && says(head + "\"conflicts\": [\"a.b\", 5]}", "other mods' IDs") &&
          says(head + "\"conflicts\": [\"A\"]}", "other mods' IDs") && says(head + "\"conflicts\": [\"my.mod\"]}", "other mods' IDs"));
    CHECK(read(nullptr, "") == "" && blocks_out.size() == out); // the last record's memory goes back

    // No mods folder, and no way to read files: no mods, either way.
    disk.log.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 0 && !tsuchi::mod_at(0) && !tsuchi::find_mod("base.lib"));
    const auto opener = t::fn::fs_open_folder;
    t::fn::fs_open_folder = nullptr;
    CHECK(tsuchi::find_mods() == TSUCHI_NOT_READY && folder.size(mod, "notes.txt", &size) == TSUCHI_NOT_READY && folder.list(mod, "", buffer, sizeof buffer, &needed) == TSUCHI_NOT_READY);
    t::fn::fs_open_folder = opener;

    // Only tsuchi and tsuchi. belong to the framework; a matching four-byte prefix is a mod's own.
    for (const char* id : {"tsuc", "tsuc.mod", "tsuchine"}) plain(id, "1.0.0");
    manifest("tsuchi", R"({"id":"tsuchi","name":"N","version":"1.0.0"})");
    CHECK(tsuchi::find_mods() == TSUCHI_OK && problem("tsuc") == "<ready>" && problem("tsuc.mod") == "<ready>" && problem("tsuchine") == "<ready>");
    CHECK(problem("tsuchi") == "its folder's name is one the framework keeps for itself: tsuchi");
    tsuchi::forget_mods();
    for (const char* id : {"tsuc", "tsuc.mod", "tsuchine", "tsuchi"}) disk.files.erase(mods_at + id + "/mod.json");
    // A folder of mods: some that can run, and some that cannot, each for its own reason.
    plain("base.lib", "1.4.2");
    disk.files[mods_at + "base.lib/data/hello.txt"] = "hello";
    disk.files[mods_at + "base.lib/data/deep/blob.bin"] = std::string("a\0b\0c", 5);
    plain("app.one", "2.0.0", R"(, "requires": {"base.lib": "1.2.0"}, "optional": {"not.there": "*"}, "conflicts": ["not.there.either"])");
    plain("app.two", "1.0.0", R"(, "requires": {"base.lib": "2.0.0"})");
    plain("app.three", "1.0.0", R"(, "requires": {"app.two": "*"})");
    plain("app.lonely", "1.0.0", R"(, "requires": {"gone.mod": "*"})");
    plain("app.picky", "1.0.0", R"(, "optional": {"base.lib": "1.9.0"})");
    plain("app.patch", "1.0.0", R"(, "requires": {"base.lib": "1.4.3"})");
    plain("app.minor", "1.0.0", R"(, "requires": {"base.lib": "1.3.9"})");
    plain("app.mild", "1.0.0", R"(, "optional": {"app.two": "9.0.0"}, "conflicts": ["app.two"])");
    disk.files[mods_at + std::string(48, 'n') + "/mod.json"] = "{}";
    plain("app.easy", "1.0.0", R"(, "optional": {"base.lib": "1.4.2", "app.two": "*"})");
    manifest("broken", "{ nope");
    disk.files[mods_at + "empty.folder/readme.txt"] = "no manifest here";
    manifest("Bad Name", R"({"id": "Bad Name", "name": "N", "version": "1.0.0"})");
    manifest("tsuchi.core", R"({"id": "tsuchi.core", "name": "N", "version": "1.0.0"})");
    plain("ring.a", "1.0.0", R"(, "requires": {"ring.b": "*"})");
    plain("ring.b", "1.0.0", R"(, "optional": {"ring.a": "*"})");
    plain("ring.c", "1.0.0", R"(, "requires": {"ring.a": "*"})");
    plain("foe.one", "1.0.0", R"(, "conflicts": ["not.there", "zeta.first", "base.lib"])");
    plain("zeta.first", "1.0.0");
    plain("alpha.last", "1.0.0", R"(, "requires": {"zeta.first": "1.0.0"})");
    disk.files[mods_at + "stray-file.txt"] = "a file among the folders is no mod";
    disk.files[mods_at + "actor.test/notes.txt"] = "a note";
    disk.files[mods_at + "actor.test/more/one.txt"] = "1";
    // Found, and forgotten: nothing of them is left.
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 22 && closed() && disk.well_formed && blocks_out.size() > out);
    CHECK(problem("tsuchi.core") == "its folder's name is one the framework keeps for itself: tsuchi.core");
    tsuchi::forget_mods();
    CHECK(tsuchi::mod_count() == 0 && blocks_out.size() == out);
    // No memory for one mod's manifest, or a manifest the system gives less of than there is: that mod is blocked, and the rest are found.
    refused_size = disk.files[mods_at + "zeta.first/mod.json"].size();
    CHECK(tsuchi::find_mods() == TSUCHI_OK && problem("zeta.first") == "there is no memory to read its mod.json" && problem("alpha.last") == "it needs zeta.first, which cannot run" &&
          problem("base.lib") == "<ready>");
    refused_size = 0, disk.short_reads = true;
    CHECK(tsuchi::find_mods() == TSUCHI_OK && problem("zeta.first") == "its mod.json could not be read" && problem("base.lib") == "its mod.json could not be read");
    disk.short_reads = false;
    // No memory when the list of them has to grow: nothing is kept.
    refused_size = 32 * sizeof(tsuchi::ModRecord);
    CHECK(tsuchi::find_mods() == TSUCHI_NO_MEMORY && tsuchi::mod_count() == 0 && blocks_out.size() == out);
    refused_size = 0;
    // In whatever order the system lists the folders, the mods are kept by ID.
    disk.backward = true;
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 22 && closed() && disk.well_formed);
    disk.backward = false;
    std::string ids;
    for (uint32_t i = 0; i < tsuchi::mod_count(); ++i) ids += std::string(tsuchi::mod_at(i)->id) + " ";
    CHECK(ids == "? ? actor.test alpha.last app.easy app.lonely app.mild app.minor app.one app.patch app.picky app.three app.two base.lib broken empty.folder foe.one ring.a ring.b ring.c tsuchi.core zeta.first ");
    CHECK(problem("app.patch") == "it needs base.lib 1.4.3" && problem("app.minor") == "<ready>" && problem("app.mild") == "<ready>");
    std::vector<std::string> unnamed{tsuchi::mod_at(0)->problem.get(), tsuchi::mod_at(1)->problem.get()}; // in the order the system listed them
    std::sort(unnamed.begin(), unnamed.end());
    CHECK(unnamed[0] == "its folder's name is no ID: Bad Name" && unnamed[1] == "its folder's name is no ID: " + std::string(48, 'n') && problem("broken") == "mod.json, line 1, column 3: a name is missing: names are texts" &&
          problem("empty.folder") == "it has no mod.json" && problem("actor.test") == "it has no mod.json");
    CHECK(problem("app.two") == "it needs base.lib 2.0.0" && problem("app.three") == "it needs app.two, which cannot run" && problem("app.lonely") == "it needs gone.mod, which is not installed" &&
          problem("app.picky") == "it works with base.lib only from 1.9.0" && problem("foe.one") == "it conflicts with zeta.first, which is installed");
    CHECK(problem("ring.a") == "it and another mod each need the other first" && problem("ring.b") == "it and another mod each need the other first" &&
          problem("ring.c") == "it needs ring.a, which cannot run");
    // Those that can run start after what they ask for, and by ID otherwise.
    const auto order = [](const char* id) { return tsuchi::find_mod(id)->order; };
    CHECK(problem("base.lib") == "<ready>" && problem("app.one") == "<ready>" && problem("app.easy") == "<ready>" && problem("zeta.first") == "<ready>" && problem("alpha.last") == "<ready>");
    CHECK(order("app.mild") == 0 && order("base.lib") == 1 && order("app.easy") == 2 && order("app.minor") == 3 && order("app.one") == 4 && order("zeta.first") == 5 &&
          order("alpha.last") == 6);
    CHECK(!tsuchi::find_mod("base.lib")->context && about.version_of(mod, "base.lib", buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND); // found, and not started yet

    // Started: each has its context. What a mod is told of itself, and of the others.
    tsuchi::start_mods();
    tsuchi::target::observe_status();
    const auto observed_found = tsuchi::target::status.mods_found;
    const auto observed_running = tsuchi::target::status.mods_running;
    const auto observed_blocked = tsuchi::target::status.mods_blocked;
    CHECK(observed_found == tsuchi::mod_count() && observed_found > 0 && observed_running > 0 && observed_blocked > 0 && observed_running + observed_blocked == observed_found);
    tsuchi::target::observe_status();
    CHECK(tsuchi::target::status.mods_found == observed_found && tsuchi::target::status.mods_running == observed_running &&
          tsuchi::target::status.mods_blocked == observed_blocked);
    TSUCHI_Mod* library = tsuchi::find_mod("base.lib")->context, *app = tsuchi::find_mod("app.one")->context;
    CHECK(problem("base.lib") == "<running>" && problem("alpha.last") == "<running>" && library && app && tsuchi::mod_usable(library) && tsuchi::mod_usable(app) && !tsuchi::find_mod("app.two")->context);
    CHECK(about.id(app, buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, "app.one") && needed == 8 && about.name(app, buffer, sizeof buffer, &needed) == TSUCHI_OK &&
          !std::strcmp(buffer, "The app.one mod") && about.version(app, buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, "2.0.0") && needed == 6);
    CHECK(about.version(app, buffer, 5, &needed) == TSUCHI_CAPACITY && needed == 6 && about.id(app, buffer, 7, &needed) == TSUCHI_CAPACITY && about.name(app, buffer, 3, &needed) == TSUCHI_CAPACITY);
    CHECK(about.version_of(app, "base.lib", buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, "1.4.2") && about.version_of(app, "app.two", buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND &&
          about.version_of(app, "nobody", buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND && about.version_of(app, "base.lib", buffer, 5, &needed) == TSUCHI_CAPACITY);
    // A context with no manifest behind it has its ID and nothing more.
    CHECK(about.id(mod, buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, "actor.test") && about.name(mod, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND &&
          about.version(mod, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND);
    // The framework's version is one number wherever it is told: by the call, in the host table, and to a manifest.
    uint32_t running_api_version = ~0u;
    CHECK(TSUCHI_HAS(&about, framework) && about.framework(app, &running_api_version) == TSUCHI_OK && running_api_version == TSUCHI_API_VERSION && running_api_version == tsuchi::host.api_version);
    CHECK(about.framework(app, nullptr) == TSUCHI_INVALID && about.framework(nullptr, &running_api_version) == TSUCHI_INVALID);
    const uint32_t lines = tsuchi::log_lines();
    CHECK(about.log(app, "before anyone listens") == TSUCHI_OK && tsuchi::log_lines() == lines + 1);
    tsuchi::set_log_sink(log_sink);
    CHECK(about.log(app, "hello") == TSUCHI_OK && about.log(library, "") == TSUCHI_OK && logged.lines == "[app.one] hello\n[base.lib] \n" && tsuchi::log_lines() == lines + 3);

    // A mod's own files: what its folder holds, and no way out of it.
    disk.log.clear();
    CHECK(folder.size(library, "data/hello.txt", &size) == TSUCHI_OK && size == 5 && folder.size(library, "mod.json", &size) == TSUCHI_OK && size > 20 &&
          folder.size(library, "data/missing.txt", &size) == TSUCHI_NOT_FOUND && folder.size(library, "data", &size) == TSUCHI_NOT_FOUND && closed());
    bool there = false;
    CHECK(folder.exists(library, "data/hello.txt", &there) == TSUCHI_OK && there && folder.exists(library, "data/nope.txt", &there) == TSUCHI_OK && !there &&
          folder.exists(library, "data", &there) == TSUCHI_OK && !there);
    std::memset(buffer, 'x', sizeof buffer);
    CHECK(folder.read(library, "data/hello.txt", 1, buffer, 3, &size) == TSUCHI_OK && size == 3 && !std::memcmp(buffer, "ellx", 4) && folder.read(library, "data/hello.txt", 5, buffer, 3, &size) == TSUCHI_OK &&
          size == 0 && folder.read(library, "data/deep/blob.bin", 0, buffer, 100, &size) == TSUCHI_OK && size == 5 && !std::memcmp(buffer, "a\0b\0c", 5) &&
          folder.read(library, "nope", 0, buffer, 3, &size) == TSUCHI_NOT_FOUND);
    size = 77;
    CHECK(folder.read(library, "data/hello.txt", 1, nullptr, 0, &size) == TSUCHI_OK && size == 0 && closed()); // nothing is asked of the system for no bytes
    for (const char* bad : {"", "/data/hello.txt", "../app.one/mod.json", "data/../../app.one/mod.json", "rom:/tsuchi/mods/app.one/mod.json", "data\\hello.txt"})
        CHECK(folder.size(library, bad, &size) == TSUCHI_INVALID && folder.exists(library, bad, &there) == TSUCHI_INVALID && folder.read(library, bad, 0, buffer, 3, &size) == TSUCHI_INVALID &&
              (!*bad || folder.list(library, bad, buffer, sizeof buffer, &needed) == TSUCHI_INVALID));
    disk.open_error = 0x1234;
    CHECK(folder.size(library, "nope", &size) == TSUCHI_IO && folder.exists(library, "nope", &there) == TSUCHI_IO && folder.read(library, "nope", 0, buffer, 3, &size) == TSUCHI_IO);
    disk.open_error = t::constant::fs_no_such_path;
    const auto listed = [&](TSUCHI_Mod* whose, const char* where) {
        std::vector<std::string> names;
        char big[512];
        if (folder.list(whose, where, big, sizeof big, &needed) != TSUCHI_OK) return names;
        const char* at = big;
        for (; *at; at += std::strlen(at) + 1) names.push_back(at);
        if (uint64_t(at - big) + 1 != needed) names.push_back("<the size is wrong>");
        std::sort(names.begin(), names.end());
        return names;
    };
    CHECK(listed(library, "") == (std::vector<std::string>{"data/", "mod.json"}) && listed(library, "data") == (std::vector<std::string>{"deep/", "hello.txt"}) &&
          listed(library, "data/deep") == std::vector<std::string>{"blob.bin"} && folder.list(library, "nope", buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND &&
          folder.list(library, "data/hello.txt", buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND && closed());
    // Where a mod's file is: within its folder, or nowhere.
    tsuchi::Texts place;
    CHECK(tsuchi::mod_file_path(place, library, "data/hello.txt") && place.data == mods_at + "base.lib/data/hello.txt" && !tsuchi::mod_file_path(place, library, "../app.one/mod.json"));
    place.clear();
    CHECK(tsuchi::mod_file_path(place, library, "") && place.data == mods_at + "base.lib");
    place.clear();
    const size_t listing = blocks_out.size();
    CHECK(listed(library, "data").size() == 2 && blocks_out.size() == listing);
    // A folder with enough in it that the names outgrow their first room just as its folders are added.
    for (int i = 0; i < 6; ++i) disk.files[mods_at + "base.lib/wide/" + std::string(38, char('a' + i)) + ".f"] = "";
    for (int i = 0; i < 2; ++i) disk.files[mods_at + "base.lib/wide/" + std::string(10, char('p' + i)) + "/x"] = "";
    refused_size = 512;
    CHECK(folder.list(library, "wide", buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && blocks_out.size() == listing);
    refused_size = 0;
    CHECK(listed(library, "wide").size() == 8);
    std::memset(buffer, 'x', sizeof buffer);
    CHECK(folder.list(library, "data", buffer, 15, &needed) == TSUCHI_CAPACITY && needed == 17 && buffer[0] == 'x' && folder.list(library, "data", buffer, 17, &needed) == TSUCHI_OK && buffer[17] == 'x');
    const size_t settled = blocks_out.size();
    for (size_t refuse : {size_t(t::fs_entry::size), size_t(256)}) { // no memory to list a folder with
        refused_size = refuse;
        CHECK(folder.list(library, "data", buffer, sizeof buffer, &needed) == TSUCHI_NO_MEMORY && closed() && blocks_out.size() == settled);
        refused_size = 0;
    }
    refused_size = 256; // nor to say where a file is
    CHECK(folder.size(library, "data/hello.txt", &size) == TSUCHI_NO_MEMORY && folder.read(library, "data/hello.txt", 0, buffer, 3, &size) == TSUCHI_NO_MEMORY && blocks_out.size() == settled);
    refused_size = 0;

    // Found again: every mod is stopped and forgotten first, and what is there now is found.
    disk.files.erase(mods_at + "app.one/mod.json");
    plain("base.lib", "1.5.0");
    CHECK(tsuchi::find_mods() == TSUCHI_OK && !tsuchi::mod_usable(library) && !tsuchi::mod_usable(app) && tsuchi::mod_count() == 21 && !tsuchi::find_mod("app.one") &&
          std::string(tsuchi::find_mod("base.lib")->version.get()) == "1.5.0" && problem("base.lib") == "<ready>");
    // No memory part of the way through: nothing is kept.
    const size_t before = blocks_out.size();
    for (size_t refuse : {size_t(16 * sizeof(tsuchi::ModRecord)), size_t(256), size_t(t::fs_entry::size)}) {
        refused_size = refuse;
        CHECK(tsuchi::find_mods() == TSUCHI_NO_MEMORY && tsuchi::mod_count() == 0 && blocks_out.size() < before);
        refused_size = 0;
    }
    tsuchi::forget_mods();
    CHECK(tsuchi::mod_count() == 0);
    // What a manifest declares of the game's data: table fields and texts, changed as the mod starts, before any
    // code of its, and put back when it stops. A part written wrongly blocks its mod as the manifest is read. A
    // change the game will not take blocks it as it starts, and what was changed for it before that is put back.
    {
        std::u16string table_path = u"/G/Sheet.Sheet";
        shelf.assets[fake::engine_name(table_path.data(), table_path.size(), 1)] = w.creature_table;
        const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
        const TSUCHI_TextService& lines = *tsuchi::host.text;
        const TSUCHI_Object sheet_of{to_handle(w.creature_table)};
        const auto field = [&](const char* row, const char* path) {
            TSUCHI_Value got{};
            return sheet.get(mod, sheet_of, row, path, &got) != TSUCHI_OK ? -1 : got.kind == TSUCHI_VALUE_INTEGER ? double(got.as.integer) : got.as.number;
        };
        const auto word = [&](const char* key) {
            char buffer[64] = "<failed>";
            uint64_t bytes = 0;
            lines.get(mod, "GenseiName", key, buffer, sizeof buffer, &bytes);
            return std::string(buffer);
        };
        const double life_before = field("KOCHAPPY", "MaxLife"), kira_before = field("KOCHAPPY", "Kira");
        const std::string word_before = word("CHAPPY");
        const unsigned changes = tsuchi::table_changes(), replaced = tsuchi::text_replacements();
        const char* const made[] = {"data.good", "data.list", "data.path", "data.value", "data.words", "data.lost", "data.norow", "data.script"};
        plain("data.good", "1.0.0", R"(, "tables": {"/G/Sheet.Sheet": {"KOCHAPPY": {"MaxLife": 150, "Kira": 9}}}, "text": {"GenseiName": {"CHAPPY": "Grub-dog"}})");
        plain("data.list", "1.0.0", R"(, "tables": [])");
        plain("data.path", "1.0.0", R"(, "tables": {"Creatures": {"KOCHAPPY": {"MaxLife": 1}}})");
        plain("data.value", "1.0.0", R"(, "tables": {"/G/Sheet.Sheet": {"KOCHAPPY": {"MaxLife": [1]}}})");
        plain("data.words", "1.0.0", R"(, "text": {"GenseiName": {"CHAPPY": 5}})");
        plain("data.lost", "1.0.0", R"(, "tables": {"/G/Nowhere.Nowhere": {"KOCHAPPY": {"MaxLife": 1}}})");
        plain("data.norow", "1.0.0", R"(, "tables": {"/G/Sheet.Sheet": {"KOCHAPPY": {"MaxLife": 175}, "NOBODY": {"MaxLife": 1}}})");
        // A mod's own code runs after what its manifest declares, and sees it.
        plain("data.script", "1.0.0", R"(, "requires": {"data.good": "*"}, "text": {"GenseiName": {"CHAPPY": "Spotty"}})");
        disk.files[mods_at + "data.script/main.lua"] = "print('reads', text.get('GenseiName', 'CHAPPY'))\n";
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(problem("data.good") == "<ready>" && problem("data.lost") == "<ready>" && problem("data.norow") == "<ready>" && problem("data.script") == "<ready>");
        CHECK(problem("data.list") == "mod.json: \"tables\" is an object: a table's path, then a row's name, then a field and its new value");
        CHECK(problem("data.path") == "mod.json: \"tables\": a table is named by its path, such as /Game/Folder/Table.Table, and holds its rows");
        CHECK(problem("data.value") == "mod.json: \"tables\": a field is named by its path, and its new value is a number, true or false, text, or {\"length\": n} for an array");
        CHECK(problem("data.words") == "mod.json: \"text\": a key is named, and its new text is text");
        logged.lines.clear();
        tsuchi::start_mods();
        CHECK(problem("data.good") == "<running>" && field("KOCHAPPY", "MaxLife") == 150 && field("KOCHAPPY", "Kira") == 9);
        CHECK(problem("data.script") == "<running>" && word("CHAPPY") == "Spotty" && logged.lines.find("[data.script] reads\tSpotty") != std::string::npos);
        CHECK(problem("data.lost") == "mod.json: \"tables\": /G/Nowhere.Nowhere: the game has none of that name" && !tsuchi::find_mod("data.lost")->context);
        CHECK(problem("data.norow") == "mod.json: \"tables\": /G/Sheet.Sheet, row NOBODY, field MaxLife: the game has none of that name");
        CHECK(tsuchi::table_changes() == changes + 2 && tsuchi::text_replacements() == replaced + 2); // what data.norow changed before it failed is put back
        tsuchi::forget_mods();
        CHECK(field("KOCHAPPY", "MaxLife") == life_before && field("KOCHAPPY", "Kira") == kira_before && word("CHAPPY") == word_before &&
              tsuchi::table_changes() == changes && tsuchi::text_replacements() == replaced);
        for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json");
        disk.files.erase(mods_at + "data.script/main.lua");
        logged.lines.clear();
    }
    // Rows a manifest adds, and lengths of lists in rows that it sets.
    #include "table_rows_checks.h"
    // What a manifest declares of the game's maps: levels of the mod's to load into one, and actors of the game's
    // sublevels to remove or to move. Every mod that names the loaded map takes part. The game's actors are touched
    // only when every level asked for is in the world; a removal wins over another mod's move; an entry that names
    // an actor the sublevel does not have changes nothing; and a sublevel that comes again gets its changes again.
    // A level of a mod's that has streaming levels of its own gets them loaded where it puts them, and they are
    // waited for like the levels the manifest names. A value of an actor's that is set has the say of the mod
    // started last, a removal wins over it, and it is put back when that mod stops and when the world is left.
    {
        const uint64_t kIn = t::constant::property_param, kOut = kIn | t::constant::property_out, kResult = kOut | t::constant::property_return;
        const uint64_t main = fake::main_image();
        const auto triple = [](const char16_t* a, const char16_t* b, const char16_t* c) {
            const uint64_t type = fake::memory(0x80);
            fake::put<uint64_t>(type, fake::engine().vtable);
            fake::put<int32_t>(type + t::struct_type::bytes, 12), fake::put<int32_t>(type + t::struct_type::alignment, 4);
            fake::field(type, a, u"FloatProperty", 0, 4), fake::field(type, b, u"FloatProperty", 4, 4), fake::field(type, c, u"FloatProperty", 8, 4);
            return type;
        };
        const uint64_t place_type = triple(u"X", u"Y", u"Z"), turn_type = triple(u"Pitch", u"Yaw", u"Roll");
        const uint64_t level_type = fake::object(u"Level", w.root), world_type = fake::object(u"World", w.root);
        const uint64_t stream_type = fake::object(u"LevelStreamingDynamic", w.root), stream_default = fake::engine_object(u"StreamDefault", stream_type);
        const uint64_t scenery_type = fake::object(u"SceneryActor", w.root), root_type = fake::object(u"SceneryRoot", w.root);
        fake::put<uint64_t>(stream_type + t::class_type::default_object, stream_default);
        // What a streaming level says of itself: the level it has loaded, the world it is for, where it puts that
        // world's level, and how it starts. A world lists the streaming levels it was saved with.
        const uint64_t turn4_type = fake::memory(0x80), put_type = fake::memory(0x80);
        fake::put<uint64_t>(turn4_type, fake::engine().vtable), fake::put<uint64_t>(put_type, fake::engine().vtable);
        fake::put<int32_t>(turn4_type + t::struct_type::bytes, 16), fake::put<int32_t>(turn4_type + t::struct_type::alignment, 4);
        fake::put<int32_t>(put_type + t::struct_type::bytes, 40), fake::put<int32_t>(put_type + t::struct_type::alignment, 4);
        fake::field(turn4_type, u"X", u"FloatProperty", 0, 4), fake::field(turn4_type, u"Y", u"FloatProperty", 4, 4);
        fake::field(turn4_type, u"Z", u"FloatProperty", 8, 4), fake::field(turn4_type, u"W", u"FloatProperty", 12, 4);
        fake::field(put_type, u"Rotation", u"StructProperty", 0, 16, turn4_type), fake::field(put_type, u"Translation", u"StructProperty", 16, 12, place_type);
        fake::field(put_type, u"Scale3D", u"StructProperty", 28, 12, place_type);
        constexpr int32_t kLoadedAt = 0x40, kAssetAt = 0x48, kPutAt = 0x80, kStartsAt = 0xB0, kVolumesAt = 0xB8, kListAt = 0x40;
        fake::field(stream_type, u"LoadedLevel", u"ObjectProperty", kLoadedAt, 8, level_type);
        fake::field(stream_type, u"WorldAsset", u"SoftObjectProperty", kAssetAt, 0x30, world_type);
        fake::field(stream_type, u"LevelTransform", u"StructProperty", kPutAt, 40, put_type);
        fake::bit_field(stream_type, u"bInitiallyLoaded", kStartsAt, 0, 0xFF), fake::bit_field(stream_type, u"bInitiallyVisible", kStartsAt + 1, 0, 0xFF);
        fake::field(stream_type, u"EditorStreamingVolumes", u"ArrayProperty", kVolumesAt, 0x10, fake::loose_field(u"ObjectProperty", 8, actor_base_class));
        fake::field(world_type, u"StreamingLevels", u"ArrayProperty", kListAt, 0x10, fake::loose_field(u"ObjectProperty", 8, stream_type));
        fake::set_super(scenery_type, actor_base_class), fake::set_super(root_type, w.component_class);
        struct Scene {
            std::string log;
            std::map<uint64_t, bool> visible;
            std::map<uint64_t, uint64_t> roots;
            std::vector<uint64_t> streams;
            uint64_t world = 0;
        } scene;
        const auto text_of = [](uint64_t object) {
            char text[64] = "?";
            tsuchi::game::name_text(tsuchi::game::name_of(object), text, sizeof text);
            return std::string(text);
        };
        const auto three = [](uint64_t at) {
            char text[64];
            std::snprintf(text, sizeof text, "%g,%g,%g", fake::get<float>(at), fake::get<float>(at + 4), fake::get<float>(at + 8));
            return std::string(text);
        };
        const uint64_t load = fake::function(stream_type, function_type, u"LoadLevelInstance", 0x28, 8, [&](uint64_t, uint64_t arguments) {
            std::string name;
            const uint64_t data = fake::get<uint64_t>(arguments + 8 + t::array::data);
            for (int32_t i = 0; i + 1 < fake::get<int32_t>(arguments + 8 + t::array::count); ++i) name += char(fake::get<uint16_t>(data + 2 * i));
            scene.log += "load:" + name + (fake::get<uint64_t>(arguments) == scene.world ? " " : "?world ");
            const bool comes = name != "/G/Mods/Broken";
            const uint64_t stream = comes ? fake::engine_object(u"Stream", stream_type) : 0;
            if (comes) scene.streams.push_back(stream);
            fake::put<uint8_t>(arguments + 0x18, comes), fake::put(arguments + 0x20, stream);
        }, t::constant::function_static);
        fake::flag(fake::field(load, u"WorldContextObject", u"ObjectProperty", 0, 8, world_type), kIn);
        fake::flag(fake::field(load, u"LevelName", u"StrProperty", 8, 0x10), kIn);
        fake::flag(fake::bit_field(load, u"bOutSuccess", 0x18, 0, 0xFF), kOut);
        fake::flag(fake::field(load, u"ReturnValue", u"ObjectProperty", 0x20, 8, stream_type), kResult);
        const uint64_t shown = fake::function(stream_type, function_type, u"IsLevelVisible", 1, 1,
                                              [&](uint64_t object, uint64_t arguments) { fake::put<uint8_t>(arguments, scene.visible[object]); });
        fake::flag(fake::bit_field(shown, u"ReturnValue", 0, 0, 0xFF), kResult);
        const uint64_t keep_loaded = fake::function(stream_type, function_type, u"SetShouldBeLoaded", 1, 1,
                                                    [&](uint64_t, uint64_t arguments) { scene.log += "loaded:" + std::to_string(fake::get<uint8_t>(arguments)) + " "; });
        fake::flag(fake::bit_field(keep_loaded, u"bInShouldBeLoaded", 0, 0, 0xFF), kIn);
        const uint64_t keep_shown = fake::function(stream_type, function_type, u"SetShouldBeVisible", 1, 1,
                                                   [&](uint64_t, uint64_t arguments) { scene.log += "shown:" + std::to_string(fake::get<uint8_t>(arguments)) + " "; });
        fake::flag(fake::bit_field(keep_shown, u"bInShouldBeVisible", 0, 0, 0xFF), kIn);
        // A streaming level as a level of a mod's was saved with it, and a level of a mod's coming into the world
        // with those it brings.
        const auto brought = [&](const std::u16string& asset, std::vector<float> put, bool loaded = true, bool visible = true, uint64_t volume = 0) {
            const uint64_t source = fake::engine_object(u"Brought", stream_type);
            fake::put(source + kAssetAt + t::soft_object::path, fake::name(asset));
            for (size_t i = 0; i < put.size(); ++i) fake::put<float>(source + kPutAt + 4 * i, put[i]);
            fake::put<uint8_t>(source + kStartsAt, loaded), fake::put<uint8_t>(source + kStartsAt + 1, visible);
            if (volume) {
                const uint64_t data = fake::memory(8);
                fake::put(data, volume), fake::put(source + kVolumesAt + t::array::data, data);
                fake::put<int32_t>(source + kVolumesAt + t::array::count, 1), fake::put<int32_t>(source + kVolumesAt + t::array::max, 1);
            }
            return source;
        };
        const auto comes = [&](uint64_t stream, std::vector<uint64_t> brings) {
            const uint64_t saved_in = fake::object(u"Saved", world_type, 0x60), level = fake::object(u"PersistentLevel", level_type, 0x40);
            const uint64_t data = fake::memory(8 * brings.size() + 8);
            for (size_t i = 0; i < brings.size(); ++i) fake::put(data + 8 * i, brings[i]);
            fake::put(saved_in + kListAt + t::array::data, data), fake::put<int32_t>(saved_in + kListAt + t::array::count, int32_t(brings.size()));
            fake::put(level + t::object::outer, saved_in), fake::put(stream + kLoadedAt, level);
            scene.visible[stream] = true;
        };
        const auto put_of = [&](uint64_t stream) {
            std::string text;
            for (int i = 0; i < 10; ++i) {
                char number[32];
                std::snprintf(number, sizeof number, i ? " %.2f" : "%.2f", fake::get<float>(stream + kPutAt + 4 * i));
                text += number;
            }
            return text;
        };
        fake::function(scenery_type, function_type, u"K2_DestroyActor", 0, 0, [&](uint64_t object, uint64_t) {
            scene.log += "destroy:" + text_of(object) + " ";
            fake::destroy(object);
        });
        const uint64_t get_root = fake::function(scenery_type, function_type, u"K2_GetRootComponent", 8, 8,
                                                 [&](uint64_t object, uint64_t arguments) { fake::put(arguments, scene.roots[object]); });
        fake::flag(fake::field(get_root, u"ReturnValue", u"ObjectProperty", 0, 8, w.component_class), kResult);
        fake::field(root_type, u"Mobility", u"ByteProperty", 0x60, 1);
        constexpr int32_t kChildrenAt = 0x68;
        fake::field(root_type, u"AttachChildren", u"ArrayProperty", kChildrenAt, 0x10, fake::loose_field(u"ObjectProperty", 8, root_type));
        const uint64_t set_mobility = fake::function(root_type, function_type, u"SetMobility", 1, 1, [&](uint64_t object, uint64_t arguments) {
            const uint8_t to = fake::get<uint8_t>(arguments);
            fake::put<uint8_t>(object + 0x60, to);
            scene.log += "mobility:" + text_of(object) + ":" + std::to_string(to) + " ";
            std::vector<uint64_t> below{object};
            while (to == 2 && !below.empty()) {
                const uint64_t component = below.back();
                below.pop_back();
                const uint64_t data = fake::get<uint64_t>(component + kChildrenAt);
                for (int32_t i = 0; i < fake::get<int32_t>(component + kChildrenAt + t::array::count); ++i) {
                    const uint64_t child = fake::get<uint64_t>(data + uint64_t(i) * 8);
                    fake::put<uint8_t>(child + 0x60, to), below.push_back(child);
                }
            }
        });
        fake::flag(fake::field(set_mobility, u"NewMobility", u"ByteProperty", 0, 1), kIn);
        const uint64_t set_place = fake::function(scenery_type, function_type, u"K2_SetActorLocationAndRotation", 0x1C, 4, [&](uint64_t object, uint64_t arguments) {
            scene.log += "place:" + text_of(object) + ":" + three(arguments) + "/" + three(arguments + 12) + (fake::get<uint8_t>(arguments + 0x19) ? " at once " : " sliding ");
        });
        fake::flag(fake::field(set_place, u"NewLocation", u"StructProperty", 0, 12, place_type), kIn);
        fake::flag(fake::field(set_place, u"NewRotation", u"StructProperty", 12, 12, turn_type), kIn);
        fake::flag(fake::bit_field(set_place, u"bSweep", 0x18, 0, 0xFF), kIn);
        fake::flag(fake::bit_field(set_place, u"bTeleport", 0x19, 0, 0xFF), kIn);
        const uint64_t set_scale = fake::function(scenery_type, function_type, u"SetActorScale3D", 12, 4,
                                                  [&](uint64_t object, uint64_t arguments) { scene.log += "scale:" + text_of(object) + ":" + three(arguments) + " "; });
        fake::flag(fake::field(set_scale, u"NewScale3D", u"StructProperty", 0, 12, place_type), kIn);

        constexpr int32_t kOwningAt = 0x40;
        bool map_mods_started = false;
        std::vector<uint64_t> pending_map_levels;
        fake::field(level_type, u"OwningWorld", u"ObjectProperty", kOwningAt, 8, world_type);
        // The map that is loaded: its package, its world, and one sublevel of the game's with what stands in it.
        const auto world_in = [&](const std::u16string& package_path, const std::u16string& name) {
            const uint64_t package = fake::object(package_path, w.root, 0x40), world = fake::object(name, world_type, 0x80);
            fake::put(world + t::object::outer, package);
            const uint64_t persistent = fake::object(u"Root", level_type), rule_type = fake::object(u"CarrotGameRule", w.root);
            const uint64_t rule = fake::engine_object(u"Rule", rule_type), actors = fake::memory(8);
            fake::set_super(rule_type, actor_base_class);
            fake::put<uint8_t>(rule + t::actor_play::state, uint8_t(t::constant::actor_play_state_begun));
            fake::put(actors, rule), fake::put(persistent + t::engine_level::actors, actors);
            fake::put<int32_t>(persistent + t::engine_level::actors + t::array::count, 1);
            fake::put(persistent + t::engine_level::owning_world, world), fake::put(world + t::world::persistent_level, persistent);
            return world;
        };
        const auto level_of = [&](const std::u16string& name) {
            const uint64_t owner = fake::object(name, world_type, 0x40), level = fake::object(u"PersistentLevel", level_type, 0x80);
            fake::put(level + t::object::outer, owner), fake::put(level + kOwningAt, scene.world);
            pending_map_levels.push_back(level);
            return level;
        };
        const auto actor_in = [&](uint64_t level, const std::u16string& name, uint8_t mobility) {
            const uint64_t actor = fake::engine_object(name, scenery_type), root = fake::engine_object(name + u"Root", root_type);
            fake::put(actor + t::object::outer, level), fake::put<uint8_t>(root + 0x60, mobility);
            scene.roots[actor] = root;
            fake::put<uint8_t>(actor + t::actor_play::state, map_mods_started ? 0 : uint8_t(t::constant::actor_play_state_begun));
            const int32_t count = fake::get<int32_t>(level + t::engine_level::actors + t::array::count);
            const uint64_t old = fake::get<uint64_t>(level + t::engine_level::actors), list = fake::memory(uint64_t(count + 1) * 8);
            if (count) std::memcpy(reinterpret_cast<void*>(list), reinterpret_cast<void*>(old), uint64_t(count) * 8);
            fake::put(list + uint64_t(count) * 8, actor), fake::put(level + t::engine_level::actors, list);
            fake::put<int32_t>(level + t::engine_level::actors + t::array::count, count + 1);
            return actor;
        };
        // What a mod may set of a piece of scenery: a number, a yes or no, a text, and through a reference, a
        // number of an asset that outlasts the level.
        constexpr int32_t kIntensityAt = 0x80, kLitAt = 0x84, kLabelAt = 0x88, kPresetAt = 0x98, kGlowAt = 0x40;
        const uint64_t preset_type = fake::object(u"Preset", w.root), preset = fake::engine_object(u"Preset_Cave", preset_type);
        fake::field(preset_type, u"Glow", u"FloatProperty", kGlowAt, 4);
        fake::field(scenery_type, u"Intensity", u"FloatProperty", kIntensityAt, 4), fake::bit_field(scenery_type, u"bLit", kLitAt, 0, 0xFF);
        fake::field(scenery_type, u"Label", u"StrProperty", kLabelAt, 0x10), fake::field(scenery_type, u"Scene", u"ObjectProperty", kPresetAt, 8, preset_type);
        fake::put<float>(preset + kGlowAt, 1.5f);
        const auto as_made = [&](uint64_t actor) {
            fake::put<float>(actor + kIntensityAt, 1.25f), fake::put<uint8_t>(actor + kLitAt, 0), fake::put(actor + kPresetAt, preset);
            fake::put_string(actor + kLabelAt, u"cold");
            return actor;
        };
        const auto holds = [&](uint64_t actor) {
            std::string label;
            const uint64_t data = fake::get<uint64_t>(actor + kLabelAt + t::array::data);
            for (int32_t i = 0; i + 1 < fake::get<int32_t>(actor + kLabelAt + t::array::count); ++i) label += char(fake::get<uint16_t>(data + 2 * i));
            char text[96];
            std::snprintf(text, sizeof text, "%g %d %s %g", fake::get<float>(actor + kIntensityAt), fake::get<uint8_t>(actor + kLitAt) != 0, label.c_str(), fake::get<float>(fake::get<uint64_t>(actor + kPresetAt) + kGlowAt));
            return std::string(text);
        };
        const uint64_t old_world = fake::get<uint64_t>(main + t::data::world);
        const uint8_t old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
        const uint64_t old_map_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock), map_clock = fake::memory(0x20);
        const uint8_t old_map_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
        const auto old_map_flags = t::fn::story_flags;
        fake::put(w.framework + t::game_framework::clock, map_clock);
        fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
        t::fn::story_flags = [](void*) -> uint64_t { return 0; };
        const uint64_t cave = world_in(u"/G/Cave_P", u"Cave_P"), other = world_in(u"/G/Other_P", u"Other_P");
        scene.world = cave;
        fake::put(main + t::data::world, cave), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
        uint64_t area = level_of(u"GameArea");
        uint64_t rock = actor_in(area, u"Rock_3", 0), wall = actor_in(area, u"Wall_1", 0), gate = actor_in(area, u"Gate_4", 0), lamp = actor_in(area, u"Lamp_2", 2),
                 keep = actor_in(area, u"Keep_9", 0);
        as_made(lamp), as_made(wall);
        // Gate_4's root has a light attached that may not move either.
        const uint64_t gate_light = fake::engine_object(u"Gate_4Light", root_type), gate_children = fake::memory(8);
        fake::put(gate_children, gate_light), fake::put(scene.roots[gate] + kChildrenAt, gate_children);
        fake::put<int32_t>(scene.roots[gate] + kChildrenAt + t::array::count, 1), fake::put<int32_t>(scene.roots[gate] + kChildrenAt + t::array::max, 1);
        // A live copy named like this sublevel but belonging to another world must remain untouched.
        const uint64_t foreign_area = level_of(u"GameArea");
        fake::put(foreign_area + kOwningAt, other);
        const uint64_t foreign_rock = actor_in(foreign_area, u"Rock_3", 0), foreign_wall = actor_in(foreign_area, u"Wall_1", 0);
        const uint64_t foreign_gate = actor_in(foreign_area, u"Gate_4", 0), foreign_lamp = as_made(actor_in(foreign_area, u"Lamp_2", 2));
        const uint64_t foreign_preset = fake::engine_object(u"Preset_Other", preset_type);
        fake::put<float>(foreign_preset + kGlowAt, 1.5f), fake::put(foreign_lamp + kPresetAt, foreign_preset);
        actor_in(foreign_area, u"Keep_9", 0), actor_in(foreign_area, u"Gone_7", 0);

        // The map's navmesh, with one tile of the game's in it, and two mods that bring navigation for the map.
        const uint64_t nav_type = fake::object(u"RecastNavMesh", w.root), cave_level = fake::object(u"PersistentLevel", level_type, 0x80);
        const uint64_t nav_actor = fake::object(u"Nav-Pikmin", nav_type, 0x80), nav_implementation = fake::memory(0x20), nav_mesh = fake::memory(0x40);
        const uint64_t nav_tiles = fake::memory(64 * t::navmesh_tile::size);
        fake::put(cave_level + t::object::outer, cave), fake::put(nav_actor + t::object::outer, cave_level);
        fake::put(nav_actor + t::navmesh_actor::implementation, nav_implementation), fake::put(nav_implementation + t::navmesh_implementation::mesh, nav_mesh);
        for (uint32_t i = 0; i < 20; ++i) fake::put<uint8_t>(nav_mesh + i, uint8_t(1 + i)); // the grid the file was made for
        fake::put<int32_t>(nav_mesh + t::navmesh::max_tiles, 64), fake::put(nav_mesh + t::navmesh::tiles, nav_tiles);
        fake::put<uint32_t>(nav_mesh + t::navmesh::tile_bits, 8), fake::put<uint32_t>(nav_mesh + t::navmesh::poly_bits, 20);
        fake::put<uint32_t>(nav_tiles + 5 * t::navmesh_tile::size + t::navmesh_tile::salt, 2), fake::put<uint64_t>(nav_tiles + 5 * t::navmesh_tile::size + t::navmesh_tile::header, 0x1234);
        fake::put(main + t::data::nav_allocate, &game_nav_allocate), fake::put(main + t::data::nav_release, &game_nav_release);
        fake::code().push_back(reinterpret_cast<uint64_t>(&game_nav_allocate));
        fake::code().push_back(reinterpret_cast<uint64_t>(&game_nav_release));
        t::fn::nav_remove_tile = game_nav_remove_tile, t::fn::nav_add_tile = game_nav_add_tile, t::fn::nav_invalidate_paths = game_nav_invalidate_paths;
        nav_scene = NavScene{};
        const auto navigation = [&](const std::string& id, std::vector<uint8_t> bytes, int damage = -1) {
            if (damage >= 0) bytes[size_t(damage)] ^= 0x7F;
            disk.files[mods_at + id + "/maps/cave.nav"] = std::string(bytes.begin(), bytes.end());
        };
        // These cases deliberately add ground to a world already playing. The
        // manifest still exercises the normal reader; only the named late cases
        // declare their maps through the real public script service.
        tsuchi::content_service_start();
        const auto late_plain = [&](const std::string& id, const std::string& version, const std::string& more) {
            plain(id, version);
            disk.files[mods_at + id + "/main.lua"] = "content.add([==[{" + more.substr(1) + "}]==])\n";
        };
        const auto start_late_maps = [&](uint32_t before, uint32_t after) {
            tsuchi::prepare_mods();
            CHECK(tsuchi::map_parts() == before);
            const uint64_t current = fake::get<uint64_t>(main + t::data::world);
            fake::put<uint64_t>(main + t::data::world, 0);
            tsuchi::frame(0.03f); // reset the old origin before any script has queued its batch
            fake::put(main + t::data::world, current);
            tsuchi::start_mods();
            CHECK(tsuchi::map_parts() == before); // queued scripts have not applied maps yet
            const uint64_t implementation = fake::get<uint64_t>(nav_actor + t::navmesh_actor::implementation);
            fake::put<uint64_t>(nav_actor + t::navmesh_actor::implementation, 0);
            tsuchi::frame(0.03f); // story mode preserves snapshots of already-playing actors
            fake::put(nav_actor + t::navmesh_actor::implementation, implementation);
            CHECK(tsuchi::map_parts() == after);
            fake::put<uint64_t>(main + t::data::world, 0);
            tsuchi::frame(0.03f);
            fake::put(main + t::data::world, current); // the timed scenario starts at world step zero
        };
        plain("map.nav", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "navigation": "maps/cave.nav"}])");
        plain("map.nav2", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "navigation": "maps/cave.nav"}])");
        plain("map.navbad", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "navigation": "maps/cave.nav"}])");
        plain("map.navelse", "1.0.0", R"(, "maps": [{"map": "/G/Other_P", "mode": "story", "navigation": "maps/cave.nav"}])");
        navigation("map.nav", navigation_fixture), navigation("map.nav2", navigation_fixture), navigation("map.navbad", navigation_fixture, 7), navigation("map.navelse", navigation_fixture);
        const char* const made[] = {"map.one", "map.two", "map.old", "map.broken", "map.else", "map.list", "map.nomap", "map.key", "map.nofile", "map.badfile", "map.badmove",
                                    "map.nav", "map.nav2", "map.navbad", "map.navelse", "map.nest", "map.badset", "map.badvalue", "map.dot"};
        const auto changes = [&](const std::string& id, const std::string& text) { disk.files[mods_at + id + "/maps/cave.json"] = text; };
        late_plain("map.one", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/Mods/One"], "changes": "maps/cave.json"}])");
        changes("map.one", R"({"levels": {"GameArea": {"remove": ["Rock_3"], "move": {"Wall_1": {"location": [1, 2, 3], "rotation": [0, 90, 0]},
                                "Gate_4": {"location": [7, 8, 9], "rotation": [0, 45, 0], "scale": [2, 2, 2]}},
                                "set": {"Lamp_2": {"Intensity": 4.5, "Label": "warm", "Scene.Glow": 2}, "Wall_1": {"Intensity": 9}}}}})");
        plain("map.two", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.two", R"({"levels": {"GameArea": {"remove": ["Rock_3", "Wall_1"], "move": {"Gate_4": {"location": [12, 13, 14], "rotation": [0, 0, 0]}, "Lamp_2": {"location": [4, 5, 6], "rotation": [0, 0, 0]}},
                                "set": {"Lamp_2": {"intensity": 7, "bLit": true, "Scene.Glow": 3, "Nothing": 1, "Label": 5}}}}})");
        plain("map.old", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.old", R"({"levels": {"GameArea": {"remove": ["Keep_9"], "set": {"Gone_7": {"Intensity": 2}}}}})");
        plain("map.badset", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.badset", R"({"levels": {"GameArea": {"set": {"Lamp_2": {}}}}})");
        plain("map.badvalue", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.badvalue", R"({"levels": {"GameArea": {"set": {"Lamp_2": {"Intensity": [4.5]}}}}})");
        late_plain("map.broken", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/Mods/Broken"], "changes": "maps/cave.json"}])");
        changes("map.broken", R"({"levels": {"GameArea": {"remove": ["Keep_9"]}}})");
        late_plain("map.else", "1.0.0", R"(, "maps": [{"map": "/G/Other_P", "mode": "story", "levels": ["/G/Mods/Else"]}])");
        late_plain("map.nest", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/Mods/Nest"], "changes": "maps/cave.json"}])");
        changes("map.nest", R"({"levels": {"GameArea": {"remove": ["Keep_9"]}}})");
        plain("map.list", "1.0.0", R"(, "maps": {})");
        plain("map.nomap", "1.0.0", R"(, "maps": [{"levels": ["/G/Mods/One"]}])");
        plain("map.dot", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/my.mod/One"]}])");
        plain("map.key", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/Mods/One"], "sky": "x"}])");
        plain("map.nofile", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        plain("map.badfile", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.badfile", R"({"levels": {"GameArea": {"remove": "Rock_3"}}})");
        plain("map.badmove", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "changes": "maps/cave.json"}])");
        changes("map.badmove", R"({"levels": {"GameArea": {"move": {"Wall_1": {"location": [1, 2]}}}}})");
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(problem("map.list") == "mod.json: \"maps\" is a list, one entry for each of the game's maps the mod changes");
        CHECK(problem("map.nomap") == "mod.json: \"maps\": \"map\" is the path of one of the game's maps, such as /Game/Folder/Map_P");
        CHECK(problem("map.dot").find("its only dot is the one before") != std::string::npos);
        CHECK(problem("map.key") == "mod.json: \"maps\": an entry has a key the framework does not know: sky");
        logged.lines.clear();
        start_late_maps(4, 8);
        map_mods_started = true;
        // Runtime map.one follows metadata preparation but retains its earlier
        // selected mod order. The exact overlapping-move log below checks it.
        const auto* runtime_one = tsuchi::find_mod("map.one");
        const auto* metadata_two = tsuchi::find_mod("map.two");
        CHECK(runtime_one && metadata_two && runtime_one->context && metadata_two->context &&
              runtime_one->manifest && metadata_two->manifest &&
              !runtime_one->manifest->find("maps") && metadata_two->manifest->find("maps") &&
              tsuchi::mod_rank(runtime_one->context) < tsuchi::mod_rank(metadata_two->context));
        CHECK(problem("map.one") == "<running>" && problem("map.two") == "<running>" && problem("map.old") == "<running>" && problem("map.broken") == "<running>" &&
              problem("map.else") == "<running>" && problem("map.nav") == "<running>" && problem("map.nav2") == "<running>" && problem("map.nest") == "<running>" && tsuchi::map_parts() == 8);
        CHECK(problem("map.navbad") == "mod.json: \"maps\": maps/cave.nav is no navigation file, or is damaged");
        CHECK(problem("map.navelse") == "mod.json: \"maps\": maps/cave.nav has no navigation for /G/Other_P");
        CHECK(problem("map.nofile") == "mod.json: \"maps\": maps/cave.json: the mod has no such file");
        CHECK(problem("map.badfile") == "mod.json: \"maps\": maps/cave.json: sublevel GameArea: \"remove\" is a list of actors' names");
        CHECK(problem("map.badmove") == "mod.json: \"maps\": maps/cave.json: sublevel GameArea, actor Wall_1: a move is \"location\" and \"rotation\", three numbers each, and \"scale\" when that changes too");
        CHECK(problem("map.badset") == "mod.json: \"maps\": maps/cave.json: sublevel GameArea: \"set\" holds actors by name, each with values by their paths");
        CHECK(problem("map.badvalue") == "mod.json: \"maps\": maps/cave.json: sublevel GameArea, actor Lamp_2: a value is named by its path and is a number, boolean, text, asset reference, enum name, or a nonempty struct field object. Arrays and null are not supported");
        const tsuchi::MapCounts counted = tsuchi::map_counts();
        const auto steps = [&](int count) {
            const uint64_t current = fake::get<uint64_t>(main + t::data::world);
            for (size_t i = pending_map_levels.size(); i-- > 0;) {
                const uint64_t level = pending_map_levels[i];
                if (fake::get<uint64_t>(level + t::engine_level::owning_world) != current) continue;
                thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::level_prepare.number), level, 7, 0, 0x1234);
                const uint64_t data = fake::get<uint64_t>(level + t::engine_level::actors);
                for (int32_t a = 0; a < fake::get<int32_t>(level + t::engine_level::actors + t::array::count); ++a) {
                    const uint64_t actor = fake::get<uint64_t>(data + uint64_t(a) * 8);
                    if (live(actor)) fake::put<uint8_t>(actor + t::actor_play::state, uint8_t(t::constant::actor_play_state_begun));
                }
                pending_map_levels.erase(pending_map_levels.begin() + i);
            }
            for (int i = 0; i < count; ++i) tsuchi::frame(0.03f);
        };
        const auto told = [&](const std::string& part) { return logged.lines.find(part) != std::string::npos; };
        // The map is given a moment, and then each mod's levels are asked for, in the order the mods started.
        // The entry's mode also gates geometry, changes and navigation.
        fake::put<uint8_t>(map_clock + t::clock::night, 1);
        steps(30);
        CHECK(scene.log.empty() && nav_scene.log.empty() && fake::get<float>(preset + kGlowAt) == 1.5f);
        fake::put<uint64_t>(main + t::data::world, 0);
        steps(1);
        fake::put(main + t::data::world, cave);
        fake::put<uint8_t>(map_clock + t::clock::night, 0);
        // Unknown mode state must allow navigation to retry in the same world once readable.
        fake::put<uint64_t>(w.framework + t::game_framework::clock, 0);
        steps(2);
        CHECK(scene.log.empty() && nav_scene.log.empty());
        fake::put(w.framework + t::game_framework::clock, map_clock);
        // The navigation goes in at the first step the map's navmesh is there: the game's tile out, the two of the
        // mod started last in, and the navmesh's actor told which tiles changed. The other mod is told its own is not used.
        steps(1);
        CHECK(nav_scene.log == "out:5/2 in:116 in:332 paths:Nav-Pikmin:5,0,1 " && nav_scene.taken == 2 && nav_scene.given == 0);
        CHECK(tsuchi::map_navigation_counts().navmeshes == 1 && tsuchi::map_navigation_counts().removed == 1 && tsuchi::map_navigation_counts().added == 2);
        CHECK(told("[map.nav] maps: the navigation of map.nav2 is used for /G/Cave_P, not this mod's") && !told("[map.nav2] maps:"));
        steps(28);
        CHECK(scene.log.empty() && nav_scene.log == "out:5/2 in:116 in:332 paths:Nav-Pikmin:5,0,1 ");
        steps(1);
        CHECK(scene.log == "load:/G/Mods/Broken load:/G/Mods/Nest load:/G/Mods/One " && scene.streams.size() == 2);
        CHECK(told("[map.broken] maps: /G/Mods/Broken could not be loaded into /G/Cave_P, so nothing of that map is changed for this mod"));
        // Nothing of the game's is touched while a level asked for is not in the world yet.
        scene.log.clear();
        steps(30);
        CHECK(scene.log.empty() && live(rock) && live(wall));
        // A level comes with streaming levels of its own, which the engine leaves alone. Each is asked for as the
        // mod's own levels are, and put where its level puts it; one that is not shown from the start is told so,
        // and one that volumes load is given its volumes. A mod whose level brings one that cannot be loaded
        // changes nothing of the map.
        if (scene.streams.size() != 2) { std::fflush(stdout); std::exit(failures ? failures : 1); }
        const uint64_t nest = scene.streams[0], one = scene.streams[1], volume = actor_in(area, u"Volume_1", 0);
        comes(nest, {brought(u"/G/Mods/Broken.Broken", {0, 0, 0, 1, 0, 0, 0, 1, 1, 1})});
        comes(one, {brought(u"/G/Mods/Deep.Deep", {0, 0, 0.70710678f, 0.70710678f, 100, 0, 0, 2, 2, 2}),
                    brought(u"/G/Mods/Later.Later", {0, 0, 0, 1, 0, 0, 0, 1, 1, 1}, true, false),
                    brought(u"/G/Mods/Near.Near", {0, 0, 0, 1, 0, 0, 0, 1, 1, 1}, false, true, volume)});
        steps(15);
        CHECK(scene.log == "load:/G/Mods/Broken load:/G/Mods/Deep load:/G/Mods/Later shown:0 load:/G/Mods/Near loaded:0 " && scene.streams.size() == 5);
        CHECK(told("[map.nest] maps: /G/Mods/Broken, which a level of this mod brings with it, could not be loaded into /G/Cave_P, so further map changes stop for this mod"));
        CHECK(problem("map.nest") == "<running>" && !tsuchi::find_mod("map.nest")->problem.get()[0]);
        const uint64_t deep = scene.streams[2], by_volume = scene.streams[4];
        CHECK(put_of(deep) == "0.00 0.00 0.71 0.71 100.00 0.00 0.00 2.00 2.00 2.00" && put_of(scene.streams[3]) == "0.00 0.00 0.00 1.00 0.00 0.00 0.00 1.00 1.00 1.00");
        CHECK(fake::get<int32_t>(by_volume + kVolumesAt + t::array::count) == 1 && fake::get<uint64_t>(fake::get<uint64_t>(by_volume + kVolumesAt + t::array::data)) == volume);
        CHECK(tsuchi::map_counts().nested_asked == 3 && tsuchi::map_counts().nested_refused == 1 && tsuchi::map_counts().levels_asked == 2);
        // The one that is to be in the world from the start is waited for, and so is what that one brings in
        // turn, which is put where the two levels before it put it together.
        scene.log.clear();
        steps(30);
        CHECK(scene.log.empty() && live(rock) && live(wall) && holds(lamp) == "1.25 0 cold 1.5");
        comes(deep, {brought(u"/G/Mods/Deeper.Deeper", {0, 0, 0, 1, 10, 0, 0, 1, 1, 0.5f})});
        steps(15);
        CHECK(scene.log == "load:/G/Mods/Deeper " && scene.streams.size() == 6 && put_of(scene.streams[5]) == "0.00 0.00 0.71 0.71 100.00 20.00 0.00 2.00 2.00 1.00");
        // A mod's level leaving the world gets the flag the game sets around its own sublevel hides, so the
        // game's widgets keep their screen holders; a level of the game's is left as the game removes it, and a
        // flag the game set itself stays set.
        {
            const auto remove = [&](uint64_t level) {
                thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::world_remove_level.number), scene.world, level, 0);
            };
            const uint64_t guard = running.context + t::game_context::level_removal_guard, mine = fake::get<uint64_t>(deep + kLoadedAt);
            fake::put<uint8_t>(guard, 0), removal_guard_seen = 0xFF;
            remove(mine);
            CHECK(removal_guard_seen == 1 && fake::get<uint8_t>(guard) == 0);
            removal_guard_seen = 0xFF;
            remove(area);
            CHECK(removal_guard_seen == 0 && fake::get<uint8_t>(guard) == 0);
            fake::put<uint8_t>(guard, 1), removal_guard_seen = 0xFF;
            remove(mine);
            CHECK(removal_guard_seen == 1 && fake::get<uint8_t>(guard) == 1);
            fake::put<uint8_t>(guard, 0);
        }
        CHECK(live(rock) && live(wall) && tsuchi::map_counts().nested_asked == 4);
        scene.log.clear();
        comes(scene.streams[5], {});
        steps(15);
        // Removals first, each actor once, then the moves of what is left. A root that may not move is made movable
        // for the move and put back, and so is what that made movable under it; one that may is left alone.
        CHECK(scene.log == "destroy:Rock_3 destroy:Wall_1 mobility:Gate_4Root:2 place:Gate_4:7,8,9/0,45,0 at once scale:Gate_4:2,2,2 mobility:Gate_4Root:0 mobility:Gate_4Light:0 "
                           "mobility:Gate_4Root:2 place:Gate_4:12,13,14/0,0,0 at once mobility:Gate_4Root:0 mobility:Gate_4Light:0 place:Lamp_2:4,5,6/0,0,0 at once ");
        CHECK(fake::get<uint8_t>(scene.roots[gate] + 0x60) == 0 && fake::get<uint8_t>(gate_light + 0x60) == 0);
        CHECK(!live(rock) && !live(wall) && live(gate) && live(lamp) && live(keep));
        CHECK(live(foreign_rock) && live(foreign_wall) && live(foreign_gate) && holds(foreign_lamp) == "1.25 0 cold 1.5");
        CHECK(told("[map.old] maps: GameArea has no actor Gone_7, so nothing of that sublevel is changed for this mod"));
        // Values last. Each has the say of the mod started last, whose spelling of a path may differ, and the
        // other mod is told whose is used. A value the game will not take, or does not have, is told and left,
        // and the next mod's say on it stands. A path may lead through a reference into another object. Nothing
        // is set on an actor a mod removed.
        CHECK(holds(lamp) == "7 1 warm 3");
        CHECK(told("[map.one] maps: Intensity of Lamp_2 in GameArea is set by map.two, not by this mod\n") &&
              told("[map.one] maps: Scene.Glow of Lamp_2 in GameArea is set by map.two, not by this mod\n") &&
              told("[map.two] maps: Nothing of Lamp_2 in GameArea could not be set: the actor has no such value\n") &&
              told("[map.two] maps: Label of Lamp_2 in GameArea could not be set: it is of a kind that does not take such a value\n") &&
              told("[map.two] Removed Wall_1 from GameArea for this map.\n") &&
              !told("Moved Wall_1") && !told("of Wall_1") &&
              told("[map.one] maps: Label of Lamp_2 in GameArea was set for this map.\n"));
        CHECK(tsuchi::map_counts().values_set == counted.values_set + 4 && tsuchi::map_counts().values_refused == counted.values_refused + 2 &&
              tsuchi::map_counts().values_put_back == counted.values_put_back);
        // Done once for a sublevel that stays.
        scene.log.clear(), logged.lines.clear();
        steps(60);
        CHECK(scene.log.empty() && logged.lines.find("maps:") == std::string::npos);
        // Stopping an earlier part preserves the start order of the remaining, overlapping moves.
        tsuchi::mod_stop(tsuchi::find_mod("map.nest")->context);
        // The game loads the sublevel again: its actors are back, and so are the changes.
        // The retired copy and its actors stay live until collection; only the current world's copy is accepted.
        const uint64_t retired = area, retired_gate = gate, retired_lamp = lamp;
        fake::put<uint64_t>(retired + kOwningAt, 0);
        area = level_of(u"GameArea");
        rock = actor_in(area, u"Rock_3", 0), wall = actor_in(area, u"Wall_1", 0), gate = actor_in(area, u"Gate_4", 2), lamp = as_made(actor_in(area, u"Lamp_2", 2));
        steps(15);
        CHECK(scene.log == "destroy:Rock_3 destroy:Wall_1 place:Gate_4:7,8,9/0,45,0 at once scale:Gate_4:2,2,2 place:Gate_4:12,13,14/0,0,0 at once place:Lamp_2:4,5,6/0,0,0 at once " && !live(rock) && live(gate));
        CHECK(holds(lamp) == "7 1 warm 3" && live(retired) && live(retired_gate) && holds(retired_lamp) == "7 1 warm 3");
        scene.log.clear();
        steps(45);
        CHECK(scene.log.empty());
        // Both already handled level objects can remain owned by this world until collection. Keep both records.
        fake::put(retired + kOwningAt, cave);
        steps(45);
        CHECK(scene.log.empty());
        // A mod stops: each value it had the say on gets the next mod's, or with no other say, what it was.
        tsuchi::mod_stop(tsuchi::find_mod("map.two")->context);
        CHECK(holds(lamp) == "4.5 0 warm 2" && tsuchi::map_counts().values_put_back == counted.values_put_back + 2);
        // Another map: its own mods' levels, once it is past its start with nothing of the game's to wait for.
        scene.log.clear(), scene.world = other;
        fake::put(main + t::data::world, other);
        steps(299);
        CHECK(scene.log.empty());
        // The world that was left: what was set there is as it was before any mod's say, the asset that outlasts
        // the level too.
        CHECK(holds(lamp) == "1.25 0 cold 1.5" && tsuchi::map_counts().values_put_back == counted.values_put_back + 7);
        // Existing leave-play hooks remove an actor's says before another frame, even for a shared asset.
        const uint64_t leaving_a = as_made(actor_in(area, u"Leaving_A", 2)), leaving_b = as_made(actor_in(area, u"Leaving_B", 2));
        const uint64_t leaving_type = fake::object(u"LeavingActor", w.root);
        fake::set_super(leaving_type, w.actor_class);
        fake::field(leaving_type, u"Scene", u"ObjectProperty", kPresetAt, 8, preset_type);
        fake::put(leaving_a + t::object::class_, leaving_type), fake::put(leaving_b + t::object::class_, leaving_type);
        fake::put(leaving_a, table_a()), fake::put(leaving_b, table_a());
        registers(leaving_a), registers(leaving_b);
        const char* immediate_text = R"({"Level":{"First":{"Scene.Glow":8},"Second":{"Scene.Glow":9}}})";
        const tsuchi::Json* immediate = tsuchi::read_json(immediate_text, std::strlen(immediate_text));
        CHECK(immediate);
        if (immediate) {
            const tsuchi::Json& level = *immediate->first;
            tsuchi::values_say(0xFACE1, tsuchi::find_mod("map.else")->context, handle(leaving_a).id, level, *level.first);
            tsuchi::values_say(0xFACE1, tsuchi::find_mod("map.else")->context, handle(leaving_b).id, level, *level.first->next);
            tsuchi::values_settle();
            CHECK(fake::get<float>(preset + kGlowAt) == 9);
            ends_play(leaving_b, t::constant::end_play_removed);
            CHECK(fake::get<float>(preset + kGlowAt) == 8);
            ends_play(leaving_a, t::constant::end_play_removed);
            CHECK(fake::get<float>(preset + kGlowAt) == 1.5f);
            tsuchi::release_json(immediate);
        }
        fake::destroy(leaving_a), fake::destroy(leaving_b);
        steps(1);
        CHECK(scene.log == "load:/G/Mods/Else ");
        // An accepted stream whose package never produces a visible level has a deadline.
        scene.log.clear();
        steps(720);
        CHECK(scene.log.empty() && told("[map.else] maps: an awaited level did not arrive in /G/Other_P"));
        CHECK(problem("map.else") == "<running>" && !tsuchi::find_mod("map.else")->problem.get()[0]);
        scene.log.clear();
        steps(240);
        CHECK(scene.log.empty());
        // A mod that stops takes its part with it.
        tsuchi::forget_mods();
        CHECK(tsuchi::map_parts() == 0);
        scene.log.clear();
        fake::put(main + t::data::world, cave);
        steps(60);
        CHECK(scene.log.empty());
        // A later navigation declaration whose ground fails never replaces the earlier good navigation.
        late_plain("map.nav2", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "levels": ["/G/Mods/Broken"], "navigation": "maps/cave.nav"}])");
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        start_late_maps(3, 8);
        scene.world = cave;
        nav_scene.log.clear();
        steps(299);
        CHECK(nav_scene.log.empty());
        steps(1);
        CHECK(nav_scene.log.empty());
        steps(1);
        CHECK(!nav_scene.log.empty() && told("[map.nav2] maps: /G/Mods/Broken could not be loaded"));
        tsuchi::forget_mods();
        // Stop after 600 attempts even if the implementation arrives on the next frame.
        plain("map.nav2", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "mode": "story", "navigation": "maps/cave.nav"}])");
        disk.files.erase(mods_at + "map.nav2/main.lua");
        fake::put<uint64_t>(nav_actor + t::navmesh_actor::implementation, 0);
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        start_late_maps(4, 8);
        scene.world = cave;
        nav_scene.log.clear();
        fake::put<uint64_t>(main + t::data::world, 0);
        steps(1);
        fake::put(main + t::data::world, cave);
        steps(599);
        CHECK(nav_scene.log.empty() && !told("[map.nav2] maps: stopped waiting"));
        steps(1);
        CHECK(nav_scene.log.empty() && told("[map.nav2] maps: stopped waiting for navigation in /G/Cave_P after 600 attempts"));
        fake::put(nav_actor + t::navmesh_actor::implementation, nav_implementation);
        steps(2);
        CHECK(nav_scene.log.empty());
        tsuchi::forget_mods();
        // A missing mode now installs ground navigation at night and in Olimar play too.
        for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/main.lua");
        plain("map.nav2", "1.0.0", R"(, "maps": [{"map": "/G/Cave_P", "navigation": "maps/cave.nav"}])");
        for (unsigned mode = 0; mode < 3; ++mode) {
            fake::put<uint64_t>(main + t::data::world, 0);
            fake::put<uint8_t>(map_clock + t::clock::night, mode == 1);
            t::fn::story_flags = mode == 2 ? +[](void*) -> uint64_t { return t::constant::story_of_olimar; }
                                         : +[](void*) -> uint64_t { return 0; };
            CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods(); steps(1);
            const auto nav_before = tsuchi::map_navigation_counts();
            nav_scene.log.clear(); fake::put(main + t::data::world, cave); steps(301);
            CHECK(!nav_scene.log.empty() && tsuchi::map_navigation_counts().navmeshes == nav_before.navmeshes + 1);
            tsuchi::forget_mods();
        }
        // Object-only night entries keep shared navigation; declaring empty levels replaces it.
        // Explicit navigation replaces the shared declaration, rather than swapping twice.
        const char* mode_entries[] = {
            R"(, "maps": [{"map":"/G/Cave_P","navigation":"maps/cave.nav"},{"map":"/G/Cave_P","mode":"night","placements":[]}])",
            R"(, "maps": [{"map":"/G/Cave_P","navigation":"maps/cave.nav"},{"map":"/G/Cave_P","mode":"night","levels":[]}])",
            R"(, "maps": [{"map":"/G/Cave_P","navigation":"maps/cave.nav"},{"map":"/G/Cave_P","mode":"night","navigation":"maps/cave.nav"}])"
        };
        for (unsigned which = 0; which < 3; ++which) {
            plain("map.nav2", "1.0.0", mode_entries[which]);
            fake::put<uint64_t>(main + t::data::world, 0);
            fake::put<uint8_t>(map_clock + t::clock::night, 1);
            t::fn::story_flags = +[](void*) -> uint64_t { return 0; };
            CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods(); steps(1);
            const auto before = tsuchi::map_navigation_counts();
            nav_scene.log.clear(); fake::put(main + t::data::world, cave); steps(301);
            CHECK(tsuchi::map_navigation_counts().navmeshes == before.navmeshes + (which == 1 ? 0 : 1));
            CHECK(nav_scene.log.empty() == (which == 1));
            tsuchi::forget_mods();
        }
        // Navigation declared while the map runs, after the map's navigation was seen to, is put in then.
        {
            plain("map.nav2", "1.0.0", R"(, "maps": [{"map":"/G/Cave_P","placements":[]}])");
            fake::put<uint64_t>(main + t::data::world, 0);
            CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods(); steps(1);
            nav_scene.log.clear(); fake::put(main + t::data::world, cave); steps(301);
            CHECK(nav_scene.log.empty());
            const char* declared = R"([{"map":"/G/Cave_P","navigation":"maps/cave.nav"}])";
            const tsuchi::Json* section = tsuchi::read_json(declared, std::strlen(declared));
            tsuchi::Text wrong;
            const auto before = tsuchi::map_navigation_counts();
            CHECK(section && tsuchi::manifest_part("maps", tsuchi::find_mod("map.nav2")->context, *section, wrong, true) == TSUCHI_OK);
            wrong.clear();
            steps(301);
            CHECK(!nav_scene.log.empty() && tsuchi::map_navigation_counts().navmeshes == before.navmeshes + 1);
            tsuchi::forget_mods();
            tsuchi::release_json(section);
        }
        for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/main.lua"), disk.files.erase(mods_at + id + "/maps/cave.json"), disk.files.erase(mods_at + id + "/maps/cave.nav");
        // A content-only exported map has no edits to the game's sublevels. Its periodic stream checks must
        // not walk the whole UObject table. Measure the same 120 frames without a part, then with an empty
        // added-level declaration. The existing frame services still run in both measurements.
        fake::put<uint8_t>(map_clock + t::clock::night, 0);
        fake::put(main + t::data::world, cave);
        fake::measured_read_address = main + t::data::object_count;
        fake::measured_reads = 0;
        steps(120);
        const uint64_t ordinary_reads = fake::measured_reads;
        plain("map.content-only", "1.0.0", R"(, "maps": [{"map":"/G/Cave_P","levels":[]}])");
        CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
        CHECK(problem("map.content-only") == "<running>" && tsuchi::map_parts() == 1);
        fake::measured_reads = 0;
        steps(120);
        const uint64_t content_reads = fake::measured_reads;
        std::printf("map content-only workload: ordinary=%llu content=%llu object slots=%d\n",
                    static_cast<unsigned long long>(ordinary_reads), static_cast<unsigned long long>(content_reads),
                    fake::get<int32_t>(main + t::data::object_count));
        // Allow bounded per-part handle/status checks, independent of the table's thousands of slots.
        CHECK(content_reads <= ordinary_reads + 512);
        tsuchi::forget_mods(); disk.files.erase(mods_at + "map.content-only/mod.json");
        fake::measured_read_address = 0;
        fake::put(main + t::data::world, old_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
        t::fn::nav_remove_tile = nullptr, t::fn::nav_add_tile = nullptr, t::fn::nav_invalidate_paths = nullptr;
        fake::put<uint64_t>(main + t::data::nav_allocate, 0), fake::put<uint64_t>(main + t::data::nav_release, 0);
        fake::put(w.framework + t::game_framework::clock, old_map_clock);
        fake::put<uint8_t>(running.context + t::game_context::rule, old_map_rule);
        t::fn::story_flags = old_map_flags;
        logged.lines.clear();
    }
    // The "placements" of a maps entry: lists of gameplay objects for the game to place in one of its maps and to
    // save as it does its own. A list is given once the game has made the map's generators and every level asked
    // for is in the world: each row as a generator at the end of the map's own, made and started by the game's
    // functions, and after the rows the list's mark, under a number made from the mod's ID and the list's path.
    // A map that has the mark, as one restored from a save has, is given nothing of the list again. Every mod
    // that names the map gives its lists, in the order the mods started. A mod whose level could not be loaded
    // gives none, and neither does a mod that has stopped.
    {
        namespace g = t::generator;
        namespace k = t::generator_keeper;
        const uint64_t kIn = t::constant::property_param, kOut = kIn | t::constant::property_out, kResult = kOut | t::constant::property_return;
        const uint64_t main = fake::main_image();
        static ClassRef world_ref{"World"}, stream_ref{"LevelStreamingDynamic"};
        const uint64_t world_type = find_class(world_ref), stream_type = find_class(stream_ref);
        CHECK(world_type && stream_type);
        // The engine takes none of these mods' levels.
        const uint64_t load = fake::function(stream_type, function_type, u"LoadLevelInstance", 0x28, 8, [](uint64_t, uint64_t arguments) {
            fake::put<uint8_t>(arguments + 0x18, 0), fake::put<uint64_t>(arguments + 0x20, 0);
        }, t::constant::function_static);
        fake::flag(fake::field(load, u"WorldContextObject", u"ObjectProperty", 0, 8, world_type), kIn);
        fake::flag(fake::field(load, u"LevelName", u"StrProperty", 8, 0x10), kIn);
        fake::flag(fake::bit_field(load, u"bOutSuccess", 0x18, 0, 0xFF), kOut);
        fake::flag(fake::field(load, u"ReturnValue", u"ObjectProperty", 0x20, 8, stream_type), kResult);

        // A placement list as the game describes one: an array of rows, each laid out as a generator, which names
        // its class by path. The first row of a mod's list names none.
        constexpr int32_t kRowsAt = 0x40;
        const uint64_t row_type = fake::memory(0x80), list_type = fake::object(u"PlaceList", w.root);
        fake::put<uint64_t>(row_type, fake::engine().vtable);
        fake::put<int32_t>(row_type + t::struct_type::bytes, int32_t(g::size)), fake::put<int32_t>(row_type + t::struct_type::alignment, 8);
        fake::field(row_type, u"SoftRefActorClass", u"SoftObjectProperty", kRowClassAt, int32_t(t::soft_object::size), actor_base_class);
        fake::field(list_type, u"ActorGeneratorList", u"ArrayProperty", kRowsAt, 0x10, fake::loose_field(u"StructProperty", int32_t(g::size), row_type));
        struct Row {
            uint64_t number;
            std::u16string type;
            uint32_t count;
            float x;
        };
        const auto shelve = [](std::u16string path, uint64_t asset) { shelf.assets[fake::engine_name(path.data(), path.size(), 1)] = asset; };
        const auto list_of = [&](const std::u16string& package, const std::u16string& name, std::vector<Row> rows) {
            const uint64_t asset = fake::object(name, list_type, 0x80), data = fake::memory(rows.size() * g::size + 8);
            for (size_t i = 0; i < rows.size(); ++i) {
                const uint64_t row = data + i * g::size;
                fake::put(row + g::placement, rows[i].number), fake::put(row + g::count, rows[i].count), fake::put(row + kRowPlaceAt, rows[i].x);
                const uint64_t owned = fake::memory(3);
                fake::put<uint8_t>(owned, 0xA1), fake::put<uint8_t>(owned + 1, 0xB2), fake::put<uint8_t>(owned + 2, 0xC3);
                fake::put(row + kRowBytesAt, owned), fake::put<int32_t>(row + kRowBytesAt + t::array::count, 3), fake::put<int32_t>(row + kRowBytesAt + t::array::max, 3);
                fake::put<int32_t>(row + kRowClassAt + t::soft_object::index, -1);
                if (!rows[i].type.empty()) fake::put<uint64_t>(row + kRowClassAt + t::soft_object::path, fake::engine_name(rows[i].type.data(), rows[i].type.size(), 1));
            }
            fake::put(asset + kRowsAt + t::array::data, data);
            fake::put<int32_t>(asset + kRowsAt + t::array::count, int32_t(rows.size())), fake::put<int32_t>(asset + kRowsAt + t::array::max, int32_t(rows.size()));
            shelve(package + u"." + name, asset);
            return asset;
        };
        const uint64_t pot = fake::object(u"Pot_C", w.root), bug = fake::object(u"Bug_C", w.root), rug = fake::object(u"Rug_C", w.root);
        fake::set_super(pot, w.actor_class), fake::set_super(bug, w.creature_class);
        shelve(u"/G/Pot.Pot_C", pot), shelve(u"/G/Bug.Bug_C", bug), shelve(u"/G/Rug.Rug_C", rug);
        list_of(u"/G/M/Pots", u"Pots", {{0x9000, u"", 0, 0}, {0x2001, u"/G/Pot.Pot_C", 1, 10}, {0x2002, u"/G/Bug.Bug_C", 2, 20}});
        // A list with rows that cannot go in: one the map has, a class that is not there, a class that is no actor's,
        // one the game makes too few actors for, and one with no number.
        list_of(u"/G/M/More", u"More", {{0x9000, u"", 0, 0}, {0x2001, u"/G/Pot.Pot_C", 1, 0}, {0x2003, u"/G/No.No_C", 1, 0}, {0x2004, u"/G/Rug.Rug_C", 1, 0},
                                        {0x2005, u"/G/Pot.Pot_C", 1, 0}, {0, u"/G/Pot.Pot_C", 1, 0}});
        list_of(u"/G/N/Gates", u"Gates", {{0x9000, u"", 0, 0}, {0x3001, u"/G/Pot.Pot_C", 1, 30}});
        list_of(u"/G/N/Odd", u"Odd", {{0x9001, u"/G/Pot.Pot_C", 1, 0}, {0x3002, u"/G/Pot.Pot_C", 1, 0}}); // no mark
        list_of(u"/G/N/Bare", u"Bare", {});
        shelve(u"/G/N/Not.Not", pot); // something else than a list
        list_of(u"/G/M/Far", u"Far", {{0x9000, u"", 0, 0}, {0x4001, u"/G/Pot.Pot_C", 1, 40}});
        list_of(u"/G/M/Lost", u"Lost", {{0x9000, u"", 0, 0}, {0x5001, u"/G/Pot.Pot_C", 1, 50}});

        // The map: its package and world, what makes its placed objects, and its clock.
        const auto world_in = [&](const std::u16string& package_path, const std::u16string& name) {
            const uint64_t package = fake::object(package_path, w.root, 0x40), world = fake::object(name, world_type, 0x40);
            fake::put(world + t::object::outer, package);
            return world;
        };
        const Name area_name = fake::name(u"DenArea");
        const auto generators_of = [&](uint64_t keeper) { return fake::get<int32_t>(keeper + k::generators + t::array::count); };
        const auto generator_at = [&](uint64_t keeper, int32_t place) { return fake::get<uint64_t>(keeper + k::generators + t::array::data) + uint64_t(place) * g::size; };
        // A keeper with the game's own two generators, or with those another holds, as a save gives them back.
        const auto keeper_of = [&](uint64_t saved, bool built) {
            const uint64_t keeper = fake::memory(0x40);
            const int32_t count = saved ? generators_of(saved) : 2;
            const uint64_t data = fake::memory(size_t(count) * g::size);
            if (saved) std::memcpy(reinterpret_cast<void*>(data), reinterpret_cast<void*>(generator_at(saved, 0)), size_t(count) * g::size);
            for (int32_t i = 0; !saved && i < 2; ++i)
                fake::put<uint64_t>(data + i * g::size + g::placement, 0x1001 + i), fake::put(data + i * g::size + g::sublevel, area_name), fake::put(data + i * g::size + g::index, i);
            fake::put(keeper + k::generators + t::array::data, data);
            fake::put(keeper + k::generators + t::array::count, count), fake::put(keeper + k::generators + t::array::max, count);
            fake::put<uint8_t>(keeper + k::built, built ? uint8_t(t::constant::generators_built) : 1);
            fake::put(w.framework + t::game_framework::generators, keeper);
            placing.keeper = keeper;
            return keeper;
        };
        const auto enter = [&](uint64_t world) {
            placing.world = world;
            fake::put(main + t::data::world, world);
        };
        const uint64_t old_world = fake::get<uint64_t>(main + t::data::world), old_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
        const uint8_t old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
        const auto old_load = t::fn::load_soft_object;
        const auto old_place_flags = t::fn::story_flags;
        const uint8_t old_place_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
        fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
        t::fn::story_flags = [](void*) -> uint64_t { return 0; };
        const uint64_t clock = fake::memory(0x20), clock_table = fake::memory(0x40);
        fake::put(clock, clock_table), fake::put(clock_table + t::clock_table::day, &game_day);
        fake::code().push_back(reinterpret_cast<uint64_t>(&game_day));
        fake::put(w.framework + t::game_framework::clock, clock), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
        t::fn::load_soft_object = game_load_soft_object;
        t::fn::generator_copy = game_generator_copy, t::fn::generator_array_grow = game_generator_array_grow, t::fn::generator_spawn = game_generator_spawn;
        t::fn::actor_start_spawned = game_actor_start_spawned, t::fn::spawn_request_destroy = game_request_destroy;
        placing = Placing{};
        t::fn::generator_destroy = game_generator_destroy;
        fake::function(w.actor_class, function_type, u"K2_DestroyActor", 0, 0, [](uint64_t object, uint64_t) { fake::destroy(object); });

        const char* const made[] = {"place.one", "place.two", "place.else", "place.broken", "place.bad", "place.dot"};
        const auto late_plain = [&](const std::string& id, const std::string& version, const std::string& more) {
            plain(id, version);
            disk.files[mods_at + id + "/main.lua"] = "content.add([==[{" + more.substr(1) + "}]==])\n";
        };
        plain("place.one", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "placements": ["/G/M/Pots", "/G/M/More"]}])");
        plain("place.two", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "placements": ["/G/N/Gates", "/G/N/None", "/G/N/Odd", "/G/N/Bare", "/G/N/Not"]}])");
        plain("place.else", "1.0.0", R"(, "maps": [{"map": "/G/Far_P", "placements": ["/G/M/Far"]}])");
        late_plain("place.broken", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "levels": ["/G/Mods/Broken"], "placements": ["/G/M/Lost"]}])");
        plain("place.bad", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "placements": "/G/M/Pots"}])");
        plain("place.dot", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "placements": ["/G/M/Pots.Pots"]}])");
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(problem("place.bad") == "mod.json: \"maps\": \"placements\" is a list of the mod's placement lists, each by the path of its package");
        CHECK(problem("place.dot") == std::string("mod.json: \"maps\": \"placements\": /G/M/Pots.Pots") + tsuchi::content_path::kNoPackage);
        logged.lines.clear();
        tsuchi::start_mods();
        CHECK(tsuchi::map_parts() == 3); // the runtime ground gate is queued by main.lua
        tsuchi::frame(0.03f);
        CHECK(problem("place.one") == "<running>" && problem("place.two") == "<running>" && problem("place.else") == "<running>" && problem("place.broken") == "<running>" &&
              tsuchi::map_parts() == 4);
        const auto steps = [](int count) {
            for (int i = 0; i < count; ++i) tsuchi::frame(0.03f);
        };
        const auto told = [&](const std::string& part) { return logged.lines.find(part) != std::string::npos; };
        const tsuchi::PlacementCounts before = tsuchi::map_placement_counts();
        const auto counted = [&] {
            const tsuchi::PlacementCounts now = tsuchi::map_placement_counts();
            char text[96];
            std::snprintf(text, sizeof text, "%u/%u/%u/%u lists, %u/%u/%u rows, %u actors", now.lists_given - before.lists_given, now.lists_known - before.lists_known,
                          now.lists_refused - before.lists_refused, now.lists_partial - before.lists_partial, now.rows_given - before.rows_given,
                          now.rows_known - before.rows_known, now.rows_refused - before.rows_refused, now.actors_made - before.actors_made);
            return std::string(text);
        };

        // The map made fresh. Nothing is given while a level asked for has not been tried, nor while the game has
        // not made the map's generators.
        const uint64_t den = world_in(u"/G/Den_P", u"Den_P"), far = world_in(u"/G/Far_P", u"Far_P");
        uint64_t keeper = keeper_of(0, true);
        enter(den);
        steps(314);
        CHECK(placing.log.empty() && generators_of(keeper) == 2 && told("[place.broken] maps: /G/Mods/Broken could not be loaded into /G/Den_P"));
        fake::put<uint8_t>(keeper + k::built, 1);
        steps(30);
        CHECK(placing.log.empty() && generators_of(keeper) == 2 && counted() == "0/0/0/0 lists, 0/0/0 rows, 0 actors");
        fake::put<uint8_t>(keeper + k::built, uint8_t(t::constant::generators_built));
        placing.short_of = 0x2005;
        steps(15);
        // Each mod's lists in the order the mods started, and one mod's in the order its manifest has them: a row
        // is copied to the end of the map's generators, its actors are made and started, and the mark comes last.
        CHECK(placing.log == "grow copy spawn:2001 start copy spawn:2002 start start copy grow copy spawn:2005 copy spawn:3001 start copy ");
        CHECK(placing.requests_good && placing.started == 4 && spawner.requests_destroyed >= 4);
        CHECK(generators_of(keeper) == 7 && counted() == "2/0/4/1 lists, 3/1/4 rows, 4 actors");
        // A row's generator: its place, its class, the part of the level the game's first generator belongs to,
        // the day, how many it has to make, and the flag of one added while the map runs.
        const uint64_t first = generator_at(keeper, 2), pots_mark = generator_at(keeper, 4), gates_mark = generator_at(keeper, 6);
        CHECK(fake::get<uint64_t>(first + g::placement) == 0x2001 && fake::get<int32_t>(first + g::index) == 2 && fake::get<uint64_t>(first + g::class_) == pot &&
              fake::get<uint64_t>(first + g::sublevel) == fake::packed(area_name) && fake::get<uint32_t>(first + g::birth_day) == 7 && fake::get<uint32_t>(first + g::left) == 1 &&
              fake::get<uint32_t>(first + g::flags) == t::constant::generator_added && fake::get<int32_t>(first + g::actors + t::array::count) == 1);
        CHECK(fake::get<uint64_t>(generator_at(keeper, 3) + g::class_) == bug && fake::get<uint32_t>(generator_at(keeper, 3) + g::left) == 2);
        // A mark: the list's first row, which makes nothing, under a number that is the framework's and no asset's.
        const uint64_t pots_number = fake::get<uint64_t>(pots_mark + g::placement), gates_number = fake::get<uint64_t>(gates_mark + g::placement);
        CHECK(pots_number && pots_number != 0x9000 && gates_number && gates_number != 0x9000 && pots_number != gates_number && pots_number != ~uint64_t(0));
        CHECK(fake::get<uint64_t>(pots_mark + g::class_) == 0 && fake::get<int32_t>(pots_mark + g::index) == 4 && fake::get<uint64_t>(pots_mark + g::sublevel) == fake::packed(area_name) &&
              fake::get<uint32_t>(pots_mark + g::flags) == 0 && fake::get<int32_t>(gates_mark + g::index) == 6);
        // What was placed is found as an object of the game's own is.
        TSUCHI_Actor found{};
        CHECK(api.find_placed(mod, 0x2001, &found) == TSUCHI_OK && from_handle(found.id) == placing.made[0] && api.find_placed(mod, 0x3001, &found) == TSUCHI_OK);
        // Each row that did not go in is told, with why, and its list gets no mark.
        CHECK(told("[place.one] maps: /G/M/More, row 1 was passed over: /G/Den_P has an object under its placement number already\n") &&
              told("[place.one] maps: /G/M/More, row 2 was not placed in /G/Den_P: its class could not be loaded\n") &&
              told("[place.one] maps: /G/M/More, row 3 was not placed in /G/Den_P: its class is no class of the game's actors\n") &&
              told("[place.one] maps: /G/M/More, row 4 was not placed in /G/Den_P: the game made fewer actors for it than it asks for\n") &&
              told("[place.one] maps: /G/M/More, row 5 was not placed in /G/Den_P: it has no placement number\n") &&
              told("[place.one] maps: the placement list /G/M/More is not marked as given to /G/Den_P, and what the map lacks of it is tried again the next time the map loads\n"));
        CHECK(told("[place.two] maps: the placement list /G/N/None was not given to /G/Den_P: it could not be loaded\n") &&
              told("[place.two] maps: the placement list /G/N/Odd was not given to /G/Den_P: it has no mark: its first row has to name no class\n") &&
              told("[place.two] maps: the placement list /G/N/Bare was not given to /G/Den_P: it is not laid out as the game's placement lists are, or has no rows\n") &&
              told("[place.two] maps: the placement list /G/N/Not was not given to /G/Den_P: it is not laid out as the game's placement lists are, or has no rows\n"));
        // The list of a mod whose level could not be loaded is not given, nor one for another map.
        CHECK(!told("/G/M/Lost") && !told("/G/M/Far") && !told("Pots") && !told("Gates"));
        // Once for a world.
        placing.log.clear(), logged.lines.clear();
        steps(240);
        CHECK(placing.log.empty() && generators_of(keeper) == 7 && !told("maps: the placement") && !told("row"));

        // The map again, as the game restores it from its save: the generators it had, the marks among them. A
        // list whose mark is there is not given again, so what was used up stays used up. The list that got no
        // mark is tried again, and the rows the map has are passed over.
        const uint64_t again = world_in(u"/G/Den_P", u"Den_P");
        keeper = keeper_of(keeper, true);
        enter(again);
        steps(345);
        CHECK(placing.log == "grow copy spawn:2005 " && generators_of(keeper) == 7 && counted() == "2/2/8/2 lists, 3/2/8 rows, 4 actors");
        CHECK(told("[place.one] maps: /G/M/More, row 4 was not placed in /G/Den_P: the game made fewer actors for it than it asks for\n") && !told("Pots") && !told("Gates"));

        // Another map: its own mod's list, at the first look, since no level is waited for there. With no generator
        // of the game's to go by, the rows keep the part of the level their list gives them.
        placing.log.clear();
        keeper = keeper_of(0, true);
        fake::put<int32_t>(keeper + k::generators + t::array::count, 0);
        enter(far);
        steps(29);
        CHECK(placing.log.empty());
        steps(1);
        CHECK(placing.log == "copy spawn:4001 start copy " && generators_of(keeper) == 2 && counted() == "3/2/8/2 lists, 4/2/8 rows, 5 actors");
        CHECK(fake::get<uint64_t>(generator_at(keeper, 0) + g::sublevel) == 0 && fake::get<uint64_t>(generator_at(keeper, 1) + g::placement) != pots_number);

        // A mod that stops gives no more lists: the map made fresh again gets the other mod's alone.
        tsuchi::mod_stop(tsuchi::find_mod("place.two")->context);
        placing.log.clear(), placing.short_of = 0;
        keeper = keeper_of(0, true);
        enter(world_in(u"/G/Den_P", u"Den_P"));
        steps(345);
        CHECK(placing.log == "grow copy spawn:2001 start copy spawn:2002 start start copy grow copy spawn:2005 start " && generators_of(keeper) == 6);
        CHECK(fake::get<uint64_t>(generator_at(keeper, 4) + g::placement) == pots_number); // the same list has the same mark in every save
        tsuchi::forget_mods();
        CHECK(tsuchi::map_parts() == 0);
        placing.log.clear();
        keeper = keeper_of(0, true);
        enter(world_in(u"/G/Den_P", u"Den_P"));
        steps(345);
        CHECK(placing.log.empty() && generators_of(keeper) == 2);

        #include "placement_checks.h"
        #include "placement_change_checks.h"
        for (uint64_t placed : placing.made) fake::destroy(placed);
        fake::put(main + t::data::world, old_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
        fake::put(w.framework + t::game_framework::clock, old_clock), fake::put<uint64_t>(w.framework + t::game_framework::generators, 0);
        t::fn::load_soft_object = old_load;
        t::fn::story_flags = old_place_flags;
        fake::put<uint8_t>(running.context + t::game_context::rule, old_place_rule);
        t::fn::generator_destroy = nullptr;
        t::fn::generator_copy = nullptr, t::fn::generator_array_grow = nullptr, t::fn::generator_spawn = nullptr, t::fn::actor_start_spawned = nullptr;
        for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/main.lua");
        logged.lines.clear();
    }
    // The "ai" of a maps entry: a file of AI values for creatures the game places in one of its maps, each named
    // by its placement number and its class. As such a creature's AI part starts, and before the game's own
    // start of it runs, the part writes its values out, the file's groups go in the place of its own, and it
    // reads the result. Every mod that names the map takes part, and a group two of them change has the bytes
    // of the mod started last. A creature made again, as one restored from a save is, gets the values again.
    {
        const uint64_t main = fake::main_image();
        static ClassRef world_ref{"World"}, level_ref{"Level"};
        const uint64_t world_type = find_class(world_ref), level_type = find_class(level_ref);
        CHECK(world_type && level_type);
        const auto old_ai_flags = t::fn::story_flags;
        const uint64_t old_ai_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
        const uint8_t old_ai_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
        const uint64_t ai_clock = fake::memory(0x20);
        fake::put(w.framework + t::game_framework::clock, ai_clock);
        fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
        t::fn::story_flags = [](void*) -> uint64_t { return 0; };
        constexpr int32_t kOwningAt = 0x40;
        fake::field(level_type, u"OwningWorld", u"ObjectProperty", kOwningAt, 8, world_type);
        const auto level_in = [&](const std::u16string& package_path, const std::u16string& name) {
            const uint64_t package = fake::object(package_path, w.root, 0x40), world = fake::object(name, world_type, 0x80);
            const uint64_t level = fake::object(u"PersistentLevel", level_type, 0x80);
            fake::put(world + t::object::outer, package), fake::put(level + t::object::outer, world), fake::put(level + kOwningAt, world);
            return level;
        };
        const auto class_in = [&](const std::u16string& package_path, const std::u16string& name) {
            const uint64_t package = fake::object(package_path, w.root, 0x40), type = fake::object(name, w.root);
            fake::put(type + t::object::outer, package), fake::set_super(type, w.creature_class);
            return type;
        };
        const uint64_t baby = class_in(u"/G/Baby", u"Baby_C"), bee = class_in(u"/G/Bee", u"Bee_C"), ant = class_in(u"/G/Ant", u"Ant_C");
        // The game's two enums that a drop's values are of.
        const uint64_t enum_class = fake::object(u"Enum", w.root);
        const auto enum_of = [&](const std::u16string& name, std::vector<int64_t> values) {
            const uint64_t type = fake::object(name, enum_class, 0x80), data = fake::memory(values.size() * t::enum_entry::size);
            for (size_t i = 0; i < values.size(); ++i) fake::put(data + i * t::enum_entry::size + t::enum_entry::value, values[i]);
            fake::put(type + t::enum_type::names + t::array::data, data);
            fake::put(type + t::enum_type::names + t::array::count, int32_t(values.size())), fake::put(type + t::enum_type::names + t::array::max, int32_t(values.size()));
            return type;
        };
        const uint64_t condition_enum = enum_of(u"EDropCondition", {0, 1, 2});
        enum_of(u"EDemoFlagType", {0, 5});
        // The function tables of the three kinds of AI part, each with the game's own start and serializers in it.
        const t::SlotGroup& start = t::slot_group::ai_part_start, &write_set = t::slot_group::ai_part_write_set;
        install_ai_native_starts();
        t::fn::ai_values_write = game_ai_values_write, t::fn::ai_values_read = game_ai_values_read;
        minds = Minds{};
        std::vector<uint64_t> creatures;
        const auto creature = [&](uint64_t level, const std::u16string& name, uint64_t type, uint64_t placement, uint32_t kind) {
            const uint64_t made = actor(name, type, u"Mind", 3, placement, component(0, 0, 0, 0)), part = fake::memory(0x100);
            fake::put(made + t::object::outer, level), fake::put(part, main + start.tables[kind - 1]);
            fake::put(part + t::scene_component::owner, made), fake::put(made + t::game_actor::ai, part);
            minds.set[part] = ai_stream(kind, 100);
            creatures.push_back(made);
            return part;
        };
        // The game starting a part, and what it wrote in its log for it.
        const auto starts = [&](uint64_t part) {
            minds.log.clear();
            thunks::call(fake::get<uint64_t>(fake::get<uint64_t>(part) + start.offset), part);
            return minds.log;
        };

        // A file: its map, and for each placement its number, its class, its kind of AI part and its groups.
        using Group = std::pair<uint8_t, std::vector<uint8_t>>;
        struct Named {
            uint64_t number;
            std::string type;
            uint8_t kind;
            std::vector<Group> groups;
        };
        const auto file_of = [](const std::string& map, const std::vector<Named>& placements) {
            std::string body;
            const auto number = [&](uint64_t value, int bytes) {
                for (int i = 0; i < bytes; ++i) body += char(value >> (8 * i));
            };
            const auto path = [&](const std::string& value) {
                number(value.size() + 1, 2);
                body += value, body += '\0';
            };
            path(map);
            for (const Named& placement : placements) {
                number(placement.number, 8), path(placement.type);
                body += char(placement.kind), body += char(placement.groups.size());
                for (const Group& group : placement.groups) {
                    body += char(group.first), number(group.second.size(), 4);
                    body.append(group.second.begin(), group.second.end());
                }
            }
            std::string head = "P4AI";
            for (uint32_t value : {uint32_t(1), uint32_t(16 + body.size()), uint32_t(placements.size())})
                for (int i = 0; i < 4; ++i) head += char(value >> (8 * i));
            return head + body;
        };
        // One drop with one condition: the two bytes that are values of the game's enums are at 32 and 41.
        const auto drops = [](uint8_t condition, uint8_t demo_flag) {
            std::vector<uint8_t> bytes(60);
            bytes[0] = 1, bytes[28] = 1, bytes[32] = condition, bytes[41] = demo_flag;
            return bytes;
        };
        const std::vector<uint8_t> patrols = {1, 0, 0, 0};
        const char* const made[] = {"mind.one", "mind.two", "mind.else", "mind.drop", "mind.wrongmap", "mind.damaged", "mind.cut", "mind.shape", "mind.larva", "mind.twice", "mind.nofile", "mind.key"};
        const auto with_file = [&](const std::string& id, const std::string& map, const std::string& bytes) {
            plain(id, "1.0.0", R"(, "maps": [{"map": ")" + map + R"(", "ai": "maps/den.ai"}])");
            if (!bytes.empty()) disk.files[mods_at + id + "/maps/den.ai"] = bytes;
        };
        const std::string larva_file = file_of("/G/Den_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(250)}, {tsuchi::ai::kPatrolEnabled, patrols}}}});
        with_file("mind.one", "/G/Den_P", file_of("/G/Den_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(250)}, {tsuchi::ai::kPatrolEnabled, patrols}}},
                                                                {0x7009, "/G/Bee.Bee_C", 1, {{tsuchi::ai::kTerritory, ai_territory(99)}}},
                                                                {0x7004, "/G/Baby.Baby_C", 1, {{tsuchi::ai::kTerritory, ai_territory(5)}}}}));
        with_file("mind.two", "/G/Den_P", file_of("/g/den_p", {{0x7001, "/g/baby.baby_c", 3, {{tsuchi::ai::kTerritory, ai_territory(400)}}}}));
        with_file("mind.else", "/G/Far_P", file_of("/G/Far_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(1)}}}}));
        with_file("mind.drop", "/G/Den_P", file_of("/G/Den_P", {{0x7002, "/G/Bee.Bee_C", 1, {{tsuchi::ai::kDrops, drops(9, 5)}}},
                                                                 {0x7003, "/G/Bee.Bee_C", 1, {{tsuchi::ai::kDrops, drops(2, 5)}, {tsuchi::ai::kTerritory, ai_territory(77)}}}}));
        with_file("mind.wrongmap", "/G/Den_P", file_of("/G/Far_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(1)}}}}));
        std::string damaged = larva_file;
        damaged[1] = 'X';
        with_file("mind.damaged", "/G/Den_P", damaged);
        with_file("mind.cut", "/G/Den_P", larva_file.substr(0, larva_file.size() - 1));
        with_file("mind.shape", "/G/Den_P", file_of("/G/Den_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, std::vector<uint8_t>(16)}}}}));
        with_file("mind.larva", "/G/Den_P", file_of("/G/Den_P", {{0x7001, "/G/Bee.Bee_C", 1, {{tsuchi::ai::kPatrolEnabled, patrols}}}})); // a group only a larva's part has
        with_file("mind.twice", "/G/Den_P", file_of("/G/Den_P", {{0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(1)}}},
                                                                  {0x7001, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kPatrolEnabled, patrols}}}}));
        with_file("mind.nofile", "/G/Den_P", "");
        plain("mind.key", "1.0.0", R"(, "maps": [{"map": "/G/Den_P", "ai": 5}])");
        const auto told = [&](const std::string& part) { return logged.lines.find(part) != std::string::npos; };

        // Before the framework has a context of its own to wrap the parts' start with, a mod that brings values
        // cannot have them set, and is blocked for it.
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(problem("mind.key") == "mod.json: \"maps\": \"ai\" is the path of a file in the mod's folder");
        tsuchi::start_mods();
        CHECK(problem("mind.one") == "mod.json: \"maps\": maps/den.ai: the game's AI parts are not as the framework knows them, so their values cannot be set");
        CHECK(tsuchi::map_ai_counts().tables == 0 && tsuchi::map_ai_counts().files == 0);
        tsuchi::forget_mods();
        tsuchi::map_ai_start(tsuchi::mod_create("tsuchi.self.ai"));
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        logged.lines.clear();
        tsuchi::start_mods();
        CHECK(problem("mind.one") == "<running>" && problem("mind.two") == "<running>" && problem("mind.else") == "<running>" && problem("mind.drop") == "<running>");
        CHECK(tsuchi::map_ai_counts().tables == 3 && tsuchi::map_ai_counts().files == 4 && tsuchi::map_ai_counts().placements == 7 && tsuchi::map_parts() == 4);
        CHECK(problem("mind.wrongmap") == "mod.json: \"maps\": maps/den.ai is made for another map, /G/Far_P");
        CHECK(problem("mind.damaged") == "mod.json: \"maps\": maps/den.ai is no AI file, or is damaged" && problem("mind.cut") == "mod.json: \"maps\": maps/den.ai is no AI file, or is damaged");
        CHECK(problem("mind.shape") == "mod.json: \"maps\": maps/den.ai has a group of values that is not shaped as the game writes it" && problem("mind.larva") == problem("mind.shape"));
        CHECK(problem("mind.twice") == "mod.json: \"maps\": maps/den.ai names a placement, or a group of one, twice");
        CHECK(problem("mind.nofile") == "mod.json: \"maps\": maps/den.ai: the mod has no such file");

        // The named creature starts: its values are in before the game's start runs. One group has the bytes of
        // the mod started last, whose spelling of the class differs by case, and the group the other mod alone
        // changes is in too. What the part wrote and what it was given to read are given back to the engine.
        const tsuchi::AiCounts before = tsuchi::map_ai_counts();
        const int allocated = fake::engine().allocated, freed = fake::engine().freed;
        const uint64_t den = level_in(u"/G/Den_P", u"Den_P");
        const uint64_t larva = creature(den, u"Larva_1", baby, 0x7001, 3);
        CHECK(starts(larva) == "write read start:400/1 " && minds.calls_good && minds.set[larva].size() == 166);
        CHECK(fake::engine().allocated - allocated == 3 && fake::engine().freed - freed == 3);
        CHECK(told("[mind.one] maps: AI values of placement 0x0000000000007001 in /G/Den_P are set by mind.two, not by this mod\n") && !told("[mind.two]"));
        // And on no other: a creature no file names, one no level placed, and one of the same number in another map.
        CHECK(starts(creature(den, u"Larva_2", baby, 0x7005, 3)) == "start:100/0 " && starts(creature(den, u"Larva_3", baby, ~uint64_t(0), 3)) == "start:100/0 ");
        CHECK(starts(creature(level_in(u"/G/Elsewhere_P", u"Elsewhere_P"), u"Larva_1", baby, 0x7001, 3)) == "start:100/0 ");
        // A creature of another class than the file says, and one with another kind of AI part, are left alone
        // and told. So is one whose drops hold a value the game has none of; one whose drops are the game's gets them.
        logged.lines.clear();
        CHECK(starts(creature(den, u"Ant_1", ant, 0x7009, 1)) == "start:100 " && starts(creature(den, u"Larva_4", baby, 0x7004, 3)) == "start:100/0 ");
        CHECK(told("[mind.one] maps: the AI values for placement 0x0000000000007009 in /G/Den_P were not set: the creature there is of another class than the file says\n") &&
              told("[mind.one] maps: the AI values for placement 0x0000000000007004 in /G/Den_P were not set: the creature there has another kind of AI part than the file says\n"));
        const uint64_t dropper = creature(den, u"Bee_3", bee, 0x7003, 1);
        CHECK(starts(creature(den, u"Bee_2", bee, 0x7002, 1)) == "start:100 " && starts(dropper) == "write read start:77 " && minds.set[dropper].size() == 158 + 56 && minds.set[dropper][20] == 1);
        CHECK(told("[mind.drop] maps: the AI values for placement 0x0000000000007002 in /G/Den_P were not set: its drops hold a value that is none of the game's\n"));
        // A part whose own values are not shaped as the framework knows them keeps them, and so does one that
        // does not write them with the game's own serializer.
        const uint64_t odd = creature(den, u"Larva_1", baby, 0x7001, 3);
        minds.set[odd] = {1, 2, 3};
        CHECK(starts(odd) == "write start:? " && minds.set[odd].size() == 3);
        CHECK(told("[mind.two] maps: the AI values for placement 0x0000000000007001 in /G/Den_P were not set: the creature's own values are not shaped as the framework knows them\n"));
        fake::put(main + start.tables[2] + write_set.offset, main + 0x1234);
        CHECK(starts(creature(den, u"Larva_1", baby, 0x7001, 3)) == "start:100/0 ");
        CHECK(told("[mind.one] maps: the AI values for placement 0x0000000000007001 in /G/Den_P were not set: the creature's AI part does not write its values the game's own way\n"));
        fake::put(main + start.tables[2] + write_set.offset, main + write_set.stock[1]);
        CHECK(fake::engine().allocated - allocated == fake::engine().freed - freed && minds.calls_good);
        CHECK(tsuchi::map_ai_counts().set == before.set + 2 && tsuchi::map_ai_counts().refused == before.refused + 6);

        // The map restored from a save: the game makes the creature again, with the values the save holds, and
        // it gets the mods' again.
        const uint64_t den_again = level_in(u"/G/Den_P", u"Den_P");
        CHECK(starts(creature(den_again, u"Larva_1", baby, 0x7001, 3)) == "write read start:400/1 ");
        // Another map has its own mod's values.
        CHECK(starts(creature(level_in(u"/G/Far_P", u"Far_P"), u"Larva_1", baby, 0x7001, 3)) == "write read start:1/0 ");
        // A mod that stops sets nothing more: the next mod's bytes are used, and with no mod left, the game's own.
        tsuchi::mod_stop(tsuchi::find_mod("mind.two")->context);
        CHECK(starts(creature(den_again, u"Larva_1", baby, 0x7001, 3)) == "write read start:250/1 " && tsuchi::map_ai_counts().files == 3 && tsuchi::map_ai_counts().placements == 6);
        tsuchi::forget_mods();
        CHECK(starts(creature(den_again, u"Larva_1", baby, 0x7001, 3)) == "start:100/0 " && tsuchi::map_ai_counts().files == 0 && tsuchi::map_ai_counts().placements == 0);
        CHECK(tsuchi::map_ai_counts().set == before.set + 5 && fake::engine().allocated - allocated == fake::engine().freed - freed && minds.calls_good);

        #include "ai_checks.h"

        for (uint64_t each : creatures) fake::destroy(each);
        t::fn::ai_values_write = nullptr, t::fn::ai_values_read = nullptr;
        for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/maps/den.ai");
        logged.lines.clear();
        fake::put(w.framework + t::game_framework::clock, old_ai_clock);
        fake::put<uint8_t>(running.context + t::game_context::rule, old_ai_rule);
        t::fn::story_flags = old_ai_flags;
    }
    // And left running for what comes after.
    plain("app.one", "2.0.0", R"(, "requires": {"base.lib": "1.2.0"})");
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 22);
    tsuchi::start_mods();
    logged.lines.clear();
    disk.log.clear();
}

// ---- Dialogs -----------------------------------------------------------------------------

// The operations of a function the game holds, in the game's order.
struct TalkOperations {
    void (*end)(void*);
    void (*end_and_free)(void*);
    void* (*clone)(const void*);
    void (*copy)(const void*, void*);
    void (*end_again)(void*);
    void (*end_and_free_again)(void*);
    void (*call)(const void*, const uint64_t*, const uint32_t*);
};
const TalkOperations& operations_of(uint64_t function) { return **reinterpret_cast<const TalkOperations* const*>(function); }
// Where a request keeps where its function is: after the room for a small one.
const uint64_t kTalkFunction = t::dialog_request::answerer + 0x20;

struct Talk {
    std::string log;
    std::vector<uint64_t> posted;     // requests handed to the game's queue
    std::vector<std::string> answers; // what was heard
    enum { kCopies, kClones, kLeaves, kFails } transfer = kCopies;
    uint64_t stock_text = 0, widget = 0, other_widget = 0, block = 0;
    bool two_buttons = true, other_first = false, nested = false, ask_again = false;
} talk;
void game_dialog_request(void* request) {
    std::memset(request, 0xEE, t::dialog_request::size); // the game fills in what it wants; the rest is not zero
    fake::put<uint64_t>(reinterpret_cast<uint64_t>(request) + kTalkFunction, 0);
    talk.log += "made ";
}
void game_dialog_dispose(void* request) {
    const auto at = reinterpret_cast<uint64_t>(request);
    if (const uint64_t held = fake::get<uint64_t>(at + kTalkFunction)) {
        if (held == at + t::dialog_request::answerer) operations_of(held).end_again(reinterpret_cast<void*>(held));
        else operations_of(held).end_and_free_again(reinterpret_cast<void*>(held));
        fake::put<uint64_t>(at + kTalkFunction, 0);
    }
    talk.log += "disposed ";
}
// The game moves a function: a copy goes where it is wanted, in place or in memory of its own, and the one handed over is ended.
void game_function_transfer(void* source, void* destination) {
    void* held = nullptr;
    std::memcpy(&held, static_cast<uint8_t*>(source) + 0x20, 8);
    if (talk.transfer == Talk::kFails || !held) return;
    const auto to = reinterpret_cast<uint64_t>(destination);
    const TalkOperations& operations = operations_of(reinterpret_cast<uint64_t>(held));
    if (talk.transfer == Talk::kClones) {
        fake::put(to + 0x20, reinterpret_cast<uint64_t>(operations.clone(held)));
    } else {
        operations.copy(held, destination);
        fake::put(to + 0x20, to);
    }
    if (talk.transfer == Talk::kLeaves) return; // the one handed over is left for whoever handed it over to end
    operations.end(held);
    std::memset(static_cast<uint8_t*>(source) + 0x20, 0, 8);
}
void game_push_dialog(void*, void* request) { talk.posted.push_back(reinterpret_cast<uint64_t>(request)); }
// A text as the game was handed it: one the test's engine made from a string, or the game's own.
std::string talk_text(uint64_t text) {
    const uint64_t index = fake::get<uint64_t>(text);
    if (!index) return "<stock>";
    const uint64_t counter = fake::get<uint64_t>(text + t::text_value::counter); // whoever shows it keeps a share of its own
    fake::put<int32_t>(counter + t::shared_count::holders, fake::get<int32_t>(counter + t::shared_count::holders) + 1);
    std::string narrow;
    for (char16_t unit : announced.made[index - 1]) narrow += char(unit);
    return narrow;
}
uint64_t game_dialog_buttons(uint64_t widget, uint64_t kind, uint64_t text, uint64_t first) {
    talk.log += std::string("buttons") + (widget == talk.widget ? "" : "-other") + ":" + std::to_string(uint32_t(kind)) + ":" + talk_text(text) + (first == 0x1111 ? " " : "? ");
    return 0;
}
void game_rich_text_set(void* block, void* text) {
    talk.log += std::string("body") + (reinterpret_cast<uint64_t>(block) == talk.block ? "" : "-elsewhere") + ":" + talk_text(reinterpret_cast<uint64_t>(text)) + " ";
}
uint64_t game_dialog_setup(uint64_t widget, uint64_t kind, uint64_t layout, uint64_t) {
    const auto buttons = [&](uint64_t on) { thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_buttons_call), on, kind, talk.stock_text, 0x1111); };
    talk.log += "setup:" + std::to_string(uint32_t(kind)) + "/" + std::to_string(int32_t(uint32_t(layout))) + " ";
    if (std::exchange(talk.other_first, false)) buttons(talk.other_widget);
    if (std::exchange(talk.nested, false)) // the game sets up one of its own in the middle of this one
        thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.other_widget, 5, 2, 0);
    if (talk.two_buttons) buttons(widget);
    return 0;
}
// The game takes a request from its line and shows it: the widget is set up from it, the game keeps a copy of the function
// to answer with, and the request is ended.
uint64_t game_shows(uint64_t request, int32_t layout = 0) {
    const uint8_t kind = fake::get<uint8_t>(request + t::dialog_request::kind);
    talk.two_buttons = kind != t::constant::dialog_message;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, kind, uint64_t(uint32_t(layout)), request);
    const uint64_t held = fake::get<uint64_t>(request + kTalkFunction);
    const uint64_t kept = held ? reinterpret_cast<uint64_t>(operations_of(held).clone(reinterpret_cast<void*>(held))) : 0;
    game_dialog_dispose(reinterpret_cast<void*>(request));
    return kept;
}
void game_lets_go(uint64_t kept) { operations_of(kept).end_and_free(reinterpret_cast<void*>(kept)); }
// The player's answer: the name of what they did, none when the dialog was closed from code, and the number of the button.
void game_answers(uint64_t kept, uint64_t name, uint32_t button) { operations_of(kept).call(reinterpret_cast<void*>(kept), &name, &button); }
void on_answered(void*, const TSUCHI_DialogAnswered* event) {
    talk.answers.push_back(std::to_string(event->dialog.id) + (event->accepted ? ":yes" : ":no"));
    TSUCHI_Dialog another{};
    if (std::exchange(talk.ask_again, false)) tsuchi::host.dialog->confirm(mod, "And again?", &another);
}

void conversing() {
    const TSUCHI_DialogService& ask = *tsuchi::host.dialog;
    const uint64_t main = fake::main_image();
    namespace r = t::dialog_request;
    const uint32_t accept = t::constant::dialog_accept;
    TSUCHI_Dialog id{};
    uint64_t hook = 0;
    void* unused = nullptr;
    const auto log_is = [](const char* expected) {
        const bool same = talk.log == expected;
        if (!same) std::printf("  log: [%s]\n", talk.log.c_str());
        talk.log.clear();
        return same;
    };
    const auto heard = [](std::initializer_list<const char*> expected) {
        const bool same = talk.answers == std::vector<std::string>(expected.begin(), expected.end());
        if (!same)
            for (const std::string& answer : talk.answers) std::printf("  heard: %s\n", answer.c_str());
        talk.answers.clear();
        return same;
    };
    const auto posted = [] { // the request handed in last, taken from the line
        const uint64_t last = talk.posted.empty() ? 0 : talk.posted.back();
        if (last) talk.posted.pop_back();
        return last;
    };
    for (auto function : {reinterpret_cast<uint64_t>(&game_push_dialog)}) fake::code().push_back(function);
    talk.widget = fake::object(u"Dialog", w.component_class, 0x60), talk.other_widget = fake::object(u"Dialog", w.component_class, 0x60);
    talk.block = fake::object(u"Body", w.component_class, 0x40), talk.stock_text = fake::memory(t::text_value::size);
    fake::put(talk.widget + t::dialog_widget::text, to_weak_value(talk.block));
    TSUCHI_Subscription listening{};
    CHECK(ask.on_answered(mod, on_answered, nullptr, &listening) == TSUCHI_OK);
    TSUCHI_Mod* self = tsuchi::mod_create("tsuchi.self.dialog"), *game = tsuchi::mod_create("the.dialog");
    TSUCHI_Mod* poor = tsuchi::mod_create("actor.guest3"), *rival = tsuchi::mod_create("actor.guest4");
    const uint32_t owned = tsuchi::owned_count(mod), self_owned = tsuchi::owned_count(self);
    const auto patch = [&](unsigned site) {
        const t::CodeSite& info = t::code_sites[site];
        fake::put<uint32_t>(main + info.address, 0x94000000u | uint32_t((info.entry - info.address) >> 2)); // as the patch leaves it
    };
    const auto start = [&] { tsuchi::dialogs_start(self, reinterpret_cast<void*>(&tsuchi_dialog_on_setup)); };

    // Everything the game needs is there but the framework's own two calls: first neither, then one, then both.
    const uint64_t queue = fake::memory(0x20), queue_table = fake::memory(0x20);
    fake::put(queue, queue_table), fake::put(queue_table + t::request_queue_table::push, &game_push_dialog);
    fake::put(running.context + t::game_context::dialogs, queue);
    t::fn::dialog_request = game_dialog_request, t::fn::dialog_request_dispose = game_dialog_dispose, t::fn::dialog_function_transfer = game_function_transfer;
    t::fn::rich_text_set = game_rich_text_set;
    start();
    CHECK(ask.confirm(mod, "Ready?", &id) == TSUCHI_NOT_READY && tsuchi::owned_count(self) == self_owned);
    patch(t::code_site::dialog_setup_call);
    CHECK(tsuchi::hook_code_site(game, t::code_site::dialog_setup_call, reinterpret_cast<void*>(&game_dialog_setup), &hook, &unused) == TSUCHI_OK);
    start();
    CHECK(ask.confirm(mod, "Ready?", &id) == TSUCHI_NOT_READY && ask.message(mod, "Saved.", &id) == TSUCHI_NOT_READY && tsuchi::owned_count(self) == self_owned); // the one it took is given back
    talk.two_buttons = false;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, 0);
    CHECK(log_is("setup:5/2 ")); // the game's own goes on as ever
    patch(t::code_site::dialog_buttons_call);
    CHECK(tsuchi::hook_code_site(game, t::code_site::dialog_buttons_call, reinterpret_cast<void*>(&game_dialog_buttons), &hook, &unused) == TSUCHI_OK);
    start();
    start(); // and once more changes nothing
    CHECK(tsuchi::owned_count(self) == self_owned + 2);

    // What keeps a dialog from being asked for.
    CHECK(ask.confirm(mod, "\xff", &id) == TSUCHI_INVALID && ask.message(mod, "\xff", &id) == TSUCHI_INVALID && log_is(""));
    const auto refused = [&](TSUCHI_Result with, const char* why) {
        const bool right = ask.confirm(mod, "Ready?", &id) == with && ask.message(mod, "Saved.", &id) == with && talk.posted.empty() && tsuchi::dialogs_waiting() == 0 &&
                           tsuchi::owned_count(mod) == owned && talk.log.empty();
        if (!right) std::printf("  not refused as expected: %s [%s]\n", why, talk.log.c_str());
        talk.log.clear();
        return right;
    };
    fake::put<uint64_t>(running.context + t::game_context::dialogs, 0);
    CHECK(refused(TSUCHI_NOT_READY, "no line to hand a request in"));
    const size_t labels_before = blocks_out.size();
    CHECK(ask.ask(mod, "Ready?", "Install", "Cancel", &id) == TSUCHI_NOT_READY &&
          blocks_out.size() == labels_before && talk.posted.empty());
    fake::put(running.context + t::game_context::dialogs, queue);
    fake::put<uint64_t>(queue_table + t::request_queue_table::push, 0x1234);
    CHECK(refused(TSUCHI_NOT_READY, "a line without the function for it"));
    fake::put(queue_table + t::request_queue_table::push, &game_push_dialog);
    const uint64_t old_context = fake::get<uint64_t>(main + t::data::game_context);
    fake::put<uint64_t>(main + t::data::game_context, 0);
    CHECK(refused(TSUCHI_NOT_READY, "no game context"));
    fake::put(main + t::data::game_context, old_context);
    t::fn::dialog_request = nullptr;
    CHECK(refused(TSUCHI_NOT_READY, "no maker of requests"));
    t::fn::dialog_request = game_dialog_request, t::fn::dialog_request_dispose = nullptr;
    CHECK(refused(TSUCHI_NOT_READY, "no ender of requests"));
    t::fn::dialog_request_dispose = game_dialog_dispose, t::fn::dialog_function_transfer = nullptr;
    CHECK(refused(TSUCHI_NOT_READY, "no mover of functions"));
    t::fn::dialog_function_transfer = game_function_transfer;
    const auto make_text = t::fn::text_from_string;
    t::fn::text_from_string = nullptr;
    CHECK(refused(TSUCHI_NOT_READY, "no maker of texts"));
    t::fn::text_from_string = make_text, t::fn::rich_text_set = nullptr; // a message's text could not be put on it: a question can still be asked
    CHECK(ask.message(mod, "Saved.", &id) == TSUCHI_NOT_READY && talk.posted.empty() && tsuchi::dialogs_waiting() == 0 && log_is(""));
    t::fn::rich_text_set = game_rich_text_set;
    const size_t out = blocks_out.size();
    refused_size = 14; // no memory for the text of either
    CHECK(refused(TSUCHI_NO_MEMORY, "no memory for the text") && blocks_out.size() == out);
    refused_size = 0;
    watched_fail_after = 1; // allow the text, then refuse the first dialog-list allocation
    CHECK(refused(TSUCHI_NO_MEMORY, "no memory for the list") && blocks_out.size() == out);
    watched_fail_after = -1;
    const int freed = fake::engine().freed;
    talk.transfer = Talk::kFails; // the game does not take the function
    CHECK(ask.confirm(mod, "Ready?", &id) == TSUCHI_NOT_READY && log_is("made disposed ") && talk.posted.empty() && tsuchi::dialogs_waiting() == 0 &&
          tsuchi::owned_count(mod) == owned && fake::engine().freed == freed + 1);
    talk.transfer = Talk::kCopies;
    const size_t settled = blocks_out.size(); // with the list made
    fake::engine().out_of_memory = true;
    CHECK(refused(TSUCHI_NO_MEMORY, "no memory for the request") && blocks_out.size() == settled);
    fake::engine().out_of_memory = false;
    refused_multiple = 40; // no room to note whose it is
    CHECK(ask.confirm(poor, "Ready?", &id) == TSUCHI_NO_MEMORY && talk.posted.empty() && tsuchi::dialogs_waiting() == 0 && blocks_out.size() == settled && log_is(""));
    refused_multiple = 0;

    // A question: the game's own request for its dialog with two buttons, with a function of the framework's to answer.
    const int destroyed = words.objects_destroyed;
    id.id = 77;
    CHECK(ask.confirm(mod, "Ready \xC3\xA9?", &id) == TSUCHI_OK && id.id == 1 && log_is("made ") && talk.posted.size() == 1 && tsuchi::dialogs_waiting() == 1 &&
          tsuchi::owned_count(mod) == owned + 1);
    uint64_t request = posted();
    CHECK(fake::get<uint8_t>(request + r::kind) == t::constant::dialog_question && fake::get<uint8_t>(request + r::shape) == t::constant::dialog_shape &&
          fake::get<uint8_t>(request + r::mode) == t::constant::dialog_mode && fake::get<int32_t>(request + r::player) == -1 &&
          fake::get<uint64_t>(request + kTalkFunction) == request + r::answerer && fake::get<uint8_t>(request + r::kind + 1) == 0xEE);
    tsuchi::frame(0.03f);
    CHECK(heard({}) && tsuchi::dialogs_waiting() == 1); // nothing yet
    // Shown: the text on it is the mod's, and the engine's text is let go of once the game has its own share.
    const int freed_shown = fake::engine().freed;
    uint64_t kept = game_shows(request, 3);
    CHECK(fake::engine().freed == freed_shown); // the function was in the request itself: ending it frees nothing
    CHECK(log_is("setup:9/3 buttons:9:Ready \xE9? disposed ") && announced.made.back() == u"Ready \u00E9?" &&
          fake::get<int32_t>(announced.counter + t::shared_count::holders) == 1 && words.objects_destroyed == destroyed);
    tsuchi::frame(0.03f);
    CHECK(heard({}) && tsuchi::dialogs_waiting() == 1); // shown, and not answered
    // Accepted. It is told at the next frame, and once only, and with that the dialog is over and forgotten.
    game_answers(kept, 0x55, accept), game_answers(kept, 0x55, accept + 1);
    CHECK(heard({}));
    tsuchi::frame(0.03f);
    CHECK(heard({"1:yes"}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled);
    // The game keeps its copy of the function for as long as it likes. What it does with it afterward changes nothing.
    game_answers(kept, 0x55, accept + 1);
    const uint64_t late = reinterpret_cast<uint64_t>(operations_of(kept).clone(reinterpret_cast<void*>(kept)));
    const int freed_late = fake::engine().freed;
    game_lets_go(late), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(heard({}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled && fake::engine().freed == freed_late + 2);

    // The other button, a dialog closed from code, and one answered and let go of within a frame.
    const auto asked = [&](bool question, const char* text) {
        TSUCHI_Dialog number{};
        const TSUCHI_Result result = question ? ask.confirm(mod, text, &number) : ask.message(mod, text, &number);
        if (result != TSUCHI_OK) std::printf("  not asked: %d\n", int(result));
        talk.log.clear();
        return number.id;
    };
    const auto answered = [&](bool question, uint64_t name, uint32_t button) {
        const uint64_t number = asked(question, "Well?");
        const uint64_t shown = game_shows(posted());
        game_answers(shown, name, button), game_lets_go(shown);
        tsuchi::frame(0.03f);
        const bool gone = tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled;
        if (!gone) std::printf("  not forgotten\n");
        talk.log.clear();
        return gone && talk.answers.size() == 1 && talk.answers[0] == std::to_string(number) + ":yes" ? 1 : gone && talk.answers.size() == 1 ? 0 : -1;
    };
    CHECK(answered(true, 0x55, accept) == 1 && heard({"2:yes"}) && answered(true, 0x55, accept + 1) == 0 && heard({"3:no"}));
    CHECK(answered(true, 0x55, accept - 1) == 0 && heard({"4:no"}) && answered(true, 0, accept) == 0 && heard({"5:no"}));
    // A message: the game's dialog with one button. Its text goes on once the game has set it up; any button closes it.
    id.id = asked(false, "Saved.");
    request = posted();
    CHECK(id.id == 6 && fake::get<uint8_t>(request + r::kind) == t::constant::dialog_message && fake::get<uint8_t>(request + r::shape) == t::constant::dialog_shape &&
          fake::get<int32_t>(request + r::player) == -1);
    kept = game_shows(request);
    CHECK(log_is("setup:43/0 body:Saved. disposed ") && fake::get<int32_t>(announced.counter + t::shared_count::holders) == 1 && words.objects_destroyed == destroyed);
    game_answers(kept, 0x55, accept + 3), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(heard({"6:yes"}) && tsuchi::dialogs_waiting() == 0);
    CHECK(answered(false, 0, accept) == 0 && heard({"7:no"}));
    // A message whose widget has lost the block its text goes in: the game's text stays.
    id.id = asked(false, "Saved.");
    fake::put<uint64_t>(talk.widget + t::dialog_widget::text, to_weak_value(0));
    kept = game_shows(posted());
    CHECK(log_is("setup:43/0 disposed "));
    fake::put(talk.widget + t::dialog_widget::text, to_weak_value(talk.block));
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(heard({"8:yes"}));
    // A question is not given a message's text, nor a message a question's, whatever the game calls for them.
    id.id = asked(false, "Saved.");
    request = posted();
    talk.two_buttons = true;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 43, 0, request);
    CHECK(log_is("setup:43/0 buttons:43:Saved. body:Saved. "));
    game_dialog_dispose(reinterpret_cast<void*>(request)); // and the game drops it unanswered: that is an answer of no
    tsuchi::frame(0.03f);
    CHECK(log_is("disposed ") && heard({"9:no"}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled);

    // The game's own dialogs keep their texts: a request with no function, one with the game's, and none at all.
    const uint64_t stock_request = fake::memory(r::size), stock_function = fake::memory(0x20);
    talk.two_buttons = true;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, stock_request);
    CHECK(log_is("setup:5/2 buttons:5:<stock> "));
    fake::put(stock_request + kTalkFunction, stock_function), fake::put(stock_function, fake::memory(0x40));
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, stock_request);
    CHECK(log_is("setup:5/2 buttons:5:<stock> "));
    fake::put<uint64_t>(stock_request + kTalkFunction, 0x40); // not memory
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, stock_request);
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, 0);
    CHECK(log_is("setup:5/2 buttons:5:<stock> setup:5/2 buttons:5:<stock> "));
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_buttons_call), talk.widget, 5, talk.stock_text, 0x1111); // outside any set-up
    CHECK(log_is("buttons:5:<stock> "));
    // Within the set-up of a mod's: only its own widget's text is the mod's, and only once.
    id.id = asked(true, "Mine?");
    request = posted();
    fake::put(stock_request + kTalkFunction, stock_function), fake::put(stock_function + 8, id); // the game's own function, which happens to hold that number
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 5, 2, stock_request);
    CHECK(log_is("setup:5/2 buttons:5:<stock> "));
    talk.other_first = talk.nested = true;
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_setup_call), talk.widget, 9, 0, request);
    CHECK(log_is("setup:9/0 buttons-other:9:<stock> setup:5/2 buttons-other:5:<stock> buttons:9:Mine? "));
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::dialog_buttons_call), talk.widget, 9, talk.stock_text, 0x1111); // not after it
    CHECK(log_is("buttons:9:<stock> "));
    // The game copies the function about, in place and in memory of its own; the dialog lasts as long as any copy.
    const uint64_t in_place = fake::memory(0x30);
    const uint64_t first = fake::get<uint64_t>(request + kTalkFunction);
    operations_of(first).copy(reinterpret_cast<void*>(first), reinterpret_cast<void*>(in_place));
    kept = reinterpret_cast<uint64_t>(operations_of(in_place).clone(reinterpret_cast<void*>(in_place)));
    game_dialog_dispose(reinterpret_cast<void*>(request));
    operations_of(in_place).end(reinterpret_cast<void*>(in_place));
    tsuchi::frame(0.03f);
    CHECK(heard({}) && tsuchi::dialogs_waiting() == 1);
    fake::engine().out_of_memory = true;
    CHECK(operations_of(kept).clone(reinterpret_cast<void*>(kept)) == nullptr); // no memory for one more copy
    fake::engine().out_of_memory = false;
    const int freed_before = fake::engine().freed;
    game_lets_go(kept); // the last goes with no answer
    CHECK(fake::engine().freed == freed_before + 1);
    tsuchi::frame(0.03f);
    CHECK(heard({"10:no"}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled);
    // When the game moves the function into memory of its own instead, it is all the same.
    talk.transfer = Talk::kClones;
    id.id = asked(true, "Moved?");
    request = posted();
    CHECK(id.id == 11 && fake::get<uint64_t>(request + kTalkFunction) != request + r::answerer);
    const int freed_moved = fake::engine().freed;
    kept = game_shows(request);
    CHECK(log_is("setup:9/0 buttons:9:Moved? disposed ") && fake::engine().freed == freed_moved + 1); // ending the request frees the function's own memory
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(heard({"11:yes"}) && tsuchi::dialogs_waiting() == 0 && blocks_out.size() == settled);
    // And when it leaves the one handed over alone, that one is ended by the framework: the dialog still goes when the game's copies do.
    talk.transfer = Talk::kLeaves;
    id.id = asked(true, "Left?");
    talk.transfer = Talk::kCopies;
    kept = game_shows(posted());
    CHECK(id.id == 12 && log_is("setup:9/0 buttons:9:Left? disposed "));
    game_lets_go(kept); // with no answer: the game has none left, and that is told
    tsuchi::frame(0.03f);
    CHECK(heard({"12:no"}) && tsuchi::dialogs_waiting() == 0 && blocks_out.size() == settled);

    // Two at once: the game is handed one at a time, in the order they were asked for.
    const uint64_t one = asked(true, "One?"), two = asked(false, "Two.");
    CHECK(one == 13 && two == 14 && tsuchi::dialogs_waiting() == 2 && talk.posted.size() == 1);
    request = posted();
    CHECK(fake::get<uint8_t>(request + r::kind) == t::constant::dialog_question);
    tsuchi::frame(0.03f);
    CHECK(talk.posted.empty() && log_is("")); // the second waits
    kept = game_shows(request);
    game_answers(kept, 0x55, accept + 1), game_lets_go(kept);
    CHECK(log_is("setup:9/0 buttons:9:One? disposed ") && talk.posted.empty());
    tsuchi::frame(0.03f);
    CHECK(heard({"13:no"}) && log_is("made ") && talk.posted.size() == 1 && tsuchi::dialogs_waiting() == 1); // told, and the next handed over
    kept = game_shows(posted());
    CHECK(log_is("setup:43/0 body:Two. disposed "));
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    // Whoever hears of one may ask for another, which is handed over at once when none is ahead of it.
    talk.ask_again = true;
    tsuchi::frame(0.03f);
    CHECK(heard({"14:yes"}) && tsuchi::dialogs_waiting() == 1 && log_is("made ") && talk.posted.size() == 1);
    request = posted();
    // A mod that stops while its dialog waits its turn: it is never shown.
    CHECK(ask.message(poor, "Never.", &id) == TSUCHI_OK && id.id == 16 && tsuchi::dialogs_waiting() == 2 && talk.posted.empty() && log_is(""));
    tsuchi::mod_stop(poor);
    CHECK(tsuchi::dialogs_waiting() == 1);
    // When the game does not take the next in line, that is told as an answer of no.
    const uint64_t unlucky = asked(true, "Unlucky?");
    CHECK(unlucky == 17 && talk.posted.empty() && tsuchi::dialogs_waiting() == 2);
    kept = game_shows(request);
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    talk.transfer = Talk::kFails;
    tsuchi::frame(0.03f);
    talk.transfer = Talk::kCopies;
    CHECK(heard({"15:yes"}) && log_is("setup:9/0 buttons:9:And again? disposed made disposed ") && talk.posted.empty() && tsuchi::dialogs_waiting() == 1);
    tsuchi::frame(0.03f);
    CHECK(heard({"17:no"}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned && blocks_out.size() == settled);
    // The same when there is nowhere to hand it in by the time its turn comes. The one behind it is next, in its turn.
    const uint64_t early = asked(true, "Early?"), stranded = asked(true, "Stranded?"), patient = asked(false, "Patient.");
    kept = game_shows(posted());
    game_answers(kept, 0, accept), game_lets_go(kept);
    fake::put<uint64_t>(running.context + t::game_context::dialogs, 0);
    tsuchi::frame(0.03f);
    fake::put(running.context + t::game_context::dialogs, queue);
    CHECK(early == 18 && stranded == 19 && heard({"18:no"}) && log_is("setup:9/0 buttons:9:Early? disposed ") && talk.posted.empty() && tsuchi::dialogs_waiting() == 2);
    tsuchi::frame(0.03f);
    CHECK(heard({"19:no"}) && tsuchi::dialogs_waiting() == 1 && log_is("made ") && talk.posted.size() == 1);
    // Three, the first answered: the second is handed over, not the third.
    const uint64_t third = asked(true, "Third?"), fourth = asked(true, "Fourth?");
    kept = game_shows(posted());
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(patient == 20 && heard({"20:yes"}) && log_is("setup:43/0 body:Patient. disposed made ") && talk.posted.size() == 1);
    kept = game_shows(posted());
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(third == 21 && heard({"21:yes"}) && log_is("setup:9/0 buttons:9:Third? disposed made "));
    kept = game_shows(posted());
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(fourth == 22 && heard({"22:yes"}) && log_is("setup:9/0 buttons:9:Fourth? disposed ") && tsuchi::dialogs_waiting() == 0 && blocks_out.size() == settled);
    // A mod that stops while the game has its dialog: the game still shows it, and nobody is told the answer.
    CHECK(ask.message(rival, "Bye.", &id) == TSUCHI_OK && id.id == 23 && tsuchi::dialogs_waiting() == 1 && log_is("made "));
    const uint64_t rival_request = posted();
    tsuchi::mod_stop(rival);
    CHECK(tsuchi::dialogs_waiting() == 1);
    kept = game_shows(rival_request);
    CHECK(log_is("setup:43/0 body:Bye. disposed "));
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(heard({}) && tsuchi::dialogs_waiting() == 0 && blocks_out.size() == settled);
    // And one that stops listening hears no more, though its dialog is answered.
    CHECK(tsuchi::host.cancel(mod, listening) == TSUCHI_OK);
    id.id = asked(true, "Anyone?");
    kept = game_shows(posted());
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(id.id == 24 && log_is("setup:9/0 buttons:9:Anyone? disposed ") && heard({}) && tsuchi::dialogs_waiting() == 0 && tsuchi::owned_count(mod) == owned - 1 && blocks_out.size() == settled);
    // A question needs nothing a message's text is put on with.
    t::fn::rich_text_set = nullptr;
    CHECK(ask.confirm(mod, "Still?", &id) == TSUCHI_OK && id.id == 25);
    t::fn::rich_text_set = game_rich_text_set;
    kept = game_shows(posted());
    game_answers(kept, 0x55, accept), game_lets_go(kept);
    tsuchi::frame(0.03f);
    CHECK(log_is("made setup:9/0 buttons:9:Still? disposed ") && tsuchi::dialogs_waiting() == 0 && words.well_formed);
    // Closed from code while on the screen: one message that ends it goes to the game, however often close is
    // called, and the answer is told once.
    {
        TSUCHI_Dialog closing{};
        TSUCHI_Subscription hearing{};
        CHECK(ask.on_answered(mod, on_answered, nullptr, &hearing) == TSUCHI_OK);
        CHECK(ask.confirm(mod, "Close me?", &closing) == TSUCHI_OK && tsuchi::dialogs_waiting() == 1);
        const uint64_t shown = game_shows(posted());
        talk.log.clear();
        const uint64_t manager = fake::memory(0x400), old_manager = fake::get<uint64_t>(w.framework + t::game_framework::ui);
        fake::put(w.framework + t::game_framework::ui, manager);
        fake::put(manager + t::ui_manager::open_dialog, talk.widget);
        CHECK(ask.close(mod, closing) == TSUCHI_OK && talk.posted.size() == 1);
        CHECK(ask.close(mod, closing) == TSUCHI_OK && talk.posted.size() == 1);
        posted();
        game_answers(shown, 0, accept), game_lets_go(shown);
        tsuchi::frame(0.03f);
        CHECK(heard({(std::to_string(closing.id) + ":no").c_str()}) && tsuchi::dialogs_waiting() == 0);
        CHECK(ask.close(mod, closing) == TSUCHI_NOT_FOUND);
        fake::put(w.framework + t::game_framework::ui, old_manager);
        tsuchi::cancel(mod, hearing);
        talk.log.clear();
    }
}

int script(const char* path) {
    // Enough actors that a list of them outgrows the bindings' first buffer.
    // Check the host setup separately from the bindings' count and buffer retry.
    uint64_t before_pellets = 0;
    const TSUCHI_Result before_result = api.list(mod, {}, nullptr, 0, &before_pellets);
    CHECK(before_result == TSUCHI_CAPACITY && before_pellets > 0);
    std::vector<uint64_t> pellets;
    for (int i = 0; i < 70; ++i) pellets.push_back(actor(u"Pellet", w.actor_class, u"Pellet", 4, 1000 + uint64_t(i), component(float(i), 0, 0, 0)));
    uint64_t after_pellets = 0;
    const TSUCHI_Result after_result = api.list(mod, {}, nullptr, 0, &after_pellets);
    const auto live_pellets = std::count_if(pellets.begin(), pellets.end(), [](uint64_t value) { return live(value) && to_handle(value); });
    if (after_pellets != before_pellets + pellets.size() || live_pellets != 70) {
        // Keep failure evidence small: object count, name cursor, ring tail and liveness.
        std::fprintf(stderr, "pellet fixture: before=%llu after=%llu live=%lld objects=%d/%llu names=%u/%llu tail_returns_to_head=%d\n",
                     static_cast<unsigned long long>(before_pellets), static_cast<unsigned long long>(after_pellets),
                     static_cast<long long>(live_pellets), fake::get<int32_t>(fake::main_image() + t::data::object_count),
                     static_cast<unsigned long long>(t::constant::max_objects), fake::next_name(),
                     static_cast<unsigned long long>(t::constant::name_block_count) << t::constant::names_per_block_shift,
                     fake::get<uint64_t>(w.last_node + t::actor_node::next) == w.framework + t::game_framework::actor_list);
    }
    CHECK(after_result == TSUCHI_CAPACITY && live_pellets == 70 && after_pellets == before_pellets + pellets.size());
    if (failures) return failures;
    const bool javascript = std::string(path).size() >= 3 && std::string(path).substr(std::string(path).size()-3) == ".js";
    tsuchi_js::State* js = nullptr;
    JSContext* ctx = nullptr;
    lua_State* L = nullptr;
    if (javascript) {
        js = tsuchi_js::create(mod, &tsuchi::host);
        if (!js) return 1;
        ctx = tsuchi_js::context(js);
        tsuchi_js::set_error_handler([](const char* text) { std::fprintf(stderr,"%s\n",text); });
        tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));
        uint64_t in_play = 0; api.list(mod, {}, nullptr, 0, &in_play);
        JS_SetPropertyStr(ctx, global, "actors_in_play", JS_NewInt64(ctx, static_cast<int64_t>(in_play)));
        JS_SetPropertyStr(ctx, global, "framework_version", JS_NewInt32(ctx, TSUCHI_API_VERSION));
        JS_SetPropertyStr(ctx, global, "rock", tsuchi_js::push_handle(ctx, "actor", to_handle(w.rock)));
        JS_SetPropertyStr(ctx, global, "stone", tsuchi_js::push_handle(ctx, "actor", to_handle(w.stone)));
        JS_SetPropertyStr(ctx, global, "idol", tsuchi_js::push_handle(ctx, "actor", to_handle(idol)));
        JS_SetPropertyStr(ctx, global, "hound", tsuchi_js::push_handle(ctx, "actor", to_handle(w.hound)));
        JS_SetPropertyStr(ctx, global, "chief", tsuchi_js::push_handle(ctx, "actor", to_handle(w.chief)));
        JS_SetPropertyStr(ctx, global, "chief_pikmin", tsuchi_js::push_handle(ctx, "actor", to_handle(w.thrown_blue)));
        JS_SetPropertyStr(ctx, global, "sprig", tsuchi_js::push_handle(ctx, "actor", to_handle(w.sprig)));
        JS_SetPropertyStr(ctx, global, "loner", tsuchi_js::push_handle(ctx, "actor", to_handle(w.loner)));
        JS_SetPropertyStr(ctx, global, "beast", tsuchi_js::push_handle(ctx, "actor", to_handle(w.beast)));
        JS_SetPropertyStr(ctx, global, "corpse", tsuchi_js::push_handle(ctx, "actor", to_handle(w.corpse)));
        JS_SetPropertyStr(ctx, global, "idol_asset", tsuchi_js::push_handle(ctx, "object", to_handle(w.idol_asset)));
        JS_SetPropertyStr(ctx, global, "creature_table", tsuchi_js::push_handle(ctx, "object", to_handle(w.creature_table)));
        JS_SetPropertyStr(ctx, global, "red_material", tsuchi_js::push_handle(ctx, "object", to_handle(red_material)));
        JS_SetPropertyStr(ctx, global, "dancer_mesh", tsuchi_js::push_handle(ctx, "component", to_handle(dancer_mesh)));
        JS_SetPropertyStr(ctx, global, "walk_clip", tsuchi_js::push_handle(ctx, "object", to_handle(walk_clip)));
        std::ifstream file(path, std::ios::binary); std::ostringstream buffer; buffer << file.rdbuf();
        const std::string source = buffer.str();
        if (!tsuchi_js::evaluate(ctx, source.c_str(), source.size(), "objects.js", true)) { tsuchi_js::report(ctx); return 1; }
    } else {
    L = luaL_newstate();
    luaL_openlibs(L);
    tsuchi_lua::open(L, mod, &tsuchi::host);
    uint64_t in_play = 0;
    api.list(mod, {}, nullptr, 0, &in_play);
    lua_pushinteger(L, static_cast<lua_Integer>(in_play));
    lua_setglobal(L, "actors_in_play");
    lua_pushinteger(L, TSUCHI_API_VERSION);
    lua_setglobal(L, "framework_version");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.rock));
    lua_setglobal(L, "rock");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.stone));
    lua_setglobal(L, "stone");
    tsuchi_lua::push_handle(L, "actor", to_handle(idol));
    lua_setglobal(L, "idol");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.hound));
    lua_setglobal(L, "hound");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.chief));
    lua_setglobal(L, "chief");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.thrown_blue));
    lua_setglobal(L, "chief_pikmin");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.sprig));
    lua_setglobal(L, "sprig");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.loner));
    lua_setglobal(L, "loner");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.beast));
    lua_setglobal(L, "beast");
    tsuchi_lua::push_handle(L, "actor", to_handle(w.corpse));
    lua_setglobal(L, "corpse");
    tsuchi_lua::push_handle(L, "object", to_handle(w.idol_asset));
    lua_setglobal(L, "idol_asset");
    tsuchi_lua::push_handle(L, "object", to_handle(w.creature_table));
    lua_setglobal(L, "creature_table");
    tsuchi_lua::push_handle(L, "object", to_handle(red_material));
    lua_setglobal(L, "red_material");
    tsuchi_lua::push_handle(L, "component", to_handle(dancer_mesh));
    lua_setglobal(L, "dancer_mesh");
    tsuchi_lua::push_handle(L, "object", to_handle(walk_clip));
    lua_setglobal(L, "walk_clip");
    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "%s\n", luaL_tolstring(L, -1, nullptr));
        return 1;
    }
    }
    // The script is listening. An actor comes and goes, and the script says what it heard.
    const uint64_t visitor = tabled(table_a(), u"Visitor", u"Sprout", 600, 12);
    registers(visitor);
    tsuchi::frame(0.03f);
    ends_play(visitor);
    fake::destroy(visitor);
    tsuchi::frame(0.03f);
    // The Bulborb is hit, and then hit for all it has left.
    const uint64_t bulborb_life = fake::get<uint64_t>(w.bulborb + t::game_actor::life);
    game_hits(bulborb_life, w.captain, 0, 10), game_hits(bulborb_life, w.captain, 0, 100);
    // The player switches to their dog.
    people.steered[int32_t(t::constant::team_story)] = w.hound;
    // The day ends.
    in_stages({u"Story", u"DayEnd"});
    // A Pikmin is eaten, by the Bulborb.
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::pikmin_report_kill), w.red, w.bulborb, 1);
    // And another dies of a cause the framework has no name for.
    thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::pikmin_report_kill), w.red, 0, 40);
    // The game shows the script's question, which is accepted, and drops its message unshown.
    if (talk.posted.size() == 1) {
        const uint64_t shown = game_shows(talk.posted[0]);
        game_answers(shown, 0x55, t::constant::dialog_accept), game_lets_go(shown);
        tsuchi::frame(0.03f); // told, and the message handed over in its turn
        if (talk.posted.size() == 2) game_dialog_dispose(reinterpret_cast<void*>(talk.posted[1]));
    }
    // And the captain starts doing something else.
    now_doing(fake::get<uint64_t>(fake::get<uint64_t>(w.captain + t::game_actor::ai) + t::ai_component::actions), {u"Control", u"Whistle"});
    tsuchi::frame(0.03f);
    if (javascript) {
        const char* source = "verify();";
        if (!tsuchi_js::evaluate(ctx, source, std::strlen(source), "verify.js", false)) { tsuchi_js::report(ctx); return 1; }
        tsuchi::mod_stop(mod);
        tsuchi_js::destroy(js);
    } else {
        lua_getglobal(L, "verify");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) { std::fprintf(stderr,"%s\n",luaL_tolstring(L,-1,nullptr)); return 1; }
        lua_close(L);
    }
    return 0;
}


#include "records.h"
// The newest durable copy, without the generation envelope, for existing value checks.
std::string saved_record(const std::string& at) {
    const std::string second=at.substr(0,at.size()-5)+".2.json";
    const auto generation=[](const std::string& text) {
        return text.rfind("{\"counter\": ",0)==0 ? std::stoull(text.substr(12)) : 0ull;
    };
    std::string text=disk.files.count(at) ? disk.files[at] : "";
    if(disk.files.count(second) && generation(disk.files[second])>generation(text)) text=disk.files[second];
    if(text.rfind("{\"counter\": ",0)==0) text="{"+text.substr(text.find(", ")+2);
    return text;
}
void restart_records(const std::string& at) {
    tsuchi::forget_records();
    disk.files.erase(at.substr(0,at.size()-5)+".2.json");
}

#include "new_maps_checks.h"

#include "early_maps.inc"
#include "initial_streaming.inc"

} // namespace

#include "javascript_mods_checks.h"
#include "restart_checks.h"

// ---- A navigation file ---------------------------------------------------------------------

// A file of one map with one navmesh of two tiles is read, each tile's block is built as the navmesh takes it,
// and a file with anything wrong in it is refused whole.
void navigating() {
    namespace nav = tsuchi::nav;
    std::vector<uint8_t> bytes;
    const auto u16 = [&](uint32_t v) { bytes.push_back(uint8_t(v)), bytes.push_back(uint8_t(v >> 8)); };
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(v >> (8 * i)));
    };
    const auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = uint8_t(v >> (8 * i));
    };
    const auto text = [&](const std::string& s) {
        u16(uint32_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    };
    const auto run = [&](uint8_t first, uint32_t count) { // bytes that tell where they came from
        for (uint32_t i = 0; i < count; ++i) bytes.push_back(uint8_t(first + i));
    };
    // A stored tile: its reference and size, its ten counts, its header, its records, and its empty cache.
    const auto tile = [&](int32_t x, int32_t verts, int32_t polys, int32_t links, int32_t meshes, int32_t off_mesh, int32_t segments, int32_t clusters) {
        u32(0), u32(0), u32(0);
        const int32_t counts[10] = {verts, polys, links, meshes, 0, 0, 0, off_mesh, segments, clusters};
        for (int32_t count : counts) u32(uint32_t(count));
        const size_t header = bytes.size();
        bytes.resize(header + nav::kHeader, 0);
        put32(header, nav::kTileMagic), put32(header + 4, nav::kTileVersion), put32(header + 8, uint32_t(x)), put32(header + 12, 7), put32(header + 16, 1);
        put32(header + 24, uint32_t(polys)), put32(header + 28, uint32_t(verts)), put32(header + 32, uint32_t(links)), put32(header + 36, uint32_t(meshes));
        put32(header + 52, uint32_t(off_mesh)), put32(header + 100, uint32_t(clusters)), put32(header + 104, uint32_t(segments));
        for (uint32_t i = 60; i < 100; ++i) bytes[header + i] = uint8_t(0xA0 + i - 60); // the ten numbers
        put32(header + 108, 0x11223344), put32(header + 112, 0x55667788);              // where the segments' polygons and vertices begin
        run(0x10, 12u * verts), run(0x20, nav::kPoly * polys), run(0x30, nav::kDetailMeshStored * meshes);
        run(0x40, nav::kOffMeshStored * off_mesh), run(0x70, 4u * off_mesh), run(0x80, nav::kSegmentStored * segments), run(0xC0, nav::kClusterStored * clusters);
        run(0xE0, 2u * meshes);
        u32(0);
    };
    const char mark[] = "P4MNAVM1";
    bytes.insert(bytes.end(), mark, mark + 8);
    u32(1);
    text("/G/Cave_P");
    u32(1);
    text("Nav-Pikmin");
    const size_t size_at = bytes.size();
    u32(0);
    const size_t payload = bytes.size();
    u32(0), u32(nav::kVersion), u32(0), u32(2);
    run(0x01, 20), u32(64), u32(4096); // where the grid begins and how wide a tile is; the most tiles; the most polygons
    tile(3, 0, 0, 0, 0, 0, 0, 0);
    tile(4, 1, 1, 2, 1, 1, 1, 1);
    put32(size_at, uint32_t(bytes.size() - payload)), put32(payload + 8, uint32_t(bytes.size() - payload - 8));

    nav::Map maps[2];
    nav::Navmesh meshes[2];
    const auto read = [&](const std::vector<uint8_t>& from, uint32_t map_room = 2, uint32_t mesh_room = 2) {
        nav::File file{from.data(), uint32_t(from.size()), maps, map_room, 0, meshes, mesh_room, 0};
        const nav::Read result = nav::read(&file);
        return result == nav::Read::ok ? (file.map_count == 1 && file.navmesh_count == 1 ? result : nav::Read::full) : file.map_count || file.navmesh_count ? nav::Read::ok : result;
    };
    CHECK(read(bytes) == nav::Read::ok);
    CHECK(std::string(reinterpret_cast<const char*>(bytes.data()) + maps[0].path, maps[0].path_length) == "/G/Cave_P" && maps[0].navmesh_count == 1);
    CHECK(std::string(reinterpret_cast<const char*>(bytes.data()) + meshes[0].name, meshes[0].name_length) == "Nav-Pikmin" && meshes[0].tile_count == 2 &&
          meshes[0].max_tiles == 64 && meshes[0].payload == payload);
    std::vector<nav::Tile> tiles;
    nav::each_tile(bytes.data(), meshes[0], [&](const nav::Tile& found) {
        tiles.push_back(found);
        return true;
    });
    CHECK(tiles.size() == 2 && tiles[0].x == 3 && tiles[0].stored == nav::kEmptyTile && tiles[0].memory == 116 && tiles[1].x == 4 && tiles[1].y == 7 && tiles[1].layer == 1);
    CHECK(tiles.size() == 2 && tiles[1].memory == 116 + 12 + 32 + 32 + 12 + 40 + 64 + 20 + 4);
    // The block: the header's words in the order they have in memory, each record where it belongs, and the room
    // for links left empty.
    std::vector<uint8_t> block(tiles.size() == 2 ? tiles[1].memory : 0, 0xFF);
    if (tiles.size() == 2) nav::build_tile(tiles[1], block.data());
    const auto word = [&](size_t at) { return nav::detail::u32_at(block.data() + at); };
    const auto is_run = [&](size_t at, uint8_t first, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i)
            if (block[at + i] != uint8_t(first + i)) return false;
        return true;
    };
    CHECK(block.size() == 332 && word(0) == nav::kTileMagic && word(8) == 4 && word(60) == 1 /* segments */ && word(64) == 0x11223344 && word(68) == 0x55667788 &&
          word(72) == 1 /* clusters */ && is_run(76, 0xA0, 40));
    CHECK(block.size() == 332 && is_run(116, 0x10, 12) && is_run(128, 0x20, 32) && word(160) == 0 && word(188) == 0 && is_run(192, 0x30, 10) && block[202] == 0 && block[203] == 0);
    // The connection off the mesh at 204: place and radius, its height from after all of them, the user's number,
    // the polygon, then side and flags the other way round.
    CHECK(block.size() == 332 && is_run(204, 0x40, 28) && is_run(232, 0x70, 4) && is_run(236, 0x40 + 32, 4) && is_run(240, 0x40 + 28, 2) && block[242] == 0x40 + 31 &&
          block[243] == 0x40 + 30);
    // The segment at 244: start A, end A, start B, end B, radius, no height, the user's number, then polygon, polygons and flags.
    CHECK(block.size() == 332 && is_run(244, 0x80, 12) && is_run(256, 0x80 + 24, 12) && is_run(268, 0x80 + 12, 12) && is_run(280, 0x80 + 36, 12) && is_run(292, 0x80 + 48, 4) &&
          word(296) == 0 && is_run(300, 0x80 + 56, 4) && is_run(304, 0x80 + 52, 4));
    CHECK(block.size() == 332 && is_run(308, 0xC0, 12) && word(320) == 0 && word(324) == 0 && is_run(328, 0xE0, 2) && block[330] == 0);

    // Refused whole: another mark, a file cut short, a version or a count that is not right, a tile with a cache,
    // bytes left over, and tables with no room.
    auto wrong = bytes;
    wrong[7] = '2';
    CHECK(read(wrong) == nav::Read::not_navigation && read(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 11)) == nav::Read::not_navigation);
    CHECK(read(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1)) == nav::Read::cut_short && read(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20)) == nav::Read::cut_short);
    wrong = bytes, wrong[payload + 4] = 12;
    CHECK(read(wrong) == nav::Read::bad_value);
    wrong = bytes, wrong[payload + 12] = 3; // three tiles, it says
    CHECK(read(wrong) == nav::Read::bad_value);
    wrong = bytes, wrong[wrong.size() - 1] = 1; // the last tile's cache is not empty
    CHECK(read(wrong) == nav::Read::bad_value);
    wrong = bytes, wrong[14] = ' '; // the map's path
    CHECK(read(wrong) == nav::Read::bad_value);
    wrong = bytes, wrong.push_back(0);
    CHECK(read(wrong) == nav::Read::bad_value || read(wrong) == nav::Read::cut_short);
    CHECK(read(bytes, 0, 2) == nav::Read::full && read(bytes, 2, 0) == nav::Read::full);
    navigation_fixture = bytes;
}

#include "content_test.h"
#include "creatures_test.h"
#include "shader_checks.h"
#include "check_mods_test.h"
#include "records_checks.h"
#include "map_checks.h"
#include "navigation_checks.h"

int main(int argc, char** argv) {
    const bool early_only = argc == 3 && std::strcmp(argv[2], "--early-maps") == 0;
    const bool initial_only = argc == 3 && std::strcmp(argv[2], "--initial-streaming") == 0;
    const bool ai_only = argc == 3 && std::strcmp(argv[2], "--ai-order") == 0;
    const bool table_only = argc == 3 && std::strcmp(argv[2], "--table-atomic") == 0;
    const bool maps_only = argc == 3 && std::strcmp(argv[2], "--map-tables") == 0;
    const bool shader_only = argc == 3 && std::strcmp(argv[2], "--shader-targets") == 0;
    const bool restarts_only = argc == 3 && std::strcmp(argv[2], "--restarts") == 0;
    if (argc != 2 && !ai_only && !shader_only && !early_only && !initial_only && !table_only && !maps_only && !restarts_only) return 2;
    build();
    if (early_only) { early_maps(); return failures; }
    if (initial_only) { initial_streaming(); return failures; }
    if (restarts_only) { // the restarts alone, with the file functions and the script runtimes the full run has by then
        t::fn::fs_open_file = game_fs_open_file, t::fn::fs_close_file = game_fs_close_file;
        t::fn::fs_file_size = game_fs_file_size, t::fn::fs_read_file = game_fs_read_file;
        t::fn::fs_open_folder = game_fs_open_folder, t::fn::fs_read_folder = game_fs_read_folder, t::fn::fs_close_folder = game_fs_close_folder;
        tsuchi_lua::register_script_mods();
        tsuchi_js::register_script_mods();
        restarting();
        if (failures) std::printf("%d failed\n", failures);
        return failures;
    }
    arithmetic();
    content_paths();
    ground_modes();
    lists();
    descriptions();
    poses();
    lives();
    events();
    spawning();
    moving();
    suspending();
    install_parts(); // the engine's side of parts: bodies switches one off, and the sections after use the rest
    bodies();
    parts();
    added_parts();
    materials();
    if (shader_only) {
        stocking();
        t::fn::fs_open_file = game_fs_open_file, t::fn::fs_close_file = game_fs_close_file;
        t::fn::fs_file_size = game_fs_file_size, t::fn::fs_read_file = game_fs_read_file;
        t::fn::fs_open_folder = game_fs_open_folder, t::fn::fs_read_folder = game_fs_read_folder, t::fn::fs_close_folder = game_fs_close_folder;
        tsuchi::set_log_sink(log_sink);
        shader_files();
        shading();
        return failures;
    }
    leaving();
    behaviors();
    fighting();
    running_game();
    playing();
    leading_captains();
    herding();
    roaming();
    stocking();
    tabling();
    if (table_only) { array_lengths(); return failures; }
    wording();
    announcing();
    journeying();
    saving();
    animating();
    filing();
    scripting();
    compiling();
    keeping();
    configuring();
    conversing(); // installs the game dialogs before public SDK mods use them
    if (maps_only) { tsuchi::set_log_sink(log_sink); mapping(); return failures; }
    checking_in_the_game(ai_only);
    // AI hooks live for the framework process. Its cold-start SDK proof runs in a
    // separate process, preserving the later installer's pristine-start regressions.
    if (ai_only) {
        if (failures) std::printf("%d failed\n", failures);
        return failures;
    }
    javascript_mods();
    records_checks();
    navigating();
    navigation_retries();
    adding_content();
    adding_creatures();
    installing();
    restarting();
    tsuchi::mod_stop(ai_native_mod);
    mapping();
    shader_files();
    shading();
    // The production observer publishes only the counters used by map acceptance.
    tsuchi::target::observe_status();
    const auto& published = tsuchi::target::status;
    CHECK(sizeof(tsuchi::target::Status) == 6352);
    CHECK(offsetof(tsuchi::target::Status, site_jump) == 192);
    CHECK(offsetof(tsuchi::target::Status, map_values_set) == 6148);
    CHECK(offsetof(tsuchi::target::Status, shader_level_requests) == 6152);
    // Shader sets' and held slots' costs are published with the counters they are copied from.
    const auto shader_costs = tsuchi::shader_counts();
    const auto hold_costs = tsuchi::material_hold_counts();
    CHECK(published.shader_level_requests == shader_costs.level_requests && shader_costs.level_requests >= shader_costs.copies_asked);
    CHECK(published.shader_copies_asked == shader_costs.copies_asked && published.shader_copies_asked > 0);
    CHECK(published.shader_copies_ready == shader_costs.copies_ready && published.shader_stand_ins == shader_costs.stand_ins);
    CHECK(published.shader_slots_set == shader_costs.slots_set && published.shader_parts_asked == shader_costs.parts_asked);
    CHECK(published.hold_kept == hold_costs.kept && hold_costs.kept > 0);
    CHECK(published.hold_changed == hold_costs.changed && published.hold_set == hold_costs.set && hold_costs.set <= hold_costs.changed + hold_costs.kept);
    CHECK(published.shader_steps == shader_costs.steps.runs && shader_costs.steps.runs > 0 && published.hold_passes == tsuchi::hold_pass_cost().runs);
    const auto maps = tsuchi::map_counts();
    const auto navigation = tsuchi::map_navigation_counts();
    CHECK(published.map_parts == tsuchi::map_parts());
    CHECK(published.map_levels_asked == tsuchi::map_counts().levels_asked && published.map_levels_asked > 0);
    CHECK(published.map_levels_refused == maps.levels_refused);
    CHECK(published.map_entries_taken == maps.entries_taken);
    CHECK(published.map_entries_refused == maps.entries_refused);
    CHECK(published.map_removed == maps.removed);
    CHECK(published.map_moved == maps.moved);
    CHECK(published.map_values_refused == maps.values_refused);
    CHECK(published.map_values_set == maps.values_set && published.map_values_set > 0);
    CHECK(published.nav_meshes == navigation.navmeshes);
    CHECK(published.nav_added == navigation.added);
    CHECK(published.nav_removed == navigation.removed);
    CHECK(published.nav_refused == navigation.refused);
    CHECK(published.newmap_menus_listed == tsuchi::new_map_counts().menus_listed);
    if (failures) std::printf("%d failed\n", failures);
    return failures ? failures : script(argv[1]);
}
