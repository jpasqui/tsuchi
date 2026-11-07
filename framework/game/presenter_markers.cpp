// Life gauges and the other marks on the screen that hang from a point in the world, between steps.
//
// The game places them once a step: it projects the point through the player's view, then sets the widget's place
// in the viewport or its render translation from the result. Between steps the world and the camera are drawn
// between the last two steps, but such a mark would stay where the step left it. The presenter notes each
// projection the game makes and the placement that follows it as one mark: the point, where the game saw it, and
// the place it gave the widget. Before each picture it projects the point between the last two steps through the
// picture's own view and moves the widget by the difference. A placement the game repeats between steps from the
// step's own values is given the picture's place instead. Life gauges and action markers set their place without
// the calls; their placements, a table entry and a code site, are noted directly from the projection they make.
// The castaways' role icons and the treasures' markers are placed in a canvas panel, far from where the point was
// projected; such a placement is taken as a mark when the same function of the game projected just before it.
#include "presenter_markers.h"

#include <string.h>

#include "game.h"
#include "hooks.h"
#include "native_routes.h"
#include "objects.h"
#include "presenter_logic.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using namespace presenter;

PresentedMoment moment{};
MarkerCounts counts{};
void count(uint32_t& counter) { ++counter; } // the game's thread only

double at() { return double(moment.steps) - 1 + moment.fraction; }

constexpr float kNearPlace = 1000;    // pixels: a placement this near the projection just before it is placed from it
constexpr float kFarthest = 150;      // a point that moved farther than this in a step jumped
constexpr uint64_t kForgetAfter = 30; // steps without a placement before a mark is forgotten
constexpr uint64_t kSameFunction = 0x3000; // a placement this far after a projection in the game's code is of the same function

enum class MarkKind : uint8_t { viewport, translation, inline_place, gauge, bar, cursor, slot };

// Weak UObject identities plus the native initialized child. No caller-stack pointer survives observation.
struct CursorIdentity {
    uint64_t component, owner, controller, world, ready;
};

struct Mark {
    uint64_t widget, weak, controller, from, to, placed_step, shown_frame;
    float a[3], b[3]; // the point at the last two steps it moved at
    float seen[2];    // where the newest point was projected at its step
    float placed[2];  // what the game gave the widget for it
    float shown[2];   // what this picture gives it
    bool used, pair, pixels, relative;
    MarkKind kind;
    CursorIdentity cursor;
};
constexpr unsigned kMarks = 256;
Mark marks[kMarks] = {};

// Where the game projects points from, for the log: the return address in its code, and how many of its
// projections were noted as a mark.
struct Caller {
    uint64_t at;
    uint32_t projected, noted;
};
constexpr unsigned kCallers = 24;
Caller callers[kCallers] = {};

// The newest projection on the game's thread that no placement has taken yet.
struct Projection {
    int32_t counted; // its place in callers, or -1
    bool valid, relative;
    uint64_t caller, controller, frame, output, world; // caller: where the game's code continues after it, or 0
    float point[3], screen[3];
} last{};
bool presenting = false; // the presenter's own projections and placements are not noted

using WorldToScreen = decltype(t::code_site::world_to_screen)::Function;
using ViewportPosition = decltype(t::code_site::viewport_position)::Function;
using RenderTranslation = decltype(t::code_site::render_translation)::Function;
using GaugePlace = decltype(t::slot::life_gauge_place)::Function;
using MarkerPlace = decltype(t::code_site::action_marker_place)::Function;
using CanvasPosition = decltype(t::code_site::canvas_position)::Function;
WorldToScreen next_project = nullptr;
ViewportPosition next_position = nullptr;
RenderTranslation next_translation = nullptr;
GaugePlace next_gauge = nullptr;
GaugePlace next_bar = nullptr;
CanvasPosition next_canvas = nullptr;
uint64_t project_hook = 0, canvas_hook = 0;
MarkerPlace next_marker = nullptr;

Mark* mark_of(uint64_t widget, bool add) {
    Mark* empty = nullptr;
    for (Mark& mark : marks) {
        if (mark.used && mark.widget == widget) return &mark;
        if (!mark.used && !empty) empty = &mark;
    }
    if (!add || !empty) return nullptr;
    *empty = {};
    empty->used = true, empty->widget = widget, empty->weak = game::to_weak_value(widget);
    return empty;
}

float scale_of(uint64_t widget) {
    const float scale = t::bound(t::fn::widget_scale) ? t::fn::widget_scale(reinterpret_cast<void*>(widget)) : 1.0f;
    return scale > 0.01f && finite(scale) ? scale : 1.0f;
}

// The game projected `point` to `screen` and placed `widget` at `placed`: the newest step of that mark.
void note(MarkKind kind, uint64_t widget, const Projection& from, const float placed[2], bool pixels,
          const CursorIdentity* cursor = nullptr) {
    Mark* mark = mark_of(widget, true);
    if (!mark) return;
    const uint64_t weak = game::to_weak_value(widget);
    if (mark->kind != kind || mark->controller != from.controller || mark->relative != from.relative || mark->weak != weak ||
        (cursor && memcmp(&mark->cursor, cursor, sizeof *cursor)))
        mark->to = 0, mark->pair = false;
    mark->weak = weak;
    if (cursor) mark->cursor = *cursor;
    mark->kind = kind, mark->controller = from.controller, mark->pixels = pixels, mark->relative = from.relative;
    const float* point = from.point;
    const float* screen = from.screen;
    if (!mark->to) {
        memcpy(mark->b, point, sizeof mark->b), mark->to = moment.steps;
    } else if (memcmp(mark->b, point, sizeof mark->b)) {
        if (moment.steps != mark->to) {
            float move = 0;
            mark->pair = moment.steps - mark->to <= 4 && !jumped(mark->b, point, kFarthest / 4, move);
            memcpy(mark->a, mark->b, sizeof mark->a), mark->from = mark->to, mark->to = moment.steps;
        }
        memcpy(mark->b, point, sizeof mark->b);
    }
    memcpy(mark->seen, screen, sizeof mark->seen);
    memcpy(mark->placed, placed, sizeof mark->placed);
    mark->placed_step = moment.steps, mark->shown_frame = 0;
    count(counts.noted);
    if (from.counted >= 0) ++callers[from.counted].noted;
}

bool project(uint64_t controller, bool relative, const float point[3], float screen[2]) {
    float out[3] = {};
    presenting = true;
    const bool front = next_project(point[0], point[1], point[2], reinterpret_cast<void*>(controller), out, relative);
    presenting = false;
    if (!front || !finite(out[0]) || !finite(out[1])) return false;
    screen[0] = out[0], screen[1] = out[1];
    return true;
}

// ---- The game's calls --------------------------------------------------------------------------------------

bool world_to_screen(float x, float y, float z, void* controller, void* out, bool relative) {
    const bool front = next_project(x, y, z, controller, out, relative);
    if (presenting || !moment.active || !on_game_thread()) return front;
    last.valid = front && out;
    last.counted = -1;
    last.caller = hook_invocation(project_hook).caller;
    if (last.caller) {
        const uint64_t at = last.caller - game::main_base();
        for (unsigned i = 0; i < kCallers && last.counted < 0; ++i)
            if (callers[i].at == at || !callers[i].at) callers[i].at = at, ++callers[i].projected, last.counted = int32_t(i);
    }
    if (last.valid) {
        last.controller = reinterpret_cast<uint64_t>(controller), last.frame = moment.frame, last.relative = relative;
        last.output = reinterpret_cast<uint64_t>(out), last.world = game::to_weak_value(game::global<uint64_t>(t::data::world));
        last.point[0] = x, last.point[1] = y, last.point[2] = z;
        // This exact WithDistance site stores XY at05868D70 and distance atD74 before its sole true returnD98.
        // It is not the two-float ProjectWorldToScreen API. Failure paths never read the caller's output here.
        memcpy(last.screen, out, sizeof last.screen);
    }
    return front;
}

// A placement right after a projection, near where it projected, is placed from it.
bool from_projection(float px, float py) {
    if (!last.valid || last.frame != moment.frame) return false;
    last.valid = false;
    const float dx = px - last.screen[0], dy = py - last.screen[1];
    return dx * dx + dy * dy < kNearPlace * kNearPlace;
}

// Between steps the game may place a mark again from the step's values; it is given the picture's place.
bool repeated(uint64_t widget, float x, float y, float out[2]) {
    Mark* mark = mark_of(widget, false);
    if (!mark || mark->shown_frame != moment.frame || mark->placed[0] != x || mark->placed[1] != y) return false;
    memcpy(out, mark->shown, sizeof mark->shown);
    count(counts.repeated);
    return true;
}

void viewport_position(float x, float y, void* widget, bool pixels) {
    if (presenting || !moment.active || !widget || !on_game_thread()) return next_position(x, y, widget, pixels);
    const uint64_t at_widget = reinterpret_cast<uint64_t>(widget);
    float shown[2];
    if (repeated(at_widget, x, y, shown)) return next_position(shown[0], shown[1], widget, pixels);
    const float scale = pixels ? 1.0f : scale_of(at_widget);
    if (from_projection(x * scale, y * scale)) {
        const float placed[2] = {x, y};
        note(MarkKind::viewport, at_widget, last, placed, pixels);
    }
    next_position(x, y, widget, pixels);
}

// A widget's place in a canvas panel is in the canvas's units, offset from its anchors and lifted as the game
// likes, so it is far from the point's place on the screen. It is a mark when the function that projected just
// before placed it: the projection's caller and the placement's are a little apart in the same function.
void canvas_position(float x, float y, void* slot) {
    if (presenting || !moment.active || !slot || !on_game_thread()) return next_canvas(x, y, slot);
    const uint64_t at_slot = reinterpret_cast<uint64_t>(slot);
    float shown[2];
    if (repeated(at_slot, x, y, shown)) return next_canvas(shown[0], shown[1], slot);
    const uint64_t caller = hook_invocation(canvas_hook).caller;
    if (last.valid && last.frame == moment.frame && last.caller && caller > last.caller && caller - last.caller < kSameFunction) {
        last.valid = false;
        const float placed[2] = {x, y};
        note(MarkKind::slot, at_slot, last, placed, false);
    }
    next_canvas(x, y, slot);
}

void render_translation(float x, float y, void* widget) {
    if (presenting || !moment.active || !widget || !on_game_thread()) return next_translation(x, y, widget);
    const uint64_t at_widget = reinterpret_cast<uint64_t>(widget);
    float shown[2];
    if (repeated(at_widget, x, y, shown)) return next_translation(shown[0], shown[1], widget);
    const float scale = scale_of(at_widget);
    if (from_projection(x * scale, y * scale)) {
        const float placed[2] = {x, y};
        note(MarkKind::translation, at_widget, last, placed, false);
    }
    next_translation(x, y, widget);
}

// A life gauge or an action marker projects its point and sets its place in the viewport itself, without the
// call: what it projected in the call and the place it set are its mark. What it adds to the point (a lift to one
// side, the screen's scale, a push back onto the screen) stays as it is.
void placed_inline(MarkKind kind, uint64_t widget) {
    const bool projected = last.valid && last.frame == moment.frame;
    const Projection from = last;
    last.valid = false; // its own projection is not another's placement
    float placed[2];
    const uint64_t position = kind == MarkKind::bar ? t::life_bar::position : t::life_gauge::position;
    if (!projected || !moment.active || !widget || !on_game_thread() || !game::read(widget + position, placed)) return;
    note(kind, widget, from, placed, false);
}
void gauge_place(void* gauge, float seconds) {
    last.valid = false;
    next_gauge(gauge, seconds);
    placed_inline(MarkKind::gauge, reinterpret_cast<uint64_t>(gauge));
}
void bar_place(void* gauge, float seconds) {
    last.valid = false;
    next_bar(gauge, seconds);
    placed_inline(MarkKind::bar, reinterpret_cast<uint64_t>(gauge));
}
void marker_place(void* marker) {
    last.valid = false;
    next_marker(marker);
    placed_inline(MarkKind::inline_place, reinterpret_cast<uint64_t>(marker));
}

uint64_t live_weak(uint64_t object) {
    const uint64_t weak = object ? game::to_weak_value(object) : 0;
    return weak && game::from_weak_value(weak) == object ? weak : 0;
}

bool cursor_identity(uint64_t widget, uint64_t component, uint64_t controller, CursorIdentity& out) {
    namespace c = t::cursor_placement;
    const uint64_t main = game::main_base();
    uint64_t widget_table = 0, component_table = 0, owner = 0, ready = 0;
    if (!widget || !component || !controller || !live_weak(widget) || !game::read(widget, widget_table) ||
        widget_table != main + c::widget_table || !game::read(widget + c::ready, ready) || !ready ||
        !game::read(component, component_table) || component_table != main + c::component_table ||
        !game::read(component + t::throw_cursor::owner, owner))
        return false;
    out = {live_weak(component), live_weak(owner), live_weak(controller),
           live_weak(game::global<uint64_t>(t::data::world)), ready};
    return out.component && out.owner && out.controller && out.world &&
           game::from_weak_reference(owner + c::owner_controller) == controller;
}

// Where the placement is inlined into the cursor's update, the route is inside it: its stack frame holds the
// update's widget and component and the return address. Where the update calls it, the route is where the call
// returns, with both still in the update's registers. Either way the producer's projection output is an offset
// from the stack. These are immediate association checks only; note() copies values and weak identities, never
// this output/stack pointer.
bool cursor_owner(const NativeFrame& frame, uint64_t& widget, uint64_t& component) {
    namespace r = t::route_frame::cursor_written;
    widget = frame.x[r::widget];
    if (!t::present(r::saved_component)) {
        component = frame.x[r::component];
        return component != 0;
    }
    uint64_t saved = 0, returned = 0;
    return game::read(frame.sp + r::saved_widget, saved) && saved == widget && game::read(frame.sp + r::saved_component, component) &&
           game::read(frame.sp + r::saved_return, returned) && returned == game::main_base() + t::cursor_placement::producer_return;
}
void cursor_placed(NativeFrame& frame) {
    if (presenting || !moment.active || !on_game_thread() || !last.valid || last.frame != moment.frame) return;
    const Projection from = last;
    last = {};
    uint64_t widget = 0, component = 0;
    namespace r = t::route_frame::cursor_written;
    if (!frame.sp || frame.sp > UINT64_MAX - r::frame_end || !from.relative || from.output != frame.sp + r::projected ||
        !cursor_owner(frame, widget, component))
        return;
    CursorIdentity identity{};
    float projected[3], cached[2], placed[2];
    if (!cursor_identity(widget, component, from.controller, identity) || identity.world != from.world ||
        !game::read(from.output, projected) || memcmp(projected, from.screen, sizeof projected) ||
        !game::read(widget + t::cursor_placement::screen, cached) ||
        !game::read(widget + t::cursor_placement::position, placed))
        return;
    for (float value : from.point) if (!finite(value)) return;
    for (float value : projected) if (!finite(value)) return;
    // Cache504/508 already includes PositionOffset and native distance corrections; it need not equal the
    // raw projection. Preserve that offset via placed/seen rather than mistaking it for a different owner.
    for (float value : cached) if (!finite(value)) return;
    for (float value : placed) if (!finite(value)) return;
    note(MarkKind::cursor, widget, from, placed, false, &identity);
}
void cursor_unchanged(NativeFrame& frame) {
    // B.EQ: with Z clear, native code has not stored its new placement yet; the written route will observe it.
    if (frame.nzcv & (1ull << 30)) cursor_placed(frame);
}

bool cursor_alive(const Mark& mark) {
    CursorIdentity now{};
    return cursor_identity(mark.widget, game::from_weak_value(mark.cursor.component), mark.controller, now) &&
           !memcmp(&now, &mark.cursor, sizeof now);
}

} // namespace

bool markers_start(TSUCHI_Mod* self) {
    uint64_t id = 0;
    bool all = hook_site(self, t::code_site::world_to_screen, &world_to_screen, &next_project, &project_hook) == TSUCHI_OK;
    // Without the projection there is nothing to place from.
    if (!next_project) return false;
    all = hook_site(self, t::code_site::viewport_position, &viewport_position, &next_position, &id) == TSUCHI_OK && all;
    all = hook_site(self, t::code_site::render_translation, &render_translation, &next_translation, &id) == TSUCHI_OK && all;
    all = hook_slot(self, t::slot::life_gauge_place, &gauge_place, &next_gauge, kHookNeedsStock, &id) == TSUCHI_OK && all;
    all = hook_slot(self, t::slot::life_bar_place, &bar_place, &next_bar, kHookNeedsStock, &id) == TSUCHI_OK && all;
    all = hook_site(self, t::code_site::canvas_position, &canvas_position, &next_canvas, &canvas_hook) == TSUCHI_OK && all;
    all = hook_site(self, t::code_site::action_marker_place, &marker_place, &next_marker, &id) == TSUCHI_OK && all;
    // A build whose written route sits after the whole placement has no unchanged path of its own to observe.
    if (t::present(t::native_routes[t::native_route::cursor_unchanged].address))
        all = native_route(t::native_route::cursor_unchanged, &cursor_unchanged) && all;
    all = native_route(t::native_route::cursor_written, &cursor_placed) && all;
    return all;
}

void markers_moment(const PresentedMoment& now) {
    moment = now;
    last = {};
}

void markers_present() {
    if (!moment.active || !(moment.parts & kPartMarkers) || !next_project) return;
    const double place = at();
    for (Mark& mark : marks) {
        if (!mark.used) continue;
        if (mark.placed_step + kForgetAfter < moment.steps || !mark.weak || game::from_weak_value(mark.weak) != mark.widget ||
            (mark.kind == MarkKind::cursor && !cursor_alive(mark))) {
            mark.used = false;
            continue;
        }
        // Only a mark placed at the newest step is where the game wants it; one it stopped placing is left be.
        if (mark.placed_step != moment.steps) continue;
        if (mark.kind == MarkKind::gauge && !game::read_or<uint8_t>(mark.widget + t::life_gauge::shown)) continue;
        if (mark.kind == MarkKind::bar && !game::read_or<uint8_t>(mark.widget + t::life_bar::shown)) continue;
        const float f = mark.pair ? place_between(mark.from, mark.to, place) : 1;
        float point[3], screen[2];
        for (unsigned i = 0; i < 3; ++i) point[i] = f < 1 ? mark.a[i] + (mark.b[i] - mark.a[i]) * f : mark.b[i];
        if (!project(mark.controller, mark.relative, point, screen)) continue;
        // A slot's place is in its canvas's units, the screen's over its scale, as a widget's own place is.
        const uint64_t scaled = mark.kind == MarkKind::slot ? game::read_or<uint64_t>(mark.widget + t::canvas_slot::content) : mark.widget;
        const float scale = mark.pixels ? 1.0f : scale_of(scaled);
        mark.shown[0] = mark.placed[0] + (screen[0] - mark.seen[0]) / scale;
        mark.shown[1] = mark.placed[1] + (screen[1] - mark.seen[1]) / scale;
        mark.shown_frame = moment.frame;
        presenting = true;
        if (mark.kind == MarkKind::translation) next_translation(mark.shown[0], mark.shown[1], reinterpret_cast<void*>(mark.widget));
        else if (mark.kind == MarkKind::slot) next_canvas(mark.shown[0], mark.shown[1], reinterpret_cast<void*>(mark.widget));
        else next_position(mark.shown[0], mark.shown[1], reinterpret_cast<void*>(mark.widget), mark.pixels);
        presenting = false;
        count(counts.moved);
    }
}

void markers_forget() {
    for (Mark& mark : marks) mark.used = false;
    last = {};
}

void marker_callers(char* text, unsigned capacity) {
    unsigned used = 0;
    const auto put = [&](char c) { if (used + 1 < capacity) text[used++] = c; };
    const auto number = [&](uint64_t value, unsigned base) {
        char digits[20];
        unsigned many = 0;
        do digits[many++] = "0123456789ABCDEF"[value % base];
        while ((value /= base) && many < sizeof digits);
        while (many) put(digits[--many]);
    };
    for (Caller& caller : callers) {
        if (!caller.at) break;
        put(' '), number(caller.at, 16), put(':'), number(caller.projected, 10), put('/'), number(caller.noted, 10);
        caller = {};
    }
    if (capacity) text[used] = 0;
}

MarkerCounts marker_counts(bool clear) {
    MarkerCounts out = counts;
    out.marks = 0;
    for (const Mark& mark : marks) out.marks += mark.used;
    if (clear) counts = {};
    return out;
}

} // namespace tsuchi
