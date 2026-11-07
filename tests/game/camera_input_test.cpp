// Camera/input against a made-up game with real generated validation, registry, and shared Holds.
#include "actor_events.h"
#include "actors.h"
#include "angles.h"
#include "camera_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "input_service.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "people.h"
#include "presenter.h"
#include "registry.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
namespace t = tsuchi::game::table;
using namespace tsuchi;
#define CHECK(x)                                                                                                                           \
    do {                                                                                                                                   \
        ++checks;                                                                                                                          \
        if (!(x)) {                                                                                                                        \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                                                           \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)
static unsigned checks = 0;
static std::vector<void (*)()> steps;
static std::vector<void (*)()> finishes;
static std::vector<void (*)(uint64_t, uint64_t, ActorLeft)> actor_leaves;
static std::vector<void (*)(uint64_t, uint64_t, bool)> ordinary_leaves;
static bool watch_supported = true;
static unsigned watched = 0;
static std::vector<std::pair<uint64_t, uint64_t>> ranges;
static std::set<uint64_t> live_objects;
static uint64_t thread = 1, captain = 0, scenery = 0, pc = 0, gf = 0, game_camera = 0, pcm = 0, local = 0;
static uint64_t* imports = nullptr;
const uint64_t first_import =
    t::data::input_full_key.address < t::data::input_handheld.address ? t::data::input_full_key.address : t::data::input_handheld.address;
static unsigned import_slot(uint64_t address) {
    return unsigned((address - first_import) / 8);
}
static uint32_t routed_pad = 0, raw_buttons = 0, pending = 0, foreign_calls = 0, shake_calls = 0, stop_calls = 0;
static int32_t routes[4] = {-1, -1, -1, -1};
static bool sampling = false, world_ready = true, fail_snapshot = false, wrong_camera_class = false;
static uint32_t view_limit = 60;
static void* allocator(size_t n) {
    return fail_snapshot && n == 40 ? nullptr : std::malloc(n);
}
static void (*after_camera)(uint64_t, uint64_t);
static uint64_t (*after_input)(uint32_t, uint64_t, bool);
using Poll = void (*)(void*, const void*);
static std::map<uint64_t, Poll> polls;
template <class T> void put(uint64_t at, const T& value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}
template <class T> T get(uint64_t at) {
    T v;
    std::memcpy(&v, reinterpret_cast<void*>(at), sizeof v);
    return v;
}
static uint64_t bytes(size_t n) {
    auto* p = std::calloc(1, n);
    const uint64_t a = reinterpret_cast<uint64_t>(p);
    ranges.push_back({a, n});
    live_objects.insert(a);
    return a;
}
static void tick() {
    for (auto step : steps)
        step();
    for (auto finish : finishes)
        finish();
}
static uint64_t probe() {
    return thread;
}
static void foreign(void*, const void*);
namespace tsuchi::game {
uint64_t module_containing(uint64_t) { return 0; }
uint64_t module_symbol(uint64_t, const char*) { return 0; }
uint64_t main_base() {
    return reinterpret_cast<uint64_t>(imports) - first_import;
}
bool readable(uint64_t at, uint64_t size) {
    for (auto r : ranges)
        if (at >= r.first && at - r.first <= r.second && size <= r.second - (at - r.first))
            return true;
    return false;
}
bool writable(uint64_t at, uint64_t size) {
    return readable(at, size);
}
bool executable(uint64_t at) {
    return at > 0x10000;
}
bool live(uint64_t at) {
    return live_objects.count(at) != 0;
}
uint64_t from_handle(uint64_t h) {
    return live(h) ? h : 0;
}
uint64_t to_handle(uint64_t a) {
    return live(a) ? a : 0;
}
bool is_a(uint64_t object, ClassRef& ref) {
    CHECK(!std::strcmp(ref.name, t::name::camera_follow_class));
    return object == game_camera && !wrong_camera_class;
}
bool is_engine_actor(uint64_t a) {
    return a == captain || a == scenery;
}
bool is_game_actor(uint64_t a) {
    return a == captain;
}
uint64_t part_owner(uint64_t) {
    return 0;
}
uint64_t game_framework() {
    return world_ready ? gf : 0;
}
uint64_t player_captain(uint32_t p) {
    return !p && world_ready ? captain : 0;
}
bool player_pad(uint32_t p, uint32_t& out) {
    if (p >= kPlayerSlots || !world_ready)
        return false;
    out = routes[p] < 0 ? routed_pad : uint32_t(routes[p]);
    return true;
}
bool get_transform(uint64_t actor, Transform& out) {
    if (!live(actor))
        return false;
    out = {{100, 200, 300}, {0, 0, 0}};
    return true;
}
} // namespace tsuchi::game
namespace tsuchi {
static bool in_step = false;
bool frame_stepping() {
    return in_step;
}
bool after_step(void (*f)()) {
    finishes.push_back(f);
    return true;
}
bool each_frame(void (*f)()) {
    steps.push_back(f);
    return true;
}
bool on_actor_left(void (*f)(uint64_t, uint64_t, ActorLeft)) {
    actor_leaves.push_back(f);
    return true;
}
void ordinary_lifecycle_listener(void (*f)(uint64_t, uint64_t, bool)) {
    ordinary_leaves.push_back(f);
}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t actor) {
    CHECK(actor == scenery);
    ++watched;
    return watch_supported ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}
uint64_t ordinary_lifecycle_restore_part(uint64_t h, uint64_t) {
    return game::from_handle(h);
}
void presenter_camera_after(void (*f)(uint64_t, uint64_t)) {
    after_camera = f;
}
void presenter_input_after(uint64_t (*f)(uint32_t, uint64_t, bool)) {
    after_input = f;
}
bool presenter_sampling_input() {
    return sampling;
}
bool presenter_drawing_view() {
    return true;
}
uint32_t presenter_limit() {
    return view_limit;
}
bool presenter_sample_input(uint32_t pad, void* state) {
    sampling = true;
    if (polls.count(t::data::input_full_key.address))
        polls.at(t::data::input_full_key.address)(state, &pad);
    else {
        // The stock PLT path binds its getter before returning this first sample.
        imports[import_slot(t::data::input_full_key.address)] = reinterpret_cast<uint64_t>(&foreign);
        foreign(state, &pad);
    }
    sampling = false;
    const uint64_t at = reinterpret_cast<uint64_t>(state) + t::npad_state::buttons;
    put(at, get<uint64_t>(at) | pending);
    return true;
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t, void* f, uint32_t flags, uint64_t* id, void** next) {
    CHECK(flags == 0);
    const uint64_t offset = entry - game::main_base();
    *next = reinterpret_cast<void*>(imports[import_slot(offset)]);
    polls[offset] = reinterpret_cast<Poll>(f);
    *id = entry;
    return TSUCHI_OK;
}
void camera_start();
void input_start(TSUCHI_Mod*);
} // namespace tsuchi
static void foreign(void* out, const void*) {
    ++foreign_calls;
    std::memset(out, 0, t::npad_state::size);
    const uint64_t a = reinterpret_cast<uint64_t>(out);
    put(a + t::npad_state::buttons, uint64_t(raw_buttons));
    put(a + t::input_state::connected, uint32_t(1));
    const int32_t axes[] = {-32767, 123, 456, 32767};
    std::memcpy(reinterpret_cast<void*>(a + t::input_state::sticks), axes, sizeof axes);
}
static void* controller(void*, int32_t p) {
    return p == 0 && world_ready ? reinterpret_cast<void*>(pc) : nullptr;
}
static void screen(void*, void* w, void* h) {
    *static_cast<int32_t*>(w) = 1920;
    *static_cast<int32_t*>(h) = 1080;
}
static uint32_t expected_shake = 0;
static void shake(void*, uint32_t kind, void* actor) {
    CHECK(kind == expected_shake && reinterpret_cast<uint64_t>(actor) == captain);
    ++shake_calls;
}
static void stop(void* mgr, bool now) {
    CHECK(reinterpret_cast<uint64_t>(mgr) == pcm && now);
    ++stop_calls;
}
static bool split_projection = false, recursive_projection = false, bypass_projection = false;
static uint64_t projection_scratch = 0;
static bool project(void* controller, const void* point, void* output, bool relative) {
    CHECK(reinterpret_cast<uint64_t>(controller) == pc && !relative);
    TSUCHI_View stock{{9, 8, 7}, {6, 5, 4}, 30};
    put(projection_scratch, stock);
    if (!bypass_projection)
        after_camera(local, projection_scratch);
    CHECK(bypass_projection || get<float>(projection_scratch + t::view_info::fov) == (view_limit == 30 ? 30 : 90));
    if (recursive_projection) {
        TSUCHI_ScreenPoint ignored;
        CHECK(camera::table.world_to_screen(mod_create("projection.inner"), 0, static_cast<const TSUCHI_Vec3*>(point), &ignored) ==
              TSUCHI_BUSY);
    }
    const auto v = get<TSUCHI_View>(projection_scratch);
    const auto q = *static_cast<const TSUCHI_Vec3*>(point);
    const float yaw = v.rotation.yaw / 57.2957795f, pitch = v.rotation.pitch / 57.2957795f;
    const float cy = math::cosine(yaw), sy = math::sine(yaw), cp = math::cosine(pitch), sp = math::sine(pitch);
    const float x = q.x - v.location.x, y = q.y - v.location.y, z = q.z - v.location.z;
    const float forward = x * cp * cy + y * cp * sy + z * sp;
    if (!(forward > 0))
        return false;
    auto& result = *static_cast<TSUCHI_ScreenPoint*>(output);
    const float focal = (split_projection ? 480.0f : 960.0f) / (math::sine(v.fov / 114.591559f) / math::cosine(v.fov / 114.591559f));
    result.x = (split_projection ? 1440.0f : 960.0f) + (-x * sy + y * cy) * focal / forward;
    result.y = 540.0f - (-x * sp * cy - y * sp * sy + z * cp) * focal / forward;
    return true;
}

int main() {
    uint64_t import_slots[3] = {};
    imports = import_slots;
    ranges.push_back({reinterpret_cast<uint64_t>(imports), sizeof import_slots});
    captain = bytes(0x1000);
    pc = bytes(0x1000);
    gf = bytes(0x1000);
    game_camera = bytes(0xB00);
    pcm = bytes(0x3000);
    local = bytes(0x100);
    projection_scratch = bytes(t::view_info::size);
    const uint64_t part = bytes(0xE00), vt = bytes(0x1000), demo = bytes(0x500), output = bytes(t::view_info::size);
    put(captain + t::game_actor::player, part);
    put(part + t::camera_service::player_camera, game_camera);
    put(pc + t::player_controller::camera_manager, pcm);
    put(local + t::local_player::controller, pc);
    put(gf + t::camera_service::demo_manager, demo);
    put(gf + t::camera_service::shake_manager, bytes(8));
    put(pcm, vt);
    put(vt + t::camera_service::stop_shakes.offset, reinterpret_cast<uint64_t>(&stop));
    t::fn::camera_controller = controller;
    t::fn::camera_screen = screen;
    t::fn::camera_shake = shake;
    t::fn::camera_project = project;
    for (auto& f : import_slots)
        f = reinterpret_cast<uint64_t>(&foreign);
    put(game_camera + t::camera_service::distance, 500.0f);
    put(game_camera + t::camera_service::pitch, 30.0f);
    put(game_camera + t::camera_service::heading, 220.0f);
    put(game_camera + t::camera_service::follow, uint8_t(7));
    const TSUCHI_Vec3 old_look{1, 2, 3}, old_pole{120, 240, 360};
    put(game_camera + t::camera_service::look, old_look);
    put(game_camera + t::camera_service::pole, old_pole);
    const TSUCHI_View view{{0, 0, 0}, {0, 0, 0}, 90};
    put(pcm + t::camera_service::pov, view);
    set_allocator({allocator, std::free});
    set_thread_probe(probe);
    note_game_thread();
    TSUCHI_Mod *a = mod_create("game_camera.a"), *b = mod_create("game_camera.b"), *self = mod_create("tsuchi.self");
    camera_start();
    imports[import_slot(t::data::input_full_key.address)] = game::main_base() + t::constant::input_resolver;
    input_start(self);
    CHECK(polls.count(t::data::input_full_key.address) == 0);
    const auto& c = camera::table;
    const auto& i = input::table;
    TSUCHI_View read{};
    TSUCHI_Zoom zoom{800, 45}, z{};
    float yaw = 0;
    TSUCHI_Look look{{captain}, {50, 0, 20}}, read_look{};
    TSUCHI_ScreenSize size{};
    TSUCHI_ScreenPoint point{};
    TSUCHI_Buttons buttons = 0;
    TSUCHI_PadState pad{};
    bool key = false;
    CHECK(i.pad(a, 0, &pad) == TSUCHI_NOT_READY);
    CHECK(c.view(a, 0, &read) == TSUCHI_OK && read.fov == 90);
    // Native 30 FPS can project before any blended picture exists.
    const TSUCHI_Vec3 native_goal{100, 8, 7};
    view_limit = 30;
    CHECK(c.world_to_screen(a, 0, &native_goal, &point) == TSUCHI_OK);
    CHECK(get<TSUCHI_View>(projection_scratch).fov == 30);
    view_limit = 60;
    CHECK(c.world_to_screen(a, 0, &native_goal, &point) == TSUCHI_NOT_READY);
    CHECK(c.zoom(a, 0, &z) == TSUCHI_OK && z.distance == 500);
    CHECK(c.heading(a, 0, &yaw) == TSUCHI_OK && yaw == 40);
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(c.hold_heading(b, 0, 25) == TSUCHI_BUSY);
    CHECK(c.hold_look(b, 0, &look) == TSUCHI_BUSY);
    CHECK(c.hold_heading(a, 0, 390) == TSUCHI_OK);
    CHECK(c.heading(a, 0, &yaw) == TSUCHI_OK && yaw == 30);
    put(output + t::view_info::rotation, TSUCHI_Rotation{4,-42,7});
    after_camera(local,output);
    CHECK(get<TSUCHI_Rotation>(output+t::view_info::rotation).yaw==-42);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && read_look.actor.id == captain);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 0);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == 150);
    // The callback receives an already-interpolated view and leaves position unchanged.
    put(output + t::view_info::location, TSUCHI_Vec3{0, 0, 0});
    put(output + t::view_info::rotation, TSUCHI_Rotation{4,20,7});
    after_camera(local,output);
    CHECK(get<TSUCHI_Rotation>(output+t::view_info::rotation).yaw==20);
    // The native view fixture now supplies the look-at orientation that the
    // engine computes from the held goal. Projection must use that shown POV.
    put(output + t::view_info::rotation, TSUCHI_Rotation{std::atan2(320.0f,250.0f)*57.2957795f,53.1301f,0});
    put(output + t::view_info::fov, 90.0f);
    after_camera(local, output);
    CHECK(get<TSUCHI_Vec3>(output + t::view_info::location).x == 0);
    CHECK(std::fabs(get<TSUCHI_Rotation>(output + t::view_info::rotation).yaw - 53.1301f) < .01f);
    CHECK(c.view(a, 0, &read) == TSUCHI_OK && std::fabs(read.rotation.yaw - 53.1301f) < .01f);
    CHECK(c.screen_size(a, 0, &size) == TSUCHI_OK && size.width == 1920 && size.height == 1080);
    TSUCHI_Vec3 goal{150, 200, 320};
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_OK);
    CHECK(std::fabs(point.x - 960) < .05f && std::fabs(point.y - 540) < .05f);
    CHECK(get<TSUCHI_View>(projection_scratch).location.x == 9 && get<TSUCHI_View>(projection_scratch).rotation.yaw == 5 &&
          get<TSUCHI_View>(projection_scratch).fov == 30);
    split_projection = true;
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_OK && std::fabs(point.x - 1440) < .05f);
    split_projection = false;
    recursive_projection = true;
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_OK);
    recursive_projection = false;
    bypass_projection = true;
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_UNSUPPORTED);
    bypass_projection = false;
    // Switching to native frames discards the retained blended view. A later
    // higher-rate call waits for its first displayed picture again.
    view_limit = 30;
    auto native_view = view;
    native_view.fov = 60;
    put(pcm + t::camera_service::pov, native_view);
    CHECK(c.view(a, 0, &read) == TSUCHI_OK && read.fov == 60);
    CHECK(c.world_to_screen(a, 0, &native_goal, &point) == TSUCHI_OK);
    view_limit = 60;
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_NOT_READY);
    after_camera(local, output);
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_OK);
    goal = {-150, -200, -320};
    CHECK(c.world_to_screen(a, 0, &goal, &point) == TSUCHI_NOT_FOUND);
    const uint32_t kinds[] = {
        TSUCHI_SHAKE_KIND_CAPTAIN_HURT, TSUCHI_SHAKE_KIND_PIKMIN_POUND, TSUCHI_SHAKE_KIND_GATE_DOWN,
        TSUCHI_SHAKE_KIND_STEP_HARD, TSUCHI_SHAKE_KIND_DROP_HEAVY, TSUCHI_SHAKE_KIND_DROP_MIDDLE,
        TSUCHI_SHAKE_KIND_DROP_LIGHT, TSUCHI_SHAKE_KIND_CRASH_HARD, TSUCHI_SHAKE_KIND_BOSS_DROP,
        TSUCHI_SHAKE_KIND_ROCK_HIT, TSUCHI_SHAKE_KIND_BOMB, TSUCHI_SHAKE_KIND_DOG_CRUSH,
        TSUCHI_SHAKE_KIND_DOG_WEAK_DAMAGE, TSUCHI_SHAKE_KIND_DOG_HEAVY_DAMAGE, TSUCHI_SHAKE_KIND_MICRO,
        TSUCHI_SHAKE_KIND_SMALL, TSUCHI_SHAKE_KIND_MEDIUM, TSUCHI_SHAKE_KIND_LARGE,
    };
    for (expected_shake = 0; expected_shake < 18; ++expected_shake) {
        CHECK(kinds[expected_shake] == expected_shake);
        CHECK(c.shake(a, 0, kinds[expected_shake]) == TSUCHI_OK && shake_calls == expected_shake + 1);
    }
    CHECK(c.shake(a, 0, 18) == TSUCHI_INVALID && shake_calls == 18);
    CHECK(c.stop_shakes(a, 0, true) == TSUCHI_OK && stop_calls == 1);
    // Pauses put all three changed fields back, then resume from the game's new goals.
    put(game_camera + t::camera_service::mode, uint8_t(1));
    tick();
    CHECK(polls.count(t::data::input_full_key.address) == 1);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 7);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == old_look.x);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::pole).y == old_pole.y);
    put(game_camera + t::camera_service::mode, uint8_t(0));
    tick();
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 0);
    put(demo + t::camera_service::demo_hold, uint32_t(1));
    tick();
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 7);
    put(demo + t::camera_service::demo_hold, uint32_t(0));
    tick();
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 0);
    CHECK(c.release_look(b, 0) == TSUCHI_NOT_FOUND);
    CHECK(c.release_look(a, 0) == TSUCHI_OK);
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == old_look.x);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::pole).y == old_pole.y);
    CHECK(c.release_heading(a, 0) == TSUCHI_OK && get<float>(game_camera + t::camera_service::heading) == 220);
    CHECK(c.release_zoom(a, 0) == TSUCHI_OK && get<float>(game_camera + t::camera_service::distance) == 500);
    CHECK(c.release_zoom(a, 0) == TSUCHI_NOT_FOUND);

    // A live camera of a different class is refused before its native fields are touched.
    wrong_camera_class = true;
    CHECK(c.zoom(a, 0, &z) == TSUCHI_NOT_READY);
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_NOT_READY);
    CHECK(c.hold_heading(a, 0, 30) == TSUCHI_NOT_READY);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_NOT_READY);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 500);
    CHECK(get<float>(game_camera + t::camera_service::heading) == 220);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 7);
    wrong_camera_class = false;
    // An absent follow camera restores its old live camera at once, while the player hold waits.
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(c.hold_heading(a, 0, 30) == TSUCHI_OK);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    put(part + t::camera_service::player_camera, uint64_t(0));
    tick();
    CHECK(get<float>(game_camera + t::camera_service::distance) == 500);
    CHECK(get<float>(game_camera + t::camera_service::heading) == 220);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 7);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == old_look.x);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::pole).y == old_pole.y);
    put(part + t::camera_service::player_camera, game_camera);
    tick();
    CHECK(get<float>(game_camera + t::camera_service::distance) == 800);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 0);
    CHECK(c.release_zoom(a, 0) == TSUCHI_OK);
    CHECK(c.release_heading(a, 0) == TSUCHI_OK);
    CHECK(c.release_look(a, 0) == TSUCHI_OK);
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(c.hold_heading(a, 0, 30) == TSUCHI_OK);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    put(part + t::camera_service::player_camera, uint64_t(0));
    tick();
    mod_stop(a);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 500);
    CHECK(get<float>(game_camera + t::camera_service::heading) == 220);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 7);
    put(part + t::camera_service::player_camera, game_camera);
    a = mod_create("camera.resumed");
    // Camera changes restore the old game_camera and capture the new game_camera before writing it.
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(c.hold_heading(a, 0, 90) == TSUCHI_OK);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    const uint64_t prior = game_camera;
    game_camera = bytes(0xB00);
    put(part + t::camera_service::player_camera, game_camera);
    put(game_camera + t::camera_service::distance, 900.0f);
    put(game_camera + t::camera_service::pitch, 12.0f);
    put(game_camera + t::camera_service::heading, 70.0f);
    put(game_camera + t::camera_service::follow, uint8_t(3));
    tick();
    CHECK(get<float>(prior + t::camera_service::distance) == 500);
    CHECK(get<float>(prior + t::camera_service::heading) == 220);
    CHECK(get<uint8_t>(prior + t::camera_service::follow) == 7);
    mod_stop(a);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 900);
    CHECK(get<float>(game_camera + t::camera_service::heading) == 70);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    CHECK(owned_count(a) == 0);
    a = mod_create("game_camera.c");
    const auto saved_count = owned_count(a);
    fail_snapshot = true;
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_NO_MEMORY && owned_count(a) == saved_count);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    fail_snapshot = false;
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    live_objects.erase(captain);
    tick();
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    live_objects.insert(captain);
    // Pooling ends the target hold before its still-live generation is put away.
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    for (auto left : actor_leaves)
        left(captain, captain, ActorLeft::put_away);
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == 0);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    for (auto left : actor_leaves)
        left(captain, captain, ActorLeft::destroyed);
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    scenery = bytes(0x300);
    const uint64_t scenery_root = bytes(0x400);
    put(scenery + t::engine_actor::root, scenery_root);
    put(scenery_root + t::scene_component::location, TSUCHI_Vec3{400, 500, 600});
    const TSUCHI_Look ordinary_target{{scenery}, {0, 0, 0}};
    watch_supported = false;
    CHECK(c.hold_look(a, 0, &ordinary_target) == TSUCHI_UNSUPPORTED);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    watch_supported = true;
    CHECK(c.hold_look(a, 0, &ordinary_target) == TSUCHI_OK && watched == 2);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == 400);
    for (auto left : ordinary_leaves)
        left(scenery, scenery, false);
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == 0);

    CHECK(c.hold_look(a, 0, &ordinary_target) == TSUCHI_OK);
    for (auto left : ordinary_leaves)
        left(scenery, scenery, true);
    live_objects.erase(scenery);
    tick();
    after_camera(local, output); // No callback resolves the now-stale target after native destruction.
    CHECK(c.look(a, 0, &read_look) == TSUCHI_OK && !read_look.actor.id);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 3);
    CHECK(get<TSUCHI_Vec3>(game_camera + t::camera_service::look).x == 0);
    live_objects.insert(scenery);
    // Restoration never writes a dead/reused camera generation, even when its memory is readable.
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(c.hold_heading(a, 0, 30) == TSUCHI_OK);
    CHECK(c.hold_look(a, 0, &look) == TSUCHI_OK);
    live_objects.erase(game_camera);
    put(game_camera + t::camera_service::distance, 777.0f);
    put(game_camera + t::camera_service::heading, 123.0f);
    put(game_camera + t::camera_service::follow, uint8_t(9));
    CHECK(c.release_zoom(a, 0) == TSUCHI_OK);
    CHECK(c.release_heading(a, 0) == TSUCHI_OK);
    CHECK(c.release_look(a, 0) == TSUCHI_OK);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 777);
    CHECK(get<float>(game_camera + t::camera_service::heading) == 123);
    CHECK(get<uint8_t>(game_camera + t::camera_service::follow) == 9);
    live_objects.insert(game_camera);
    put(game_camera + t::camera_service::distance, 900.0f);
    put(game_camera + t::camera_service::heading, 70.0f);
    put(game_camera + t::camera_service::follow, uint8_t(3));

    // All three import chains call the foreign wrapper and agree about injection and hiding.
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK);
    CHECK(i.key(a, 4, &key) == TSUCHI_NOT_READY);
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    CHECK(i.hold_hidden(b, 0, 1) == TSUCHI_BUSY);
    CHECK(i.hold_hidden(a, 0, 2) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 1 && pad.pressed == 1);
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 1);
    CHECK(i.hidden(a, 0, &buttons) == TSUCHI_OK && buttons == 2);
    alignas(8) uint8_t state[t::npad_state::most] = {};
    raw_buttons = 2;
    for (const auto& poll : polls) {
        const unsigned before = foreign_calls;
        poll.second(state, &routed_pad);
        CHECK(foreign_calls == before + 1);
        CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 1);
    }
    sampling = true;
    polls.at(t::data::input_full_key.address)(state, &routed_pad);
    sampling = false;
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) ==
          2); // hidden physical input survives, own injection stays out of the latch
    pending = 2;
    raw_buttons = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 3 && pad.pressed == 3 && pad.left_x == -32767 && pad.right_y == 32767);
    CHECK(after_input(0, 2, true) == 1);
    pending = 0;
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 1);
    CHECK(after_input(0, 0, true) == 1);
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 1);
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    tick();
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0); // expires even when the game reads no npad slot
    tsuchi::in_step = true;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    for (auto finish : finishes)
        finish();
    tsuchi::in_step = false;
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0); // current-step requests expire before the next step
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    CHECK(after_input(0, 2, true) == 2);
    CHECK(i.release_pressed(a, 0) == TSUCHI_NOT_FOUND);
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 8);
    CHECK(presenter_sample_input(0, state));
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 0);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0);
    CHECK(after_input(0, 0, true) == 0); // no synthetic tap remains after release before the world reads

    // Actual-controller ownership is coherent across routed player aliases and hold kinds.
    const unsigned empty = owned_count(a);
    routes[0] = routes[1] = 0;
    routes[2] = routes[3] = 2;
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    CHECK(owned_count(a) == empty + 1);
    CHECK(i.hold_pressed(b, 1, 2, 2) == TSUCHI_BUSY);
    CHECK(i.hold_hidden(b, 1, 2) == TSUCHI_BUSY);
    CHECK(i.hold_pressed(a, 1, 4, 2) == TSUCHI_OK);
    CHECK(owned_count(a) == empty + 1); // Retaking through an alias changes the one hold.
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 4);
    CHECK(i.pressed(a, 1, &buttons) == TSUCHI_OK && buttons == 4);
    CHECK(after_input(0, 0, true) == 4);
    routes[1] = 1;
    CHECK(i.release_pressed(a, 1) == TSUCHI_OK); // Captured alias releases even after rerouting.
    CHECK(owned_count(a) == empty);
    CHECK(i.release_pressed(a, 0) == TSUCHI_NOT_FOUND);
    CHECK(after_input(0, 0, true) == 0);
    routes[1] = 0;
    CHECK(i.hold_hidden(a, 0, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 1, 2) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(i.hold_pressed(b, 1, 4, 1) == TSUCHI_BUSY);
    routes[1] = 1;
    CHECK(i.hold_hidden(a, 1, 8) == TSUCHI_OK && owned_count(a) == empty + 2);
    CHECK(i.hidden(a, 0, &buttons) == TSUCHI_OK && buttons == 2);
    CHECK(i.release_hidden(a, 1) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(after_input(0, 2, true) == 0); // Player zero keeps its old association.
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK && owned_count(a) == empty);
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    routes[0] = 1;
    CHECK(i.hold_pressed(a, 0, 8, 2) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(after_input(0, 0, true) == 0 && after_input(1, 0, true) == 8);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    routes[0] = routes[1] = 0;
    raw_buttons = 2;
    CHECK(i.hold_hidden(a, 0, 2) == TSUCHI_OK);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 2);
    TSUCHI_PadState alias{};
    CHECK(i.pad(a, 1, &alias) == TSUCHI_OK && alias.held == pad.held && alias.pressed == pad.pressed);
    CHECK(after_input(0, 2, true) == 0);
    tick(); // Hidden physical input still commits once when there is no native controller read.
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0 && pad.released == 0);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.pressed == 0); // Repeated reads don't change history.
    routes[0] = routes[1] = routes[2] = routes[3] = 1;
    tick(); // Controller zero is still sampled while temporarily unassigned.
    routes[0] = routes[1] = 0;
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0);
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    raw_buttons = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 2);
    CHECK(i.pad(a, 1, &alias) == TSUCHI_OK && alias.released == 2);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 0);
    // A current-step mod write commits even if a pause/menu prevents every native npad read.
    tsuchi::in_step = true;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.pressed == 4);
    for (auto finish : finishes)
        finish();
    tsuchi::in_step = false;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 4);
    // Release and stop remove synthetic input but preserve a shorter-than-step physical tap.
    pending = 2;
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    CHECK(presenter_sample_input(0, state));
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 2);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 2);
    pending = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 2);
    pending = 1;
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    mod_stop(a);
    CHECK(after_input(0, 1, true) == 1);
    tick();
    CHECK(i.pad(b, 0, &pad) == TSUCHI_OK && pad.held == 1 && pad.pressed == 1);
    pending = 0;
    tick();
    CHECK(i.pad(b, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 1);
    a = mod_create("input.routes");
    for (auto& route : routes)
        route = -1;

    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 0, 8) == TSUCHI_OK);
    routed_pad = 1;
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    CHECK(after_input(0, 8, true) == 8);
    routed_pad = 0;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 0, 8) == TSUCHI_OK);
    routed_pad = 1;
    mod_stop(a);
    CHECK(after_input(0, 8, true) == 8);
    CHECK(owned_count(a) == 0);
    routed_pad = 0;
    a = mod_create("game_camera.d");
    CHECK(c.hold_zoom(a, 0, &zoom) == TSUCHI_OK);
    CHECK(i.hold_pressed(a, 0, 1, 1) == TSUCHI_OK);
    thread = 2;
    CHECK(c.release_zoom(a, 0) == TSUCHI_INVALID);
    CHECK(i.release_pressed(a, 0) == TSUCHI_INVALID);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 800);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_INVALID);
    CHECK(c.shake(a, 0, 0) == TSUCHI_INVALID);
    thread = 1;
    mod_stop(a);
    CHECK(get<float>(game_camera + t::camera_service::distance) == 900);
    CHECK(after_input(0, 0, true) == 0);
    CHECK(c.view(b, 4, &read) == TSUCHI_INVALID);
    CHECK(c.zoom(b, 4, &z) == TSUCHI_INVALID);
    CHECK(c.heading(b, 4, &yaw) == TSUCHI_INVALID);
    CHECK(c.look(b, 4, &read_look) == TSUCHI_INVALID);
    CHECK(c.screen_size(b, 4, &size) == TSUCHI_INVALID);
    zoom.distance = NAN;
    CHECK(c.hold_zoom(b, 0, &zoom) == TSUCHI_INVALID);
    CHECK(c.hold_heading(b, 0, INFINITY) == TSUCHI_INVALID);
    look.point.x = NAN;
    CHECK(c.hold_look(b, 0, &look) == TSUCHI_INVALID);
    CHECK(i.key(b, 256, &key) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 0, 1) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 1, 0) == TSUCHI_INVALID);
    CHECK(i.hold_hidden(b, 0, 0) == TSUCHI_INVALID);
    CHECK(i.pad(b, 4, &pad) == TSUCHI_INVALID);
    CHECK(i.release_hidden(b, 4) == TSUCHI_INVALID);
    CHECK(i.release_pressed(b, 4) == TSUCHI_INVALID);
    CHECK(i.hidden(b, 4, &buttons) == TSUCHI_INVALID);
    CHECK(i.pressed(b, 4, &buttons) == TSUCHI_INVALID);
    world_ready = false;
    CHECK(c.view(b, 0, &read) == TSUCHI_NOT_READY);
    CHECK(c.hold_zoom(b, 0, &z) == TSUCHI_NOT_READY);
    CHECK(i.hold_hidden(b, 0, 1) == TSUCHI_NOT_READY);
    CHECK(i.pressed(b, 0, &buttons) == TSUCHI_NOT_READY);
    CHECK(i.hidden(b, 0, &buttons) == TSUCHI_NOT_READY);
    std::printf("game_camera/input: %u shared-hold and stand-in checks passed\n", checks);
}
