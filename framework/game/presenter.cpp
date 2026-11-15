// The presenter's game side: the frame limit, and the hooks that step the world thirty times a second and keep the
// pictures between steps right. The arithmetic is in presenter_logic.h.
#include "presenter.h"

#include <string.h>

#include "engine_call.h"
#include "files.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "json.h"
#include "mods.h"
#include "native_routes.h"
#include "objects.h"
#include "presenter_effects.h"
#include "presenter_logic.h"
#include "presenter_markers.h"
#include "presenter_scene.h"
#include "registry.h"

namespace tsuchi {
namespace {
void (*camera_after)(uint64_t, uint64_t) = nullptr;


namespace t = game::table;
using namespace presenter;

// The input service's filter, applied to what a controller's device read finds, and whether the presenter is
// reading a controller itself. While it is, the input service leaves what the system says alone.
uint64_t (*input_after)(uint32_t, uint64_t, bool) = nullptr;
bool sampling_input = false;

constexpr const char* kSettingsFile = "rom:/tsuchi/settings.json";

// What the framework's settings ask for.
uint32_t wanted = 30, configured_limit = 30, requested_limit = 30;
bool policy_held = false;
enum class SyncChoice { game, on, off };
SyncChoice wanted_sync = SyncChoice::game, applied_sync = SyncChoice::game;
SyncChoice configured_sync = SyncChoice::game, requested_sync = SyncChoice::game;
uint32_t wanted_pose = 0, configured_pose = 0, requested_pose = 0, applied_pose = 0;
uint32_t pose_retry = 0;
bool pose_held = false;
void select_policy() {
    wanted = policy_held ? requested_limit : configured_limit;
    wanted_sync = policy_held ? requested_sync : configured_sync;
    const uint32_t pose = pose_held ? requested_pose : configured_pose;
    if (pose != wanted_pose) pose_retry = 0;
    wanted_pose = pose;
}
uint64_t settings_checked = 0, settings_hash = 0;
bool settings_present = false;
bool log_rows = false;
// For a test that takes pictures slower than the game makes them: every frame counts as this long, and the game's
// thread waits this long at the start of each frame. Neither is for play.
double test_frame_seconds = 0;
uint32_t test_pause_ms = 0;
uint64_t test_pause_after = 0, world_since = 0; // microseconds after the newest world began
// And only in a world whose name starts with this, when it is set, so the title and the loading screens keep their pace.
char test_pause_world[64] = {};
bool pause_world = false;
// The parts drawn between steps; a test leaves some out to measure what each costs.
uint32_t parts = kAllParts;
// A picture between steps waits until a picture's length after the one before, so the pictures come evenly; a
// test turns it off to measure without.
bool even_pictures = true;
constexpr uint64_t kHoldEarly = 200;  // microseconds before its time a held picture goes on, for the wait's own lateness
constexpr uint64_t kHoldLeast = 200;  // a shorter hold is not waited for
constexpr uint64_t kHoldLatest = 60;  // nor one past this share, in hundredths, of a picture's length after its frame began

TSUCHI_Mod* self = nullptr;
uint64_t (*microseconds)() = nullptr;
void (*wait_us)(uint32_t) = nullptr;

// Where each hook continues.
using WorldTick = decltype(t::code_site::world_tick)::Function;
using FamilyMade = decltype(t::code_site::view_family)::Function;
using WidgetTick = decltype(t::slot_group::widget_tick)::Function;
using Redraw = decltype(t::slot::redraw_viewports)::Function;
using ViewPoint = decltype(t::slot::view_point)::Function;
using NpadRead = decltype(t::slot::npad_read)::Function;
using CounterUpdate = decltype(t::code_site::pikmin_counter_update)::Function;
WorldTick next_world = nullptr;
FamilyMade next_family = nullptr;
Redraw next_redraw = nullptr;
ViewPoint next_view = nullptr;
NpadRead next_npad = nullptr;
CounterUpdate next_counter_update = nullptr;
constexpr unsigned kWidgetTables = sizeof t::tables::widget_tick_tables / sizeof t::tables::widget_tick_tables[0];
constexpr unsigned kWidgetEntries = kWidgetTables + 3; // direct animation, HUD-number and digit superclass ticks
WidgetTick next_widget[kWidgetEntries] = {};
WidgetTick next_widget_base = nullptr;
WidgetTick next_widget_script = nullptr;
bool ready = false; // the world's update and the engine's drawing are hooked: the presenter can run

// The limit in force, and what the engine had before anything was changed.
uint32_t applied = 30;
struct Baseline {
    uint64_t engine;
    uint8_t fixed;
    float rate, user_limit;
    int32_t vsync;
    bool held;
} baseline{};

Clock clock;
Frame now;
bool active = false, in_draw = false, in_step = false, stepped = false;
uint64_t world_key = 0, pictures = 0, frames = 0;

struct Camera {
    uint64_t player, world, manager, target, subject, used;
    uint8_t projection;
    CameraHistory history;
};
constexpr unsigned kCameras = 4;
Camera cameras[kCameras] = {};
ViewTime view_time;
WidgetPaces paces;
WidgetBases widget_bases;
WidgetPaces widget_scripts;
struct WidgetScope { void* widget; uint64_t key; float seconds; } paced_widget{};
InputLatch latch;

// The log's row, once a second.
struct Counts {
    uint32_t pictures, steps, world_skipped, cameras, families, widget_runs, widget_waits, npad_step, npad_other, latched;
    uint64_t widget_entries; // which of the widget entries ticked, one bit each
    uint64_t marking_us;     // spent marking what is still between steps, before the drawing
    uint64_t drawing_us;     // spent in the engine's drawing on the game's thread, the end of the frame's sends among it
    uint32_t fractions[5];
    uint32_t frames;
    float frame_ms[240];
    // The game's thread: from each frame's start to the end of its drawing, and whether the frame stepped.
    float game_ms[240];
    bool stepped[240];
    // The game's thread from the end of each frame's drawing to the next frame's start: the engine's own end of
    // the frame, the interface, and the waits for the renderer and the limit. Whether that frame stepped.
    float tail_ms[240];
    bool tail_stepped[240];
    uint32_t tails;
    // From each frame's start to the next one's: the engine's own frame length.
    float start_ms[240];
    uint32_t starts;
    uint64_t world_us, framework_us; // in the world's update, and in the framework's own frame
    uint32_t world_updates;
    // The pictures held to come evenly, the time they waited, and how much later than asked they went on.
    uint32_t held;
    uint64_t held_us, late_us;
} counts{};
uint64_t second_started = 0, checked_at = 0, last_picture = 0, frame_began = 0, drawn_at = 0, last_began = 0;
bool drawn_stepped = false;
bool log_started = false;

uint64_t world_now() { return game::global<uint64_t>(t::data::world); }
uint64_t engine_now() { return game::global<uint64_t>(t::data::engine); }

void note(const char* text) { mod_note(kFrameworkLogId, text); }

// ---- The frame limit ----------------------------------------------------------------------------

TSUCHI_Result console(const char* command) {
    static game::ClassRef library{"KismetSystemLibrary"};
    const uint64_t type = game::find_class(library), context = world_now();
    if (!type || !context) return TSUCHI_NOT_READY;
    game::EngineCall call(type, "ExecuteConsoleCommand");
    TSUCHI_Result result = call.set("WorldContextObject", game::object_value(game::to_handle(context)));
    if (result == TSUCHI_OK) result = call.set_text("Command", command);
    return result == TSUCHI_OK ? call.invoke() : result;
}

TSUCHI_Result vsync_value(int32_t& out) {
    static game::ClassRef library{"KismetSystemLibrary"};
    const uint64_t type = game::find_class(library);
    if (!type) return TSUCHI_NOT_READY;
    game::EngineCall call(type, "GetConsoleVariableIntValue");
    TSUCHI_Result result = call.set_text("VariableName", "r.VSync");
    if (result == TSUCHI_OK) result = call.invoke();
    game::Value value;
    if (result == TSUCHI_OK) result = call.get("ReturnValue", value);
    if (result != TSUCHI_OK) return result;
    if (value.kind != game::ValueKind::integer || value.integer < INT32_MIN || value.integer > INT32_MAX) return TSUCHI_INVALID;
    out = int32_t(value.integer);
    return TSUCHI_OK;
}

TSUCHI_Result set_vsync(int32_t value) {
    char command[32] = "r.VSync ", digits[12];
    unsigned at = 8, many = 0;
    uint64_t magnitude = value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value);
    if (value < 0) command[at++] = '-';
    do digits[many++] = char('0' + magnitude % 10);
    while (magnitude /= 10);
    while (many) command[at++] = digits[--many];
    command[at] = 0;
    return console(command);
}

void forget_pictures() {
    for (Camera& camera : cameras) camera = {};
    view_time.forget();
    paces.forget();
    widget_bases.forget();
    widget_scripts.forget();
    latch.clear();
    clock.forget();
    scene_forget();
    effects_forget();
    markers_forget();
}

// Puts the engine at a limit. At 30 what it had before comes back. False while it cannot be done yet.
bool set_limit(uint32_t limit) {
    const uint64_t engine = engine_now();
    uint8_t flags = 0;
    float rate = 0;
    if (!engine || !world_now() || !t::bound(t::fn::set_max_fps) || !game::read(engine + t::engine::flags, flags) ||
        !game::read(engine + t::engine::fixed_rate, rate))
        return false;
    const uint64_t user = game::read_or<uint64_t>(engine + t::engine::user_settings);
    if (!baseline.held || baseline.engine != engine) {
        int32_t vsync = 0;
        if (vsync_value(vsync) != TSUCHI_OK) return false;
        baseline = {engine, uint8_t(flags & t::constant::engine_fixed_rate), rate, game::read_or<float>(user + t::user_settings::frame_rate_limit), vsync, true};
    }
    // With no override in force, the live value is the game's own preference: an override gives that one back.
    if (wanted_sync != SyncChoice::game && applied_sync == SyncChoice::game && vsync_value(baseline.vsync) != TSUCHI_OK) return false;
    const bool fixed = limit == 30;
    const bool restore_sync = wanted_sync == SyncChoice::game && applied_sync != SyncChoice::game;
    // The game may have chosen its own VSync since the once-a-second look: a live value that is not the
    // override's is that choice, and the one given back.
    int32_t live = 0;
    if (restore_sync && vsync_value(live) == TSUCHI_OK && live != (applied_sync == SyncChoice::on ? 1 : 0)) baseline.vsync = live;
    int32_t vsync = wanted_sync == SyncChoice::on ? 1 : wanted_sync == SyncChoice::off ? 0 : baseline.vsync;
    if (wanted_sync == SyncChoice::game && !restore_sync && vsync_value(vsync) != TSUCHI_OK) return false;
    // NVN clamps interval0 to one refresh when VSync is on. Keep the refresh choice independent of the cap.
    if (console(!vsync ? "rhi.SyncInterval 0" : fixed ? "rhi.SyncInterval 2" : "rhi.SyncInterval 1") != TSUCHI_OK) return false;
    if ((wanted_sync != SyncChoice::game || restore_sync) && set_vsync(vsync) != TSUCHI_OK) return false;
    const uint8_t mask = uint8_t(t::constant::engine_fixed_rate);
    game::write<uint8_t>(engine + t::engine::flags, uint8_t((flags & ~mask) | (fixed ? baseline.fixed : 0)));
    if (fixed) game::write<float>(engine + t::engine::fixed_rate, baseline.rate);
    // An explicit VSync choice at30 still needs a30FPS cap when no refresh wait is requested.
    const float most = fixed && wanted_sync == SyncChoice::game ? baseline.user_limit : float(limit);
    if (user) game::write<float>(user + t::user_settings::frame_rate_limit, most);
    t::fn::set_max_fps(reinterpret_cast<void*>(engine), most);
    applied = limit;
    applied_sync = wanted_sync;
    active = !fixed;
    forget_pictures();
    note(fixed                ? "presenter: frame limit 30"
         : limit == 60   ? "presenter: frame limit 60 (experimental: higher frame rates are still being worked on)"
         : limit == 120  ? "presenter: frame limit 120 (experimental: higher frame rates are still being worked on)"
                         : "presenter: no frame limit (experimental: higher frame rates are still being worked on)");
    return true;
}

// Once a second: the game may have set its own limit since, as it does when it applies its settings.
void keep_limit() {
    const uint64_t engine = engine_now();
    const uint64_t user = game::read_or<uint64_t>(engine + t::engine::user_settings);
    const bool fixed = game::read_or<uint8_t>(engine + t::engine::flags) & t::constant::engine_fixed_rate;
    const float limit = game::read_or<float>(user + t::user_settings::frame_rate_limit, float(applied));
    int32_t vsync = 0;
    const bool sync_changed = wanted_sync != SyncChoice::game && vsync_value(vsync) == TSUCHI_OK &&
                              vsync != (wanted_sync == SyncChoice::on ? 1 : 0);
    // The game applied its settings over the override: that is its preference now, the one removal gives back.
    if (sync_changed) baseline.vsync = vsync;
    const bool fixed_changed = applied == 30 ? fixed != bool(baseline.fixed) : fixed;
    const float wanted_limit = applied == 30 && wanted_sync == SyncChoice::game ? baseline.user_limit : float(applied);
    if (engine && (fixed_changed || (user && limit != wanted_limit) || sync_changed)) {
        note("presenter: the game changed the frame limit; set again");
        set_limit(applied);
    }
}

// ---- Steps and pictures -------------------------------------------------------------------------

// The world's limits on a frame's length, where its settings clamp it the engine's own way.
void frame_limits(double& least, double& most) {
    least = most = 0;
    const uint64_t level = game::read_or<uint64_t>(world_now() + t::world::persistent_level);
    const uint64_t settings = level ? game::read_or<uint64_t>(level + t::engine_level::world_settings) : 0;
    const uint64_t functions = settings ? game::read_or<uint64_t>(settings) : 0;
    if (!functions || game::read_or<uint64_t>(functions + t::world_settings::fix_delta) != game::main_base() + t::constant::fix_delta) return;
    float low = 0, high = 0;
    if (game::read(settings + t::world_settings::min_delta, low) && game::read(settings + t::world_settings::max_delta, high) && low >= 0 && high >= low)
        least = low, most = high;
}

// Between steps the game reads no controller, so what is held at each picture is kept for the next step.
void poll_controllers() {
    if (!t::bound(t::fn::npad_style_set)) return;
    sampling_input = true;
    for (unsigned i = 0; i < latch.count(); ++i) {
        const uint32_t id = latch.id(i);
        const uint32_t style = t::fn::npad_style_set(&id);
        alignas(8) uint8_t state[t::npad_state::most] = {};
        if (style & t::constant::npad_full_key && t::fn::npad_full_key_state) t::fn::npad_full_key_state(state, &id);
        else if (style & t::constant::npad_handheld && t::fn::npad_handheld_state) t::fn::npad_handheld_state(state, &id);
        else if (style & t::constant::npad_joy_dual && t::fn::npad_joy_dual_state) t::fn::npad_joy_dual_state(state, &id);
        else continue;
        uint64_t buttons;
        memcpy(&buttons, state + t::npad_state::buttons, sizeof buttons);
        latch.add(id, buttons);
    }
    sampling_input = false;
}

// A number written into a line.
struct Line {
    char text[512];
    unsigned used = 0;
    void add(const char* piece) {
        while (*piece && used + 1 < sizeof text) text[used++] = *piece++;
        text[used] = 0;
    }
    void add(int64_t value) {
        char digits[24];
        unsigned count = 0;
        const bool negative = value < 0;
        uint64_t left = negative ? uint64_t(-value) : uint64_t(value);
        do digits[count++] = char('0' + left % 10);
        while ((left /= 10) && count < sizeof digits);
        if (negative) add("-");
        char piece[2] = {0, 0};
        while (count) piece[0] = digits[--count], add(piece);
    }
    void add_ms(float value) { // tenths of a millisecond
        const int64_t tenths = int64_t(value * 10 + 0.5f);
        add(tenths / 10);
        add(".");
        add(tenths % 10);
    }
};

float nth(float* values, uint32_t count, uint32_t rank) {
    for (uint32_t i = 1; i < count; ++i)
        for (uint32_t j = i; j && values[j - 1] > values[j]; --j) {
            const float swap = values[j];
            values[j] = values[j - 1], values[j - 1] = swap;
        }
    return count ? values[rank < count ? rank : count - 1] : 0;
}

// The times of the second: the frames that stepped and the frames between apart, each as their median and 95th
// percentile, then the game's thread's time to the end of the drawing in each, and the time in the world's update
// and in the framework's own frame. The frame times are taken before log_row sorts them.
struct Times {
    float step[240], between[240], step_game[240], between_game[240], all[240], step_tail[240], between_tail[240];
    uint32_t steps = 0, betweens = 0, step_tails = 0, between_tails = 0;
};
Times times;

void take_times(uint32_t frames) {
    times.steps = times.betweens = times.step_tails = times.between_tails = 0;
    for (uint32_t i = 0; i < counts.tails; ++i)
        if (counts.tail_stepped[i]) times.step_tail[times.step_tails++] = counts.tail_ms[i];
        else times.between_tail[times.between_tails++] = counts.tail_ms[i];
    for (uint32_t i = 0; i < frames; ++i) {
        times.all[i] = counts.frame_ms[i];
        if (counts.stepped[i]) times.step[times.steps] = counts.frame_ms[i], times.step_game[times.steps++] = counts.game_ms[i];
        else times.between[times.betweens] = counts.frame_ms[i], times.between_game[times.betweens++] = counts.game_ms[i];
    }
}

void log_times(uint32_t frames, EffectCounts& effects) {
    Line line;
    const auto pair = [&](const char* name, float* values, uint32_t count) {
        line.add(name), line.add_ms(nth(values, count, count / 2)), line.add("/"), line.add_ms(nth(values, count, count * 95 / 100));
    };
    line.add("presenter times");
    pair(" start_ms ", counts.start_ms, counts.starts);
    pair(" frame_ms ", times.all, frames);
    pair(" step_ms ", times.step, times.steps);
    pair(" between_ms ", times.between, times.betweens);
    pair(" step_game_ms ", times.step_game, times.steps);
    pair(" between_game_ms ", times.between_game, times.betweens);
    pair(" step_tail_ms ", times.step_tail, times.step_tails);
    pair(" between_tail_ms ", times.between_tail, times.between_tails);
    pair(" render_ms ", effects.render_gap_ms, effects.render_gaps < 240 ? effects.render_gaps : 240);
    pair(" scene_ms ", effects.scene_ms, effects.scenes_timed < 240 ? effects.scenes_timed : 240);
    // How evenly the pictures came: the share in hundredths of the times between pictures, on the game's thread and
    // the render thread's, within 2.5 ms of the limit's own.
    const auto even = [&](const char* name, const float* values, uint32_t count) {
        const float period = applied ? 1000.0f / float(applied) : 0;
        uint32_t near = 0;
        for (uint32_t i = 0; i < count; ++i) near += values[i] > period - 2.5f && values[i] < period + 2.5f;
        line.add(name), line.add(int64_t(count ? near * 100 / count : 0));
    };
    even(" even ", times.all, frames);
    even(" render_even ", effects.render_gap_ms, effects.render_gaps < 240 ? effects.render_gaps : 240);
    line.add(" world_us "), line.add(int64_t(counts.world_us)), line.add(" world_updates "), line.add(int64_t(counts.world_updates));
    line.add(" framework_us "), line.add(int64_t(counts.framework_us)), line.add(" parts "), line.add(int64_t(parts));
    line.add(" held "), line.add(int64_t(counts.held)), line.add(" held_us "), line.add(int64_t(counts.held_us));
    line.add(" late_us "), line.add(int64_t(counts.late_us));
    note(line.text);
    // The calls through the places hooks are on, the most called first.
    HookSample hot[8];
    uint32_t all = 0;
    const unsigned found = hook_sample(hot, 8, &all);
    Line calls;
    calls.add("presenter hooks all "), calls.add(int64_t(all));
    for (unsigned i = 0; i < found; ++i) calls.add(" "), calls.add(hot[i].name), calls.add(" "), calls.add(int64_t(hot[i].calls));
    // And through the native routes, which every pass costs whether or not a handler is on.
    calls.add(" routes");
    for (unsigned route = 0; route < t::native_route_count; ++route)
        calls.add(" "), calls.add(t::native_routes[route].name), calls.add(" "), calls.add(int64_t(native_route_sample(route)));
    note(calls.text);
}

void log_row(uint64_t time) {
    take_times(counts.frames < 240 ? counts.frames : 240);
    Line line;
    const uint32_t frames = counts.frames < 240 ? counts.frames : 240;
    const float most = nth(counts.frame_ms, frames, frames - 1);
    const float middle = nth(counts.frame_ms, frames, frames / 2), high = nth(counts.frame_ms, frames, frames * 99 / 100);
    // How many steps the row is off from thirty a second of the wall's time. Below 30 frames a second it falls
    // behind, as the game does without the presenter.
    const int64_t expected = int64_t(((time - second_started) * 30 + 500000) / 1000000);
    line.add("presenter limit "), line.add(int64_t(applied)), line.add(" pictures "), line.add(int64_t(counts.pictures));
    line.add(" steps "), line.add(int64_t(counts.steps)), line.add(" step_error "), line.add(int64_t(counts.steps) - expected);
    line.add(" frame_ms "), line.add_ms(middle), line.add("/"), line.add_ms(high), line.add("/"), line.add_ms(most);
    line.add(" fraction");
    for (uint32_t share : counts.fractions) line.add(" "), line.add(int64_t(share));
    line.add(" world_skipped "), line.add(int64_t(counts.world_skipped));
    line.add(" camera "), line.add(int64_t(counts.cameras)), line.add(" families "), line.add(int64_t(counts.families));
    line.add(" widgets "), line.add(int64_t(counts.widget_runs)), line.add("/"), line.add(int64_t(counts.widget_waits));
    if (counts.widget_entries) {
        line.add(" entries");
        for (unsigned entry = 0; entry < 64; ++entry)
            if (counts.widget_entries >> entry & 1) line.add(" "), line.add(int64_t(entry));
    }
    line.add(" npad "), line.add(int64_t(counts.npad_step)), line.add("/"), line.add(int64_t(counts.npad_other));
    line.add(" latched "), line.add(int64_t(counts.latched));
    const SceneCounts scene = scene_counts(true);
    line.add(" roots "), line.add(int64_t(scene.roots)), line.add("/"), line.add(int64_t(scene.root_blends)), line.add("/"), line.add(int64_t(scene.root_sends));
    line.add(" bones "), line.add(int64_t(scene.bones)), line.add("/"), line.add(int64_t(scene.bone_blends)), line.add("/"), line.add(int64_t(scene.bone_sends));
    line.add(" cuts "), line.add(int64_t(scene.cuts)), line.add(" jumps "), line.add(int64_t(scene.jumps)), line.add(" snaps "), line.add(int64_t(scene.snaps));
    line.add(" unseen "), line.add(int64_t(scene.unseen)), line.add(" reduced "), line.add(int64_t(scene.reduced));
    line.add(" small "), line.add(int64_t(scene.small)), line.add("/"), line.add(int64_t(scene.small_roots)), line.add("/"),
        line.add(int64_t(scene.still_roots));
    EffectCounts effects = effect_counts(true);
    line.add(" crowds "), line.add(int64_t(effects.crowd_parts)), line.add("/"), line.add(int64_t(effects.crowd_instances)), line.add("/"),
        line.add(int64_t(effects.crowd_held));
    line.add(" counters "), line.add(int64_t(effects.counters_moved)), line.add("/"), line.add(int64_t(effects.counter_sends));
    line.add(" renders "), line.add(int64_t(effects.renders_known)), line.add("/"), line.add(int64_t(effects.renders));
    line.add(" particles "), line.add(int64_t(effects.sprites)), line.add("/"), line.add(int64_t(effects.meshes));
    line.add(" materials "), line.add(int64_t(effects.material_values)), line.add("/"), line.add(int64_t(effects.material_blends)), line.add("/"),
        line.add(int64_t(effects.material_restores)), line.add("/"), line.add(int64_t(effects.material_snaps));
    const MarkerCounts marks = marker_counts(true);
    line.add(" marks "), line.add(int64_t(marks.marks)), line.add("/"), line.add(int64_t(marks.noted)), line.add("/"),
        line.add(int64_t(marks.moved)), line.add("/"), line.add(int64_t(marks.repeated));
    line.add(" marking_us "), line.add(int64_t(counts.marking_us));
    line.add(" drawing_us "), line.add(int64_t(counts.drawing_us)), line.add(" rendering_us "), line.add(int64_t(effects.rendering_us));
    note(line.text);
    char callers[400];
    marker_callers(callers, sizeof callers);
    Line projected;
    projected.add("presenter projections"), projected.add(callers);
    note(projected.text);
    log_times(frames, effects);
    // The selected limit is a ceiling, not a measurement. These are game-thread pictures and renderer calls,
    // not display scans; keep their measured rates beside the wall-clock interval that produced the counts.
    Line rate;
    const uint64_t elapsed = time - second_started;
    rate.add("presenter rate target "), rate.add(int64_t(applied));
    rate.add(" pictures_per_second "), rate.add_ms(elapsed ? float(double(counts.pictures) * 1000000 / double(elapsed)) : 0);
    rate.add(" render_calls_per_second "), rate.add_ms(elapsed ? float(double(effects.renders) * 1000000 / double(elapsed)) : 0);
    rate.add(" sample_ms "), rate.add_ms(float(elapsed) / 1000);
    rate.add(" vsync_requested "), rate.add(wanted_sync == SyncChoice::on ? "on" : wanted_sync == SyncChoice::off ? "off" : "game");
    int32_t vsync = 0;
    rate.add(" vsync_effective ");
    if (vsync_value(vsync) == TSUCHI_OK) rate.add(int64_t(vsync));
    else rate.add("unknown");
    note(rate.text);
}

// ---- Hooks ------------------------------------------------------------------------------------

// The world's update, timed for the log's times.
void timed_world(void* world, uint32_t kind, float seconds) {
    const uint64_t began = log_rows && microseconds ? microseconds() : 0;
    next_world(world, kind, seconds);
    if (began) counts.world_us += microseconds() - began;
    ++counts.world_updates;
}

void world_tick(void* world, uint32_t kind, float seconds) {
    if (kind != t::constant::world_tick_all || reinterpret_cast<uint64_t>(world) != world_now() || !on_game_thread())
        return next_world(world, kind, seconds);
    if (!active) return timed_world(world, kind, seconds);
    if (!now.step || stepped) {
        ++counts.world_skipped;
        return;
    }
    stepped = true;
    // The engine's own frame length is a step's within it, as at the fixed rate.
    const uint64_t length = game::main_base() + t::data::app_delta;
    double before = 0;
    const bool scoped = game::read(length, before) && game::write(length, kStep);
    in_step = true;
    timed_world(world, kind, kStepSeconds);
    in_step = false;
    if (scoped) game::write(length, before);
    latch.clear();
    const uint64_t at = reinterpret_cast<uint64_t>(world);
    view_time.capture(clock.steps(), {game::read_or<float>(at + t::world::seconds), game::read_or<float>(at + t::world::delta_seconds),
                                      game::read_or<float>(at + t::world::real_seconds)});
}

// A frame that steps draws after the world's update, and one between steps draws at once, so without a hold the
// pictures would come the update's length late and then that early, though the frames themselves come evenly. A
// picture between steps is held until a picture's length after the one before, and never past six tenths of its
// frame, so the frame's own work after the drawing still fits in it. Only at a limit: with none there is no length.
void hold_picture() {
    if (!active || now.step || !even_pictures || !wait_us || !microseconds || !last_picture || !frame_began || (applied != 60 && applied != 120))
        return;
    const uint64_t length = 1000000 / applied;
    uint64_t until = last_picture + length - kHoldEarly;
    const uint64_t latest = frame_began + length * kHoldLatest / 100;
    if (until > latest) until = latest;
    const uint64_t time = microseconds();
    if (until < time + kHoldLeast) return;
    wait_us(uint32_t(until - time));
    const uint64_t woke = microseconds();
    ++counts.held;
    counts.held_us += woke - time;
    if (woke > until) counts.late_us += woke - until;
}

void redraw(void* engine, bool show) {
    if (!on_game_thread()) return next_redraw(engine, show);
    hold_picture();
    ++pictures;
    ++counts.pictures;
    ++counts.fractions[!active || now.fraction >= 1 ? 4 : uint32_t(now.fraction * 4)];
    // The time between pictures on the wall's clock: at the fixed rate the engine's own frame length is always 1/30.
    const uint64_t time = microseconds ? microseconds() : 0;
    const uint32_t frame = counts.frames;
    if (frame < 240) counts.frame_ms[frame] = last_picture && time > last_picture ? float(time - last_picture) / 1000 : 0;
    if (frame < 240) counts.stepped[frame] = !active || now.step;
    last_picture = time;
    ++counts.frames;
    frame_picture(active ? now.fraction : 1.0f, float(now.seconds));
    const uint64_t marking = microseconds ? microseconds() : 0;
    scene_present();
    effects_present();
    if (microseconds) counts.marking_us += microseconds() - marking;
    in_draw = true;
    markers_present(); // its projections go through the picture's view
    const uint64_t drawing = microseconds ? microseconds() : 0;
    next_redraw(engine, show);
    if (microseconds) {
        const uint64_t drawn = microseconds();
        counts.drawing_us += drawn - drawing;
        if (frame < 240) counts.game_ms[frame] = frame_began && drawn > frame_began ? float(drawn - frame_began) / 1000 : 0;
        drawn_at = drawn, drawn_stepped = !active || now.step;
    }
    in_draw = false;
}

// Which of the cameras this player's is, by who it is, what it looks through and whom it follows. A change of any
// of these starts its pictures again at the newest view.
Camera* camera_of(uint64_t player, uint64_t output) {
    const uint64_t controller = game::read_or<uint64_t>(player + t::local_player::controller);
    const uint64_t manager = controller ? game::read_or<uint64_t>(controller + t::player_controller::camera_manager) : 0;
    if (!manager || (output < manager + t::camera_manager::size && output + t::view_info::size > manager)) return nullptr;
    const uint64_t target = game::read_or<uint64_t>(manager + t::camera_manager::view_target);
    uint64_t subject = 0;
    // The game's camera notes whom it follows; only its own finder is known to leave that note right.
    const uint64_t functions = target ? game::read_or<uint64_t>(target) : 0;
    if (functions && game::read_or<uint64_t>(functions + t::game_camera::subject_finder) == game::main_base() + t::constant::camera_subject) {
        const uint64_t part = game::from_weak_reference(target + t::game_camera::subject);
        const uint64_t owner = game::read_or<uint64_t>(target + t::game_camera::subject_owner);
        if (part && game::read_or<uint64_t>(part + t::scene_component::owner) == owner) subject = owner;
    }
    const uint64_t keys[] = {game::to_weak_value(player), world_key, game::to_weak_value(manager), target ? game::to_weak_value(target) : 0,
                             subject ? game::to_weak_value(subject) : 0};
    if (!keys[0] || !keys[2] || (target && !keys[3])) return nullptr;
    const uint8_t projection = game::read_or<uint8_t>(output + t::view_info::projection);
    Camera* found = nullptr;
    for (Camera& camera : cameras)
        if (camera.player == keys[0]) found = &camera;
    if (!found) {
        found = &cameras[0];
        for (Camera& camera : cameras)
            if (camera.used < found->used) found = &camera;
        *found = {};
        found->player = keys[0];
    }
    if (found->world != keys[1] || found->manager != keys[2] || found->target != keys[3] || found->subject != keys[4] ||
        found->projection != projection) {
        found->history.forget();
        found->world = keys[1], found->manager = keys[2], found->target = keys[3], found->subject = keys[4], found->projection = projection;
    }
    found->used = pictures;
    return found;
}

void blend_view(void* player, void* output, int32_t eye) {
    next_view(player, output, eye);
    if (!active || !in_draw || !player || !output || !on_game_thread()) return;
    const uint64_t out = reinterpret_cast<uint64_t>(output);
    Camera* camera = camera_of(reinterpret_cast<uint64_t>(player), out);
    CameraPose pose;
    if (!camera || !game::read(out + t::view_info::location, pose.location) || !game::read(out + t::view_info::rotation, pose.rotation) ||
        !game::read(out + t::view_info::fov, pose.fov) || !game::read(out + t::view_info::desired_fov, pose.desired_fov))
        return;
    if (!(parts & kPartCamera)) return scene_view(pose.location, pose.fov); // the step's own view
    const uint64_t manager = game::from_weak_value(camera->manager);
    const bool cut = game::read_or<uint8_t>(manager + t::camera_manager::cut) & t::constant::camera_cut;
    CameraPose shown;
    if (!camera->history.capture(clock.steps(), pose, cut) || !camera->history.sample(now.fraction, shown)) return;
    game::write(out + t::view_info::location, shown.location);
    game::write(out + t::view_info::rotation, shown.rotation);
    game::write(out + t::view_info::fov, shown.fov);
    game::write(out + t::view_info::desired_fov, shown.desired_fov);
    scene_view(shown.location, shown.fov);
    ++counts.cameras;
}

void view_point(void* player, void* output, int32_t eye) {
    blend_view(player, output, eye);
    if (camera_after && player && output && on_game_thread())
        camera_after(reinterpret_cast<uint64_t>(player), reinterpret_cast<uint64_t>(output));
}

void family_made(void* family, const void* values) {
    next_family(family, values);
    if (!active || !in_draw || !family || !values || !on_game_thread()) return;
    const uint64_t at = reinterpret_cast<uint64_t>(values), made = reinterpret_cast<uint64_t>(family);
    const uint64_t scene = game::read_or<uint64_t>(at + t::view_family_values::scene);
    if (!scene || game::read_or<uint64_t>(scene + t::scene_world::world) != world_now() ||
        !(game::read_or<uint8_t>(at + t::view_family_values::flags) & t::constant::view_times_set))
        return;
    const WorldTimes given{game::read_or<float>(at + t::view_family_values::world_seconds), game::read_or<float>(at + t::view_family_values::delta_seconds),
                           game::read_or<float>(at + t::view_family_values::real_seconds)};
    WorldTimes shown;
    if (!view_time.sample(pictures, clock.steps(), now.fraction, given, shown)) return;
    game::write(made + t::view_family::world_seconds, shown.seconds);
    game::write(made + t::view_family::delta_seconds, shown.delta);
    game::write(made + t::view_family::real_seconds, shown.real);
    effects_family(shown.seconds, now.fraction);
    ++counts.families;
}

void npad_read(void* device) {
    if (!on_game_thread()) return next_npad(device);
    // The device's own read stays physical, so the latch keeps what the player held. The input service's
    // presses and hidden buttons are applied once, after the latch.
    const bool before = sampling_input;
    sampling_input = true;
    next_npad(device);
    sampling_input = before;
    const uint64_t at = reinterpret_cast<uint64_t>(device);
    uint32_t id;
    if (!game::read(at + t::npad_device::id, id)) return;
    const uint64_t read = game::read_or<uint64_t>(at + t::npad_device::buttons);
    uint64_t buttons = read;
    if (active) {
        latch.know(id);
        if (in_step) {
            ++counts.npad_step;
            buttons |= latch.held(id);
            latch.read(id, buttons);
            if (buttons != read) ++counts.latched;
        } else ++counts.npad_other;
    }
    if (input_after) buttons = input_after(id, buttons, !active || in_step);
    if (buttons != read) game::write(at + t::npad_device::buttons, buttons);
}

void counter_update(void* counter, uint64_t flags) {
    // Actor classification fills the write bank during a world update. The
    // null-world tickable publishes that bank on every engine picture; doing
    // that between steps would publish an empty bank and clear the real count.
    // Initial, forced and explicitly requested recounts retain native behavior.
    uint8_t initialized = 0, forced = 0;
    const uint64_t object = reinterpret_cast<uint64_t>(counter);
    if (active && !now.step && on_game_thread() && object && flags == 0 &&
        game::read(object + t::pikmin_counter::initialized, initialized) && initialized &&
        game::read(object + t::pikmin_counter::force_recount, forced) && !forced)
        return;
    next_counter_update(counter, flags);
}

// The game's integer counters remain paced, while its observed visual base gets this picture's own delta.
// Nested paced parents inherit that delta rather than the child's accumulated delta.
struct PacedWidgetScope {
    WidgetScope before;
    PacedWidgetScope(void* widget, uint64_t key, float seconds) : before(paced_widget) {
        paced_widget = {widget, key, before.widget == widget ? before.seconds : seconds};
    }
    ~PacedWidgetScope() { paced_widget = before; }
};

void widget_base_tick(void* widget, const void* geometry, float seconds) {
    if (active && on_game_thread() && widget == paced_widget.widget && paced_widget.key) {
        if (!widget_bases.admit(paced_widget.key, frames)) return;
        // With no room to retain this widget, intermediate base calls cannot be supplied. Keep the native
        // accumulated delta in that case, so the visual animation does not run slower than before.
        if (widget_bases.known(paced_widget.key)) seconds = paced_widget.seconds;
    }
    next_widget_base(widget, geometry, seconds);
}

void widget_script_tick(void* widget, const void* geometry, float seconds) {
    // The game's Blueprint Event Tick may itself count frames. Keep its prior cadence and accumulated
    // delta inside a paced native widget; UMG sequence updates and ordinary unpaced widgets stay separate.
    if (active && on_game_thread() && widget == paced_widget.widget && paced_widget.key && widget_bases.known(paced_widget.key) &&
        !widget_scripts.admit(paced_widget.key, 0, clock.steps(), seconds))
        return;
    next_widget_script(widget, geometry, seconds);
}

template <unsigned I>
void widget_tick(void* widget, const void* geometry, float seconds) {
    if (!on_game_thread()) return next_widget[I](widget, geometry, seconds);
    const uint64_t key = active ? game::to_weak_value(reinterpret_cast<uint64_t>(widget)) : 0;
    PacedWidgetScope scope(widget, key, seconds);
    if (active) {
        if (key && !paces.admit(key, I, clock.steps(), seconds)) {
            ++counts.widget_waits;
            if (next_widget_base && widget_bases.known(key)) widget_base_tick(widget, geometry, paced_widget.seconds);
            return;
        }
    }
    ++counts.widget_runs;
    counts.widget_entries |= 1ull << I;
    next_widget[I](widget, geometry, seconds);
}

template <unsigned... I>
struct Indices {};
template <unsigned N, unsigned... I>
struct MakeIndices : MakeIndices<N - 1, N - 1, I...> {};
template <unsigned... I>
struct MakeIndices<0, I...> {
    using type = Indices<I...>;
};
struct WidgetHooks {
    WidgetTick at[kWidgetEntries];
};
template <unsigned... I>
constexpr WidgetHooks widget_hooks(Indices<I...>) {
    return {{&widget_tick<I>...}};
}
constexpr WidgetHooks kWidgetHooks = widget_hooks(MakeIndices<kWidgetEntries>::type{});

} // namespace

void presenter_start(TSUCHI_Mod* owner, uint64_t (*clock_microseconds)(), void (*wait)(uint32_t)) {
    self = owner;
    microseconds = clock_microseconds;
    wait_us = wait;
    uint64_t id = 0;
    // The world's update and the drawing make the presenter; without them it stays at 30. The rest are put on
    // when the limit first goes above 30 (hook_between).
    ready = hook_site(self, t::code_site::world_tick, &world_tick, &next_world, &id) == TSUCHI_OK &&
            hook_slot(self, t::slot::redraw_viewports, &redraw, &next_redraw, kHookNeedsStock, &id) == TSUCHI_OK;
}

// The parts a list names. A name it does not know is told, and every part is drawn.
static uint32_t parts_of(const Json* listed) {
    static const struct {
        const char* name;
        uint32_t part;
    } kNames[] = {{"camera", kPartCamera}, {"roots", kPartRoots}, {"bones", kPartBones}, {"crowds", kPartCrowds},
                  {"particles", kPartParticles}, {"markers", kPartMarkers}, {"materials", kPartMaterials}};
    if (listed->kind != Json::Kind::list) return note("settings.json: test_parts is a list of part names"), kAllParts;
    uint32_t chosen = 0;
    for (const Json* item = listed->first; item; item = item->next) {
        uint32_t found = 0;
        for (const auto& known : kNames)
            if (item->kind == Json::Kind::text && !strcmp(item->text, known.name)) found = known.part;
        if (!found) return note("settings.json: test_parts names camera, roots, bones, crowds, particles, markers and materials"), kAllParts;
        chosen |= found;
    }
    return chosen;
}

static void default_settings() {
    configured_limit = 30, configured_sync = SyncChoice::game, log_rows = false;
    configured_pose = 0;
    test_frame_seconds = 0, test_pause_ms = 0, test_pause_after = 0, test_pause_world[0] = 0;
    parts = kAllParts, even_pictures = true;
}

static void read_settings(bool only_changed) {
    char* data = nullptr;
    uint64_t size = 0;
    const TSUCHI_Result read = game::read_whole_file(kSettingsFile, data, size);
    if (read != TSUCHI_OK) {
        if (read == TSUCHI_NOT_FOUND && settings_present) default_settings(), select_policy(), settings_present = false;
        return;
    }
    // The framework's few presentation settings are small. A malformed oversized file is never parsed.
    if (size > 16384) return release(data);
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t i = 0; i < size; ++i) hash = (hash ^ uint8_t(data[i])) * 1099511628211ull;
    if (only_changed && settings_present && hash == settings_hash) return release(data);
    settings_present = true, settings_hash = hash;
    const Json* root = read_json(data, size);
    if (!root || root->kind != Json::Kind::object) {
        note("settings.json is not a JSON object; the prior settings stay in force");
    } else {
        default_settings();
        if (const Json* sync = root->find("vsync")) {
            if (sync->kind == Json::Kind::boolean) configured_sync = sync->boolean ? SyncChoice::on : SyncChoice::off;
            else note("settings.json: vsync is true or false; the game's preference is used");
        }
        if (const Json* limit = root->find("frame_limit")) {
            if (limit->kind == Json::Kind::number && limit->whole && (limit->integer == 30 || limit->integer == 60 || limit->integer == 120 || limit->integer == 0))
                configured_limit = uint32_t(limit->integer);
            else
                note("settings.json: frame_limit is 30, 60, 120 or 0; it stays 30");
        }
        if (const Json* pose = root->find("pose_presentation")) {
            if (pose->kind == Json::Kind::text && !strcmp(pose->text, "smooth")) configured_pose = 1;
            else if (pose->kind != Json::Kind::text || strcmp(pose->text, "selective"))
                note("settings.json: pose_presentation is selective or smooth; it stays selective");
        }
        if (const Json* rows = root->find("presenter_log")) {
            if (rows->kind == Json::Kind::boolean) log_rows = rows->boolean;
            else note("settings.json: presenter_log is true or false");
        }
        if (const Json* length = root->find("test_frame_seconds"))
            test_frame_seconds = length->kind == Json::Kind::number && length->number > 0 && length->number <= 0.1 ? length->number : 0;
        if (const Json* wait = root->find("test_pause_ms"))
            test_pause_ms = wait->kind == Json::Kind::number && wait->whole && wait->integer > 0 && wait->integer <= 1000 ? uint32_t(wait->integer) : 0;
        if (const Json* after = root->find("test_pause_after_seconds"))
            test_pause_after = after->kind == Json::Kind::number && after->whole && after->integer >= 0 ? uint64_t(after->integer) * 1000000 : 0;
        if (const Json* world = root->find("test_pause_world")) {
            if (world->kind == Json::Kind::text && strlen(world->text) < sizeof test_pause_world) memcpy(test_pause_world, world->text, strlen(world->text) + 1);
            else note("settings.json: test_pause_world is the start of a world's name");
        }
        if (const Json* listed = root->find("test_parts")) parts = parts_of(listed);
        if (const Json* even = root->find("test_even_pictures")) {
            if (even->kind == Json::Kind::boolean) even_pictures = even->boolean;
            else note("settings.json: test_even_pictures is true or false");
        }
    }
    if (root) release_json(root);
    release(data);
    select_policy();
}

void presenter_settings() {
    read_settings(false);
    settings_checked = microseconds ? microseconds() : 0;
}

// The hooks that keep the pictures between steps right, put on when the limit first goes above 30. At 30 the
// presenter changes nothing, and a hook that does nothing still costs every call through it. False when one the
// presenter needs could not be put on: the limit then stays 30.
static bool hook_between() {
    static int tried = 0; // 1: on, -1: one it needs is missing
    if (tried) return tried > 0;
    uint64_t id = 0;
    // Each keeps one thing right; one that cannot be put on leaves only that thing as at 30.
    hook_slot(self, t::slot::view_point, &view_point, &next_view, 0, &id);
    hook_slot(self, t::slot::npad_read, &npad_read, &next_npad, kHookNeedsStock, &id);
    hook_site(self, t::code_site::view_family, &family_made, &next_family, &id);
    hook_site(self, t::code_site::widget_base_tick, &widget_base_tick, &next_widget_base, &id);
    hook_site(self, t::code_site::widget_script_tick, &widget_script_tick, &next_widget_script, &id);
    hook_site(self, t::code_site::pikmin_counter_update, &counter_update, &next_counter_update, &id);
    for (unsigned i = 0; i < kWidgetTables; ++i)
        hook_group_entry(self, t::slot_group::widget_tick, i, kWidgetHooks.at[i], &next_widget[i], &id);
    hook_site(self, t::code_site::widget_animation_tick, kWidgetHooks.at[kWidgetTables], &next_widget[kWidgetTables], &id);
    hook_site(self, t::code_site::widget_hud_number_tick, kWidgetHooks.at[kWidgetTables + 1], &next_widget[kWidgetTables + 1], &id);
    hook_site(self, t::code_site::widget_digit_tick, kWidgetHooks.at[kWidgetTables + 2], &next_widget[kWidgetTables + 2], &id);
    scene_start(self);                 // without it, what moves in the world is shown at its steps
    effects_start(self, microseconds); // and without this, the Pikmin's tops and the particles
    markers_start(self);               // and the gauges and markers on the screen
    bool all = next_view && next_npad && next_family && next_widget_base && next_widget_script && next_counter_update;
    for (WidgetTick& next : next_widget)
        if (!next) all = false; // an unhooked widget would count frames at the picture rate
    tried = all ? 1 : -1;
    if (!all) note("presenter: a hook it needs could not be put on; the frame limit stays 30");
    return all;
}

bool presenter_frame(float seconds) {
    frame_began = microseconds ? microseconds() : 0;
    if (drawn_at && frame_began > drawn_at && counts.tails < 240)
        counts.tail_ms[counts.tails] = float(frame_began - drawn_at) / 1000, counts.tail_stepped[counts.tails++] = drawn_stepped;
    drawn_at = 0;
    if (last_began && frame_began > last_began && counts.starts < 240) counts.start_ms[counts.starts++] = float(frame_began - last_began) / 1000;
    last_began = frame_began;
    stepped = false;
    const uint64_t time = microseconds ? microseconds() : 0;
    if (time && time - settings_checked >= 1000000) settings_checked = time, read_settings(true);
    // Tried once a second until the game can take it: it needs a world, and the engine's class of console calls.
    static uint32_t wait = 0;
    const bool changed = wanted != applied || wanted_sync != applied_sync;
    if (ready && changed && wanted != 30 && !hook_between()) ready = false;
    if (ready && changed && (!wait || !--wait) && !set_limit(wanted)) wait = 30;
    if (wanted_pose != applied_pose && (!pose_retry || !--pose_retry)) {
        if (!wanted_pose || (ready && scene_start(self))) {
            scene_pose_policy(wanted_pose == 1);
            applied_pose = wanted_pose;
        } else pose_retry = 30;
    }
    if (baseline.held && (active || wanted_sync != SyncChoice::game) && time - checked_at >= 1000000) checked_at = time, keep_limit();
    if (!active) {
        now = {true, 1.0f, double(seconds)};
    } else {
        const uint64_t key = game::to_weak_value(world_now());
        if (key != world_key) world_key = key, world_since = time, pause_world = false, forget_pictures();
        // The world's name is known once it has begun.
        game::Name name{};
        char text[64] = {};
        if (test_pause_ms && !pause_world)
            pause_world = !test_pause_world[0] ||
                          (game::world_name(name) && game::name_text(name, text, sizeof text) && !memcmp(text, test_pause_world, strlen(test_pause_world)));
        double least, most;
        frame_limits(least, most);
        if (test_pause_ms && wait_us && pause_world && time - world_since >= test_pause_after) wait_us(test_pause_ms * 1000);
        now = clock.advance(test_frame_seconds > 0 ? test_frame_seconds : seconds, least, most);
        if (!now.step) poll_controllers();
    }
    if (now.step) ++counts.steps;
    const PresentedMoment moment{active, clock.steps(), now.fraction, ++frames, parts};
    scene_moment(moment);
    effects_moment(moment);
    markers_moment(moment);
    // The log's row, once a second: the first second only starts the count.
    if (log_rows && time - second_started >= 1000000) {
        if (log_started) log_row(time);
        log_started = true;
        second_started = time;
        counts = {};
    }
    return now.step;
}

void presenter_frame_done() {
    if (log_rows && microseconds && frame_began) counts.framework_us += microseconds() - frame_began;
}

bool presenter_draws() { return next_redraw != nullptr; }
bool presenter_in_step() { return in_step; }

void presenter_cut_camera(uint64_t actor) {
    const uint64_t key = game::to_weak_value(actor);
    for (Camera& camera : cameras)
        if (key && (camera.target == key || camera.subject == key)) camera.history.forget();
}
uint32_t presenter_limit_wanted() { return wanted; }
uint32_t presenter_limit() { return applied; }

void presenter_camera_after(void (*apply)(uint64_t, uint64_t)) { camera_after = apply; }
bool presenter_drawing_view() { return in_draw; }
void presenter_input_after(uint64_t (*filter)(uint32_t, uint64_t, bool)) { input_after = filter; }
bool presenter_sampling_input() { return sampling_input; }

bool presenter_sample_input(uint32_t pad, void* state) {
    if (!on_game_thread() || !state || !t::bound(t::fn::npad_style_set)) return false;
    latch.know(pad);
    sampling_input = true;
    const uint32_t style = t::fn::npad_style_set(&pad);
    const auto poll = style & t::constant::npad_full_key ? t::fn::npad_full_key_state :
                      style & t::constant::npad_handheld ? t::fn::npad_handheld_state :
                      style & t::constant::npad_joy_dual ? t::fn::npad_joy_dual_state : nullptr;
    if (poll) poll(state, &pad);
    sampling_input = false;
    if (!poll) return false;
    uint8_t* const at = static_cast<uint8_t*>(state) + t::npad_state::buttons;
    uint64_t buttons;
    memcpy(&buttons, at, sizeof buttons);
    buttons |= latch.held(pad);
    // The input service samples once per world step. This keeps the physical baseline even when a menu reads
    // no controller in that step.
    latch.read(pad, buttons);
    memcpy(at, &buttons, sizeof buttons);
    return true;
}

} // namespace tsuchi

namespace tsuchi {
TSUCHI_Result presenter_request(bool held, uint32_t limit, uint32_t sync) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (held && (limit != 0 && limit != 30 && limit != 60 && limit != 120)) return TSUCHI_INVALID;
    if (held && sync > 2) return TSUCHI_INVALID;
    if (held && !ready) return TSUCHI_NOT_READY;
    policy_held = held;
    requested_limit = limit; requested_sync = SyncChoice(sync);
    select_policy();
    return TSUCHI_OK;
}
void presenter_policy(uint32_t& limit, uint32_t& sync) {
    limit = wanted; sync = uint32_t(wanted_sync);
}
TSUCHI_Result presenter_pose_request(bool held, uint32_t mode) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (held && mode > 1) return TSUCHI_INVALID;
    // Scene hooks may be installed before a higher-rate frame is selected. They
    // pass through at thirty; an unheld default never installs them here.
    if (held && (!ready || !self || !scene_start(self))) return TSUCHI_NOT_READY;
    pose_held = held;
    requested_pose = mode;
    select_policy();
    return TSUCHI_OK;
}
uint32_t presenter_pose_policy() { return wanted_pose; }
}
