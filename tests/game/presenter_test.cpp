// The presenter against a made-up game: the clock, snapping, the camera's resets, the view's clocks, the widgets
// that count frames and the controllers' presses. Host stand-ins, never a game result.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "engine_call.h"
#include "files.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "mods.h"
#include "native_routes.h"
#include "objects.h"
#include "presenter.h"
#include "presenter_effects.h"
#include "presenter_logic.h"
#include "presenter_markers.h"
#include "presenter_scene.h"
#include "registry.h"

namespace t = tsuchi::game::table;
using namespace tsuchi;
using namespace tsuchi::presenter;

static int failures = 0, checks = 0;
#define CHECK(x)                                                     \
    do {                                                             \
        ++checks;                                                    \
        if (!(x)) {                                                  \
            ++failures;                                              \
            std::printf("line %d: %s\n", __LINE__, #x);              \
            std::fflush(stdout);                                     \
        }                                                            \
    } while (0)

// ---- The made-up game -----------------------------------------------------------------------------

static unsigned char* image = nullptr; // main, as large as the highest global it has
static uint64_t main_at() { return reinterpret_cast<uint64_t>(image); }
template <class T>
static void put(uint64_t at, T value) { std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <class T>
static T get(uint64_t at) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}
static uint64_t block(size_t bytes) { return reinterpret_cast<uint64_t>(std::calloc(1, bytes)); }

static uint64_t engine, user, world, level, world_settings, settings_table, player, controller, manager, camera, camera_table,
    subject_part, subject_actor, device, scene, values, family, widget_a, widget_b;
static std::string settings_file;
static std::vector<std::string> notes, commands;
static std::vector<std::pair<float, float>> pictures;
static float max_fps = -1;
static int32_t initial_vsync = 1, current_vsync = 1, sync_interval = 2;
static int32_t game_vsync = 1; // the game's latest VSync preference, as the game last wrote it
static bool vsync_read_allowed = true;
static bool settings_read_allowed = true;
// Original NVN present06051400..06051418: VSync on clamps even interval0 to one refresh.
static int native_swap_interval() { return current_vsync ? std::max(1, std::min(sync_interval, 4)) : 0; }
static uint32_t raw_buttons = 0, style = 1;
static uint64_t refused_pose_address = 0, expired_pose_weak = 0;
static uint64_t remapped_weak_object = 0;
static bool game_thread = true;

namespace tsuchi::game {
uint64_t main_base() { return main_at(); }
bool readable(uint64_t address, uint64_t bytes) {
    return address >= 0x100000 && (!refused_pose_address || address > refused_pose_address || address + bytes <= refused_pose_address);
}
bool writable(uint64_t address, uint64_t) { return address >= 0x100000; }
bool executable(uint64_t address) { return address >= 0x100000; }
uint64_t to_handle(uint64_t object) { return object; }
uint64_t to_weak_value(uint64_t object) { return object && object == remapped_weak_object ? object ^ (1ull << 60) : object; }
uint64_t from_weak_value(uint64_t packed) {
    if (packed == expired_pose_weak || (remapped_weak_object && packed == remapped_weak_object)) return 0;
    return remapped_weak_object && packed == (remapped_weak_object ^ (1ull << 60)) ? remapped_weak_object : packed;
}
uint64_t from_weak_reference(uint64_t at) { return get<uint64_t>(at); }
uint64_t find_class(ClassRef&) { return 0x5000000; }
TSUCHI_Result read_whole_file(const char* path, char*& data, uint64_t& size) {
    if (!settings_read_allowed) return TSUCHI_IO;
    if (std::strcmp(path, "rom:/tsuchi/settings.json") || settings_file.empty()) return TSUCHI_NOT_FOUND;
    size = settings_file.size();
    data = static_cast<char*>(tsuchi::allocate(size + 1));
    std::memcpy(data, settings_file.c_str(), size + 1);
    return TSUCHI_OK;
}
static std::string command_text, variable_name;
EngineCall::EngineCall(uint64_t object, const char* function) : EngineCall(object, function, {}) {}
EngineCall::EngineCall(uint64_t, const char* function, CallGuard) : object_(0), state_(TSUCHI_OK) {
    function_ = !std::strcmp(function, "ExecuteConsoleCommand") ? 1 : !std::strcmp(function, "GetConsoleVariableIntValue") ? 2 : 0;
    if (!function_) state_ = TSUCHI_NOT_FOUND;
}
EngineCall::~EngineCall() {}
TSUCHI_Result EngineCall::set(const char*, const Value&) { return state_; }
TSUCHI_Result EngineCall::set_text(const char* name, const char* text) {
    if (!std::strcmp(name, "Command")) command_text = text;
    if (!std::strcmp(name, "VariableName")) variable_name = text;
    return state_;
}
TSUCHI_Result EngineCall::invoke() {
    if (function_ == 2 && !vsync_read_allowed) return TSUCHI_NOT_READY;
    if (function_ == 1) {
        commands.push_back(command_text);
        if (command_text.rfind("r.VSync ", 0) == 0) current_vsync = std::atoi(command_text.c_str() + 8);
        if (command_text.rfind("rhi.SyncInterval ", 0) == 0) sync_interval = std::atoi(command_text.c_str() + 17);
    }
    return state_;
}
TSUCHI_Result EngineCall::get(const char* name, Value& out) {
    if (function_ != 2 || variable_name != "r.VSync" || std::strcmp(name, "ReturnValue")) return TSUCHI_NOT_FOUND;
    out = integer_value(current_vsync);
    return TSUCHI_OK;
}
bool world_name(Name&) { return false; }
bool name_text(Name, char*, size_t, size_t*) { return false; }
} // namespace tsuchi::game

// Where the game's code continues after each code site's call, by the site's number: the made-up game's functions
// that project and then place say it.
static std::map<uint64_t, uint64_t> callers;
tsuchi::HookInvocation tsuchi::hook_invocation(uint64_t id) {
    const auto found = callers.find(id);
    return {found != callers.end() ? found->second : 0, 1, 1};
}

namespace tsuchi {
void* allocate(size_t bytes) { return std::malloc(bytes); }
void release(void* block) { std::free(block); }
bool on_game_thread() { return game_thread; }
uint64_t current_thread_token() { return 1; }
void lock_wait(uint32_t&) {}
void mod_note(const char*, const char* text) { notes.push_back(text); }
void frame_picture(float fraction, float seconds) { pictures.push_back({fraction, seconds}); }
} // namespace tsuchi

// The hooks the presenter puts on: what each entry leads to, by the hook's own function.
static std::map<uint64_t, void*> hooked;    // entry or code site -> the presenter's function
static std::map<uint64_t, void*> originals; // entry or code site -> the made-up game's own function
static uint64_t site_key(uint32_t site) { return 0x10000 + site; }
TSUCHI_Result tsuchi::hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t, void* function, uint32_t, uint64_t* id, void** original) {
    const auto found = originals.find(entry - main_at());
    if (found == originals.end()) return TSUCHI_NOT_FOUND;
    hooked[entry - main_at()] = function;
    *original = found->second;
    *id = entry;
    return TSUCHI_OK;
}
TSUCHI_Result tsuchi::hook_code_site(TSUCHI_Mod*, uint32_t site, void* function, uint64_t* id, void** original) {
    const auto found = originals.find(site_key(site));
    if (found == originals.end()) return TSUCHI_NOT_FOUND;
    hooked[site_key(site)] = function;
    *original = found->second;
    *id = site;
    return TSUCHI_OK;
}
unsigned tsuchi::hook_sample(HookSample* out, unsigned most, uint32_t* all) {
    if (all) *all = 7;
    if (!most) return 0;
    out[0] = HookSample{"scene_update", 7};
    return 1;
}
template <class F>
static F hook_of(uint64_t key) { return reinterpret_cast<F>(hooked.at(key)); }
// The presenter's hook where it has put one, or the made-up game's own function: at 30 it puts on only what it needs.
template <class F>
static F hook_or_game(uint64_t key) {
    const auto found = hooked.find(key);
    return reinterpret_cast<F>(found != hooked.end() ? found->second : originals.at(key));
}
static uint64_t slot_key(const t::Slot& slot) { return slot.table + slot.offset; }

// The world's update: what it was given, and whether the engine's own frame length was a step's within it.
// A part of an actor that the renderer draws: its place moves along x, and its pose's first bone along x too.
static uint64_t part, part_functions, actor, task, mesh;
static float part_x = 0, part_speed = 0, bone_speed = 0;
static uint32_t bone_every = 1;
// Native evaluation can publish an unchanged pose. Zero keeps the older change-only fixture behavior.
static uint32_t bone_send_every = 0;
static int32_t pose_lod = 0;
static int32_t kernel_lod = 0;
static bool native_pose_pending = false, presenter_pose_pending = false, record_pose_sends = false;
static bool coalesced_native_pose = false, change_revision_in_kernel = false;
static bool change_pose_policy_in_kernel = false;
static uint64_t fixture_pose_master = 0, mesh_pose_owner = 0;
struct PoseSend { bool native; int32_t lod; float given, shown; };
static std::vector<PoseSend> pose_sends;
static bool part_registered = false, part_drawn = true;
static uint32_t draws = 0;
static constexpr uint32_t kBones = 3;
static tsuchi::presenter::BonePose bones[kBones] = {};
static struct { uint64_t data; int32_t count, room; } bone_array{};
static std::vector<float> renderer_x, kernel_x; // the place the renderer got, and the first bone the kernel got
static std::vector<std::vector<float>> kernel_q;  // the first bone's turn the kernel got, while kept
static bool keep_kernel_q = false;
// The first bone's turn about up, in degrees a step, and the loop it wraps at (0: none); and its place's loop.
static float bone_spin = 0, bone_spin_loop = 0, bone_yaw = 0, bone_loop = 0;
static void mark(uint32_t bits) { put<uint32_t>(part + t::primitive::flags, get<uint32_t>(part + t::primitive::flags) | bits); }
static void original_scene_update(void* at, void* sent) {
    const uint64_t s = reinterpret_cast<uint64_t>(at), p = reinterpret_cast<uint64_t>(sent);
    put<uint32_t>(p + t::primitive::flags, get<uint32_t>(p + t::primitive::flags) & ~uint32_t(t::constant::render_transform_dirty));
    // The engine fills its task in, then the patch's native route has it before it is published.
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, part_x, 0, 0, 1};
    const float bounds[7] = {part_x, 0, 0, 10, 10, 10, 17}, attachment[3] = {part_x, 0, 0};
    put<uint64_t>(task + t::transform_task::scene, s);
    put<uint64_t>(task + t::transform_task::proxy, get<uint64_t>(p + t::primitive::proxy));
    std::memcpy(reinterpret_cast<void*>(task + t::transform_task::matrix), matrix, sizeof matrix);
    std::memcpy(reinterpret_cast<void*>(task + t::transform_task::world_bounds), bounds, sizeof bounds);
    std::memcpy(reinterpret_cast<void*>(task + t::transform_task::attachment), attachment, sizeof attachment);
    tsuchi::NativeFrame frame{};
    frame.x[20] = task;
    tsuchi_native_dispatch(&frame, main_at() + t::native_routes[t::native_route::scene_publish].returned);
    renderer_x.push_back(get<float>(task + t::transform_task::matrix + 48));
}
static void original_scene_remove(void*, void*) { part_registered = false; }
static void original_kernel(void*, const void* given, void*, void*, void*, void*, void*, int32_t lod, uint64_t) {
    const uint64_t array = reinterpret_cast<uint64_t>(given);
    kernel_lod = lod;
    kernel_x.push_back(get<float>(get<uint64_t>(array) + 0x10));
    if (keep_kernel_q) {
        const uint64_t data = get<uint64_t>(array);
        kernel_q.push_back({get<float>(data), get<float>(data + 4), get<float>(data + 8), get<float>(data + 12)});
    }
    if (change_revision_in_kernel) {
        put<uint32_t>(part + t::skinned_pose::revision, get<uint32_t>(part + t::skinned_pose::revision) + 1);
        change_revision_in_kernel = false;
    }
    if (change_pose_policy_in_kernel) {
        change_pose_policy_in_kernel = false;
        scene_pose_policy(false);
        scene_pose_policy(true);
    }
}
// The mesh object builds the bones itself from the component's array, as on a build without a bone kernel.
static bool kernel_inline = false;
static void original_mesh(void*, int32_t lod, void*, void*, void*, uint32_t) {
    const uint64_t owner = mesh_pose_owner ? mesh_pose_owner : part;
    const uint64_t given = owner + t::skinned_pose::arrays + uint64_t(get<int32_t>(owner + t::skinned_pose::read_index)) * 16;
    if (kernel_inline) {
        kernel_lod = lod;
        kernel_x.push_back(get<float>(get<uint64_t>(given) + 0x10));
        return;
    }
    hook_of<void (*)(void*, const void*, void*, void*, void*, void*, void*, int32_t, uint64_t)>(0x10000 + t::code_site::bone_kernel)(
        nullptr, reinterpret_cast<void*>(given), nullptr, nullptr, nullptr, nullptr, nullptr, lod, 0);
}
static void send_pose(void* sent) {
    const bool native = native_pose_pending;
    BonePose authoritative[kBones];
    std::memcpy(authoritative, bones, sizeof bones);
    const uint64_t p = reinterpret_cast<uint64_t>(sent);
    put<uint32_t>(p + t::primitive::flags, get<uint32_t>(p + t::primitive::flags) & ~uint32_t(t::constant::render_dynamic_dirty));
    hook_of<void (*)(void*, int32_t, void*, void*, void*, uint32_t)>(t::slot::mesh_update.table + t::slot::mesh_update.offset)(
        reinterpret_cast<void*>(mesh), pose_lod, sent, nullptr, nullptr, 0);
    if (record_pose_sends) {
        CHECK(native || presenter_pose_pending);
        CHECK(std::memcmp(authoritative, bones, sizeof bones) == 0);
        pose_sends.push_back({native, kernel_lod, authoritative[0].t[0], kernel_x.back()});
    }
    native_pose_pending = presenter_pose_pending = false;
}
// The carry counters' component: one counter whose point moves 30 a step along x, and the point each time the
// component's data is sent between steps.
static uint64_t counter_numbers = 0, counter_item = 0;
static bool counters_on = false;
static float counter_x = 0; // where its carried thing is: the game puts the point there each step
static std::vector<float> counter_sent;
static void mark_end(void* p) {
    if (counter_numbers && reinterpret_cast<uint64_t>(p) == counter_numbers && (get<uint32_t>(counter_numbers + t::actor_component::lifecycle_flags) & 0x80))
        counter_sent.push_back(get<float>(counter_item + t::carry_number::place)), put<uint32_t>(counter_numbers + t::actor_component::lifecycle_flags, 3u);
}
static void mark_pose(void* p) {
    const uint64_t at = reinterpret_cast<uint64_t>(p);
    if ((get<uint32_t>(at + t::primitive::flags) & t::constant::render_ready) == t::constant::render_ready) {
        presenter_pose_pending = true;
        mark(uint32_t(t::constant::render_dynamic_dirty));
        if (coalesced_native_pose) {
            // Deferred native finalization can coalesce with our mark before the worker submission.
            put<uint32_t>(part + t::skinned_pose::revision, get<uint32_t>(part + t::skinned_pose::revision) + 1);
            native_pose_pending = true;
            coalesced_native_pose = false;
        }
    }
}
static uint64_t warp_to = 0;
static void original_warp(void*, void*, void*, bool, bool, bool) {
    part_x += float(warp_to);
    mark(uint32_t(t::constant::render_transform_dirty));
}
// The drawer of instances: one instanced mesh of two instances, moved 30 and 60 a step along x.
static uint64_t drawer_object, crowd_part, crowd_data;
static float crowd_x = 0;
static bool crowds_on = false;
static std::vector<float> crowd_shown; // the first instance's x when the renderer's copy is made again
static void original_drawer(void*) {
    for (int i = 0; i < 2; ++i) {
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, crowd_x * float(i + 1), 0, 0, 1};
        std::memcpy(reinterpret_cast<void*>(crowd_data + 64 * i), m, sizeof m);
    }
}
static void mark_render_state(void*) { crowd_shown.push_back(get<float>(crowd_data + 48)); }
// A particle whose old location is 0 and whose location is 30 along x, and what each route left of it.
static uint64_t particle, render_command, renderer;
static float sprite_x = -1, sprite_old_x = -1, mesh_x = -1, offset_x = -1, offset_z = -1;
static bool rendering = false;
static void original_render(void*, void*) {
    tsuchi::NativeFrame frame{};
    frame.x[28] = particle, frame.x[22] = particle;
    const float location[3] = {30, 0, 6}, old[3] = {0, 0, 2};
    std::memcpy(frame.q[3], location, 8), std::memcpy(&frame.x[8], &location[2], 4);
    std::memcpy(frame.q[1], old, 8), std::memcpy(frame.q[2], &old[2], 4);
    tsuchi_native_dispatch(&frame, main_at() + t::native_routes[t::native_route::particle_sprite].returned);
    std::memcpy(&sprite_x, frame.q[3], 4), std::memcpy(&sprite_old_x, frame.q[1], 4);
    frame.x[2] = particle + t::particle::location;
    tsuchi_native_dispatch(&frame, main_at() + t::native_routes[t::native_route::particle_mesh_place].returned);
    mesh_x = get<float>(frame.x[2]);
    std::memcpy(frame.q[0], location, 8);
    tsuchi_native_dispatch(&frame, main_at() + t::native_routes[t::native_route::particle_mesh_xy].returned);
    std::memcpy(&offset_x, frame.q[0], 4);
    std::memcpy(frame.q[2], &location[2], 4);
    tsuchi_native_dispatch(&frame, main_at() + t::native_routes[t::native_route::particle_mesh_z].returned);
    std::memcpy(&offset_z, frame.q[2], 4);
}
// Marks on the screen: a marker widget placed in the viewport and a life gauge, each from a point that moves 60
// a step along x while the camera moves 30. The made-up projection puts a point at its distance along x from the
// view, plus 500; the screen's scale is 2.
static uint64_t marker_widget, gauge_widget, action_widget, bar_widget, icon_slot, icon_widget, hud_slot;
static float marker_x = 0;
static bool marks_on = false;
static bool projection_front = true;
// Entry7's placement is inline, outside NativeTick; its tick still owns queued work and frame timers.
static uint64_t cursor_widget, cursor_component, cursor_owner;
static bool cursor_on = false, cursor_unchanged = false;
static float cursor_anchor = 0;
static uint32_t cursor_steps = 0, cursor_counter = 0, cursor_queue = 1000, cursor_repairs = 0;
static float cursor_timer = 0;
static NativeFrame cursor_frame{};
static uint8_t cursor_stack[0x100] = {};
static void place_cursor() {
    ++cursor_steps;
    cursor_anchor += cursor_unchanged ? 30 : 60;
    put<float>(cursor_component + 0x2A4, cursor_anchor); // authoritative native cursor position
    const uint64_t sp = reinterpret_cast<uint64_t>(cursor_stack);
    std::memset(cursor_stack, 0xA5, sizeof cursor_stack);
    put<uint64_t>(sp + 0x60, cursor_widget);
    put<uint64_t>(sp + 0x68, cursor_component);
    put<uint64_t>(sp + 0x78, main_at() + t::cursor_placement::producer_return);
    float* out = reinterpret_cast<float*>(sp + 0xC0);
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        cursor_anchor, 0, 0, reinterpret_cast<void*>(controller), out, true);
    // Actual producer stores the pixel cache and DPI-adjusted viewport position without either hooked setter.
    put<float>(cursor_widget + t::cursor_placement::screen, out[0] + 12);
    put<float>(cursor_widget + t::cursor_placement::screen + 4, out[1] - 6);
    const bool same = get<float>(cursor_widget + t::cursor_placement::position) == (out[0] + 12) / 2;
    cursor_frame = {};
    cursor_frame.sp = sp, cursor_frame.x[19] = cursor_widget;
    cursor_frame.nzcv = same ? 1ull << 30 : 0;
    const NativeFrame before = cursor_frame;
    tsuchi_native_dispatch(&cursor_frame, main_at() + t::native_routes[t::native_route::cursor_unchanged].returned);
    if (!same) {
        put<float>(cursor_widget + t::cursor_placement::position, (out[0] + 12) / 2);
        put<float>(cursor_widget + t::cursor_placement::position + 4, (out[1] - 6) / 2);
        tsuchi_native_dispatch(&cursor_frame, main_at() + t::native_routes[t::native_route::cursor_written].returned);
    }
    CHECK(!std::memcmp(&before, &cursor_frame, sizeof before));
    CHECK(get<float>(cursor_component + 0x2A4) == cursor_anchor);
}
static float project_x(float x) {
    uint64_t out = block(0x40);
    hook_or_game<void (*)(void*, void*, int32_t)>(slot_key(t::slot::view_point))(reinterpret_cast<void*>(player), reinterpret_cast<void*>(out), 0);
    const float shown = x - get<float>(out) + 500;
    std::free(reinterpret_cast<void*>(out));
    return shown;
}
static bool original_project(float x, float y, float, void*, void* out, bool) {
    const float screen[3] = {project_x(x), y, 100};
    std::memcpy(out, screen, sizeof screen);
    return projection_front;
}
static void original_position(float x, float y, void* widget, bool pixels) {
    const uint64_t w = reinterpret_cast<uint64_t>(widget);
    put<float>(w + t::life_gauge::position, pixels ? x / 2 : x), put<float>(w + t::life_gauge::position + 4, pixels ? y / 2 : y);
}
static void original_translation(float x, float y, void* widget) { original_position(x, y, widget, false); }
static float widget_scale(void*) { return 2; }
static void original_gauge(void* gauge, float) {
    const uint64_t g = reinterpret_cast<uint64_t>(gauge);
    float out[3];
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        get<float>(g + t::life_gauge::anchor), 0, 0, reinterpret_cast<void*>(controller), out, true);
    put<float>(g + t::life_gauge::position, (out[0] + 7) / 2); // with something of its own added
    put<uint8_t>(g + t::life_gauge::shown, 1);
}
static void original_bar(void* gauge, float) {
    const uint64_t g = reinterpret_cast<uint64_t>(gauge);
    if (!get<uint8_t>(g + t::life_bar::shown)) return;
    float out[3];
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        get<float>(g + t::life_bar::anchor), 0, 0, reinterpret_cast<void*>(controller), out, true);
    put<float>(g + t::life_bar::position, (out[0] - 5) / 2);
}
static void original_canvas(float x, float y, void* slot) {
    const uint64_t s = reinterpret_cast<uint64_t>(slot);
    put<float>(s + t::canvas_slot::position, x), put<float>(s + t::canvas_slot::position + 4, y);
}
// A castaway's role icon: its function projects the point over the head, then places the icon in a canvas whose
// units are the screen's over its scale (2), from the canvas's middle and lifted. Between the two, the HUD places
// one of its own in a canvas from another function, which is no mark.
static void place_icon(float point) {
    float out[3];
    callers[t::code_site::world_to_screen.number] = main_at() + 0x150D8FC;
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        point, 0, 0, reinterpret_cast<void*>(controller), out, true);
    callers[t::code_site::canvas_position.number] = main_at() + 0x2400000;
    hook_of<void (*)(float, float, void*)>(site_key(t::code_site::canvas_position))(100, 50, reinterpret_cast<void*>(hud_slot));
    callers[t::code_site::canvas_position.number] = main_at() + 0x150E894;
    hook_of<void (*)(float, float, void*)>(site_key(t::code_site::canvas_position))(out[0] / 2 - 960, out[1] / 2 - 740,
                                                                                 reinterpret_cast<void*>(icon_slot));
    callers.clear();
}
static uint64_t action_projector = 0; // the controller the action marker is projected through, or 0 for the first's
static void original_action(void* marker) {
    const uint64_t m = reinterpret_cast<uint64_t>(marker);
    float out[3];
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        get<float>(m + t::life_gauge::anchor), 0, 0, reinterpret_cast<void*>(action_projector ? action_projector : controller), out, false);
    put<float>(m + t::life_gauge::position, out[0] / 2 + 3);
}
static void place_marks() {
    marker_x += 60;
    float out[3];
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        marker_x, 0, 0, reinterpret_cast<void*>(controller), out, true);
    hook_of<void (*)(float, float, void*, bool)>(site_key(t::code_site::viewport_position))(out[0], out[1], reinterpret_cast<void*>(marker_widget), true);
    put<float>(gauge_widget + t::life_gauge::anchor, marker_x);
    hook_of<void (*)(void*, float)>(slot_key(t::slot::life_gauge_place))(reinterpret_cast<void*>(gauge_widget), 1.0f / 30);
    put<float>(action_widget + t::life_gauge::anchor, marker_x);
    hook_of<void (*)(void*)>(site_key(t::code_site::action_marker_place))(reinterpret_cast<void*>(action_widget));
    put<float>(bar_widget + t::life_bar::anchor, marker_x);
    hook_of<void (*)(void*, float)>(slot_key(t::slot::life_bar_place))(reinterpret_cast<void*>(bar_widget), 1.0f / 30);
    place_icon(marker_x);
}
// A material instance the world's update gives a point each step, as the throw line's are, and the renderer's copy
// of it: the last vector and scalar each push gave it.
static uint64_t line_material = 0;
static bool line_on = false;
static float line_x = 0, line_speed = 60, line_loop = 0; // line_loop: the value wraps back by this past it
static float rendered[4] = {}, rendered_scalar = 0;
static uint8_t line_info[16] = {7, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
static void original_vector_push(void* material, const void* parameter) {
    if (reinterpret_cast<uint64_t>(material) == line_material && !std::memcmp(parameter, line_info, 16))
        std::memcpy(rendered, static_cast<const uint8_t*>(parameter) + t::material_parameter::value, sizeof rendered);
}
static void original_scalar_push(void* material, const void* parameter) {
    if (reinterpret_cast<uint64_t>(material) == line_material && !std::memcmp(parameter, line_info, 16))
        std::memcpy(&rendered_scalar, static_cast<const uint8_t*>(parameter) + t::material_parameter::value, 4);
}
// The game pushes a value only when it changed.
static void set_line(float x) {
    if (x == line_x) return;
    line_x = x;
    alignas(16) uint8_t vector[32] = {}, scalar[32] = {};
    const float value[4] = {x, 1, 2, 1}, width = x / 10;
    std::memcpy(vector, line_info, 16), std::memcpy(vector + 16, value, 16);
    std::memcpy(scalar, line_info, 16), std::memcpy(scalar + 16, &width, 4);
    hook_of<void (*)(void*, const void*)>(site_key(t::code_site::material_vector_push))(reinterpret_cast<void*>(line_material), vector);
    hook_of<void (*)(void*, const void*)>(site_key(t::code_site::material_scalar_push))(reinterpret_cast<void*>(line_material), scalar);
}
static uint32_t steps_seen = 0;
// The world's update moves the part and its pose; the end of the frame sends what changed.
static void move_part() {
    ++steps_seen;
    if (!part_registered) return;
    if (part_speed != 0) part_x += part_speed, mark(uint32_t(t::constant::render_transform_dirty));
    const bool changed = (bone_speed != 0 || bone_spin != 0) && steps_seen % bone_every == 0;
    if (changed) {
        bones[0].t[0] += bone_speed;
        if (bone_loop > 0 && bones[0].t[0] >= bone_loop) bones[0].t[0] -= bone_loop;
        if (bone_spin != 0) {
            bone_yaw += bone_spin;
            if (bone_spin_loop > 0 && bone_yaw >= bone_spin_loop) bone_yaw -= bone_spin_loop;
            const float angles[3] = {0, bone_yaw, 0};
            tsuchi::math::to_quaternion(angles, bones[0].q);
        }
    }
    if (bone_send_every ? steps_seen % bone_send_every == 0 : changed) {
        native_pose_pending = true;
        put<uint32_t>(part + t::skinned_pose::revision, get<uint32_t>(part + t::skinned_pose::revision) + 1);
        if (fixture_pose_master)
            put<uint32_t>(fixture_pose_master + t::skinned_pose::revision, get<uint32_t>(fixture_pose_master + t::skinned_pose::revision) + 1);
        mark(uint32_t(t::constant::render_dynamic_dirty));
    }
}
static void end_of_frame() {
    if (!part_registered) return;
    const uint32_t flags = get<uint32_t>(part + t::primitive::flags);
    if (flags & t::constant::render_transform_dirty)
        hook_of<void (*)(void*, void*)>(t::slot::scene_update.table + t::slot::scene_update.offset)(reinterpret_cast<void*>(scene), reinterpret_cast<void*>(part));
    if (flags & t::constant::render_dynamic_dirty) send_pose(reinterpret_cast<void*>(part));
}
static uint32_t world_updates = 0, npad_reads_in_world = 0;
static uint64_t pikmin_counter = 0;
static bool count_pikmin_in_world = false, publish_pikmin_each_picture = false;
static unsigned counter_calls = 0, counter_full_recounts = 0, counter_classifications = 0;
static int32_t following_pikmin = 60;
static uint64_t counter_cell(unsigned bank, unsigned state) {
    // One real stock type, leaf1, player2; native row0 queries sum type rows1..16.
    return pikmin_counter + bank * t::pikmin_counter::bank_stride + 3 * t::pikmin_counter_row::size +
        t::pikmin_counter::leaf_stride + state * t::pikmin_counter::mode_stride + 2 * t::pikmin_counter::player_stride;
}
static void classify_pikmin() {
    const unsigned bank = get<int32_t>(pikmin_counter + t::pikmin_counter::bank) ? 1 : 0;
    for (unsigned state : {3u, 7u}) put<int32_t>(counter_cell(bank, state), get<int32_t>(counter_cell(bank, state)) + following_pikmin);
    ++counter_classifications;
}
// Original01D8DA10: initialized flags0 publishes/flips/clears, with no actor walk.
// Initial/forced and flags1 recounts walk actors. The tickable manager calls this
// every engine picture, whereas actor01B7E710 classifies only in world updates.
static void original_counter_update(void* raw, uint64_t flags) {
    CHECK(reinterpret_cast<uint64_t>(raw) == pikmin_counter);
    ++counter_calls;
    const bool full = !get<uint8_t>(pikmin_counter + t::pikmin_counter::initialized) ||
        get<uint8_t>(pikmin_counter + t::pikmin_counter::force_recount);
    if (full) {
        std::memset(reinterpret_cast<void*>(pikmin_counter), 0, 2 * t::pikmin_counter::bank_stride);
        put<int32_t>(pikmin_counter + t::pikmin_counter::read_bank, 0);
        put<int32_t>(pikmin_counter + t::pikmin_counter::bank, 0);
    } else if (flags & 1) {
        const int32_t read = get<int32_t>(pikmin_counter + t::pikmin_counter::read_bank);
        put<int32_t>(pikmin_counter + t::pikmin_counter::bank, read);
        std::memset(reinterpret_cast<void*>(pikmin_counter + read * t::pikmin_counter::bank_stride), 0, t::pikmin_counter::bank_stride);
    }
    if (full || (flags & 1)) { classify_pikmin(); ++counter_full_recounts; }
    if (full || !(flags & 1)) {
        const int32_t write = get<int32_t>(pikmin_counter + t::pikmin_counter::bank);
        put<int32_t>(pikmin_counter + t::pikmin_counter::read_bank, write);
        put<int32_t>(pikmin_counter + t::pikmin_counter::bank, int32_t(write == 0));
        if (!full) std::memset(reinterpret_cast<void*>(pikmin_counter + (write == 0 ? 1 : 0) * t::pikmin_counter::bank_stride), 0, t::pikmin_counter::bank_stride);
    } else {
        put<int32_t>(pikmin_counter + t::pikmin_counter::bank, int32_t(get<int32_t>(pikmin_counter + t::pikmin_counter::read_bank) == 0));
    }
    put<uint8_t>(pikmin_counter + t::pikmin_counter::initialized, 1);
    put<uint8_t>(pikmin_counter + t::pikmin_counter::force_recount, 0);
}
static uint64_t now_us = 0;
// What a world's update takes on the clock, and when each picture was drawn.
static uint64_t world_cost_us = 0;
static std::vector<uint64_t> picture_us;
// The presenter's waits: each one's length, and whether it passes on the clock.
static std::vector<uint32_t> waits;
static bool waits_pass = false;
static void wait_us(uint32_t us) {
    waits.push_back(us);
    if (waits_pass) now_us += us;
}
static float world_given = 0;
static double app_delta_within = 0;
static float location_x = 0; // the camera moves 30 a step
static void original_npad(void* reader) { put<uint64_t>(reinterpret_cast<uint64_t>(reader) + t::npad_device::buttons, raw_buttons); }
static uint64_t hung_actor = 0, hung_root = 0; // an actor a mod's widget hangs from, and its root
static float hung_speed = 0;                  // and how far it goes a step
static float hung_x() { return get<float>(hung_root + t::scene_component::location); }
static void original_world(void* at, uint32_t, float seconds) {
    ++world_updates;
    now_us += world_cost_us;
    world_given = seconds;
    app_delta_within = get<double>(main_at() + t::data::app_delta);
    const uint64_t w = reinterpret_cast<uint64_t>(at);
    put<float>(w + t::world::seconds, get<float>(w + t::world::seconds) + seconds);
    put<float>(w + t::world::real_seconds, get<float>(w + t::world::real_seconds) + seconds);
    put<float>(w + t::world::delta_seconds, seconds);
    location_x += 30;
    move_part();
    if (marks_on) place_marks();
    if (hung_actor) put<float>(hung_root + t::scene_component::location, hung_x() + hung_speed);
    if (cursor_on) place_cursor();
    if (line_on) set_line(line_loop > 0 && line_x + line_speed >= line_loop ? line_x + line_speed - line_loop : line_x + line_speed);
    if (crowds_on) crowd_x += 30, hook_of<void (*)(void*)>(site_key(t::code_site::instance_drawer_update))(reinterpret_cast<void*>(drawer_object));
    if (counters_on) counter_x += 30, put<float>(counter_item + t::carry_number::place, counter_x);
    // The game reads its controllers while it updates.
    hook_or_game<void (*)(void*)>(slot_key(t::slot::npad_read))(reinterpret_cast<void*>(device));
    ++npad_reads_in_world;
    if (count_pikmin_in_world) classify_pikmin();
}
static void original_view(void*, void* out, int32_t) {
    const float pose[8] = {location_x, 0, 0, 0, location_x / 10, 0, 90, 90};
    std::memcpy(out, pose, sizeof pose);
}
static void original_family(void* made, const void* from) {
    const uint64_t a = reinterpret_cast<uint64_t>(from), b = reinterpret_cast<uint64_t>(made);
    put<float>(b + t::view_family::world_seconds, get<float>(a + t::view_family_values::world_seconds));
    put<float>(b + t::view_family::delta_seconds, get<float>(a + t::view_family_values::delta_seconds));
    put<float>(b + t::view_family::real_seconds, get<float>(a + t::view_family_values::real_seconds));
}
static float shown[8];
static float family_seconds = 0, family_delta = 0;
// The engine draws: the view, then a family made from the world's times.
static void original_redraw(void*, bool) {
    picture_us.push_back(now_us);
    end_of_frame();
    // The renderer notes in each part it draws when it drew it.
    if (part_registered && part_drawn) put<float>(part + t::primitive::last_drawn, float(++draws));
    uint64_t out = block(0x40);
    hook_or_game<void (*)(void*, void*, int32_t)>(slot_key(t::slot::view_point))(reinterpret_cast<void*>(player), reinterpret_cast<void*>(out), 0);
    std::memcpy(shown, reinterpret_cast<void*>(out), sizeof shown);
    std::free(reinterpret_cast<void*>(out));
    put<float>(values + t::view_family_values::world_seconds, get<float>(world + t::world::seconds));
    put<float>(values + t::view_family_values::delta_seconds, get<float>(world + t::world::delta_seconds));
    put<float>(values + t::view_family_values::real_seconds, get<float>(world + t::world::real_seconds));
    hook_or_game<void (*)(void*, const void*)>(site_key(t::code_site::view_family))(reinterpret_cast<void*>(family), reinterpret_cast<void*>(values));
    family_seconds = get<float>(family + t::view_family::world_seconds);
    family_delta = get<float>(family + t::view_family::delta_seconds);
    // The render thread draws the family: its renderer keeps a copy of the family's world time.
    if (rendering) {
        put<float>(renderer + t::scene_renderer::world_seconds, family_seconds);
        hook_of<void (*)(void*, void*)>(site_key(t::code_site::scene_render))(reinterpret_cast<void*>(render_command), nullptr);
    }
}
static std::map<uint64_t, std::vector<float>> widget_ticks;
static std::map<uint64_t, std::vector<float>> widget_base_ticks;
static std::map<uint64_t, std::vector<float>> widget_script_ticks;
static bool nested_widget_bases = false, nested_numeric_base = false;
// Resolve the genuine native site when offered; baseline has no such hook and runs the original.
static uint64_t numeric_key(uint64_t address) {
    for(unsigned i=0;i<t::code_site_count;++i)if(t::code_sites[i].address==address)return site_key(i);
    return 0x100000000ull+address;
}
static const void* numeric_geometry = nullptr;
template<unsigned I> static void original_numeric(void* widget,const void* geometry,float seconds) {
    CHECK(geometry==numeric_geometry);
    hook_or_game<void (*)(void*,const void*,float)>(site_key(t::code_site::widget_base_tick))(widget,geometry,seconds);
    widget_ticks[reinterpret_cast<uint64_t>(widget)*64+I].push_back(seconds);
}
static void original_widget_script(void* widget, const void*, float seconds) {
    widget_script_ticks[reinterpret_cast<uint64_t>(widget)].push_back(seconds);
}
static void original_widget_base(void* widget, const void* geometry, float seconds) {
    widget_base_ticks[reinterpret_cast<uint64_t>(widget)].push_back(seconds);
    hook_or_game<void (*)(void*, const void*, float)>(site_key(t::code_site::widget_script_tick))(widget, geometry, seconds);
}
template <unsigned I>
static void original_widget(void* widget, const void* geometry, float seconds) {
    widget_ticks[reinterpret_cast<uint64_t>(widget) * 64 + I].push_back(seconds);
    if (I == 3 && nested_numeric_base)
        hook_or_game<void (*)(void*,const void*,float)>(numeric_key(0x024eafa0))(widget,geometry,seconds);
    if (I == 3 && nested_widget_bases)
        hook_of<void (*)(void*, const void*, float)>(site_key(t::code_site::widget_animation_tick))(widget, geometry, seconds);
    if (I == 7) {
        ++cursor_counter;
        if (cursor_queue) --cursor_queue;
        cursor_timer += seconds;
    }
    if ((I == 3 || I == 7 || I == 44) && !(I == 3 && nested_numeric_base))
        hook_or_game<void (*)(void*, const void*, float)>(site_key(t::code_site::widget_base_tick))(widget, geometry, seconds);
}
static void cursor_repair_chain(void* widget, const void* geometry, float seconds) {
    original_widget<7>(widget, geometry, seconds);
    ++cursor_repairs; // stand-in for the already-installed Pikmin HUD repair wrapper
}
static void set_max_fps(void*, float fps) { max_fps = fps; }
static uint32_t style_of(const void*) { return style; }
static void full_key(void* state, const void*) { std::memset(state, 0, 0x28), put<uint64_t>(reinterpret_cast<uint64_t>(state) + 8, raw_buttons); }

// One engine frame of `seconds`: the framework's frame, the world's update, then the drawing.
static bool engine_frame(float seconds) {
    const bool step = presenter_frame(seconds);
    presenter_frame_done();
    hook_of<void (*)(void*, uint32_t, float)>(site_key(t::code_site::world_tick))(reinterpret_cast<void*>(world), 2, seconds);
    if (publish_pikmin_each_picture)
        hook_or_game<void (*)(void*, uint64_t)>(site_key(t::code_site::pikmin_counter_update))(reinterpret_cast<void*>(pikmin_counter), 0);
    if (cursor_on)
        hook_of<void (*)(void*, const void*, float)>(t::slot_group::widget_tick.tables[7] + t::slot_group::widget_tick.offset)(
            reinterpret_cast<void*>(cursor_widget), nullptr, seconds);
    hook_of<void (*)(void*, bool)>(slot_key(t::slot::redraw_viewports))(reinterpret_cast<void*>(engine), true);
    return step;
}

static bool at_step(float fraction) { return fraction < 0.001f; }
static bool halfway(float fraction) { return std::fabs(fraction - 0.5f) < 0.001f; }
static uint64_t clock_us() { return now_us; }

static void build() {
    image = static_cast<unsigned char*>(std::calloc(1, 0x9FC4000));
    engine = block(0x800), user = block(0x100), world = block(0x600), level = block(0x300), world_settings = block(0x400);
    settings_table = block(0x700), player = block(0x40), controller = block(0x300), manager = block(t::camera_manager::size);
    camera = block(0xA00), camera_table = block(0x700), subject_part = block(0x100), subject_actor = block(0x100);
    device = block(0x40), scene = block(0x20), values = block(0x80), family = block(0x80), widget_a = block(0x10), widget_b = block(0x10);
    pikmin_counter = block(t::pikmin_counter::size);
    put<uint64_t>(main_at() + t::data::engine, engine);
    put<uint64_t>(main_at() + t::data::world, world);
    put<double>(main_at() + t::data::app_delta, 1.0 / 30);
    put<uint8_t>(engine + t::engine::flags, 0x41);
    put<float>(engine + t::engine::fixed_rate, 30);
    put<uint64_t>(engine + t::engine::user_settings, user);
    put<float>(user + t::user_settings::frame_rate_limit, 0);
    put<uint64_t>(world + t::world::persistent_level, level);
    put<uint64_t>(level + t::engine_level::world_settings, world_settings);
    put<uint64_t>(world_settings, settings_table);
    put<uint64_t>(settings_table + t::world_settings::fix_delta, main_at() + t::constant::fix_delta);
    put<float>(world_settings + t::world_settings::min_delta, 0.0005f);
    put<float>(world_settings + t::world_settings::max_delta, 0.4f);
    put<uint64_t>(player + t::local_player::controller, controller);
    put<uint64_t>(controller + t::player_controller::camera_manager, manager);
    put<uint64_t>(manager + t::camera_manager::view_target, camera);
    put<uint64_t>(camera, camera_table);
    put<uint64_t>(camera_table + t::game_camera::subject_finder, main_at() + t::constant::camera_subject);
    put<uint64_t>(camera + t::game_camera::subject, subject_part);
    put<uint64_t>(camera + t::game_camera::subject_owner, subject_actor);
    put<uint64_t>(subject_part + t::scene_component::owner, subject_actor);
    put<uint32_t>(device + t::npad_device::id, 0);
    put<uint64_t>(values + t::view_family_values::scene, scene);
    put<uint64_t>(scene + t::scene_world::world, world);
    put<uint8_t>(values + t::view_family_values::flags, uint8_t(t::constant::view_times_set));
    originals[site_key(t::code_site::world_tick)] = reinterpret_cast<void*>(&original_world);
    originals[site_key(t::code_site::pikmin_counter_update)] = reinterpret_cast<void*>(&original_counter_update);
    originals[site_key(t::code_site::view_family)] = reinterpret_cast<void*>(&original_family);
    originals[site_key(t::code_site::widget_animation_tick)] = reinterpret_cast<void*>(&original_widget<44>);
    originals[site_key(t::code_site::widget_base_tick)] = reinterpret_cast<void*>(&original_widget_base);
    originals[site_key(t::code_site::widget_script_tick)] = reinterpret_cast<void*>(&original_widget_script);
    originals[numeric_key(0x024eafa0)] = reinterpret_cast<void*>(&original_numeric<45>);
    originals[numeric_key(0x022e14f0)] = reinterpret_cast<void*>(&original_numeric<46>);
    originals[slot_key(t::slot::redraw_viewports)] = reinterpret_cast<void*>(&original_redraw);
    originals[slot_key(t::slot::view_point)] = reinterpret_cast<void*>(&original_view);
    originals[slot_key(t::slot::npad_read)] = reinterpret_cast<void*>(&original_npad);
    for (unsigned i = 0; i < t::slot_group::widget_tick.table_count; ++i)
        originals[t::slot_group::widget_tick.tables[i] + t::slot_group::widget_tick.offset] =
            i == 3 ? reinterpret_cast<void*>(&original_widget<3>) : i == 7 ? reinterpret_cast<void*>(&cursor_repair_chain) :
                                                                       reinterpret_cast<void*>(&original_widget<0>);
    part = block(0x700), part_functions = block(0x400), actor = block(0x200), task = block(0x100), mesh = block(0x40);
    put<uint64_t>(part, part_functions);
    put<uint32_t>(part + t::primitive::flags, uint32_t(t::constant::render_ready));
    t::fn::mark_end_of_frame = &mark_end;
    t::fn::mark_pose_dirty = &mark_pose;
    put<uint64_t>(actor + t::engine_actor::root, part);
    bone_array = {reinterpret_cast<uint64_t>(bones), int32_t(kBones), int32_t(kBones)};
    std::memcpy(reinterpret_cast<void*>(part + t::skinned_pose::arrays), &bone_array, sizeof bone_array);
    put<uint32_t>(part + t::skinned_pose::revision, 1);
    for (auto& bone : bones) bone.q[3] = 1, bone.s[0] = bone.s[1] = bone.s[2] = 1;
    originals[t::slot::scene_update.table + t::slot::scene_update.offset] = reinterpret_cast<void*>(&original_scene_update);
    originals[t::slot::scene_remove.table + t::slot::scene_remove.offset] = reinterpret_cast<void*>(&original_scene_remove);
    originals[t::slot::mesh_update.table + t::slot::mesh_update.offset] = reinterpret_cast<void*>(&original_mesh);
    originals[site_key(t::code_site::bone_kernel)] = reinterpret_cast<void*>(&original_kernel);
    originals[site_key(t::code_site::actor_warp)] = reinterpret_cast<void*>(&original_warp);
    originals[site_key(t::code_site::instance_drawer_update)] = reinterpret_cast<void*>(&original_drawer);
    originals[site_key(t::code_site::scene_render)] = reinterpret_cast<void*>(&original_render);
    const uint64_t framework = block(0x1000), holder = block(0x100), components = block(0x10);
    drawer_object = block(0x300), crowd_part = block(0x600), crowd_data = block(128);
    put<uint64_t>(main_at() + t::data::game_framework, framework);
    put<uint64_t>(framework + t::game_framework::instance_holder, holder);
    put<uint64_t>(holder + t::instance_holder::drawer, drawer_object);
    put<uint64_t>(drawer_object, main_at() + t::constant::instance_drawer_table);
    put<uint64_t>(drawer_object + t::instance_drawer::components, components);
    put<int32_t>(drawer_object + t::instance_drawer::count, 1);
    put<uint64_t>(components, crowd_part);
    put<uint64_t>(crowd_part + t::instanced_mesh::instances, crowd_data);
    put<int32_t>(crowd_part + t::instanced_mesh::count, 2);
    t::fn::mark_render_state = &mark_render_state;
    counter_numbers = block(0x100), counter_item = block(0x80);
    const uint64_t counter_entries = block(0x18);
    put<uint64_t>(framework + t::game_framework::carry_numbers, counter_numbers);
    put<uint64_t>(counter_numbers + t::carry_numbers::entries, counter_entries);
    put<int32_t>(counter_numbers + t::carry_numbers::count, 1);
    put<uint32_t>(counter_numbers + t::actor_component::lifecycle_flags, 3u);
    put<uint64_t>(counter_entries, counter_item);
    t::fn::widget_scale = &widget_scale;
    marker_widget = block(0x800), gauge_widget = block(0x800), action_widget = block(0x800), bar_widget = block(0x800);
    icon_slot = block(0x100), icon_widget = block(0x800), hud_slot = block(0x100);
    line_material = block(0x200);
    put<uint64_t>(icon_slot + t::canvas_slot::content, icon_widget);
    put<uint8_t>(bar_widget + t::life_bar::shown, 1);
    cursor_widget = block(0x800), cursor_component = block(0x800), cursor_owner = block(0x600);
    put<uint64_t>(cursor_widget, main_at() + t::cursor_placement::widget_table);
    put<uint64_t>(cursor_widget + t::cursor_placement::ready, 1);
    put<uint64_t>(cursor_component, main_at() + t::cursor_placement::component_table);
    put<uint64_t>(cursor_component + t::throw_cursor::owner, cursor_owner);
    put<uint64_t>(cursor_owner + t::cursor_placement::owner_controller, controller);
    originals[site_key(t::code_site::action_marker_place)] = reinterpret_cast<void*>(&original_action);
    originals[site_key(t::code_site::world_to_screen)] = reinterpret_cast<void*>(&original_project);
    originals[site_key(t::code_site::viewport_position)] = reinterpret_cast<void*>(&original_position);
    originals[site_key(t::code_site::render_translation)] = reinterpret_cast<void*>(&original_translation);
    originals[slot_key(t::slot::life_gauge_place)] = reinterpret_cast<void*>(&original_gauge);
    originals[slot_key(t::slot::life_bar_place)] = reinterpret_cast<void*>(&original_bar);
    originals[site_key(t::code_site::canvas_position)] = reinterpret_cast<void*>(&original_canvas);
    originals[site_key(t::code_site::material_vector_push)] = reinterpret_cast<void*>(&original_vector_push);
    originals[site_key(t::code_site::material_scalar_push)] = reinterpret_cast<void*>(&original_scalar_push);
    particle = block(0x40), renderer = block(0x100), render_command = block(0x10);
    const float old[3] = {0, 0, 2}, location[3] = {30, 0, 6};
    std::memcpy(reinterpret_cast<void*>(particle + t::particle::old_location), old, sizeof old);
    std::memcpy(reinterpret_cast<void*>(particle + t::particle::location), location, sizeof location);
    put<uint64_t>(render_command, renderer);
    t::fn::set_max_fps = &set_max_fps;
    t::fn::npad_style_set = &style_of;
    t::fn::npad_full_key_state = &full_key;
}

// ---- The clock alone ---------------------------------------------------------------------------------

static void clock_alone() {
    // At any rate from 30 up, thirty steps a second, never two in one frame, and pictures never go back.
    for (double rate : {30.0, 45.0, 60.0, 90.0, 120.0, 144.0, 240.0}) {
        Clock clock;
        double last = -1;
        bool forward = true, inside = true;
        uint64_t before = 0;
        for (int i = 0; i < int(rate * 10); ++i) {
            const Frame f = clock.advance(1.0 / rate);
            CHECK(clock.steps() - before <= 1);
            before = clock.steps();
            const double at = double(clock.steps()) - 1 + f.fraction;
            forward = forward && at >= last - 1e-9;
            inside = inside && f.fraction >= 0 && f.fraction <= 1;
            last = at;
        }
        CHECK(forward && inside);
        CHECK(clock.steps() >= 299 && clock.steps() <= 300);
    }
    // At 60 the steps alternate exactly, with pictures at the middle and at the step before.
    {
        Clock clock;
        clock.advance(1.0 / 60);
        std::vector<float> seen;
        std::vector<bool> stepped;
        for (int i = 0; i < 8; ++i) {
            const Frame f = clock.advance(1.0 / 60 + (i % 2 ? 0.0004 : -0.0004)); // within a millisecond of a sixtieth
            seen.push_back(f.fraction), stepped.push_back(f.step);
        }
        for (int i = 1; i < 8; ++i) CHECK(stepped[i] != stepped[i - 1]);
        for (int i = 1; i < 8; ++i) CHECK(std::fabs(std::fabs(seen[i] - seen[i - 1]) - 0.5f) < 0.02f);
    }
    // A rate a little under 60 on the clock (frames of 16.78 ms) still steps thirty times a second of that clock,
    // a step never twice in a frame, and the pictures keep within a hundredth of a step of where they would be.
    {
        Clock clock;
        const double frame = 0.01678;
        uint64_t before = 0;
        bool once = true;
        for (int i = 0; i < 6000; ++i) {
            clock.advance(frame);
            once = once && clock.steps() - before <= 1;
            before = clock.steps();
        }
        const double seconds = 6000 * frame;
        CHECK(once);
        CHECK(std::fabs(double(clock.steps()) - seconds * 30) <= 1.5);
    }
    // Below 30, one step a frame and no more: the game slows down as it does without the presenter.
    {
        Clock clock;
        for (int i = 0; i < 200; ++i) CHECK(clock.advance(0.05).step);
        CHECK(clock.steps() == 200);
        CHECK(clock.owed() <= kStep + 1e-12);
    }
    // A long frame, and the first picture, are shown at the newest step; the frame after does not go back.
    {
        Clock clock;
        CHECK(clock.advance(1.0 / 60).fraction == 1);
        clock.advance(1.0 / 60), clock.advance(1.0 / 60);
        const Frame long_frame = clock.advance(0.035);
        CHECK(long_frame.fraction == 1);
        const uint64_t steps = clock.steps();
        const Frame next = clock.advance(1.0 / 60);
        CHECK(double(clock.steps()) - 1 + next.fraction >= double(steps) - 1e-9);
        clock.forget();
        CHECK(clock.advance(1.0 / 60).fraction == 1);
    }
    // A frame the world's settings hold to their limits counts as that long.
    {
        Clock clock;
        CHECK(clock.advance(0.001, 0.02, 0.4).step == false);
        CHECK(std::fabs(clock.owed() - 0.02) < 1e-9);
    }
    // Bad lengths count as one step, never as no time or forever.
    {
        Clock clock;
        CHECK(clock.advance(-1).step);
        CHECK(clock.advance(NAN).step);
    }
}

static void camera_alone() {
    CameraHistory history;
    CameraPose a{{0, 0, 0}, {0, 170, 0}, 90, 90}, b{{30, 0, 0}, {0, -170, 0}, 80, 80};
    CameraPose out;
    CHECK(!history.sample(0.5f, out));
    history.capture(1, a, false);
    history.sample(0.5f, out);
    CHECK(out.location[0] == 0); // one step only: shown as it is
    history.capture(2, b, false);
    history.capture(2, a, false); // the same step again changes nothing
    history.sample(0.5f, out);
    CHECK(std::fabs(out.location[0] - 15) < 1e-4f && std::fabs(out.fov - 85) < 1e-4f);
    // The short way round: from 170 to -170 through 180, not through 0.
    CHECK(std::fabs(std::fabs(out.rotation[1]) - 180) < 0.05f);
    history.sample(1, out);
    CHECK(!std::memcmp(&out, &b, sizeof out));
    // A cut, a gap of more than one step, and a jump of more than ten meters show the new view at once.
    history.capture(3, a, true);
    CHECK(!history.paired());
    history.capture(5, b, false);
    CHECK(!history.paired());
    CameraPose far = b;
    far.location[0] += 2000;
    history.capture(6, far, false);
    CHECK(!history.paired());
    history.capture(7, far, false);
    CHECK(history.paired());
}

// The yaw of a turn about up, in degrees from -180 to 180.
static float yaw_of(const float q[4]) {
    float angles[3];
    tsuchi::math::to_angles(q, angles);
    return angles[1];
}
static float within_half(float degrees) { return tsuchi::math::within_half_turn(degrees); }

static void snaps_alone() {
    // A bone spun the same each step, its angle wrapping past 180 and its quaternion's sign flipping every step,
    // however fast: never a snap, and every picture between is the short way, halfway at a half step.
    for (float spin : {19.0f, 100.0f, 170.0f}) {
        BonePose a{}, b{}, shown;
        BoneMotion motion{};
        b.q[3] = 1, b.s[0] = b.s[1] = b.s[2] = 1;
        float yaw = 0;
        uint32_t snaps = 0;
        bool short_way = true;
        for (unsigned step = 0; step < 60; ++step) {
            a = b;
            yaw = within_half(yaw + spin);
            const float angles[3] = {0, yaw, 0};
            tsuchi::math::to_quaternion(angles, b.q);
            if (step & 1) for (float& q : b.q) q = -q;
            snaps += mark_snaps(&a, &b, 1, 1, true, &motion);
            blend_bones(&a, &b, 1, 0.5f, &shown, &motion);
            short_way = short_way && std::fabs(within_half(yaw_of(shown.q) - (yaw - spin / 2))) < 0.05f;
        }
        CHECK(snaps == 0 && short_way);
    }
    // A flower that spins a share of a turn and starts again, as one whose petals look alike after a third or a
    // fifth of a turn does: each wrap is a snap, shown at the newer step at once, and no other step is.
    for (float loop : {120.0f, 72.0f}) {
        for (float spin : {4.0f, 10.0f}) {
            BonePose a{}, b{}, shown;
            BoneMotion motion{};
            b.q[3] = 1, b.s[0] = b.s[1] = b.s[2] = 1;
            float yaw = 0;
            unsigned wraps = 0, cut_wraps = 0, other_cuts = 0;
            for (unsigned step = 0; step < 200; ++step) {
                a = b;
                yaw += spin;
                const bool wrap = yaw >= loop;
                if (wrap) yaw -= loop;
                const float angles[3] = {0, yaw, 0};
                tsuchi::math::to_quaternion(angles, b.q);
                const bool cut = mark_snaps(&a, &b, 1, 1, true, &motion) == 1;
                if (step == 0) continue; // a bone's first change is not judged
                wraps += wrap, cut_wraps += wrap && cut, other_cuts += !wrap && cut;
                blend_bones(&a, &b, 1, 0.5f, &shown, &motion);
                const float expected = wrap ? yaw : yaw - spin / 2;
                CHECK(std::fabs(within_half(yaw_of(shown.q) - expected)) < 0.05f);
            }
            CHECK(wraps >= 5 && cut_wraps == wraps && other_cuts == 0);
        }
    }
    // A place that slides and starts again, and a size that pops back from nothing: each wrap is a snap.
    {
        BonePose a{}, b{}, shown;
        BoneMotion motion{};
        b.q[3] = 1, b.s[0] = b.s[1] = b.s[2] = 1;
        unsigned wraps = 0, cut_wraps = 0, other_cuts = 0;
        for (unsigned step = 0; step < 200; ++step) {
            a = b;
            b.t[0] += 10;
            bool wrap = b.t[0] >= 300;
            if (wrap) b.t[0] -= 300;
            // Shrinks to nothing over 20 steps and stays so, then is whole again at once.
            const unsigned phase = step % 40;
            const float size = phase < 20 ? 1 - float(phase) / 20 : phase < 39 ? 0 : 1;
            wrap = wrap || phase == 39;
            for (float& part : b.s) part = size;
            const bool cut = mark_snaps(&a, &b, 1, 1, true, &motion) == 1;
            if (step == 0) continue;
            wraps += wrap, cut_wraps += wrap && cut, other_cuts += !wrap && cut;
            blend_bones(&a, &b, 1, 0.5f, &shown, &motion);
            if (wrap) CHECK(!std::memcmp(&shown, &b, sizeof shown));
        }
        CHECK(wraps >= 10 && cut_wraps == wraps && other_cuts == 0);
    }
    // Ordinary motion is never a snap: a swing to and fro, a turn that speeds up from rest to 120 degrees a step
    // and keeps on, and a fast start from that.
    {
        BonePose a{}, b{};
        BoneMotion motion{};
        b.q[3] = 1, b.s[0] = b.s[1] = b.s[2] = 1;
        uint32_t snaps = 0;
        float yaw = 0, speed = 0;
        for (unsigned step = 0; step < 220; ++step) {
            a = b;
            if (step < 100) yaw = 60 * tsuchi::math::sine(float(step) * 2 * tsuchi::math::kPi / 40);
            else if (step < 120) speed = speed > 0 ? speed * 1.8f : 2, yaw = within_half(yaw + (speed > 120 ? 120 : speed));
            else if (step < 200) yaw = within_half(yaw + 120);
            else if (step == 200) b.t[0] += 90, yaw = within_half(yaw + 120); // a move from rest, still turning
            else yaw = within_half(yaw + 120);
            const float angles[3] = {0, yaw, 0};
            tsuchi::math::to_quaternion(angles, b.q);
            snaps += mark_snaps(&a, &b, 1, 1, true, &motion);
        }
        CHECK(snaps == 0);
        // A pose made every four steps, turning 20 degrees a step: 80 between its two steps is not a snap.
        BoneMotion every{};
        a = b = BonePose{};
        b.q[3] = 1;
        yaw = 0;
        for (unsigned update = 0; update < 20; ++update) {
            a = b;
            yaw = within_half(yaw + 80);
            const float angles[3] = {0, yaw, 0};
            tsuchi::math::to_quaternion(angles, b.q);
            snaps += mark_snaps(&a, &b, 1, 4, true, &every);
        }
        CHECK(snaps == 0);
        // A zeroed bone, as the unused rows of a pose are: no turn, no snap.
        BonePose zero{};
        BoneMotion none{};
        for (unsigned step = 0; step < 4; ++step) snaps += mark_snaps(&zero, &zero, 1, 1, true, &none);
        CHECK(snaps == 0 && none.now.turn == 0);
    }
    // The same step made again: the newest change is taken again against the same step before.
    {
        BonePose a{}, b{};
        BoneMotion motion{};
        a.q[3] = b.q[3] = 1;
        b.t[0] = 5;
        mark_snaps(&a, &b, 1, 1, true, &motion);
        a = b, b.t[0] = 10;
        CHECK(mark_snaps(&a, &b, 1, 1, true, &motion) == 0);
        b.t[0] = 300; // remade at once far away
        CHECK(mark_snaps(&a, &b, 1, 1, false, &motion) == 1 && motion.before.move == 5);
        b.t[0] = 15; // and remade again where it should be
        CHECK(mark_snaps(&a, &b, 1, 1, false, &motion) == 0);
    }
    // A part's place, as a root's: the same rule.
    {
        const Change steady{0.1f, 0, 0}, wrapped{1.5f, 0, 0}, swing{0.3f, 0, 0}, popped{0, 0, 1};
        CHECK(!snapped(steady, steady) && snapped(wrapped, steady) && !snapped(wrapped, Change{0.5f, 0, 0}) && !snapped(swing, Change{}));
        CHECK(snapped(popped, Change{}) && !snapped(Change{0, 99, 0}, Change{}) && snapped(Change{0, 101, 0}, Change{}));
    }
}

static void widgets_alone() {
    WidgetPaces paces;
    float seconds = 1.0f / 60;
    CHECK(paces.admit(1, 0, 1, seconds) && seconds == 1.0f / 60);
    seconds = 1.0f / 60;
    CHECK(!paces.admit(1, 0, 1, seconds));
    seconds = 1.0f / 60;
    CHECK(paces.admit(1, 0, 2, seconds) && std::fabs(seconds - 1.0f / 30) < 1e-7f);
    seconds = 1.0f / 60;
    CHECK(paces.admit(1, 1, 2, seconds)); // another entry of the same widget is its own
    // A full table lets a widget tick as it would without the presenter.
    for (uint64_t key = 10; key < 10 + WidgetPaces::kRoom; ++key) seconds = 0.01f, paces.admit(key, 0, 2, seconds);
    seconds = 0.01f;
    CHECK(paces.admit(999, 0, 2, seconds));
    seconds = 0.01f;
    CHECK(paces.admit(999, 0, 2, seconds));
}

static void animation_periods_alone() {
    // A cyclic turn crosses its authored wrap and may change quaternion sign.
    // Compare the shown direction with a continuous turn over four loops;
    // preserve both authoritative input buffers, including their packed tails.
    for (unsigned keys : {19u, 49u}) {
        for (unsigned step = 0; step < 4 * keys; ++step) {
            BonePose a{}, b{}, shown;
            a.s[0] = a.s[1] = a.s[2] = b.s[0] = b.s[1] = b.s[2] = 1;
            a.t[0] = float(step), b.t[0] = float(step + 1);
            a.unused[0] = 17, b.unused[0] = 23;
            const float from[3] = {0, float(step % keys) * 360 / keys, 0};
            const float to[3] = {0, float((step + 1) % keys) * 360 / keys, 0};
            math::to_quaternion(from, a.q), math::to_quaternion(to, b.q);
            if (step & 1) for (float& q : b.q) q = -q;
            const BonePose saved_a = a, saved_b = b;
            for (float fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                blend_bones(&a, &b, 1, fraction, &shown);
                const float angles[3] = {0, (float(step % keys) + fraction) * 360 / keys, 0};
                float expected[4];
                math::to_quaternion(angles, expected);
                float cosine = 0;
                for (unsigned i = 0; i < 4; ++i) cosine += shown.q[i] * expected[i];
                CHECK(std::fabs(std::fabs(cosine) - 1) < 0.0001f);
                CHECK(std::fabs(shown.t[0] - step - fraction) < 0.00001f);
                CHECK(shown.unused[0] == 23);
            }
            CHECK(!std::memcmp(&a, &saved_a, sizeof a) && !std::memcmp(&b, &saved_b, sizeof b));
        }
    }
    // Materials using either view clock must see one second per simulation
    // second. Check repeated families and the delta sum as well as absolute
    // clocks; a correct absolute time alone would miss a full-step delta on
    // every intervening picture. Zero denotes an irregular uncapped schedule.
    for (unsigned fps : {30u, 60u, 120u, 144u, 0u}) {
        Clock simulation;
        ViewTime view;
        WorldTimes world_times{0, kStepSeconds, 100};
        const Frame first = simulation.advance(kStep);
        view.capture(simulation.steps(), world_times);
        WorldTimes shown{};
        CHECK(view.sample(0, simulation.steps(), first.fraction, world_times, shown));
        double elapsed = 0, deltas = 0;
        const double intervals[] = {1.0 / 73, 1.0 / 141, 1.0 / 90, 1.0 / 104};
        for (unsigned frame = 1; elapsed < 20; ++frame) {
            const double seconds = fps ? 1.0 / fps : intervals[frame % 4];
            elapsed += seconds;
            const Frame next = simulation.advance(seconds);
            if (next.step) {
                world_times.seconds += kStepSeconds, world_times.real += kStepSeconds;
                view.capture(simulation.steps(), world_times);
            }
            const WorldTimes before = shown;
            CHECK(view.sample(frame, simulation.steps(), next.fraction, world_times, shown));
            CHECK(shown.seconds >= before.seconds && shown.real >= before.real && shown.delta >= 0);
            CHECK(shown.seconds <= world_times.seconds && world_times.seconds - shown.seconds <= kStepSeconds + 0.0001f);
            deltas += shown.delta;
            WorldTimes repeated;
            CHECK(view.sample(frame, simulation.steps(), next.fraction, world_times, repeated) && same_bits(repeated, shown));
        }
        CHECK(std::fabs(deltas - shown.seconds) < 0.001);
        CHECK(std::fabs((shown.real - 100) - shown.seconds) < 0.01);
        CHECK(std::fabs(shown.seconds - elapsed) <= kStep + 0.001);
    }
    // The world's step gets shorter, as it does when time is slowed: a picture that crosses the step is given the
    // world time it moved on, half of the longer step and half of the shorter one.
    {
        ViewTime view;
        WorldTimes world_times{0, 1.0f / 30, 0};
        WorldTimes shown{};
        view.capture(1, world_times);
        world_times.seconds += 1.0f / 30, world_times.real += 1.0f / 30;
        view.capture(2, world_times);
        CHECK(view.sample(0, 2, 0.5f, world_times, shown) && std::fabs(shown.seconds - 1.0f / 60) < 1e-6f);
        const float before = shown.seconds;
        world_times.seconds += 1.0f / 60, world_times.delta = 1.0f / 60, world_times.real += 1.0f / 30;
        view.capture(3, world_times);
        CHECK(view.sample(1, 3, 0.5f, world_times, shown));
        CHECK(std::fabs(shown.seconds - before - 0.025f) < 1e-6f && std::fabs(shown.delta - 0.025f) < 1e-6f);
    }
    std::puts("loop and view clock conservation passed");
}

// ---- The presenter against the made-up game ------------------------------------------------------------

static void at_thirty() {
    // Without a settings file nothing of the game changes: every frame steps, at its own length, shown as it is.
    presenter_settings();
    CHECK(presenter_limit_wanted() == 30);
    for (int i = 0; i < 10; ++i) CHECK(engine_frame(1.0f / 30));
    CHECK(world_updates == 10 && world_given == 1.0f / 30);
    CHECK(commands.empty() && max_fps == -1 && get<uint8_t>(engine + t::engine::flags) == 0x41);
    CHECK(pictures.size() == 10 && pictures.back().first == 1);
    CHECK(shown[0] == location_x); // the camera as the game has it
    // At 30 only the world's update and the drawing are hooked: the rest would do nothing and cost every call.
    CHECK(hooked.size() == 2 && !hooked.count(slot_key(t::slot::scene_update)) && !hooked.count(slot_key(t::slot::view_point)));
}

static void at_sixty() {
    // Asked for 60: the fixed rate is let go, the engine and the player's settings are told 60, and the
    // system's interval between pictures is one.
    now_us = 5000000;
    engine_frame(1.0f / 30);
    put<float>(world + t::world::seconds, 0);
    put<float>(world + t::world::real_seconds, 0);
    const uint32_t updates_before = world_updates;
    const size_t pictures_before = pictures.size() - 1;
    CHECK(presenter_limit() == 60);
    CHECK(commands.size() == 1 && commands[0] == (initial_vsync ? "rhi.SyncInterval 1" : "rhi.SyncInterval 0"));
    CHECK(current_vsync == initial_vsync && native_swap_interval() == (initial_vsync ? 1 : 0));
    CHECK(max_fps == 60 && get<float>(user + t::user_settings::frame_rate_limit) == 60);
    CHECK((get<uint8_t>(engine + t::engine::flags) & 0x40) == 0);
    // Now 600 frames of a sixtieth: 300 steps, each told the world as exactly a thirtieth, with the engine's
    // frame length a thirtieth within it and back after.
    put<double>(main_at() + t::data::app_delta, 1.0 / 60);
    float sum = 0;
    bool exact = true, never_twice = true;
    int steps = 0;
    for (int i = 0; i < 600; ++i) {
        const uint32_t before = world_updates;
        const bool step = engine_frame(1.0f / 60);
        steps += step;
        never_twice = never_twice && world_updates - before <= 1;
        if (world_updates != before) {
            sum += 1.0f / 30;
            exact = exact && world_given == 1.0f / 30 && app_delta_within == 1.0 / 30;
        }
        // The engine asks twice in one frame: still one update.
        if (step) hook_of<void (*)(void*, uint32_t, float)>(site_key(t::code_site::world_tick))(reinterpret_cast<void*>(world), 2, 1.0f / 60);
        now_us += 16667;
    }
    CHECK(steps >= 299 && steps <= 301);
    CHECK(world_updates - updates_before == uint32_t(steps));
    CHECK(exact && never_twice);
    CHECK(get<double>(main_at() + t::data::app_delta) == 1.0 / 60);
    CHECK(get<float>(world + t::world::seconds) == sum); // the world's time is the sum of its steps, bit for bit
    // A picture for every frame, alternately half way and at the step before.
    CHECK(pictures.size() - pictures_before == 601);
    const auto& last = pictures[pictures.size() - 1], &before_last = pictures[pictures.size() - 2];
    CHECK((at_step(last.first) && halfway(before_last.first)) || (halfway(last.first) && at_step(before_last.first)));
    CHECK(std::fabs(last.second - 1.0f / 60) < 1e-6f);
}

static void camera_in_game() {
    // Half way between two steps the view is half way between their views.
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    const float newest = location_x;
    bool half = false;
    for (int i = 0; i < 2; ++i) {
        engine_frame(1.0f / 60);
        if (halfway(pictures.back().first)) half = std::fabs(shown[0] - (location_x - 15)) < 1e-3f;
        else CHECK(std::fabs(shown[0] - (location_x - 30)) < 1e-3f);
    }
    CHECK(half && newest > 0);
    // A camera cut, a new view target, a new subject and a new projection each show the newest view at once.
    auto newest_shown = [](auto change) {
        while (pictures.empty() || !at_step(pictures.back().first)) engine_frame(1.0f / 60);
        change();
        engine_frame(1.0f / 60); // half way, but the history starts again
        return std::fabs(shown[0] - location_x) < 1e-3f;
    };
    CHECK(newest_shown([] { put<uint8_t>(manager + t::camera_manager::cut, 4); }));
    put<uint8_t>(manager + t::camera_manager::cut, 0);
    const uint64_t other = block(0xA00);
    std::memcpy(reinterpret_cast<void*>(other), reinterpret_cast<void*>(camera), 0xA00);
    CHECK(newest_shown([&] { put<uint64_t>(manager + t::camera_manager::view_target, other); }));
    const uint64_t follower = block(0x100);
    CHECK(newest_shown([&] {
        put<uint64_t>(other + t::game_camera::subject_owner, follower);
        put<uint64_t>(subject_part + t::scene_component::owner, follower);
    }));
    // The projection is the view's own; the game's view sets it, so a change is made through it.
    // After a reset the next pair blends again.
    engine_frame(1.0f / 60), engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    if (halfway(pictures.back().first)) CHECK(std::fabs(shown[0] - (location_x - 15)) < 1e-3f);
}

static void view_clocks() {
    // The family's clocks move on every picture by the share of a step the picture moved, and never back.
    float last = -1;
    bool forward = true, each = true;
    for (int i = 0; i < 20; ++i) {
        engine_frame(1.0f / 60);
        forward = forward && family_seconds >= last;
        each = each && std::fabs(family_delta - 1.0f / 60) < 1e-5f;
        last = family_seconds;
    }
    CHECK(forward && each);
    // A changed world forgets them: the first family after shows the world's own.
    const uint64_t other_world = block(0x600);
    std::memcpy(reinterpret_cast<void*>(other_world), reinterpret_cast<void*>(world), 0x600);
    put<uint64_t>(main_at() + t::data::world, other_world);
    put<uint64_t>(scene + t::scene_world::world, other_world);
    world = other_world;
    engine_frame(1.0f / 60);
    CHECK(pictures.back().first == 1);
    CHECK(family_seconds == get<float>(world + t::world::seconds));
}

static void widgets_in_game() {
    // A widget that counts frames ticks once a step with both frames' length; Slate asks every picture.
    auto tick = [](unsigned entry, uint64_t widget) {
        const uint64_t key = entry == 44 ? site_key(t::code_site::widget_animation_tick)
                                         : t::slot_group::widget_tick.tables[entry] + t::slot_group::widget_tick.offset;
        hook_of<void (*)(void*, const void*, float)>(key)(reinterpret_cast<void*>(widget), nullptr, 1.0f / 60);
    };
    widget_ticks.clear();
    widget_base_ticks.clear();
    widget_script_ticks.clear();
    for (int i = 0; i < 60; ++i) {
        engine_frame(1.0f / 60);
        tick(3, widget_a), tick(3, widget_b), tick(44, widget_a);
    }
    const auto& a = widget_ticks[widget_a * 64 + 3], &b = widget_ticks[widget_b * 64 + 3], &c = widget_ticks[widget_a * 64 + 44];
    CHECK(a.size() >= 29 && a.size() <= 31 && b.size() == a.size() && c.size() == a.size());
    float total = 0;
    for (float s : a) total += s;
    CHECK(std::fabs(total - 1.0f) < 0.05f);
    CHECK(std::fabs(a[a.size() / 2] - 1.0f / 30) < 1e-6f);
    // The native UMG base remains at the picture rate, once per widget even when two paced bodies reach it.
    const auto& visual_a = widget_base_ticks[widget_a], &visual_b = widget_base_ticks[widget_b];
    CHECK(visual_a.size() == 60 && visual_b.size() == 60);
    total = 0;
    for (float s : visual_a) total += s;
    CHECK(std::fabs(total - 1.0f) < 1e-5f);
    for (float s : visual_a) CHECK(std::fabs(s - 1.0f / 60) < 1e-6f);
    CHECK(widget_script_ticks[widget_a].size() == 30 && widget_script_ticks[widget_b].size() == 30);
    total = 0;
    for (float s : widget_script_ticks[widget_a]) total += s;
    CHECK(std::fabs(total - 1.0f) < 0.02f);
    // A subclass whose paced parent also calls the visual base receives one picture delta, not the summed
    // simulation delta passed into that parent. A widget with no native base call does not gain one.
    widget_ticks.clear(), widget_base_ticks.clear(), widget_script_ticks.clear();
    const uint64_t counters_only = block(0x10);
    nested_widget_bases = true;
    for (unsigned i = 0; i < 60; ++i) {
        engine_frame(1.0f / 60);
        tick(3, widget_a), tick(0, counters_only);
    }
    nested_widget_bases = false;
    CHECK(widget_base_ticks[widget_a].size() == 60 && widget_base_ticks[counters_only].empty());
    total = 0;
    for (float s : widget_base_ticks[widget_a]) total += s;
    CHECK(std::fabs(total - 1.0f) < 1e-5f);
    CHECK(widget_ticks[widget_a * 64 + 3].size() == 30 && widget_ticks[widget_a * 64 + 44].size() == 30);
    CHECK(widget_script_ticks[widget_a].size() == 30);
    // Outside a paced widget, the same native script thunk keeps the original per-picture cadence.
    widget_script_ticks.clear();
    for (unsigned i = 0; i < 60; ++i) {
        engine_frame(1.0f / 60);
        hook_of<void (*)(void*, const void*, float)>(site_key(t::code_site::widget_script_tick))
            (reinterpret_cast<void*>(counters_only), nullptr, 1.0f / 60);
    }
    CHECK(widget_script_ticks[counters_only].size() == 60);
}

static void numeric_widgets_in_game() {
    // Original ARM replay establishes these bodies' fixed counter/curve progression. This host integration
    // checks the actual hooked entry, distinct glyph/parent instances, the retained visual base and delta ABI.
    using Function=void (*)(void*,const void*,float);
    for(unsigned fps : {30u,60u,120u}) {
        settings_file="{\"frame_limit\": "+std::to_string(fps)+",\"vsync\":false}";
        presenter_settings();
        const uint64_t number=block(0x10),digit=block(0x10),nested=block(0x10),ordinary=block(0x10);
        int geometry=42;numeric_geometry=&geometry;
        auto tick=[&] {
            hook_or_game<Function>(numeric_key(0x024eafa0))(reinterpret_cast<void*>(number),&geometry,1.0f/fps);
            hook_or_game<Function>(numeric_key(0x022e14f0))(reinterpret_cast<void*>(digit),&geometry,1.0f/fps);
            nested_numeric_base=true;
            hook_of<Function>(t::slot_group::widget_tick.tables[3]+t::slot_group::widget_tick.offset)
                (reinterpret_cast<void*>(nested),&geometry,1.0f/fps);
            nested_numeric_base=false;
            hook_or_game<Function>(site_key(t::code_site::widget_base_tick))(reinterpret_cast<void*>(ordinary),&geometry,1.0f/fps);
        };
        for(unsigned i=0;i<8;++i){now_us+=1000000/fps;engine_frame(1.0f/fps);tick();}
        widget_ticks.clear();widget_base_ticks.clear();widget_script_ticks.clear();
        const uint32_t steps=world_updates;
        std::string parent_schedule,digit_schedule;
        for(unsigned i=0;i<fps;++i){
            const size_t before_parent=widget_ticks[number*64+45].size(),before_digit=widget_ticks[digit*64+46].size();
            now_us+=1000000/fps;engine_frame(1.0f/fps);tick();
            parent_schedule+=widget_ticks[number*64+45].size()!=before_parent?'1':'0';
            digit_schedule+=widget_ticks[digit*64+46].size()!=before_digit?'1':'0';
        }
        const auto& numbers=widget_ticks[number*64+45];const auto& digits=widget_ticks[digit*64+46];
        CHECK(world_updates-steps==30);
        CHECK(numbers.size()==30);CHECK(digits.size()==30);CHECK(widget_ticks[nested*64+45].size()==30);
        for(uint64_t widget : {number,digit,nested,ordinary}) {
            CHECK(widget_base_ticks[widget].size()==fps);
            float delta=0;for(float seconds:widget_base_ticks[widget])delta+=seconds;
            CHECK(std::fabs(delta-1)<0.00001f);
            CHECK(widget_script_ticks[widget].size()==(widget==ordinary?fps:30));
        }
        for(const auto* samples : {&numbers,&digits}) {
            float delta=0;for(float seconds:*samples)delta+=seconds;
            CHECK(std::fabs(delta-1)<0.00001f);
            for(float seconds:*samples)CHECK(std::fabs(seconds-1.0f/30)<0.00001f);
        }
        std::printf("numeric cadence fps %u parent %zu glyph %zu nested %zu base %zu ordinary_script %zu\n",fps,
            numbers.size(),digits.size(),widget_ticks[nested*64+45].size(),widget_base_ticks[number].size(),widget_script_ticks[ordinary].size());
        std::printf("numeric_schedule fps %u parent %s glyph %s\n",fps,parent_schedule.c_str(),digit_schedule.c_str());
        numeric_geometry=nullptr;
    }
}

static void pikmin_count_publication_in_game() {
    using Update = void (*)(void*, uint64_t);
    const auto update = [] (uint64_t flags) {
        hook_or_game<Update>(site_key(t::code_site::pikmin_counter_update))(reinterpret_cast<void*>(pikmin_counter), flags);
    };
    const std::vector<std::vector<float>> profiles = {{1.0f / 120}, {0.008f, 0.012f, 0.020f, 0.010f, 0.016f, 0.034f}};
    for (unsigned cap : {30u, 60u, 120u}) {
        settings_file = "{\"frame_limit\":" + std::to_string(cap) + "}";
        presenter_settings();
        for (const auto& profile : profiles) {
            std::memset(reinterpret_cast<void*>(pikmin_counter), 0, t::pikmin_counter::size);
            following_pikmin = 60;
            count_pikmin_in_world = publish_pikmin_each_picture = true;
            now_us += 33333;
            engine_frame(1.0f / 30); // Native initial recount; both paths must establish60.
            const unsigned before = counter_calls, walked = counter_full_recounts;
            unsigned steps = 0;
            int32_t visible = 60;
            for (unsigned picture = 0; picture < 120; ++picture) {
                if (picture == 61) following_pikmin = 59;
                const float seconds = profile[picture % profile.size()];
                now_us += uint64_t(seconds * 1000000);
                const bool step = engine_frame(seconds);
                steps += step;
                if (step) visible = following_pikmin;
                const unsigned bank = get<int32_t>(pikmin_counter + t::pikmin_counter::read_bank) == 1 ? 1 : 0;
                CHECK(get<int32_t>(counter_cell(bank, 3)) == visible);
                CHECK(get<int32_t>(counter_cell(bank, 7)) == visible);
            }
            CHECK(counter_calls - before == steps);
            CHECK(counter_full_recounts == walked); // Never replace publication with a full actor walk.
        }
    }
    count_pikmin_in_world = publish_pikmin_each_picture = false;
    settings_file = "{\"frame_limit\":120}";
    presenter_settings();
    now_us += 8333;
    while (engine_frame(1.0f / 120)) now_us += 8333;
    unsigned before = counter_calls;
    update(0);
    CHECK(counter_calls == before); // Ordinary initialized publication between world updates is deferred.
    put<uint8_t>(pikmin_counter + t::pikmin_counter::initialized, 0);
    update(0);
    CHECK(counter_calls == ++before); // Initial recount still walks immediately.
    put<uint8_t>(pikmin_counter + t::pikmin_counter::force_recount, 1);
    update(0);
    CHECK(counter_calls == ++before); // Explicit native forced recount is never deferred.
    update(1);
    CHECK(counter_calls == ++before); // Native refresh-in-place flag.
    update(2);
    CHECK(counter_calls == ++before); // Unknown upper flags fail open to native behavior.
    game_thread = false;
    update(0);
    CHECK(counter_calls == ++before);
    game_thread = true;
    refused_pose_address = pikmin_counter + t::pikmin_counter::initialized;
    update(0);
    CHECK(counter_calls == ++before);
    refused_pose_address = pikmin_counter + t::pikmin_counter::force_recount;
    update(0);
    CHECK(counter_calls == ++before); // Unreadable native state also fails open.
    refused_pose_address = 0;
}

static void controllers() {
    // A press that comes and goes between two steps still reaches the next step's read; one held across is not
    // made longer.
    while (!at_step(pictures.back().first)) engine_frame(1.0f / 60); // just stepped
    raw_buttons = 0x400;
    engine_frame(1.0f / 60); // a picture between steps: the press is seen
    raw_buttons = 0;
    const uint32_t reads = npad_reads_in_world;
    engine_frame(1.0f / 60); // the step: released by now, but the read gives the press
    CHECK(npad_reads_in_world == reads + 1);
    CHECK(get<uint64_t>(device + t::npad_device::buttons) == 0x400);
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    CHECK(get<uint64_t>(device + t::npad_device::buttons) == 0); // released at the step after
    // A press the step saw is not made longer: down at a step, seen again between, released by the next.
    raw_buttons = 0x2;
    engine_frame(1.0f / 60);
    if (get<uint64_t>(device + t::npad_device::buttons) != 0x2) engine_frame(1.0f / 60);
    CHECK(get<uint64_t>(device + t::npad_device::buttons) == 0x2 && at_step(pictures.back().first));
    engine_frame(1.0f / 60); // between steps, still down
    raw_buttons = 0;
    engine_frame(1.0f / 60);
    CHECK(get<uint64_t>(device + t::npad_device::buttons) == 0);
    // Without a known style nothing is added.
    style = 0;
    raw_buttons = 0x1;
    engine_frame(1.0f / 60);
    raw_buttons = 0;
    engine_frame(1.0f / 60);
    CHECK(get<uint64_t>(device + t::npad_device::buttons) == 0);
    style = 1;
}

static float service_view_x=0;
static void camera_service_after(uint64_t, uint64_t out) {
    service_view_x=get<float>(out+t::view_info::location);
    put<float>(out+t::view_info::rotation+4,17);
}
static void camera_service_test() {
    settings_file = "{\"frame_limit\": 60, \"presenter_log\": true}";
    presenter_settings();
    for (int i=0;i<6;++i) engine_frame(1.0f/60);
    presenter_camera_after(camera_service_after);
    for (int i=0;i<4;++i) {
        engine_frame(1.0f/60);
        CHECK(shown[4]==17);
        CHECK(std::fabs(service_view_x-(location_x-30*(1-pictures.back().first)))<.01f);
    }
    presenter_camera_after(nullptr);
}

static void scene_alone() {
    using namespace tsuchi::presenter;
    // A matrix of a turn, a mirrored size and a place goes to its parts and back.
    for (float yaw : {0.0f, 30.0f, 179.0f, -120.0f}) {
        const float angles[3] = {10, yaw, -5};
        Trs in{};
        tsuchi::math::to_quaternion(angles, in.q);
        in.t[0] = 100, in.t[1] = -50, in.t[2] = 7;
        in.s[0] = -2, in.s[1] = 1, in.s[2] = 3;
        float m[16], back[16];
        to_matrix(in, m);
        Trs out;
        CHECK(to_trs(m, out));
        to_matrix(out, back);
        float most = 0;
        for (int i = 0; i < 16; ++i) most = std::fmax(most, std::fabs(m[i] - back[i]));
        CHECK(most < 1e-4f);
    }
    // A sheared matrix is refused.
    float sheared[16] = {1, 0.5f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Trs refused;
    CHECK(!to_trs(sheared, refused));
    // Halfway between a quarter turn about up is an eighth.
    Trs a{{0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}, b = a, half;
    const float quarter[3] = {0, 90, 0}, eighth[3] = {0, 45, 0};
    tsuchi::math::to_quaternion(quarter, b.q);
    blend(a, b, 0.5f, half);
    float want[4];
    tsuchi::math::to_quaternion(eighth, want);
    CHECK(std::fabs(std::fabs(half.q[0] * want[0] + half.q[1] * want[1] + half.q[2] * want[2] + half.q[3] * want[3]) - 1) < 1e-4f);
    // Bounds between two places hold both boxes.
    const float x[7] = {0, 0, 0, 1, 1, 1, 2}, y[7] = {10, 0, 0, 1, 1, 1, 2};
    float both[7];
    hold_both(x, y, both);
    CHECK(std::fabs(both[0] - 5) < 1e-5f && std::fabs(both[3] - 6) < 1e-5f && both[6] >= 7 - 1e-4f);
    // A move four times the one before, or 150 cm, is a jump.
    float move = 0;
    const float from[3] = {0, 0, 0}, near[3] = {100, 0, 0}, far[3] = {400, 0, 0};
    CHECK(!jumped(from, near, 30, move) && jumped(from, far, 30, move) && !jumped(from, far, 120, move));
    // Steps apart: a picture's place between two changes two steps apart.
    CHECK(std::fabs(place_between(4, 6, 6.0) - 0.5f) < 1e-6f && place_between(4, 6, 7) == 1 && place_between(4, 6, 5) == 0 && std::fabs(place_between(5, 6, 5.25) - 0.25f) < 1e-6f);
}

static bool between(float value, float low, float high) { return value > low + 0.01f && value < high - 0.01f; }

static void scene_in_game() {
    // A part that moves 30 a step is sent to the renderer at every picture, 15 further each time: the game sends
    // it on steps, the presenter between them.
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_speed = 30, part_x = 0;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    renderer_x.clear();
    std::vector<float> per_frame;
    for (int i = 0; i < 12; ++i) {
        const size_t before = renderer_x.size();
        engine_frame(1.0f / 60);
        CHECK(renderer_x.size() > before);
        per_frame.push_back(renderer_x.back());
    }
    bool even = true;
    for (size_t i = 1; i < per_frame.size(); ++i) even = even && std::fabs(per_frame[i] - per_frame[i - 1] - 15) < 0.01f;
    CHECK(even);
    CHECK(per_frame.back() <= part_x - 15 + 0.01f); // a picture is up to a step behind
    // Not drawn, as when it is out of view, it is sent only at its steps, by the game.
    part_drawn = false;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    renderer_x.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    CHECK(renderer_x.size() == 6);
    part_drawn = true;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    // It stops: the next pictures reach where it is and then nothing more is sent.
    part_speed = 0;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(std::fabs(renderer_x.back() - part_x) < 0.01f);
    const size_t settled = renderer_x.size();
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(renderer_x.size() == settled);
    // A warp is shown at once: no picture between where it was and where it went.
    part_speed = 30;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    const float before_warp = part_x;
    warp_to = 5000;
    renderer_x.clear();
    hook_of<void (*)(void*, void*, void*, bool, bool, bool)>(site_key(t::code_site::actor_warp))(reinterpret_cast<void*>(actor), nullptr, nullptr, true, true, true);
    for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
    bool slid = false;
    for (float x : renderer_x) slid = slid || between(x, before_warp + 30, before_warp + 5000);
    CHECK(!slid);
    // So is a move too far to be a step's, with no warp.
    const float before_jump = part_x;
    part_speed = 2000;
    renderer_x.clear();
    while (!at_step(pictures.back().first)) engine_frame(1.0f / 60);
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    part_speed = 30;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    slid = false;
    for (float x : renderer_x) slid = slid || between(x, before_jump + 30, before_jump + 2000);
    CHECK(!slid);
    // Taken out of the scene, it is no longer sent.
    part_speed = 0;
    hook_of<void (*)(void*, void*)>(t::slot::scene_remove.table + t::slot::scene_remove.offset)(reinterpret_cast<void*>(scene), reinterpret_cast<void*>(part));
    put<uint64_t>(part + t::primitive::proxy, 0);
    const size_t removed = renderer_x.size();
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(renderer_x.size() == removed);

    // A pose whose first bone moves 10 a step reaches the bone kernel 5 further at every picture.
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, bone_speed = 10, bone_every = 1;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    kernel_x.clear();
    std::vector<float> bone_frames;
    for (int i = 0; i < 12; ++i) {
        const size_t before = kernel_x.size();
        engine_frame(1.0f / 60);
        CHECK(kernel_x.size() > before);
        bone_frames.push_back(kernel_x.back());
    }
    even = true;
    for (size_t i = 1; i < bone_frames.size(); ++i) even = even && std::fabs(bone_frames[i] - bone_frames[i - 1] - 5) < 0.01f;
    CHECK(even);
    // Without a bone kernel the mesh object reads the component's own array: it is lent the same blend for the
    // update, and gets its own data back after it.
    {
        const uint64_t kernel_was = t::cells[t::code_sites[t::code_site::bone_kernel.number].address.index];
        t::cells[t::code_sites[t::code_site::bone_kernel.number].address.index] = t::absent_value;
        kernel_inline = true;
        kernel_x.clear();
        bone_frames.clear();
        for (int i = 0; i < 12; ++i) {
            const size_t before = kernel_x.size();
            engine_frame(1.0f / 60);
            CHECK(kernel_x.size() > before);
            bone_frames.push_back(kernel_x.back());
            CHECK(get<uint64_t>(part + t::skinned_pose::arrays + uint64_t(get<int32_t>(part + t::skinned_pose::read_index)) * 16) == bone_array.data);
        }
        even = true;
        for (size_t i = 1; i < bone_frames.size(); ++i) even = even && std::fabs(bone_frames[i] - bone_frames[i - 1] - 5) < 0.01f;
        CHECK(even);
        kernel_inline = false;
        t::cells[t::code_sites[t::code_site::bone_kernel.number].address.index] = kernel_was;
    }
    // Far away and small on the screen, a pose waits for its steps: the kernel is reached only by the game's sends.
    warp_to = 100000;
    hook_of<void (*)(void*, void*, void*, bool, bool, bool)>(site_key(t::code_site::actor_warp))(reinterpret_cast<void*>(actor), nullptr, nullptr, true, true, true);
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    kernel_x.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    CHECK(kernel_x.size() == 6);
    // Skipping intermediate sends also means the game's sends carry the newest pose. An old interpolated
    // pose must not be left on the renderer until the next distant update.
    CHECK(!kernel_x.empty() && std::fabs(kernel_x.back() - bones[0].t[0]) < 0.001f);
    // Near again, keeping up with the camera, it is shown between steps again.
    part_x = location_x, part_speed = 30, mark(uint32_t(t::constant::render_transform_dirty));
    presenter_cut(actor);
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    kernel_x.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    CHECK(kernel_x.size() == 12);
    part_speed = 0;
    // A pose that changes only every other step, as the game does for one far away, is shown at its changes: the
    // kernel is reached only by the game's own three sends in twelve pictures, each with a whole step's pose.
    bone_every = 2;
    for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
    kernel_x.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    bool whole = kernel_x.size() == 3;
    for (float x : kernel_x) whole = whole && std::fabs(x / 10 - std::round(x / 10)) < 0.001f;
    CHECK(whole);
    CHECK(!kernel_x.empty() && std::fabs(kernel_x.back() - bones[0].t[0]) < 0.001f);
    // A warp of the actor shows its pose at once too.
    bone_every = 1;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    const float bone_before = bones[0].t[0];
    bone_speed = 3000;
    kernel_x.clear();
    hook_of<void (*)(void*, void*, void*, bool, bool, bool)>(site_key(t::code_site::actor_warp))(reinterpret_cast<void*>(actor), nullptr, nullptr, true, true, true);
    while (!at_step(pictures.back().first)) engine_frame(1.0f / 60);
    engine_frame(1.0f / 60);
    bone_speed = 0;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    slid = false;
    for (float x : kernel_x) slid = slid || between(x, bone_before + 10, bone_before + 3000);
    CHECK(!slid);
    part_registered = false, part_speed = 0, bone_speed = 0;
    put<uint64_t>(part + t::primitive::proxy, 0);
}

// Consecutive values the kernel or the renderer was given: each step's move split between the pictures, or a wrap
// shown at once. A wrap blended would show a value part of the way back.
static bool forward_or_wrapped(const std::vector<float>& values, float step, float loop, unsigned& wraps) {
    bool ok = values.size() > 4;
    wraps = 0;
    for (size_t i = 1; i < values.size(); ++i) {
        const float d = values[i] - values[i - 1];
        const bool forward = d > -0.01f && d < step + 0.01f;
        const bool wrapped = d < -loop / 2 && d > -loop - 0.01f;
        wraps += wrapped;
        if (!forward && !wrapped) std::printf("not forward or wrapped: %.3f then %.3f\n", values[i - 1], values[i]), ok = false;
    }
    return ok;
}

static void pose_wrap_in_game() {
    // A pose whose first bone spins four degrees a step and starts again every 120, as the flower on an Onion: each
    // wrap is shown at once, every other picture goes on the short way, and the pictures between steps go on.
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_drawn = true, part_speed = 30, part_x = location_x;
    mark(uint32_t(t::constant::render_transform_dirty));
    const BonePose saved = bones[0];
    bones[0].q[0] = bones[0].q[1] = bones[0].q[2] = 0, bones[0].q[3] = 1;
    bone_every = 1, bone_speed = 0, bone_yaw = 0, bone_spin = 4, bone_spin_loop = 120;
    presenter_cut(actor);
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    keep_kernel_q = true;
    kernel_q.clear();
    for (int i = 0; i < 160; ++i) engine_frame(1.0f / 60);
    std::vector<float> yaws;
    for (const std::vector<float>& q : kernel_q) yaws.push_back(yaw_of(q.data()));
    unsigned wraps = 0;
    CHECK(kernel_q.size() >= 150 && forward_or_wrapped(yaws, 2, 120, wraps) && wraps == 2);
    bool between_steps = false;
    for (size_t i = 1; i < yaws.size(); ++i) between_steps = between_steps || std::fabs(yaws[i] - yaws[i - 1] - 2) < 0.01f;
    CHECK(between_steps);
    // Spun 19 degrees a step, its angle wrapping past 180 and its turn back to the start at 360: a whole turn is the
    // same pose, so nothing is shown at once. Every picture is half a step on.
    bone_spin = 19, bone_spin_loop = 360;
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    kernel_q.clear();
    for (int i = 0; i < 160; ++i) engine_frame(1.0f / 60);
    bool even = kernel_q.size() >= 150;
    for (size_t i = 1; i < kernel_q.size(); ++i)
        even = even && std::fabs(within_half(yaw_of(kernel_q[i].data()) - yaw_of(kernel_q[i - 1].data()) - 9.5f)) < 0.05f;
    CHECK(even);
    keep_kernel_q = false;
    // A place that slides 10 a step and starts again every 300.
    bone_spin = 0, bone_speed = 10, bone_loop = 300, bones[0].t[0] = 0;
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    kernel_x.clear();
    for (int i = 0; i < 160; ++i) engine_frame(1.0f / 60);
    CHECK(forward_or_wrapped(kernel_x, 5, 300, wraps) && wraps == 2);
    bone_speed = 0, bone_loop = 0, bone_spin_loop = 0, bones[0] = saved;
    part_registered = false, part_speed = 0;
    put<uint64_t>(part + t::primitive::proxy, 0);
}

static void material_wrap_in_game() {
    // A value that goes 60 a step and starts again every 600, as a looping clip's time given to a material: each
    // wrap reaches the renderer at once, and every other picture is 30 on.
    line_on = true, line_speed = 60, line_loop = 600;
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    std::vector<float> xs, widths;
    for (int i = 0; i < 80; ++i) {
        engine_frame(1.0f / 60);
        xs.push_back(rendered[0]), widths.push_back(rendered_scalar);
    }
    unsigned wraps = 0, width_wraps = 0;
    CHECK(forward_or_wrapped(xs, 30, 600, wraps) && wraps == 4);
    CHECK(forward_or_wrapped(widths, 3, 60, width_wraps) && width_wraps == 4);
    line_on = false, line_loop = 0, line_speed = 60;
}

static void pose_plateau_in_game() {
    // Both arms hold identical bone bytes, at a real nonzero LOD. What differs is whether the native game
    // still publishes every step or actually evaluates only every second step. Presenter resends are counted
    // separately; observing the mesh hook alone must not turn them into native evaluations.
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_drawn = true, part_speed = 30, part_x = location_x;
    pose_lod = 1, record_pose_sends = true;
    mark(uint32_t(t::constant::render_transform_dirty));
    for (uint32_t cadence : {1u, 2u}) {
        bone_every = bone_send_every = cadence, bone_speed = 10;
        presenter_cut(actor);
        for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
        while (!at_step(pictures.back().first)) engine_frame(1.0f / 60);
        bone_speed = 0;
        const float plateau = bones[0].t[0];
        pose_sends.clear();
        for (int i = 0; i < 8; ++i) {
            engine_frame(1.0f / 60);
            if (cadence == 2) {
                // These extra submissions carry no new native revision, even on intervening world steps.
                mark_pose(reinterpret_cast<void*>(part));
                send_pose(reinterpret_cast<void*>(part));
            }
        }
        unsigned native_holds = 0, replay_holds = 0;
        for (const PoseSend& send : pose_sends) {
            if (send.native) ++native_holds;
            else ++replay_holds;
            CHECK(send.lod == 1 && send.given == plateau);
        }
        CHECK(native_holds == 4 / cadence && bones[0].t[0] == plateau);
        if (cadence == 1) CHECK(replay_holds <= 1); // only the last moving half-picture before the first held step

        // Resume one ordinary ten-unit native change, then inspect its first intermediate picture. The
        // continuously evaluated arm should show the old endpoint and then five; the sparse arm must show
        // the newest whole endpoint, without inventing intermediate evaluations.
        pose_sends.clear();
        bone_speed = 10;
        bool resumed = false;
        for (int i = 0; i < 4 && !resumed; ++i) {
            engine_frame(1.0f / 60);
            for (const PoseSend& send : pose_sends) resumed = resumed || send.native;
        }
        CHECK(resumed && bones[0].t[0] == plateau + 10);
        bone_speed = 0;
        const PoseSend* first = nullptr;
        for (const PoseSend& send : pose_sends) if (send.native) { first = &send; break; }
        const float first_shown = first ? first->shown : -1;
        pose_sends.clear();
        engine_frame(1.0f / 60);
        bool intermediate = pose_sends.size() == 1 && !pose_sends[0].native && pose_sends[0].lod == 1 &&
                            std::fabs(pose_sends[0].shown - (plateau + 5)) < 0.01f;
        std::printf("pose plateau native_every %u LOD %d held_native %u held_replay %u resumed_delta %.3f intermediate %d\n",
                    cadence, pose_lod, native_holds, replay_holds, first_shown - plateau, int(intermediate));
        if (cadence == 1) {
            CHECK(std::fabs(first_shown - plateau) < 0.01f && intermediate);
        } else {
            CHECK(std::fabs(first_shown - (plateau + 10)) < 0.01f && pose_sends.empty());
        }
        CHECK(bones[0].t[0] == plateau + 10);
    }
    record_pose_sends = false, bone_send_every = 0, bone_every = 1, pose_lod = 0;
    part_registered = false, part_speed = 0, bone_speed = 0;
    put<uint64_t>(part + t::primitive::proxy, 0);
}

static void pose_identity_in_game() {
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_drawn = true, part_speed = 30, part_x = location_x;
    bone_every = bone_send_every = 1, pose_lod = 1, record_pose_sends = true;
    mark(uint32_t(t::constant::render_transform_dirty));
    auto warm = []() {
        bone_speed = 10;
        presenter_cut(actor);
        for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
        while (!at_step(pictures.back().first)) engine_frame(1.0f / 60);
        pose_sends.clear();
    };
    warm();

    // Our deferred mark is followed by a genuine equal-byte native finalization before the worker send.
    // The shared dirty bit cannot identify this, but the finalized revision can. It must not create a second
    // history endpoint within this simulation step or drop the next real transition.
    bone_speed = 0, coalesced_native_pose = true;
    const float before = bones[0].t[0];
    const uint32_t revision = get<uint32_t>(part + t::skinned_pose::revision);
    engine_frame(1.0f / 60);
    CHECK(!coalesced_native_pose && get<uint32_t>(part + t::skinned_pose::revision) == revision + 1);
    CHECK(pose_sends.size() == 1 && pose_sends[0].native && std::fabs(pose_sends[0].shown - (before - 5)) < 0.01f);
    bone_speed = 10;
    engine_frame(1.0f / 60);
    CHECK(std::fabs(pose_sends.back().shown - before) < 0.01f);
    bone_speed = 0;
    engine_frame(1.0f / 60);
    CHECK(std::fabs(pose_sends.back().shown - (before + 5)) < 0.01f);

    // An unchanged revision does not make an unfinalized edit into cadence evidence.
    warm();
    bone_speed = 0;
    bones[0].t[0] += 2;
    mark_pose(reinterpret_cast<void*>(part));
    send_pose(reinterpret_cast<void*>(part));
    CHECK(pose_sends.back().shown == bones[0].t[0]);

    // A revision change inside the kernel invalidates the tentative sampled output. The final kernel call
    // must carry the game's endpoint, and the next history starts afresh.
    warm();
    bone_speed = 0, change_revision_in_kernel = true;
    const size_t kernels = kernel_x.size();
    engine_frame(1.0f / 60);
    CHECK(!change_revision_in_kernel && kernel_x.size() == kernels + 2);
    CHECK(kernel_x.back() == bones[0].t[0]);

    // Neither a valid weak master whose array was not supplied nor an unreadable revision may supply
    // full-rate cadence evidence. Both retain the conservative old behavior across identical updates.
    const uint64_t master = block(0x700);
    std::memcpy(reinterpret_cast<void*>(master + t::skinned_pose::arrays), &bone_array, sizeof bone_array);
    put<uint32_t>(master + t::skinned_pose::revision, 100);
    for (unsigned denied = 0; denied < 2; ++denied) {
        warm();
        if (denied == 0) {
            fixture_pose_master = master;
            put<uint64_t>(part + t::skinned_pose::master, master);
        } else refused_pose_address = part + t::skinned_pose::revision;
        bone_speed = 0;
        const float plateau = bones[0].t[0];
        for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
        pose_sends.clear();
        bone_speed = 10;
        for (int i = 0; i < 2; ++i) engine_frame(1.0f / 60);
        CHECK(!pose_sends.empty() && pose_sends.front().native && pose_sends.front().shown == plateau + 10);
        CHECK(bones[0].t[0] == plateau + 10);
        refused_pose_address = 0, fixture_pose_master = 0;
        put<uint64_t>(part + t::skinned_pose::master, 0);
    }

    // A real matching master source is admitted, including unchanged evaluations. Its identity changing
    // starts history anew, and an expired master reference returns to the component rather than reusing it.
    fixture_pose_master = mesh_pose_owner = master;
    put<uint64_t>(part + t::skinned_pose::master, master);
    warm();
    bone_speed = 0;
    const float plateau = bones[0].t[0];
    for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
    bone_speed = 10, pose_sends.clear();
    bool master_resumed = false;
    for (int i = 0; i < 4 && !master_resumed; ++i) {
        engine_frame(1.0f / 60);
        master_resumed = !pose_sends.empty() && pose_sends.back().native;
    }
    std::printf("master source resumed %d shown_delta %.3f input_delta %.3f\n", int(master_resumed), pose_sends.back().shown - plateau, bones[0].t[0] - plateau);
    CHECK(master_resumed && std::fabs(pose_sends.back().shown - plateau) < 0.01f && bones[0].t[0] == plateau + 10);
    bone_speed = 0;
    engine_frame(1.0f / 60);
    CHECK(std::fabs(pose_sends.back().shown - (plateau + 5)) < 0.01f);
    expired_pose_weak = master, mesh_pose_owner = fixture_pose_master = 0;
    bone_speed = 10;
    engine_frame(1.0f / 60);
    CHECK(pose_sends.back().shown == bones[0].t[0]);
    expired_pose_weak = 0;
    put<uint64_t>(part + t::skinned_pose::master, 0);

    // A LOD or bone-count switch must not blend the two renderer layouts.
    warm();
    pose_lod = 2;
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    CHECK(pose_sends.front().lod == 2 && pose_sends.front().shown == pose_sends.front().given);
    warm();
    put<int32_t>(part + t::skinned_pose::arrays + 8, 2);
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    CHECK(pose_sends.front().shown == pose_sends.front().given);
    put<int32_t>(part + t::skinned_pose::arrays + 8, int32_t(kBones));
    record_pose_sends = false, bone_send_every = 0, bone_every = 1, pose_lod = 0;
    part_registered = false, part_speed = 0, bone_speed = 0;
    put<uint64_t>(part + t::primitive::proxy, 0);
}

static void pose_policy_in_game() {
    // Drive the existing native mesh/kernel fixture with explicit moments. This
    // isolates policy behavior from the engine clock's phase and native modulo.
    // These are host stand-ins, including the renderer's submitted first bone.
    CHECK(presenter_pose_policy() == 0);
    CHECK(presenter_pose_request(true, 1) == TSUCHI_OK);
    const float previous_part_x = part_x;
    part_registered = true, part_drawn = true, part_speed = bone_speed = 0;
    pose_lod = 1, record_pose_sends = true;
    uint64_t frame = 100000, step = 1000;
    const float origin[3] = {};
    auto moment = [&](uint64_t at, float fraction) {
        scene_moment({true, at, fraction, ++frame, kPartBones});
        put<float>(part + t::primitive::last_drawn, float(++draws));
        scene_view(origin, 90);
    };
    auto reset = [&](bool far = true) {
        scene_forget();
        step += 20;
        native_pose_pending = presenter_pose_pending = false;
        refused_pose_address = expired_pose_weak = fixture_pose_master = mesh_pose_owner = 0;
        put<uint64_t>(part + t::skinned_pose::master, 0);
        put<uint64_t>(part + t::primitive::proxy, 0xABC0);
        put<uint32_t>(part + t::primitive::flags, uint32_t(t::constant::render_ready));
        put<int32_t>(part + t::skinned_pose::arrays + 8, int32_t(kBones));
        pose_lod = 1;
        part_x = far ? 100000 : 0;
        moment(step, 0);
        hook_of<void (*)(void*, void*)>(slot_key(t::slot::scene_update))(
            reinterpret_cast<void*>(scene), reinterpret_cast<void*>(part));
        pose_sends.clear();
    };
    auto native = [&](uint64_t at, float value) {
        moment(at, 0);
        bones[0].t[0] = value;
        put<uint32_t>(part + t::skinned_pose::revision, get<uint32_t>(part + t::skinned_pose::revision) + 1);
        native_pose_pending = true;
        mark(uint32_t(t::constant::render_dynamic_dirty));
        send_pose(reinterpret_cast<void*>(part));
    };
    auto picture = [&](uint64_t at, float fraction) {
        moment(at, fraction);
        scene_present();
        end_of_frame();
    };
    auto pair = [&](uint32_t gap) {
        native(step, 10);
        native(step + gap, 20);
    };

    // Smooth admits small qualified poses and native gaps through four steps.
    // The game's first submission and our subsequent resend use the same rule.
    scene_pose_policy(true);
    for (uint32_t gap : {1u, 2u, 4u, 5u}) {
        reset();
        pair(gap);
        CHECK(pose_sends.size() == 2 && pose_sends.back().native);
        CHECK(pose_sends.back().lod == 1 && pose_sends.back().given == 20);
        CHECK(pose_sends.back().shown == (gap <= 4 ? 10 : 20));
        pose_sends.clear();
        picture(step + gap, 0.5f);
        if (gap <= 4) {
            CHECK(pose_sends.size() == 1 && !pose_sends.back().native);
            CHECK(std::fabs(pose_sends.back().shown - (10 + 5.0f / gap)) < 0.001f);
            CHECK(bones[0].t[0] == 20);
            pose_sends.clear();
            picture(step + 2 * gap, 0);
            CHECK(pose_sends.size() == 1 && pose_sends.back().shown == 20);
            pose_sends.clear();
            picture(step + 2 * gap, 0.5f);
            CHECK(pose_sends.empty());
        } else {
            CHECK(pose_sends.empty());
        }
    }

    // Unknown native cadence keeps selective behavior. Relaxing the size and
    // gap filters requires an exact readable native revision/array identity.
    for (bool far : {false, true}) {
        reset(far);
        refused_pose_address = part + t::skinned_pose::revision;
        const uint32_t gap = far ? 1 : 2;
        pair(gap);
        CHECK(pose_sends.back().shown == 20);
        pose_sends.clear();
        picture(step + gap, 0.5f);
        CHECK(pose_sends.empty());
        refused_pose_address = 0;
    }

    // Equal native endpoints need no intermediate sends, even across a gap.
    reset();
    native(step, 10);
    native(step + 2, 10);
    pose_sends.clear();
    picture(step + 2, 0.5f);
    CHECK(pose_sends.empty());

    // An edit under the same revision is not another native evaluation. LOD,
    // bone-count and object-identity changes also retain their existing guards.
    reset();
    pair(2);
    bones[0].t[0] = 22;
    mark_pose(reinterpret_cast<void*>(part));
    send_pose(reinterpret_cast<void*>(part));
    CHECK(pose_sends.back().shown == 22);
    pose_sends.clear();
    picture(step + 2, 0.5f);
    CHECK(pose_sends.empty());
    reset();
    pair(2);
    pose_lod = 2;
    native(step + 4, 30);
    CHECK(pose_sends.back().lod == 2 && pose_sends.back().shown == 30);
    reset();
    pair(2);
    put<int32_t>(part + t::skinned_pose::arrays + 8, 2);
    native(step + 4, 30);
    CHECK(pose_sends.back().shown == 30);
    reset();
    pair(2);
    expired_pose_weak = part;
    pose_sends.clear();
    picture(step + 2, 0.5f);
    CHECK(pose_sends.empty());
    expired_pose_weak = 0;

    // Repeating an effective policy preserves its history. Turning smooth off
    // after a blend must submit the native endpoint despite selective filters.
    reset();
    pair(2);
    CHECK(presenter_pose_request(true, 1) == TSUCHI_OK);
    scene_pose_policy(true);
    pose_sends.clear();
    picture(step + 2, 0.5f);
    CHECK(pose_sends.size() == 1 && std::fabs(pose_sends.back().shown - 12.5f) < 0.001f);
    CHECK(presenter_pose_request(true, 0) == TSUCHI_OK);
    scene_pose_policy(false);
    pose_sends.clear();
    scene_present(); // Same-frame suppression must not retire a reset pending correction at step >300.
    const size_t before_correction = kernel_x.size();
    moment(step + 3, 0);
    scene_present(); // accepts a dirty mark; the worker has not submitted it
    CHECK(presenter_pose_pending && kernel_x.size() == before_correction);
    moment(step + 3, 0.5f);
    scene_present(); // an already dirty component must retain its correction
    CHECK(kernel_x.size() == before_correction && pose_sends.empty());
    end_of_frame();
    CHECK(pose_sends.size() == 1 && !pose_sends.back().native && pose_sends.back().shown == 20);
    pose_sends.clear();
    for (unsigned i = 0; i < 4; ++i) picture(step + 4 + i, 0.5f);
    CHECK(pose_sends.empty());

    // A policy round trip inside the original kernel invalidates the old epoch,
    // even though the final mode is still smooth. Comparing mode alone misses
    // this stale sampled submission; the last call must carry the native input.
    CHECK(presenter_pose_request(true, 1) == TSUCHI_OK);
    scene_pose_policy(true);
    reset();
    pair(2);
    pose_sends.clear();
    change_pose_policy_in_kernel = true;
    const size_t before_epoch = kernel_x.size();
    picture(step + 2, 0.5f);
    CHECK(!change_pose_policy_in_kernel && kernel_x.size() == before_epoch + 2);
    CHECK(kernel_x.back() == 20 && bones[0].t[0] == 20);
    CHECK(pose_sends.size() == 1 && pose_sends.back().shown == 20);
    pose_sends.clear();
    picture(step + 3, 0);
    CHECK(pose_sends.empty());

    CHECK(presenter_pose_request(false, 0) == TSUCHI_OK);
    scene_pose_policy(false);
    scene_forget();
    record_pose_sends = false, bone_send_every = 0, bone_every = 1, pose_lod = 0;
    part_registered = false, part_speed = bone_speed = 0;
    part_x = previous_part_x;
    native_pose_pending = presenter_pose_pending = false;
    put<uint32_t>(part + t::primitive::flags, uint32_t(t::constant::render_ready));
    put<int32_t>(part + t::skinned_pose::arrays + 8, int32_t(kBones));
    put<uint64_t>(part + t::primitive::proxy, 0);
}

static void effects_in_game() {
    // The drawer's instances move evenly: the first 15 a picture, every picture, and each picture's copy is made
    // again once.
    crowds_on = true;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    crowd_shown.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    bool even = crowd_shown.size() == 12;
    for (size_t i = 1; i < crowd_shown.size(); ++i) even = even && std::fabs(crowd_shown[i] - crowd_shown[i - 1] - 15) < 0.01f;
    CHECK(even);
    CHECK(crowd_shown.back() <= crowd_x - 15 + 0.01f); // a step behind, as the rest
    // An instance that went too far in a step is shown where it went; the list's count changing starts it again.
    const float base = crowd_x;
    crowd_x += 1000;
    crowd_shown.clear();
    while (!at_step(pictures.back().first)) engine_frame(1.0f / 60);
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    bool slid = false;
    for (float x : crowd_shown) slid = slid || between(x, base + 30, base + 1000);
    CHECK(!slid);
    put<int32_t>(crowd_part + t::instanced_mesh::count, 1);
    crowd_shown.clear();
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    CHECK(crowd_shown.empty());
    put<int32_t>(crowd_part + t::instanced_mesh::count, 2);
    crowds_on = false;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);

    // A carry counter's point moves evenly too: 15 a picture at 60, its quads sent again for each picture between
    // steps; one that jumped is shown where it went.
    counters_on = true;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    counter_sent.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    bool counted = counter_sent.size() >= 5;
    for (size_t i = 1; i < counter_sent.size(); ++i) {
        const float d = counter_sent[i] - counter_sent[i - 1];
        counted = counted && (std::fabs(d - 15) < 0.01f || std::fabs(d - 30) < 0.01f);
    }
    CHECK(counted);
    const float counter_base = counter_x;
    counter_x += 1000;
    counter_sent.clear();
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    bool counter_slid = false;
    for (float x : counter_sent) counter_slid = counter_slid || between(x, counter_base + 60, counter_base + 1000);
    CHECK(!counter_slid);
    counters_on = false;
    put<int32_t>(counter_numbers + t::carry_numbers::count, 0);
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);

    // A particle drawn halfway between steps is drawn halfway between its two locations, with its old location
    // moved the same; drawn at a step, at its old one. Each route of a mesh particle gives the same place.
    rendering = true;
    bool half = false, start = false, same = true;
    for (int i = 0; i < 8; ++i) {
        sprite_x = sprite_old_x = mesh_x = offset_x = offset_z = -1;
        engine_frame(1.0f / 60);
        const float fraction = pictures.back().first;
        if (halfway(fraction)) half = std::fabs(sprite_x - 15) < 0.01f && std::fabs(sprite_old_x + 15) < 0.01f;
        if (at_step(fraction)) start = std::fabs(sprite_x) < 0.01f;
        same = same && std::fabs(mesh_x - sprite_x) < 0.01f && std::fabs(offset_x - sprite_x) < 0.01f &&
               std::fabs(offset_z - (2 + 4 * fraction)) < 0.01f;
    }
    CHECK(half && start && same);
    // A scene of a family the presenter did not make, as a scene capture's, is drawn as the game left it.
    put<float>(renderer + t::scene_renderer::world_seconds, -5);
    sprite_x = -1;
    hook_of<void (*)(void*, void*)>(site_key(t::code_site::scene_render))(reinterpret_cast<void*>(render_command), nullptr);
    rendering = false;
    CHECK(sprite_x == 30);
}

static void markers_in_game() {
    // A marker and a gauge whose point moves 30 a step against the camera move 15 on the screen at every picture,
    // and a gauge keeps what it adds to its point.
    marks_on = true;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    std::vector<float> marker, gauge, action, bar, icon;
    bool hud_kept = true;
    for (int i = 0; i < 12; ++i) {
        engine_frame(1.0f / 60);
        marker.push_back(get<float>(marker_widget + t::life_gauge::position) * 2);
        gauge.push_back(get<float>(gauge_widget + t::life_gauge::position) * 2);
        action.push_back(get<float>(action_widget + t::life_gauge::position) * 2);
        bar.push_back(get<float>(bar_widget + t::life_bar::position) * 2);
        icon.push_back(get<float>(icon_slot + t::canvas_slot::position) * 2);
        hud_kept = hud_kept && get<float>(hud_slot + t::canvas_slot::position) == 100 && get<float>(hud_slot + t::canvas_slot::position + 4) == 50;
    }
    bool even = true;
    for (size_t i = 1; i < marker.size(); ++i)
        even = even && std::fabs(marker[i] - marker[i - 1] - 15) < 0.01f && std::fabs(gauge[i] - gauge[i - 1] - 15) < 0.01f &&
               std::fabs(action[i] - action[i - 1] - 15) < 0.01f && std::fabs(bar[i] - bar[i - 1] - 15) < 0.01f &&
               std::fabs(icon[i] - icon[i - 1] - 15) < 0.01f;
    CHECK(even);
    CHECK(std::fabs(gauge.back() - marker.back() - 7) < 0.01f && std::fabs(action.back() - marker.back() - 6) < 0.01f);
    // The bar and the icon keep what the game adds: the bar its own 5 left, the icon its canvas's middle.
    CHECK(std::fabs(bar.back() - marker.back() + 5) < 0.01f && std::fabs(icon.back() - marker.back() + 1920) < 0.01f);
    // A canvas placement of another function's, after a projection the same frame, is not moved.
    CHECK(hud_kept);
    // A shown bar that the game hides is left where the game put it.
    put<uint8_t>(bar_widget + t::life_bar::shown, 0);
    engine_frame(1.0f / 60);
    const float hidden = get<float>(bar_widget + t::life_bar::position);
    for (int i = 0; i < 3; ++i) engine_frame(1.0f / 60);
    CHECK(get<float>(bar_widget + t::life_bar::position) == hidden);
    put<uint8_t>(bar_widget + t::life_bar::shown, 1);
    // Placed no more, a mark is left where the game last put it.
    marks_on = false;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    const float left = get<float>(marker_widget + t::life_gauge::position);
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(get<float>(marker_widget + t::life_gauge::position) == left);
}

// A mod's widget hung from an actor (ui.hold_mark): the actor's root, the widget's canvas slot, and the widget's own
// visibility as the engine keeps it.
static uint64_t hung_slot = 0, hung_widget = 0;
static uint32_t hung_visibility = 0;
static std::vector<uint32_t> visibility_sets;
static void hung_set_visibility(void*, uint32_t visibility) { hung_visibility = visibility, visibility_sets.push_back(visibility); }
static uint32_t hung_get_visibility(void*) { return hung_visibility; }
static float hung_slot_x() { return get<float>(hung_slot + t::canvas_slot::position); }

static void hung_in_game() {
    hung_actor = block(0x200), hung_root = block(0x300), hung_slot = block(0x100), hung_widget = block(0x100);
    const uint64_t functions = block(0x400);
    put<uint64_t>(functions + t::widget_table::set_visibility.offset, reinterpret_cast<uint64_t>(&hung_set_visibility));
    put<uint64_t>(hung_widget, functions);
    put<uint64_t>(hung_slot + t::canvas_slot::content, hung_widget);
    put<uint64_t>(hung_actor + t::engine_actor::root, hung_root);
    put<float>(hung_root + t::scene_component::location, 1000);
    put<float>(hung_root + t::scene_component::location + 4, 0);
    const auto visibility_was = t::fn::ui_get_visibility;
    t::fn::ui_get_visibility = &hung_get_visibility;
    // Hung 40 to the side and 150 over an actor that stands still while the camera goes 30 a step: on the screen it
    // goes 15 at every picture, 7.5 in its canvas, whose units are the screen's over the scale of 2. A widget placed
    // from the step's projection would stand still for a picture and go 15 at the next.
    const float offset[3] = {0, 40, 150};
    CHECK(markers_hang(hung_slot, hung_actor, controller, offset) && markers_hung(hung_slot));
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    std::vector<float> xs;
    for (int i = 0; i < 12; ++i) {
        engine_frame(1.0f / 60);
        xs.push_back(hung_slot_x());
        CHECK(get<float>(hung_slot + t::canvas_slot::position + 4) == 20);
    }
    bool even = true;
    for (size_t i = 1; i < xs.size(); ++i) even = even && std::fabs(xs[i] - xs[i - 1] + 7.5f) < 0.01f;
    CHECK(even);
    // Where the picture's view sees the point: the actor's place against the camera's, which a picture shows up to
    // a step behind.
    CHECK(std::fabs(xs.back() * 2 - (hung_x() - (location_x - 30 * (1 - pictures.back().first)) + 500)) < 0.01f);
    // The actor goes 60 a step: 30 more on the screen at every step, so 15 at every picture, 7.5 in the canvas.
    float* moving = reinterpret_cast<float*>(hung_root + t::scene_component::location);
    hung_speed = 60;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    xs.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60), xs.push_back(hung_slot_x());
    hung_speed = 0;
    even = true;
    for (size_t i = 1; i < xs.size(); ++i) even = even && std::fabs(xs[i] - xs[i - 1] - 7.5f) < 0.01f;
    CHECK(even);
    // A jump is shown at once, with no picture between where it was and where it went.
    const float before_jump = hung_slot_x();
    *moving += 5000;
    presenter_cut(hung_actor);
    xs.clear();
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60), xs.push_back(hung_slot_x());
    bool slid = false;
    for (float x : xs) slid = slid || between(x, before_jump + 20, before_jump + 2400);
    CHECK(!slid);
    // Behind the camera its widget is hidden and left where it was; in front again it is shown as it was.
    visibility_sets.clear();
    projection_front = false;
    engine_frame(1.0f / 60);
    const float hidden_at = hung_slot_x();
    engine_frame(1.0f / 60);
    CHECK(hung_visibility == 2 && visibility_sets.size() == 1 && hung_slot_x() == hidden_at);
    projection_front = true;
    engine_frame(1.0f / 60);
    CHECK(hung_visibility == 0 && visibility_sets.size() == 2 && hung_slot_x() != hidden_at);
    // A widget the mod collapsed itself is left so behind the camera.
    hung_visibility = 1, visibility_sets.clear();
    projection_front = false;
    engine_frame(1.0f / 60), engine_frame(1.0f / 60);
    projection_front = true;
    engine_frame(1.0f / 60);
    CHECK(hung_visibility == 1 && visibility_sets.empty());
    hung_visibility = 0;
    // Hidden when its actor goes, it is shown again and let go.
    projection_front = false;
    engine_frame(1.0f / 60);
    CHECK(hung_visibility == 2);
    projection_front = true;
    expired_pose_weak = hung_actor;
    engine_frame(1.0f / 60);
    CHECK(!markers_hung(hung_slot) && hung_visibility == 0);
    const float left = hung_slot_x();
    engine_frame(1.0f / 60);
    CHECK(hung_slot_x() == left && !markers_unhang(hung_slot));
    expired_pose_weak = 0;
    // Hung again, then let go: it stays where it was last put.
    CHECK(markers_hang(hung_slot, hung_actor, controller, offset));
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(markers_unhang(hung_slot) && !markers_hung(hung_slot));
    const float released = hung_slot_x();
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(hung_slot_x() == released);
    // A number that is not finite is refused.
    const float bad[3] = {0, 0, NAN};
    CHECK(!markers_hang(hung_slot, hung_actor, controller, bad) && !markers_hung(hung_slot));
    t::fn::ui_get_visibility = visibility_was;
}

// The game's action prompts: two widgets placed each frame through the action marker's placement, one over Oatchi
// and one over the captain. The made-up game shows or collapses each through the engine's setter of a widget's
// visibility, every frame or only when its own wish changes, and projects each through a player's controller.
struct GamePrompt {
    uint64_t widget;
    uint8_t wanted;     // what the game wants it to show: 3 shown, 1 collapsed
    uint8_t told = 255; // what it last told the widget, for a game that tells only changes
    bool every_frame = true;
    bool after = false; // it tells the widget after the placement, as a widget's own tick later in the frame would
};
static GamePrompt ride{}, captain_prompt{};
static uint64_t oatchi_actor = 0, captain_actor = 0, second_controller = 0, prompt_functions = 0;
static std::vector<uint8_t> drawn_ride, drawn_captain; // the visibility each had when the picture was drawn
static void prompt_set_visibility(void* widget, uint32_t visibility) {
    put<uint8_t>(reinterpret_cast<uint64_t>(widget) + t::ui_widget::visibility, uint8_t(visibility));
}
static void* prompt_controller(void*, int32_t player) {
    return reinterpret_cast<void*>(player == 0 ? controller : player == 1 ? second_controller : 0);
}
static void tell_prompt(GamePrompt& prompt) {
    if (prompt.every_frame || prompt.told != prompt.wanted) prompt_set_visibility(reinterpret_cast<void*>(prompt.widget), prompt.wanted);
    prompt.told = prompt.wanted;
}
static void place_prompt(GamePrompt& prompt) {
    if (!prompt.after) tell_prompt(prompt);
    hook_of<void (*)(void*)>(site_key(t::code_site::action_marker_place))(reinterpret_cast<void*>(prompt.widget));
    if (prompt.after) tell_prompt(prompt);
}
static unsigned prompt_every = 1, prompt_frames = 0; // the game places its prompts on every this many frames
static void prompt_frame() {
    presenter_frame(1.0f / 60);
    presenter_frame_done();
    hook_of<void (*)(void*, uint32_t, float)>(site_key(t::code_site::world_tick))(reinterpret_cast<void*>(world), 2, 1.0f / 60);
    if (prompt_frames++ % prompt_every == 0) place_prompt(ride), place_prompt(captain_prompt);
    hook_of<void (*)(void*, bool)>(slot_key(t::slot::redraw_viewports))(reinterpret_cast<void*>(engine), true);
    drawn_ride.push_back(get<uint8_t>(ride.widget + t::ui_widget::visibility));
    drawn_captain.push_back(get<uint8_t>(captain_prompt.widget + t::ui_widget::visibility));
}
static uint8_t shown_of(const GamePrompt& prompt) { return get<uint8_t>(prompt.widget + t::ui_widget::visibility); }

static void prompts_in_game() {
    prompt_functions = block(0x400);
    put<uint64_t>(prompt_functions + t::widget_table::set_visibility.offset, reinterpret_cast<uint64_t>(&prompt_set_visibility));
    oatchi_actor = block(0x200), captain_actor = block(0x200), second_controller = block(0x300);
    for (GamePrompt* prompt : {&ride, &captain_prompt}) {
        prompt->widget = block(0xA00);
        put<uint64_t>(prompt->widget, prompt_functions);
    }
    put<uint64_t>(ride.widget + t::action_marker::actor, oatchi_actor);
    put<uint64_t>(captain_prompt.widget + t::action_marker::actor, captain_actor);
    ride.wanted = 3, captain_prompt.wanted = 3;
    const auto controller_was = t::fn::camera_controller;
    t::fn::camera_controller = &prompt_controller;
    // Watching starts with the first question, from the next frame on.
    CHECK(markers_watch_prompts(nullptr) == 2 && markers_watch_prompts(nullptr) == 1);
    prompt_frame();
    // Ride over Oatchi shows on the first player's screen, and is not shown 9 m away, where the game collapses it.
    CHECK(markers_prompt_screens(oatchi_actor) == 1 && markers_prompt_screens(captain_actor) == 1);
    ride.wanted = 1;
    prompt_frame();
    CHECK(markers_prompt_screens(oatchi_actor) == 0 && markers_prompt_screens(captain_actor) == 1);
    ride.wanted = 3;
    // Projected through the second player's view, it is on the second player's screen.
    action_projector = second_controller;
    prompt_frame();
    CHECK(markers_prompt_screens(oatchi_actor) == 2 && markers_prompt_screens(captain_actor) == 2);
    action_projector = 0;
    prompt_frame();
    CHECK(markers_prompt_screens(captain_actor) == 1);
    // Hidden on the first player's screen: every picture draws the captain's prompt hidden, though the game shows it
    // every frame, and it still reads as shown by the game. Oatchi's is left as it is.
    drawn_ride.clear(), drawn_captain.clear();
    CHECK(markers_hide_prompts(7, captain_actor, 1));
    CHECK(shown_of(captain_prompt) == 2); // at once
    for (int i = 0; i < 6; ++i) prompt_frame();
    bool hidden = true, ride_shown = true;
    for (uint8_t value : drawn_captain) hidden = hidden && value == 2;
    for (uint8_t value : drawn_ride) ride_shown = ride_shown && value == 3;
    CHECK(hidden && ride_shown && markers_prompt_screens(captain_actor) == 1);
    // So does a game that shows it after the placement: the picture is drawn with it hidden all the same.
    captain_prompt.after = true;
    drawn_captain.clear();
    for (int i = 0; i < 4; ++i) prompt_frame();
    hidden = true;
    for (uint8_t value : drawn_captain) hidden = hidden && value == 2;
    CHECK(hidden);
    captain_prompt.after = false;
    // The game collapses it meanwhile: left collapsed, and read as not shown; shown by the game again, hidden again.
    captain_prompt.wanted = 1;
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 1 && markers_prompt_screens(captain_actor) == 0);
    captain_prompt.wanted = 3;
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 2 && markers_prompt_screens(captain_actor) == 1);
    // Let go: the game's own visibility comes back at once.
    markers_show_prompts(7);
    CHECK(shown_of(captain_prompt) == 3);
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 3);
    // A game that tells its prompt only when its wish changes gets it back as well.
    captain_prompt.every_frame = false;
    CHECK(markers_hide_prompts(8, captain_actor, 1));
    for (int i = 0; i < 4; ++i) prompt_frame();
    CHECK(shown_of(captain_prompt) == 2 && markers_prompt_screens(captain_actor) == 1);
    markers_show_prompts(8);
    CHECK(shown_of(captain_prompt) == 3);
    captain_prompt.every_frame = true;
    // Hidden on the second player's screen only, a prompt on the first player's is left.
    CHECK(markers_hide_prompts(9, captain_actor, 2));
    for (int i = 0; i < 3; ++i) prompt_frame();
    CHECK(shown_of(captain_prompt) == 3);
    action_projector = second_controller;
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 2);
    action_projector = 0;
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 3);
    // Changed to every screen with the same key.
    CHECK(markers_hide_prompts(9, captain_actor, 15));
    CHECK(shown_of(captain_prompt) == 2);
    // The widget comes to point at another actor: it gets its own visibility back.
    put<uint64_t>(captain_prompt.widget + t::action_marker::actor, oatchi_actor);
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 3 && markers_prompt_screens(oatchi_actor) == 1 && markers_prompt_screens(captain_actor) == 0);
    put<uint64_t>(captain_prompt.widget + t::action_marker::actor, captain_actor);
    prompt_frame();
    CHECK(shown_of(captain_prompt) == 2);
    markers_show_prompts(9);
    CHECK(shown_of(captain_prompt) == 3);
    // Placed only on some frames, as above 30 the game places a prompt once a step: between its placements it reads
    // as it was last seen, every frame, until the game changes it.
    prompt_every = 4;
    std::vector<uint32_t> reads;
    for (int i = 0; i < 16; ++i) prompt_frame(), reads.push_back(markers_prompt_screens(oatchi_actor));
    bool steady = true;
    for (uint32_t players : reads) steady = steady && players == 1;
    CHECK(steady);
    // Collapsed by the game on a frame it does not place it: read as not shown from that frame's picture on.
    prompt_frames = 1; // the next frame places nothing
    prompt_set_visibility(reinterpret_cast<void*>(ride.widget), 1);
    ride.wanted = 1, ride.told = 1;
    prompt_frame();
    CHECK(markers_prompt_screens(oatchi_actor) == 0);
    ride.wanted = 3;
    for (int i = 0; i < 4; ++i) prompt_frame();
    CHECK(markers_prompt_screens(oatchi_actor) == 1);
    // Hidden by a hold, it stays hidden on the frames between placements, even shown by the game on one of them.
    CHECK(markers_hide_prompts(11, oatchi_actor, 1));
    drawn_ride.clear();
    for (int i = 0; i < 8; ++i) {
        if (i == 5) prompt_set_visibility(reinterpret_cast<void*>(ride.widget), 3); // outside its placement
        prompt_frame();
    }
    bool hidden_between = true;
    for (uint8_t value : drawn_ride) hidden_between = hidden_between && value == 2;
    CHECK(hidden_between && markers_prompt_screens(oatchi_actor) == 1);
    markers_show_prompts(11);
    CHECK(shown_of(ride) == 3);
    prompt_every = 1;
    // A widget that is gone is forgotten.
    expired_pose_weak = ride.widget;
    prompt_frame();
    CHECK(markers_prompt_screens(oatchi_actor) == 0);
    expired_pose_weak = 0;
    // No room past 32 holds, and no hold without an actor or a screen.
    unsigned taken = 0;
    for (uint64_t key = 100; key < 140; ++key) taken += markers_hide_prompts(key, oatchi_actor, 1);
    CHECK(taken == 32 && !markers_hide_prompts(200, 0, 1) && !markers_hide_prompts(201, oatchi_actor, 0));
    for (uint64_t key = 100; key < 140; ++key) markers_show_prompts(key);
    t::fn::camera_controller = controller_was;
}

static void materials_in_game() {
    // A value the world's update gives the renderer each step, 60 more each time, reaches it 30 more at every
    // picture: the vector and the scalar alike.
    line_on = true;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    std::vector<float> xs, widths;
    for (int i = 0; i < 12; ++i) {
        engine_frame(1.0f / 60);
        xs.push_back(rendered[0]), widths.push_back(rendered_scalar);
        CHECK(rendered[1] == 1 && rendered[2] == 2 && rendered[3] == 1);
    }
    bool even = true;
    for (size_t i = 1; i < xs.size(); ++i)
        even = even && std::fabs(xs[i] - xs[i - 1] - 30) < 0.01f && std::fabs(widths[i] - widths[i - 1] - 3) < 0.01f;
    CHECK(even);
    // A picture is up to one step behind: never past what the game gave.
    CHECK(xs.back() <= line_x && xs.back() >= line_x - 60);
    // Once the value stops changing the game pushes nothing, and the renderer is given the game's own value back.
    line_speed = 0;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(rendered[0] == line_x && rendered_scalar == line_x / 10);
    // A value pushed between steps, outside the world's update, is the game's own business and is not kept.
    alignas(16) uint8_t vector[32] = {};
    const float value[4] = {-5, 1, 2, 1};
    std::memcpy(vector, line_info, 16), std::memcpy(vector + 16, value, 16);
    hook_of<void (*)(void*, const void*)>(site_key(t::code_site::material_vector_push))(reinterpret_cast<void*>(line_material), vector);
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(rendered[0] == -5);
    // So is one pushed between steps right after the value stops, while a picture still showed it between two
    // steps: the kept steps are not shown over it.
    line_speed = 60;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    for (int i = 0; i < 4 && rendered[0] == line_x; ++i) engine_frame(1.0f / 60);
    CHECK(rendered[0] != line_x);
    line_speed = 0;
    const float later[4] = {-7, 1, 2, 1};
    std::memcpy(vector + 16, later, 16);
    hook_of<void (*)(void*, const void*)>(site_key(t::code_site::material_vector_push))(reinterpret_cast<void*>(line_material), vector);
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    CHECK(rendered[0] == -7);
    line_on = false, line_speed = 60;
}

static void cursor_inline_in_game() {
    markers_forget();
    cursor_on = true;
    cursor_anchor = location_x;
    for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
    const uint32_t steps = cursor_steps, counter = cursor_counter, queue = cursor_queue, repairs = cursor_repairs;
    const float timer = cursor_timer;
    const size_t bases = widget_base_ticks[cursor_widget].size();
    std::vector<float> places;
    for (int i = 0; i < 12; ++i) {
        engine_frame(1.0f / 60);
        places.push_back(get<float>(cursor_widget + t::cursor_placement::position) * 2);
        CHECK(get<float>(cursor_component + 0x2A4) == cursor_anchor);
        const float expected = cursor_anchor - 60 * (1 - pictures.back().first) - shown[0] + 500 + 12;
        CHECK(std::fabs(places.back() - expected) < 0.01f &&
              get<float>(cursor_widget + t::cursor_placement::position + 4) == -3);
    }
    bool even = true;
    for (size_t i = 1; i < places.size(); ++i) even = even && std::fabs(places[i] - places[i - 1] - 15) < 0.01f;
    CHECK(even);
    CHECK(cursor_steps - steps == 6 && cursor_counter - counter == 6 && queue - cursor_queue == 6);
    CHECK(cursor_repairs - repairs == 6);
    CHECK(std::fabs(cursor_timer - timer - 0.2f) < 0.0001f);
    CHECK(widget_base_ticks[cursor_widget].size() - bases == 12);
    // Anchor and camera move together: native screen position is unchanged. The unchanged branch still has
    // to admit the newest native placement; a written-store-only adapter silently loses this history.
    markers_forget();
    cursor_unchanged = true;
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 60);
    const auto noted = marker_counts(false).noted;
    const uint32_t before = cursor_steps;
    for (int i = 0; i < 8; ++i) engine_frame(1.0f / 60);
    CHECK(marker_counts(false).noted - noted == cursor_steps - before);
    std::printf("cursor inline even %d native %u ticks %u base %zu unchanged_notes %u/%u\n", int(even), cursor_steps - steps,
                cursor_counter - counter, widget_base_ticks[cursor_widget].size() - bases,
                marker_counts(false).noted - noted, cursor_steps - before);
    cursor_on = cursor_unchanged = false;
    markers_forget();
}

static void cursor_identity_guards() {
    // Direct host route dispatch exercises admission and read-only observation, not native ARM execution.
    const PresentedMoment moment{true, 100, 0.5f, 123, kPartMarkers};
    const uint64_t sp = reinterpret_cast<uint64_t>(cursor_stack);
    auto observe = [&](auto fault, bool accepted, bool unchanged = true) {
        markers_forget();
        markers_moment(moment);
        refused_pose_address = expired_pose_weak = remapped_weak_object = 0;
        put<uint64_t>(main_at() + t::data::world, world);
        put<uint64_t>(cursor_widget, main_at() + t::cursor_placement::widget_table);
        put<uint64_t>(cursor_widget + t::cursor_placement::ready, 1);
        put<uint64_t>(cursor_component, main_at() + t::cursor_placement::component_table);
        put<uint64_t>(cursor_component + t::throw_cursor::owner, cursor_owner);
        put<uint64_t>(cursor_owner + t::cursor_placement::owner_controller, controller);
        std::memset(cursor_stack, 0xA5, sizeof cursor_stack);
        put<uint64_t>(sp + 0x60, cursor_widget);
        put<uint64_t>(sp + 0x68, cursor_component);
        put<uint64_t>(sp + 0x78, main_at() + t::cursor_placement::producer_return);
        float* out = reinterpret_cast<float*>(sp + 0xC0);
        hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
            cursor_anchor, 0, 0, reinterpret_cast<void*>(controller), out, true);
        put<float>(cursor_widget + t::cursor_placement::screen, out[0] + 12);
        put<float>(cursor_widget + t::cursor_placement::screen + 4, out[1] - 6);
        put<float>(cursor_widget + t::cursor_placement::position, (out[0] + 12) / 2);
        put<float>(cursor_widget + t::cursor_placement::position + 4, (out[1] - 6) / 2);
        std::memset(&cursor_frame, 0x39, sizeof cursor_frame);
        cursor_frame.sp = sp, cursor_frame.x[19] = cursor_widget;
        cursor_frame.nzcv = unchanged ? 1ull << 30 : 0;
        fault();
        const NativeFrame before = cursor_frame;
        uint8_t stack_before[sizeof cursor_stack];
        std::memcpy(stack_before, cursor_stack, sizeof cursor_stack);
        const auto noted = marker_counts(false).noted;
        const auto route = unchanged ? t::native_route::cursor_unchanged : t::native_route::cursor_written;
        tsuchi_native_dispatch(&cursor_frame, main_at() + t::native_routes[route].returned);
        CHECK(marker_counts(false).noted - noted == unsigned(accepted));
        CHECK(!std::memcmp(&before, &cursor_frame, sizeof before));
        CHECK(!std::memcmp(stack_before, cursor_stack, sizeof cursor_stack));
        CHECK(get<float>(cursor_component + 0x2A4) == cursor_anchor);
        refused_pose_address = expired_pose_weak = 0;
        put<uint64_t>(main_at() + t::data::world, world);
    };
    observe([] {}, true);
    observe([] {}, true, false);
    observe([&] { put<uint64_t>(sp + 0x78, main_at() + t::cursor_placement::producer_return + 4); }, false);
    observe([&] { put<uint64_t>(sp + 0x60, marker_widget); }, false);
    observe([] { put<uint64_t>(cursor_widget, 0); }, false);
    observe([] { put<uint64_t>(cursor_component, 0); }, false);
    observe([] { put<uint64_t>(cursor_widget + t::cursor_placement::ready, 0); }, false);
    observe([] { put<uint64_t>(cursor_component + t::throw_cursor::owner, 0); }, false);
    observe([] { put<uint64_t>(cursor_owner + t::cursor_placement::owner_controller, player); }, false);
    observe([] { expired_pose_weak = cursor_component; }, false);
    observe([] { expired_pose_weak = cursor_widget; }, false);
    observe([] { expired_pose_weak = controller; }, false);
    observe([] { expired_pose_weak = cursor_owner; }, false);
    observe([] { expired_pose_weak = world; }, false);
    observe([] { put<uint64_t>(main_at() + t::data::world, level); }, false);
    observe([] { put<float>(cursor_widget + t::cursor_placement::screen, NAN); }, false);
    observe([] { put<float>(cursor_widget + t::cursor_placement::position, NAN); }, false);
    observe([] { refused_pose_address = cursor_widget + t::cursor_placement::position; }, false);
    observe([&] { cursor_frame.sp -= 16; }, false); // projection belongs to a different caller stack
    observe([&] { put<float>(sp + 0xC0, -1); }, false); // caller output changed after projection
    observe([&] {
        projection_front = false;
        hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
            cursor_anchor, 0, 0, reinterpret_cast<void*>(controller), reinterpret_cast<void*>(sp + 0xC0), true);
        projection_front = true;
    }, false);
    observe([&] {
        hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
            cursor_anchor, 0, 0, reinterpret_cast<void*>(controller), reinterpret_cast<void*>(sp + 0xC0), false);
    }, false);
    observe([&] {
        hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
            NAN, 0, 0, reinterpret_cast<void*>(controller), reinterpret_cast<void*>(sp + 0xC0), true);
    }, false);
    observe([&] { markers_moment({true, 100, 0.5f, 124, kPartMarkers}); }, false);
    observe([&] { cursor_frame.nzcv = 0; }, false); // unchanged site must wait for the following stores
    // Retained identities are revalidated before every picture, not just when a placement is recorded.
    for (unsigned loss = 0; loss < 6; ++loss) {
        observe([] {}, true);
        if (loss == 0) expired_pose_weak = cursor_component;
        if (loss == 1) put<uint64_t>(cursor_component + t::throw_cursor::owner, player);
        if (loss == 2) put<uint64_t>(main_at() + t::data::world, level);
        if (loss == 3) put<uint64_t>(cursor_owner + t::cursor_placement::owner_controller, player);
        if (loss == 4) put<uint64_t>(cursor_widget + t::cursor_placement::ready, 2);
        if (loss == 5) remapped_weak_object = cursor_component; // same address, different weak serial
        const auto moved = marker_counts(false).moved;
        markers_present();
        CHECK(marker_counts(false).moved == moved && marker_counts(false).marks == 0);
    }
    expired_pose_weak = remapped_weak_object = 0;
    put<uint64_t>(main_at() + t::data::world, world);
    // A replacement component at the same widget must start at its newest endpoint, not blend the old owner.
    observe([] {}, true);
    uint64_t replacement = block(0x800);
    put<uint64_t>(replacement, main_at() + t::cursor_placement::component_table);
    put<uint64_t>(replacement + t::throw_cursor::owner, cursor_owner);
    markers_moment({true, 101, 0.5f, 124, kPartMarkers});
    put<uint64_t>(sp + 0x68, replacement);
    float* out = reinterpret_cast<float*>(sp + 0xC0);
    hook_of<bool (*)(float, float, float, void*, void*, bool)>(site_key(t::code_site::world_to_screen))(
        cursor_anchor + 60, 0, 0, reinterpret_cast<void*>(controller), out, true);
    put<float>(cursor_widget + t::cursor_placement::screen, out[0] + 12);
    put<float>(cursor_widget + t::cursor_placement::position, (out[0] + 12) / 2);
    const float native_place = (out[0] + 12) / 2;
    const auto before_replacement = marker_counts(false);
    tsuchi_native_dispatch(&cursor_frame, main_at() + t::native_routes[t::native_route::cursor_written].returned);
    markers_present();
    CHECK(marker_counts(false).noted == before_replacement.noted + 1 && marker_counts(false).moved == before_replacement.moved + 1);
    CHECK(std::fabs(get<float>(cursor_widget + t::cursor_placement::position) - native_place) < 0.01f);
    std::free(reinterpret_cast<void*>(replacement));
    markers_forget();
}

static void parts_left_out() {
    // With only the camera drawn between steps, a moving part reaches the renderer only at its steps, at whole
    // steps' places, a particle is drawn at its location after its update, and the camera still moves at every
    // picture.
    settings_file = "{\"frame_limit\": 60, \"presenter_log\": true, \"test_parts\": [\"camera\"]}";
    presenter_settings();
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_speed = 30, part_x = 0;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    renderer_x.clear();
    bool camera_half = false, particles_whole = true;
    rendering = true;
    for (int i = 0; i < 12; ++i) {
        sprite_x = -1;
        engine_frame(1.0f / 60);
        // A float's step is about 0.002 at this distance, so the place is compared to a hundredth.
        if (halfway(pictures.back().first)) camera_half = camera_half || std::fabs(shown[0] - (location_x - 15)) < 1e-2f;
        particles_whole = particles_whole && sprite_x == 30;
    }
    rendering = false;
    bool whole = renderer_x.size() == 6;
    for (float x : renderer_x) whole = whole && std::fabs(std::fmod(x, 30.0f)) < 0.01f;
    CHECK(whole && camera_half && particles_whole);
    // With the roots put back, the part is sent at every picture again.
    settings_file = "{\"frame_limit\": 60, \"presenter_log\": true, \"test_parts\": [\"camera\", \"roots\"]}";
    presenter_settings();
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    renderer_x.clear();
    for (int i = 0; i < 12; ++i) engine_frame(1.0f / 60);
    CHECK(renderer_x.size() == 12);
    // A name it does not know is told, and every part is drawn.
    settings_file = "{\"frame_limit\": 60, \"presenter_log\": true, \"test_parts\": [\"camera\", \"water\"]}";
    const size_t told = notes.size();
    presenter_settings();
    CHECK(notes.size() == told + 1);
    part_registered = false, part_speed = 0;
    put<uint64_t>(part + t::primitive::proxy, 0);
}

static void pictures_held_evenly() {
    // The engine starts a frame every sixtieth of a second, and the world's update takes `cost` of each frame that
    // steps. The times between the pictures of twelve frames, and the presenter's waits in them.
    const auto run = [](const char* settings, uint64_t cost) {
        settings_file = settings;
        presenter_settings();
        world_cost_us = cost, waits_pass = true;
        for (int i = 0; i < 20; ++i) {
            const uint64_t began = now_us;
            engine_frame(1.0f / 60);
            now_us = began + 16667;
            if (i == 7) picture_us.clear(), waits.clear();
        }
        world_cost_us = 0, waits_pass = false;
        std::vector<int64_t> gaps;
        for (size_t i = 1; i < picture_us.size(); ++i) gaps.push_back(int64_t(picture_us[i] - picture_us[i - 1]));
        return gaps;
    };
    // Without the hold, a picture after a step comes the update's length late and the one between that early.
    bool uneven = true;
    for (int64_t gap : run("{\"frame_limit\": 60, \"test_even_pictures\": false}", 7000)) uneven = uneven && std::llabs(gap - 16667) > 6000;
    CHECK(uneven && waits.empty());
    // With it, a picture between steps waits for a sixtieth after the one before, so they come evenly.
    bool even = true;
    const std::vector<int64_t> gaps = run("{\"frame_limit\": 60, \"test_even_pictures\": true}", 7000);
    for (int64_t gap : gaps) even = even && std::llabs(gap - 16667) <= 300;
    CHECK(even && gaps.size() == 11 && waits.size() == 6);
    // A picture is never held past six tenths of its frame, so the frame's own work still fits.
    run("{\"frame_limit\": 60, \"test_even_pictures\": true}", 12000);
    bool capped = waits.size() == 6;
    for (uint32_t wait : waits) capped = capped && wait >= 9990 && wait <= 10000;
    CHECK(capped);
}

static void at_one_twenty() {
    // The full native-hook path: 120 pictures retain 30 simulation updates, four picture fractions, and
    // 120 visual base ticks with one second of animation time. This remains a host stand-in result.
    settings_file = "{\"frame_limit\": 120, \"vsync\": false, \"presenter_log\": true}";
    commands.clear();
    presenter_settings();
    for (unsigned i = 0; i < 8; ++i) {
        now_us += 8333;
        engine_frame(1.0f / 120);
        hook_of<void (*)(void*, const void*, float)>(t::slot_group::widget_tick.tables[3] + t::slot_group::widget_tick.offset)
            (reinterpret_cast<void*>(widget_a), nullptr, 1.0f / 120);
    }
    CHECK(presenter_limit() == 120 && max_fps == 120);
    CHECK(commands.size() == 2 && commands[0] == "rhi.SyncInterval 0" && commands[1] == "r.VSync 0");
    CHECK(current_vsync == 0 && native_swap_interval() == 0);
    widget_ticks.clear(), widget_base_ticks.clear(), widget_script_ticks.clear();
    const uint32_t updates = world_updates;
    const size_t before = pictures.size();
    uint32_t fractions[4] = {};
    nested_widget_bases = true;
    for (unsigned i = 0; i < 120; ++i) {
        now_us += 8333;
        engine_frame(1.0f / 120);
        hook_of<void (*)(void*, const void*, float)>(t::slot_group::widget_tick.tables[3] + t::slot_group::widget_tick.offset)
            (reinterpret_cast<void*>(widget_a), nullptr, 1.0f / 120);
        const unsigned share = unsigned(pictures.back().first * 4 + 0.001f);
        if (share < 4) ++fractions[share];
    }
    nested_widget_bases = false;
    CHECK(pictures.size() - before == 120 && world_updates - updates == 30);
    for (uint32_t count : fractions) CHECK(count >= 29 && count <= 31);
    CHECK(widget_base_ticks[widget_a].size() == 120 && widget_ticks[widget_a * 64 + 3].size() == 30);
    CHECK(widget_script_ticks[widget_a].size() == 30);
    float seconds = 0;
    for (float delta : widget_base_ticks[widget_a]) seconds += delta;
    CHECK(std::fabs(seconds - 1.0f) < 1e-5f);
    // Applying game settings can turn VSync back on without changing the selected FPS limit.
    current_vsync = 1;
    now_us += 1100000;
    engine_frame(1.0f / 120);
    CHECK(presenter_limit() == 120 && current_vsync == 0 && native_swap_interval() == 0);
}

static void visual_capacity_in_game() {
    // Saturate the visual history while the frame-counter pace entries later become reusable. An untracked
    // new widget still runs its old30Hz base with accumulated delta, rather than losing half its animation time.
    const auto tick = [](uint64_t widget) {
        hook_of<void (*)(void*, const void*, float)>(t::slot_group::widget_tick.tables[3] + t::slot_group::widget_tick.offset)
            (reinterpret_cast<void*>(widget), nullptr, 1.0f / 60);
    };
    settings_file = "{\"frame_limit\": 30}";
    presenter_settings(), engine_frame(1.0f / 60);
    settings_file = "{\"frame_limit\": 60}";
    presenter_settings(), engine_frame(1.0f / 60);
    for (unsigned i = 0; i < WidgetBases::kRoom; ++i) tick(block(0x10));
    for (unsigned i = 0; i < 64; ++i) engine_frame(1.0f / 60);
    const uint64_t overflow = block(0x10);
    for (unsigned i = 0; i < 8; ++i) engine_frame(1.0f / 60), tick(overflow);
    widget_base_ticks.clear(), widget_script_ticks.clear();
    for (unsigned i = 0; i < 60; ++i) engine_frame(1.0f / 60), tick(overflow);
    CHECK(widget_base_ticks[overflow].size() == 30 && widget_script_ticks[overflow].size() == 30);
    float seconds = 0;
    for (float delta : widget_base_ticks[overflow]) seconds += delta;
    CHECK(std::fabs(seconds - 1.0f) < 1e-5f);
}

static void vsync_modes() {
    // The same configured cap can be synchronized or unsynchronized, and live file changes/removal do not
    // invent a new VSync choice. The window policy is the original NVN code, not the host's display rate.
    const auto change = [](const std::string& text, uint32_t cap, int32_t vsync, int interval) {
        settings_file = text;
        now_us += 1100000;
        engine_frame(1.0f / 60); // live poll; no explicit call to presenter_settings
        CHECK(presenter_limit() == cap && current_vsync == vsync && sync_interval == interval);
        CHECK(max_fps == (text.empty() ? 0 : float(cap)));
        CHECK(native_swap_interval() == (vsync ? std::max(1, interval) : 0));
    };
    for (uint32_t cap : {30u, 60u, 120u, 0u}) {
        change("{\"frame_limit\":" + std::to_string(cap) + ",\"vsync\":true}", cap, 1, cap == 30 ? 2 : 1);
        const size_t before = commands.size();
        now_us += 1100000;
        engine_frame(1.0f / 60);
        CHECK(commands.size() == before); // unchanged polling cannot reset camera/pose/clock history
        current_vsync = game_vsync = 0; // game settings cannot silently replace explicit VSync on
        now_us += 1100000;
        engine_frame(1.0f / 60);
        CHECK(current_vsync == 1);
        change("{\"frame_limit\":" + std::to_string(cap) + ",\"vsync\":false}", cap, 0, 0);
    }
    // Removing the override restores the game's latest preference, written while the override was in
    // force, not the value seen when the first override began.
    change("{\"frame_limit\":120}", 120, game_vsync, game_vsync ? 1 : 0);
    current_vsync = game_vsync = game_vsync ? 0 : 1; // without an override, preserve the game's own live choice
    now_us += 1100000;
    engine_frame(1.0f / 60);
    CHECK(current_vsync == game_vsync);
    change("{\"frame_limit\":60,\"vsync\":false}", 60, 0, 0);
    settings_read_allowed = false;
    now_us += 1100000;
    engine_frame(1.0f / 60);
    CHECK(presenter_limit() == 60 && current_vsync == 0); // transient IO is not file removal
    settings_read_allowed = true;
    // The game chooses VSync while an override is in force, and the override is released before the
    // once-a-second look has seen that choice: the game's newest choice is the one given back.
    change("{\"frame_limit\":60}", 60, game_vsync, game_vsync ? 1 : 0);
    current_vsync = game_vsync = 1;
    change("{\"frame_limit\":60,\"vsync\":true}", 60, 1, 1);
    current_vsync = game_vsync = 0;
    settings_file = "{\"frame_limit\":60}";
    presenter_settings();
    engine_frame(1.0f / 60);
    CHECK(presenter_limit() == 60 && current_vsync == 0);
    // The preference the game set with no override in force is the one an override later gives back.
    change("", 30, game_vsync, game_vsync ? 2 : 0);
    CHECK(get<uint8_t>(engine + t::engine::flags) == 0x41 && max_fps == 0);
}

static void limit_drop_in_game() {
    // A part is shown between two steps when the limit drops to 30 and it stops: the game's own place is sent to
    // the renderer, though the game does not send the part again.
    settings_file = "{\"frame_limit\": 60}";
    presenter_settings(), engine_frame(1.0f / 60);
    put<uint64_t>(part + t::primitive::proxy, 0xABC0);
    part_registered = true, part_drawn = true, part_speed = 30, bone_speed = 0;
    for (int i = 0; i < 6; ++i) engine_frame(1.0f / 60);
    for (int i = 0; i < 4 && std::fabs(renderer_x.back() - part_x) < 0.01f; ++i) engine_frame(1.0f / 60);
    CHECK(std::fabs(renderer_x.back() - part_x) > 1);
    part_speed = 0;
    settings_file = "{\"frame_limit\": 30}";
    presenter_settings();
    for (int i = 0; i < 4; ++i) engine_frame(1.0f / 30);
    CHECK(presenter_limit() == 30 && std::fabs(renderer_x.back() - part_x) < 0.01f);
    settings_file = "{\"frame_limit\": 60}";
    presenter_settings(), engine_frame(1.0f / 60);
}

static void back_to_thirty() {
    // Asked for 30 again: what the engine had comes back, and every frame steps.
    settings_file = "{\"frame_limit\": 30}";
    commands.clear();
    presenter_settings();
    CHECK(presenter_limit_wanted() == 30);
    engine_frame(1.0f / 60);
    CHECK(presenter_limit() == 30);
    CHECK(commands.size() == 1 && commands[0] == (game_vsync ? "rhi.SyncInterval 2" : "rhi.SyncInterval 0"));
    CHECK(current_vsync == game_vsync && native_swap_interval() == (game_vsync ? 2 : 0));
    CHECK(get<uint8_t>(engine + t::engine::flags) == 0x41 && get<float>(engine + t::engine::fixed_rate) == 30);
    CHECK(get<float>(user + t::user_settings::frame_rate_limit) == 0 && max_fps == 0);
    for (int i = 0; i < 5; ++i) CHECK(engine_frame(1.0f / 30));
    CHECK(pictures.back().first == 1);
    // A wrong value is told, and changes nothing.
    settings_file = "{\"frame_limit\": 45}";
    const size_t told = notes.size();
    presenter_settings();
    CHECK(presenter_limit_wanted() == 30 && notes.size() == told + 1);
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "counter-unhooked")) {
        build();
        originals.erase(site_key(t::code_site::pikmin_counter_update));
        presenter_start(reinterpret_cast<TSUCHI_Mod*>(0x1), &clock_us, &wait_us);
        at_thirty();
        settings_file = "{\"frame_limit\":120}";
        presenter_settings();
        engine_frame(1.0f / 30);
        CHECK(presenter_limit() == 30);
        std::printf("%d checks, %d failed\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "read-refused")) {
        build();
        presenter_start(reinterpret_cast<TSUCHI_Mod*>(0x1), &clock_us, &wait_us);
        at_thirty();
        vsync_read_allowed = false;
        settings_file = "{\"frame_limit\": 120}";
        presenter_settings();
        engine_frame(1.0f / 30);
        CHECK(presenter_limit() == 30 && commands.empty() && max_fps == -1);
        CHECK(get<uint8_t>(engine + t::engine::flags) == 0x41);
        std::printf("%d checks, %d failed\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1) initial_vsync = current_vsync = std::atoi(argv[1]);
    game_vsync = initial_vsync;
    clock_alone();
    camera_alone();
    widgets_alone();
    animation_periods_alone();
    snaps_alone();
    build();
    presenter_start(reinterpret_cast<TSUCHI_Mod*>(0x1), &clock_us, &wait_us);
    CHECK(presenter_draws());
    at_thirty();
    // Global presenter readiness alone does not qualify a pose request. A
    // partial scene-hook attempt can retry once the missing native site exists.
    const uint64_t bone_site = site_key(t::code_site::bone_kernel);
    void* bone_original = originals.at(bone_site);
    originals.erase(bone_site);
    CHECK(presenter_pose_request(true, 1) == TSUCHI_NOT_READY);
    CHECK(presenter_pose_request(true, 0) == TSUCHI_NOT_READY);
    CHECK(presenter_pose_policy() == 0 && presenter_limit_wanted() == 30);
    originals[bone_site] = bone_original;
    CHECK(presenter_pose_request(true, 1) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 1);
    CHECK(presenter_pose_request(false, 0) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 0);
    settings_file = "{\"frame_limit\": 60, \"presenter_log\": true}";
    presenter_settings();
    CHECK(presenter_limit_wanted() == 60);
    at_sixty();
    camera_in_game();
    view_clocks();
    widgets_in_game();
    controllers();
    scene_alone();
    scene_in_game();
    pose_wrap_in_game();
    pose_plateau_in_game();
    pose_identity_in_game();
    pose_policy_in_game();
    effects_in_game();
    markers_in_game();
    hung_in_game();
    prompts_in_game();
    materials_in_game();
    material_wrap_in_game();
    parts_left_out();
    cursor_inline_in_game();
    cursor_identity_guards();
    pictures_held_evenly();
    at_one_twenty();
    numeric_widgets_in_game();
    vsync_modes();
    visual_capacity_in_game();
    pikmin_count_publication_in_game();
    camera_service_test();
    limit_drop_in_game();
    back_to_thirty();
    presenter_camera_after(camera_service_after);
    engine_frame(1.0f/30);
    CHECK(shown[4]==17 && service_view_x==location_x);
    presenter_camera_after(nullptr);
    // A row a second, and the second's times after it: at 60 half the frames step, half are between.
    bool rows = false, times = false, calls = false;
    for (const std::string& line : notes) {
        rows = rows || line.rfind("presenter limit 60 pictures ", 0) == 0;
        calls = calls || (line.rfind("presenter hooks all 7 scene_update 7 routes ", 0) == 0 &&
                          line.find("scene_publish ") != std::string::npos);
        times = times || (line.rfind("presenter times start_ms 16.7/16.7 frame_ms 16.7/16.7 step_ms 16.7/16.7 between_ms 16.7/16.7", 0) == 0 &&
                          line.find(" world_updates 30 ") != std::string::npos);
    }
    CHECK(rows && times && calls);
    // A mod's policy does not rewrite the file or bypass the presenter. The
    // latest file preference returns when the request ends.
    CHECK(presenter_request(true, 60, 2) == TSUCHI_OK);
    engine_frame(1.0f / 60);
    CHECK(presenter_limit() == 60 && current_vsync == 0);
    settings_file = "{\"frame_limit\": 120, \"vsync\": true}";
    presenter_settings();
    CHECK(presenter_limit_wanted() == 60);
    CHECK(presenter_request(false, 30, 0) == TSUCHI_OK);
    engine_frame(1.0f / 120);
    CHECK(presenter_limit_wanted() == 120 && presenter_limit() == 120 && current_vsync == 1);
    CHECK(presenter_request(true, 45, 0) == TSUCHI_INVALID);
    CHECK(presenter_limit_wanted() == 120);
    // Requested pose policy and frame policy are independent. A held pose
    // policy restores the latest file choice, including changes while held.
    CHECK(presenter_pose_policy() == 0);
    CHECK(presenter_pose_request(true, 2) == TSUCHI_INVALID);
    CHECK(presenter_pose_policy() == 0);
    settings_file = "{\"frame_limit\": 120, \"vsync\": true, \"pose_presentation\": \"smooth\"}";
    presenter_settings();
    CHECK(presenter_pose_policy() == 1 && presenter_limit_wanted() == 120);
    CHECK(presenter_pose_request(true, 0) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 0);
    CHECK(presenter_request(true, 60, 2) == TSUCHI_OK);
    settings_file = "{\"frame_limit\": 120, \"vsync\": true, \"pose_presentation\": \"selective\"}";
    presenter_settings();
    CHECK(presenter_pose_policy() == 0 && presenter_limit_wanted() == 60);
    CHECK(presenter_pose_request(true, 1) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 1 && presenter_limit_wanted() == 60);
    CHECK(presenter_pose_request(false, 0) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 0 && presenter_limit_wanted() == 60);
    CHECK(presenter_request(false, 30, 0) == TSUCHI_OK);
    CHECK(presenter_limit_wanted() == 120 && presenter_pose_policy() == 0);
    CHECK(presenter_pose_request(true, 0) == TSUCHI_OK);
    settings_file = "{\"frame_limit\": 120, \"vsync\": true, \"pose_presentation\": \"smooth\"}";
    presenter_settings();
    CHECK(presenter_pose_policy() == 0);
    CHECK(presenter_pose_request(false, 0) == TSUCHI_OK);
    CHECK(presenter_pose_policy() == 1);
    // An omitted optional preference selects the unchanged default again.
    settings_file = "{\"frame_limit\": 120, \"vsync\": true}";
    presenter_settings();
    CHECK(presenter_pose_policy() == 0 && presenter_limit_wanted() == 120);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
