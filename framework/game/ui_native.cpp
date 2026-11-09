// Generated forms share the Settings category, focus and CmnVerticalScroll path.
#include "ui_native.h"
#include "ui_screens.h"
#include "ui_mod_list_native.h"
#include "ui_feedback.h"
#include "ui_label_layout.h"
#include "ui_form_layout.h"
#include "ui_native_classes.h"
#include "cost_clock.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "mods.h"
#include "native_routes.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include <string.h>
#if __has_include("input_service.h")
#include "input_service.h"
#define TSUCHI_UI_INPUT 1
#else
#define TSUCHI_UI_INPUT 0
#endif
namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef category_class{t::name::ui_mod_list_category_class}, row_class{t::name::ui_mod_list_row_class};
game::ClassRef panel_class{t::name::ui_mod_list_panel_class}, panel_base_class{t::name::ui_panel_base_class};
game::ClassRef slot_class{t::name::ui_mod_list_slot_class};
bool wrapped = false, opening = false;
uint32_t closing_depth = 0;
TSUCHI_Mod* input_mod = nullptr;
using Npad = decltype(t::slot::npad_read)::Function;
Npad next_npad = nullptr;
bool (*buttons_provider)(uint32_t, uint64_t&) = nullptr;
bool (*menu_dispatcher)(uint64_t, uint32_t, bool&, bool&) = nullptr;
bool input_step_registered = false;
bool layout_step_registered = false;
uint64_t layout_epoch = 1;
void layout_frame() { ++layout_epoch; }
void (*button_listener)(uint64_t) = nullptr;
struct Array { uint64_t data; int32_t count, capacity; };
struct Row {
    UiKind kind;
    uint64_t widget, label, value, selector, inner, left, right;
    Text caption, value_text;
    Texts labels;
    uint32_t choice;
    int64_t integer, minimum, maximum, step;
    bool rendered, message_fit;
    float wrap_width;
    UiLabelFitStamp fit;
};
struct View {
    TSUCHI_Mod* mod;
    uint64_t screen, host, tree, parent, scroll, category, group, baseline_group, source_data, rows_data;
    Array before;
    int32_t before_selected, before_previous;
    float scroll_x, scroll_y, total, viewport;
    uint32_t revision, calls, selected, reading;
    // The row read is not the control in focus: a passive row past the last control or before the first.
    bool away;
    bool retired, showing, delivered, closing, owned, installed, borrowed, have_scroll, confirm_wait;
    bool logged_cold, logged_warm, report_warm;
    uint64_t previous_buttons, used, build_us, install_us;
    uint64_t layout_epoch, layout_revision;
    bool measuring;
    List<Row> rows;
    UiFooterLease footer;
    UiLabelFitStamp heading_fit;
};
void report(View& view, bool warm, uint64_t build_us, uint64_t install_us);
constexpr uint32_t cache_size = 8;
View* cache[cache_size]{};
uint64_t next_use = 1;
const Kind cache_kind{"ui.native.form"};
uint64_t live(uint64_t handle) { return game::from_handle(handle); }
uint64_t child(uint64_t object, uint64_t field) {
    const uint64_t result = object ? game::read_or<uint64_t>(object + field) : 0;
    return game::live(result) ? result : 0;
}
bool visible(uint64_t object, uint32_t value) {
    const auto set = object ? game::method(object, t::widget_table::set_visibility) : nullptr;
    if (!set) return false;
    set(reinterpret_cast<void*>(object), value); return true;
}
bool associated(const View& view) {
    const uint64_t host = live(view.host), tree = live(view.tree), parent = live(view.parent), scroll = live(view.scroll);
    return host && tree && parent && scroll && child(host, t::ui_host::tree) == tree &&
        child(tree, t::ui_tree::root) == parent && child(host, t::ui_host::scroll) == scroll;
}
bool building(const View& view) { return !view.retired && mod_usable(view.mod) && associated(view); }
bool installed(const View& view) {
    const uint64_t host = live(view.host);
    return host && view.installed && game::read_or<Array>(host + t::ui_mod_list_menu::source).data == view.source_data;
}
bool model(const View& view) {
    const UiScreen* screen = ui_active_screen();
    return !view.retired && view.showing && screen && screen->id == view.screen && mod_usable(view.mod) &&
        associated(view) && installed(view) && game::read_or<uint8_t>(live(view.host) + t::ui_host::state) == 1;
}
void invalidate_layout(View& view) { view.layout_epoch = layout_epoch; ++view.layout_revision; }
void free_rows(List<Row>& rows) {
    for (uint32_t i = 0; i < rows.count; ++i) {
        rows[i].caption.clear(); rows[i].value_text.clear(); rows[i].labels.clear();
    }
    rows.clear();
}
void finish(View* view) {
    if (--view->calls || view->owned || view->showing || view->delivered) return;
    if (const uint64_t category = live(view->category)) {
        if (game::read_or<Array>(category + t::ui_mod_list_category::rows).data == view->rows_data)
            game::write(category + t::ui_mod_list_category::rows, Array{});
        game::write<uint64_t>(category + t::ui_mod_list_category::owner, 0);
    }
    if (view->source_data) game::release_engine_memory(reinterpret_cast<void*>(view->source_data));
    if (view->rows_data) game::release_engine_memory(reinterpret_cast<void*>(view->rows_data));
    ui_mod_list_native_unpin(view->host, view->group);
    free_rows(view->rows); release(view);
}
void restore(View& view) {
    const bool was_installed = view.installed;
    view.installed = false;
    struct Returned {
        View& view;
        ~Returned() {
            if (view.borrowed) { view.borrowed = false; ui_mod_list_native_return_source(view.host, view.source_data); }
            const uint64_t group = view.baseline_group; view.baseline_group = 0;
            if (group) ui_mod_list_native_unpin(view.host, group);
        }
    } returned{view};
    if (!was_installed) return;
    const uint64_t host = live(view.host);
    if (!host || game::read_or<Array>(host + t::ui_mod_list_menu::source).data != view.source_data) return;
    if (!game::write(host + t::ui_mod_list_menu::source, view.before)) return;
    // Source arrays must unwind even during host teardown. Only an active
    // native menu can safely rebuild Slate or restore its focus.
    if (!associated(view) || game::read_or<uint8_t>(host + t::ui_host::state) != 1) {
        game::write<int32_t>(host + t::ui_mod_list_menu::selected, view.before_selected);
        game::write<int32_t>(host + t::ui_mod_list_menu::previous, view.before_previous);
        return;
    }
    // Refresh clears the outgoing category focus while its objects remain pinned.
    t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host));
    if (!associated(view) || game::read_or<Array>(host + t::ui_mod_list_menu::source).data != view.before.data) return;
    game::write<int32_t>(host + t::ui_mod_list_menu::selected, view.before_selected);
    game::write<int32_t>(host + t::ui_mod_list_menu::previous, -1);
    if (view.before_selected >= 0) t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host));
    if (!associated(view) || game::read_or<Array>(host + t::ui_mod_list_menu::source).data != view.before.data) return;
    game::write<int32_t>(host + t::ui_mod_list_menu::previous, view.before_previous);
    const uint64_t scroll = live(view.scroll), panel = child(scroll, t::ui_mod_list_scroll::panel);
    if (view.have_scroll && panel && t::bound(t::fn::ui_mod_list_transform) && t::fn::ui_mod_list_scrollbar &&
        game::read_or<float>(scroll + t::ui_mod_list_scroll::total) == view.total &&
        game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport) == view.viewport) {
        game::write<float>(panel + t::ui_mod_list_scroll::x, view.scroll_x);
        game::write<float>(panel + t::ui_mod_list_scroll::y, view.scroll_y);
        t::fn::ui_mod_list_transform(reinterpret_cast<void*>(panel));
        if (associated(view) && game::read_or<Array>(host + t::ui_mod_list_menu::source).data == view.before.data)
            t::fn::ui_mod_list_scrollbar(reinterpret_cast<void*>(scroll));
    }
}
void detach(View* view) {
    if (view->closing || !view->showing) return;
    ++view->calls; view->closing = true; view->showing = false; ++closing_depth;
    ui_footer_release(view->footer);
    restore(*view);
    view->closing = false; --closing_depth; finish(view);
}
void close(void* pointer) {
    auto* view = static_cast<View*>(pointer); ++view->calls;
    // The core may defer this callback until an in-flight renderer call ends.
    // Cache retirement can hide the source earlier, but cannot drop this ref.
    view->delivered = false;
    detach(view); finish(view);
}
void retire(View* view) {
    ++view->calls; view->retired = true;
    for (auto& kept : cache) if (kept == view) kept = nullptr;
    if (view->owned) { view->owned = false; disown(view->mod, cache_kind, reinterpret_cast<uint64_t>(view)); }
    if (view->showing) detach(view);
    else if (!view->closing) restore(*view);
    finish(view);
}
void owner_stopped(void* pointer, uint64_t) {
    auto* view = static_cast<View*>(pointer); view->owned = false; retire(view);
}
bool pin(View& view, uint64_t object) { return object && ui_mod_list_native_pin(view.host, view.group, object); }
uint64_t named(uint64_t widget, const char* name) {
    if (!strcmp(name, t::name::ui_mod_list_title_child)) if (const uint64_t bound = child(widget, t::ui_mod_list_category::title)) return bound;
    if (!strcmp(name, t::name::ui_mod_list_panel_child)) if (const uint64_t bound = child(widget, t::ui_mod_list_category::panel)) return bound;
    uint64_t pending[128]{};
    pending[0] = child(child(widget, t::ui_host::tree), t::ui_tree::root);
    uint32_t count = pending[0] ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t current = pending[i];
        if (game::name_is(game::name_of(current), name)) return current;
        ui_native_seed_class(panel_base_class, game::class_of(current));
        if (!game::is_a(current, panel_base_class)) continue;
        const Array slots = game::read_or<Array>(current + t::ui_panel::slots);
        if (slots.count < 0 || slots.count > 128 || slots.capacity < slots.count ||
            (slots.count && !game::readable(slots.data, uint64_t(slots.count) * 8))) return 0;
        for (int32_t j = 0; j < slots.count; ++j) {
            const uint64_t next = child(game::read_or<uint64_t>(slots.data + uint64_t(j) * 8), t::ui_panel_slot::content);
            bool seen = !next;
            for (uint32_t k = 0; k < count; ++k) seen = seen || pending[k] == next;
            if (!seen) { if (count == 128) return 0; pending[count++] = next; }
        }
    }
    return 0;
}
bool make_text(const char* text, game::EngineText& made) {
    char16_t units[1283]; size_t count = 0;
    if (!game::utf8_to_units(text, units, 1282, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    made = t::fn::text_from_string(string); return true;
}
bool label(View& view, uint64_t handle, const char* text) {
    const bool active = view.showing;
    const uint64_t widget = live(handle); game::EngineText made{};
    if (!widget || !make_text(text, made)) return false;
    const bool allowed = (active ? model(view) : building(view)) && live(handle) == widget;
    const uint64_t opened = view.used;
    if (allowed) { invalidate_layout(view); t::fn::rich_text_set(reinterpret_cast<void*>(widget), &made); }
    game::drop_shared(made.counter());
    return allowed && view.used == opened && (active ? model(view) : building(view)) && live(handle) == widget;
}
bool append(View& view, uint64_t widget, const char* text) {
    game::EngineText made{};
    if (!make_text(text, made)) return false;
    const bool allowed = building(view);
    if (allowed) t::fn::ui_mod_list_append_choice(reinterpret_cast<void*>(widget), &made);
    game::drop_shared(made.counter()); return allowed;
}
bool replace(View& view, uint64_t selector, uint32_t index, const char* text) {
    const Array choices = game::read_or<Array>(selector + t::ui_mod_list_selector::choices);
    if (choices.count <= 0 || choices.capacity < choices.count || index >= uint32_t(choices.count)) return false;
    const uint64_t destination = choices.data + uint64_t(index) * sizeof(game::EngineText);
    game::EngineText before{}, made{};
    if (!game::read(destination, before) || !make_text(text, made)) return false;
    if (!model(view) || !game::write(destination, made)) { game::drop_shared(made.counter()); return false; }
    // The returned FText share moves into the engine's existing choice element.
    game::drop_shared(before.counter()); return true;
}
void integer_text(int64_t value, char out[21]) {
    uint64_t magnitude = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
    char reverse[20]; uint32_t count = 0, next = 0;
    do { reverse[count++] = char('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (value < 0) out[next++] = '-';
    while (count) out[next++] = reverse[--count]; out[next] = 0;
}
bool copy_shape(const UiScreen& screen, List<Row>& rows) {
    if (!screen.controls.count || screen.controls.count > t::constant::ui_mod_list_rows_limit || !rows.reserve(screen.controls.count)) return false;
    for (uint32_t i = 0; i < screen.controls.count; ++i) {
        const UiControl& control = screen.controls[i]; Row row{};
        row.kind = control.kind; row.choice = control.selected;
        row.integer = control.integer; row.minimum = control.minimum; row.maximum = control.maximum; row.step = control.step;
        char number[21]; integer_text(control.integer, number);
        const char* value = control.kind == UiKind::integer ? number : control.kind == UiKind::text ? control.text.get() : control.kind == UiKind::button ? "Select" : "";
        if (!row.caption.set(control.label.get()) || !row.value_text.set(value)) { row.caption.clear(); row.value_text.clear(); return false; }
        bool okay = true;
        for (const char* text = control.labels.first(); text && okay; text = control.labels.after(text)) okay = row.labels.add(text);
        if (!okay || !rows.add(row)) { row.caption.clear(); row.value_text.clear(); row.labels.clear(); return false; }
    }
    return true;
}
bool same_layout(const View& view, const List<Row>& rows) {
    if (view.rows.count != rows.count) return false;
    for (uint32_t i = 0; i < rows.count; ++i) {
        if (view.rows[i].kind != rows[i].kind) return false;
        const Texts& a = view.rows[i].labels; const Texts& b = rows[i].labels;
        const char* x = a.first(); const char* y = b.first();
        while (x && y) { if (strcmp(x, y)) return false; x = a.after(x); y = b.after(y); }
        if (x || y) return false;
    }
    return true;
}
bool build(View& view) {
    const uint64_t host = live(view.host), kind = child(host, t::ui_mod_list_menu::category_class);
    if (!kind || !pin(view, host) || !pin(view, kind)) return false;
    ui_native_seed_class(category_class, kind);
    const uint64_t category = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(host), reinterpret_cast<void*>(kind), nullptr));
    view.category = game::to_handle(category);
    if (!building(view) || !category || !game::is_a(category, category_class) || !pin(view, category)) return false;
    const uint64_t title = named(category, t::name::ui_mod_list_title_child), panel = named(category, t::name::ui_mod_list_panel_child);
    if (panel) ui_native_seed_class(panel_class, game::class_of(panel));
    if (!title || !panel || !game::is_a(panel, panel_class) ||
        !game::write<uint64_t>(category + t::ui_mod_list_category::title, title) ||
        !game::write<uint64_t>(category + t::ui_mod_list_category::panel, panel)) return false;
    const Array empty = game::read_or<Array>(category + t::ui_mod_list_category::rows);
    if (empty.data || empty.count || empty.capacity) return false;
    view.rows_data = reinterpret_cast<uint64_t>(game::engine_memory(uint64_t(view.rows.count) * 8));
    view.source_data = reinterpret_cast<uint64_t>(game::engine_memory(8));
    if (!view.rows_data || !view.source_data || !building(view)) return false;
    const uint64_t row_kind = child(category, t::ui_mod_list_category::row_class);
    if (!row_kind || !pin(view, row_kind)) return false;
    ui_native_seed_class(row_class, row_kind);
    for (uint32_t i = 0; i < view.rows.count && building(view); ++i) {
        Row& row = view.rows[i];
        const uint64_t widget = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(host), reinterpret_cast<void*>(row_kind), nullptr));
        row.widget = game::to_handle(widget);
        if (!widget || !game::is_a(widget, row_class) || !pin(view, widget)) return false;
        t::fn::ui_mod_list_bind_row(reinterpret_cast<void*>(widget));
        if (!building(view)) return false;
        const uint64_t selector = child(widget, t::ui_mod_list_row::selector);
        row.label = game::to_handle(child(widget, t::ui_mod_list_row::title)); row.selector = game::to_handle(selector);
        row.inner = game::to_handle(child(selector, t::ui_mod_list_selector::inner));
        row.value = game::to_handle(child(selector, t::ui_mod_list_selector::value));
        row.left = game::to_handle(child(selector, t::ui_mod_list_selector::left)); row.right = game::to_handle(child(selector, t::ui_mod_list_selector::right));
        if (!row.label || !selector || !row.value) return false;
        if (row.kind == UiKind::choice) {
            for (const char* text = row.labels.first(); text && building(view); text = row.labels.after(text)) if (!append(view, widget, text)) return false;
        } else for (uint32_t n = 0; n < (row.kind == UiKind::integer ? 3u : 1u) && building(view); ++n) if (!append(view, widget, row.value_text.get())) return false;
        const uint32_t selected = row.kind == UiKind::choice ? row.choice : row.kind == UiKind::integer ? 1 : 0;
        const auto valid = game::method(selector, t::ui_mod_list_selector::valid);
        if (!building(view) || !valid || !valid(reinterpret_cast<void*>(selector), int32_t(selected)) || !building(view)) return false;
        if (!game::write<uint8_t>(widget + t::ui_mod_list_row::kind, uint8_t(t::constant::ui_mod_list_item)) ||
            !game::write<uint8_t>(widget + t::ui_mod_list_row::parent, 0) ||
            !game::write<int32_t>(widget + t::ui_mod_list_row::count, -1) ||
            !game::write<uint8_t>(widget + t::ui_mod_list_row::enabled, row.kind != UiKind::text) ||
            !game::write<uint8_t>(selector + t::ui_mod_list_selector::wrap, 0) ||
            !game::write<uint8_t>(selector + t::ui_mod_list_selector::enabled, 1) ||
            !game::write<uint32_t>(selector + t::ui_mod_list_selector::choice, selected) ||
            !game::write<uint32_t>(widget + t::ui_mod_list_row::shadow, selected)) return false;
        const auto slot = t::fn::ui_add_child(reinterpret_cast<void*>(panel), reinterpret_cast<void*>(widget));
        if (!slot || !building(view) || !game::write<uint64_t>(view.rows_data + uint64_t(i) * 8, widget)) return false;
    }
    return building(view) && game::write(category + t::ui_mod_list_category::rows, Array{view.rows_data, int32_t(view.rows.count), int32_t(view.rows.count)}) &&
        game::write<uint8_t>(category + t::ui_mod_list_category::kind, uint8_t(t::constant::ui_mod_list_category)) &&
        game::write<int32_t>(category + t::ui_mod_list_category::span, int32_t(view.rows.count)) &&
        game::write<uint64_t>(category + t::ui_mod_list_category::owner, host) && game::write<uint64_t>(view.source_data, category);
}
bool alive(void* pointer) {
    const auto& view = *static_cast<View*>(pointer);
    return !view.retired && view.showing && associated(view) && live(view.category) && installed(view) &&
        game::read_or<uint8_t>(live(view.host) + t::ui_host::state) == 1;
}
bool fit_message(View& view, Row& row) {
    const uint64_t opened = view.used;
    const uint64_t widget = live(row.widget), label = live(row.label), slot = child(label, t::ui_mod_list_tab::slot);
    const auto current = [&] { return view.used == opened && model(view) && live(row.widget) == widget &&
        live(row.label) == label && child(label, t::ui_mod_list_tab::slot) == slot; };
    if (slot && !row.message_fit) ui_native_seed_class(slot_class, game::class_of(slot));
    if (!widget || !label || !slot || !game::is_a(slot, slot_class) || !t::bound(t::fn::ui_slot_anchors) || !t::bound(t::fn::ui_slot_alignment) ||
        !t::bound(t::fn::ui_slot_auto_size) || !t::bound(t::fn::ui_slot_offsets) || !t::bound(t::fn::ui_cached_geometry) || !t::bound(t::fn::ui_rich_text_sync)) return false;
    float geometry[13]{};
    const uint64_t at = reinterpret_cast<uint64_t>(t::fn::ui_cached_geometry(reinterpret_cast<void*>(widget)));
    if (!current()) return false;
    // Use the row, never the label's own desired width: wrapping must not feed
    // back into its next available width. Wait for a real arrangement.
    if (!at || !game::read(at, geometry) || !ui_label_fit_detail::geometry(geometry) || geometry[0] <= 40) return true;
    const float width = geometry[0] - 40;
    if (row.message_fit && !ui_label_fit_detail::changed(row.wrap_width, width, .01f)) return true;
    struct Margin { float left, top, width, height; } offsets{};
    float anchors[4]{}, alignment[2]{};
    uint8_t auto_wrap = 0;
    if (!game::read(slot + t::ui_mod_list_tab::offsets, offsets) || !game::read(slot + t::ui_mod_list_tab::anchors, anchors) ||
        !game::read(slot + t::ui_mod_list_tab::alignment, alignment) || !game::read(label + t::ui_rich_text::auto_wrap, auto_wrap) ||
        !(offsets.top > -10000 && offsets.top < 10000 && offsets.height > -10000 && offsets.height < 10000 &&
          anchors[1] >= 0 && anchors[1] <= 1 && anchors[3] >= anchors[1] && anchors[3] <= 1 && alignment[1] >= 0 && alignment[1] <= 1)) return false;
    // Publish the style stamp before native setters: they can synchronously
    // reenter an update while Slate still holds the previous arrangement.
    invalidate_layout(view); row.message_fit = true; row.wrap_width = width;
    if (!game::write<uint8_t>(label + t::ui_rich_text::wrapping_policy, 1) ||
        !game::write<uint8_t>(label + t::ui_rich_text::auto_wrap, uint8_t(auto_wrap & ~1u)) ||
        !game::write<float>(label + t::ui_rich_text::wrap_width, width)) return false;
    t::fn::ui_slot_anchors(reinterpret_cast<void*>(slot), 0, 0, 1, 0);
    if (!current()) return false;
    t::fn::ui_slot_alignment(reinterpret_cast<void*>(slot), 0, 0);
    if (!current()) return false;
    // These owned messages use the row origin. A centered stock one-line
    // slot can put a growing paragraph above its row. Keep native desired
    // height and render transform instead of freezing its 120-unit box.
    t::fn::ui_slot_auto_size(reinterpret_cast<void*>(slot), true);
    if (!current()) return false;
    t::fn::ui_slot_offsets(reinterpret_cast<void*>(slot), 20, 0, 20, offsets.height);
    if (!current()) return false;
    if (t::bound(t::fn::ui_text_justification)) t::fn::ui_text_justification(reinterpret_cast<void*>(label), 0);
    if (!current()) return false;
    if (game::read_or<uint64_t>(label + t::ui_rich_text::slate)) {
        t::fn::ui_rich_text_sync(reinterpret_cast<void*>(label));
        if (!current()) return false;
        // The sync makes the block's decorators again. The glyphs of the text it already shows were made by the
        // old ones and draw nothing, though they keep their space; showing the text again makes them anew.
        game::EngineText shown{}, blank{};
        if (game::read(label + t::ui_footer_label::text, shown) && shown.counter() && ui_retain_text(shown) && make_text("", blank)) {
            t::fn::rich_text_set(reinterpret_cast<void*>(label), &blank);
            game::drop_shared(blank.counter());
            if (current()) t::fn::rich_text_set(reinterpret_cast<void*>(label), &shown);
            game::drop_shared(shown.counter());
        }
    }
    return current();
}
bool row_art(View& view, Row& row) {
    const uint64_t opened = view.used;
    const auto current = [&] { return view.used == opened && model(view); };
    if (!current()) return false;
    if (row.kind == UiKind::text) {
        const uint64_t label = live(row.label), selector = live(row.selector);
        if (!label || !selector || !visible(selector, uint32_t(t::constant::widget_hidden)) || !current()) return false;
        return current() && fit_message(view, row);
    }
    if (row.kind == UiKind::choice) return true;
    const bool less = row.kind == UiKind::integer && uint64_t(row.integer) - uint64_t(row.minimum) >= uint64_t(row.step);
    const bool more = row.kind == UiKind::integer && uint64_t(row.maximum) - uint64_t(row.integer) >= uint64_t(row.step);
    const uint64_t left = live(row.left), right = live(row.right);
    if (left && !visible(left, less ? uint32_t(t::constant::widget_shown) : uint32_t(t::constant::widget_hidden))) return false;
    if (!current()) return false;
    if (right && !visible(right, more ? uint32_t(t::constant::widget_shown) : uint32_t(t::constant::widget_hidden))) return false;
    return current();
}
bool sync_row(View& view, Row& row, const UiControl& control) {
    const uint64_t opened = view.used;
    const auto current_view = [&] { return view.used == opened && model(view); };
    const uint64_t widget = live(row.widget), selector = live(row.selector);
    if (!widget || !selector) return false;
    char number[21]; integer_text(control.integer, number);
    const char* value = control.kind == UiKind::integer ? number : control.kind == UiKind::text ? control.text.get() : "Select";
    const uint32_t selected = control.kind == UiKind::choice ? control.selected : control.kind == UiKind::integer ? 1 : 0;
    const int64_t integer = control.integer, minimum = control.minimum, maximum = control.maximum, step = control.step;
    const UiKind kind = control.kind;
    const auto redraw = game::method(selector, t::ui_mod_list_selector::redraw);
    if (!redraw) return false;
    // Copy before calling into UMG: a native callback can destroy the screen.
    Text caption, current;
    bool okay = caption.set(control.label.get()) && current.set(value);
    if (okay && kind == UiKind::text) {
        const size_t a = strlen(caption.get()), b = strlen(current.get());
        Text combined;
        okay = combined.reserve(a + b + 3);
        if (okay) { memcpy(combined.data, caption.get(), a); memcpy(combined.data + a, ": ", 2); memcpy(combined.data + a + 2, current.get(), b + 1); caption.clear(); caption = combined; }
    }
    const bool changed = !row.rendered || row.choice != selected || (kind == UiKind::integer && strcmp(row.value_text.get(), current.get()));
    if (okay && (!row.rendered || strcmp(row.caption.get(), caption.get()))) {
        row.rendered = false;
        if (kind == UiKind::text) row.message_fit = false;
        okay = label(view, row.label, caption.get());
    }
    if (okay && current_view() && kind == UiKind::integer && strcmp(row.value_text.get(), current.get())) okay = replace(view, selector, 1, current.get());
    if (okay && current_view() && changed) {
        row.rendered = false;
        okay = game::write<uint32_t>(selector + t::ui_mod_list_selector::choice, selected) &&
            game::write<uint32_t>(widget + t::ui_mod_list_row::shadow, selected);
        if (okay) redraw(reinterpret_cast<void*>(selector));
    }
    if (okay && current_view()) {
        row.choice = selected; row.integer = integer; row.minimum = minimum; row.maximum = maximum; row.step = step;
        okay = row_art(view, row);
    }
    if (okay && current_view()) okay = row.caption.set(caption.get()) && row.value_text.set(current.get());
    if (okay && current_view()) row.rendered = true;
    caption.clear(); current.clear(); return okay && current_view();
}
void reconcile_layout(View& view) {
    if (view.measuring || view.layout_epoch == layout_epoch || !model(view) || !t::bound(t::fn::ui_cached_geometry) ||
        !t::bound(t::fn::ui_mod_list_reveal) || !t::bound(t::fn::ui_mod_list_transform) || !t::bound(t::fn::ui_mod_list_scrollbar) ||
        !view.rows.count || view.rows.count > kUiFormLayoutRows) return;
    struct Measuring { View& view; Measuring(View& v) : view(v) { view.measuring = true; } ~Measuring() { view.measuring = false; } } measuring{view};
    const uint64_t opened = view.used, revision = view.layout_revision;
    const uint64_t host = live(view.host), scroll = live(view.scroll), category = live(view.category);
    const uint64_t heading = child(category, t::ui_mod_list_category::title), panel = child(category, t::ui_mod_list_category::panel);
    const uint32_t count = view.rows.count;
    const auto current = [&] { return view.used == opened && view.layout_revision == revision &&
        view.layout_epoch != layout_epoch && model(view) && live(view.category) == category &&
        child(category, t::ui_mod_list_category::title) == heading && child(category, t::ui_mod_list_category::panel) == panel; };
    if (!host || !scroll || !category || !heading || !panel || !current()) return;
    const Array source = game::read_or<Array>(host + t::ui_mod_list_menu::source);
    const Array active = game::read_or<Array>(host + t::ui_mod_list_menu::active);
    const Array rows = game::read_or<Array>(category + t::ui_mod_list_category::rows);
    const Array native = game::read_or<Array>(scroll + t::ui_mod_list_scroll::heights);
    const auto same = [](Array a, Array b) { return a.data == b.data && a.count == b.count && a.capacity == b.capacity; };
    const auto owned = [&] {
        return current() && same(game::read_or<Array>(host + t::ui_mod_list_menu::source), source) &&
            same(game::read_or<Array>(host + t::ui_mod_list_menu::active), active) &&
            same(game::read_or<Array>(category + t::ui_mod_list_category::rows), rows) &&
            same(game::read_or<Array>(scroll + t::ui_mod_list_scroll::heights), native) &&
            game::read_or<uint64_t>(source.data) == category && game::read_or<uint64_t>(active.data) == category &&
            game::read_or<uint64_t>(category + t::ui_mod_list_category::owner) == host &&
            game::read_or<int32_t>(category + t::ui_mod_list_category::span) == int32_t(count) &&
            game::read_or<uint8_t>(scroll + t::ui_mod_list_scroll::variable_height) == 1 &&
            game::read_or<int32_t>(scroll + t::ui_mod_list_scroll::extent_count) == int32_t(count + 1);
    };
    if (source.data != view.source_data || source.count != 1 || source.capacity != 1 ||
        active.count != 1 || active.capacity < 1 || active.capacity > int32_t(kUiFormLayoutRows) || !game::readable(active.data, 8) ||
        rows.data != view.rows_data || rows.count != int32_t(count) || rows.capacity != int32_t(count) ||
        native.count != int32_t(count + 1) || native.capacity < native.count || native.capacity > int32_t(kUiFormLayoutRows * 2) ||
        !game::readable(native.data, uint64_t(native.count) * sizeof(float)) || !owned()) return;
    const float spacing = game::read_or<float>(scroll + t::ui_mod_list_scroll::spacing);
    const float before_total = game::read_or<float>(scroll + t::ui_mod_list_scroll::total);
    const float viewport = game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport);
    if (!ui_label_fit_detail::finite(before_total) || before_total <= 0 ||
        !ui_label_fit_detail::finite(viewport) || viewport <= 0 || viewport >= 100000) return;
    const auto geometry = [&](uint64_t object, float (&result)[13]) {
        const uint64_t at = reinterpret_cast<uint64_t>(t::fn::ui_cached_geometry(reinterpret_cast<void*>(object)));
        return owned() && at && game::read(at, result) && ui_label_fit_detail::geometry(result);
    };
    float category_geometry[13]{}, heading_geometry[13]{}, panel_geometry[13]{};
    float row_geometry[kUiFormLayoutRows][13]{}, heights[kUiFormLayoutRows + 1]{}, total = 0;
    if (!geometry(category, category_geometry) || !geometry(heading, heading_geometry) || !geometry(panel, panel_geometry)) return;
    float previous_label_bottom = -3.402823466e38f;
    for (uint32_t i = 0; i < count; ++i) {
        const Row& row = view.rows[i]; const uint64_t widget = live(row.widget);
        if (!widget || game::read_or<uint64_t>(rows.data + uint64_t(i) * 8) != widget || !geometry(widget, row_geometry[i])) return;
        if (row.kind != UiKind::text) continue;
        const uint64_t label = live(row.label); float label_geometry[13]{};
        if (!row.message_fit || !label || !geometry(label, label_geometry) || !owned()) return;
        const float* arranged = row_geometry[i];
        const float bottom = label_geometry[12] + label_geometry[1] * label_geometry[10];
        const float right = label_geometry[11] + label_geometry[0] * label_geometry[7];
        const float render_y = game::read_or<float>(label + t::ui_widget::render_y);
        const float render_scale_y = game::read_or<float>(label + t::ui_widget::render_scale_y);
        const float layout_top = label_geometry[12] - render_y * arranged[10];
        const float layout_bottom = bottom - render_y * arranged[10];
        // Reject mixed pre/post-layout snapshots, including a newly wrapped
        // label whose row has not grown yet. Its native render translation is
        // separate from the Canvas arrangement; do not erase or guess it.
        if (ui_label_fit_detail::changed(label_geometry[10], arranged[10], .01f) ||
            ui_label_fit_detail::changed(row.wrap_width, arranged[0] - 40, .01f) ||
            !ui_label_fit_detail::finite(render_y) || render_y <= -100000 || render_y >= 100000 || render_scale_y != 1 ||
            label_geometry[0] > row.wrap_width + .01f || layout_top < arranged[12] - .01f ||
            layout_bottom > arranged[12] + arranged[1] * arranged[10] + .01f ||
            label_geometry[12] < category_geometry[12] - .01f ||
            bottom > category_geometry[12] + category_geometry[1] * category_geometry[10] + .01f ||
            label_geometry[11] < arranged[11] - .01f || right > arranged[11] + arranged[0] * arranged[7] + .01f ||
            label_geometry[12] < previous_label_bottom - .01f) return;
        previous_label_bottom = bottom;
    }
    if (!ui_form_layout(category_geometry, heading_geometry, panel_geometry, &row_geometry[0][0], count, spacing, heights, total) || !owned() ||
        game::read_or<float>(scroll + t::ui_mod_list_scroll::spacing) != spacing ||
        game::read_or<float>(scroll + t::ui_mod_list_scroll::total) != before_total ||
        game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport) != viewport) return;
    bool changed = ui_label_fit_detail::changed(total, before_total, .01f);
    for (uint32_t i = 0; i <= count; ++i) {
        const float before = game::read_or<float>(native.data + uint64_t(i) * sizeof(float));
        if (!ui_label_fit_detail::finite(before) || before <= 0) return;
        changed = changed || ui_label_fit_detail::changed(heights[i], before, .01f);
    }
    for (uint32_t i = 0; i < count; ++i)
        if (!live(view.rows[i].widget) || game::read_or<uint64_t>(rows.data + uint64_t(i) * 8) != live(view.rows[i].widget)) return;
    if (!changed || !game::writable(native.data, uint64_t(native.count) * sizeof(float)) ||
        !game::writable(scroll + t::ui_mod_list_scroll::total, sizeof(float)) || !owned()) return;
    // No native call occurs between the bounds checks and this publication.
    // Refresh rebuilds the original native extents when our source unwinds.
    memcpy(reinterpret_cast<void*>(native.data), heights, uint64_t(native.count) * sizeof(float));
    memcpy(reinterpret_cast<void*>(scroll + t::ui_mod_list_scroll::total), &total, sizeof total);
    const uint32_t reading = !view.away && view.selected < count ? view.selected : view.reading;
    if (reading >= count || !owned()) return;
    t::fn::ui_mod_list_reveal(reinterpret_cast<void*>(scroll), reading + 1);
    if (!owned()) return;
    const uint64_t content = child(scroll, t::ui_mod_list_scroll::panel);
    if (!content) return;
    t::fn::ui_mod_list_transform(reinterpret_cast<void*>(content));
    if (owned() && child(scroll, t::ui_mod_list_scroll::panel) == content) t::fn::ui_mod_list_scrollbar(reinterpret_cast<void*>(scroll));
}
bool update(void* pointer, const UiScreen& source) {
    auto* view = static_cast<View*>(pointer); ++view->calls;
    const uint32_t revision = source.revision;
    const uint64_t opened = view->used;
    bool okay = alive(view);
    const auto current = [&] { return okay && view->used == opened && model(*view); };
    if (current() && view->revision != revision) {
        char title[641]; memcpy(title, source.title.get(), strlen(source.title.get()) + 1);
        okay = label(*view, game::to_handle(child(live(view->category), t::ui_mod_list_category::title)), title);
        for (uint32_t i = 0; i < view->rows.count && current(); ++i) {
            const UiScreen* screen = ui_active_screen();
            if (i >= screen->controls.count || screen->controls[i].kind != view->rows[i].kind) { okay = false; break; }
            okay = sync_row(*view, view->rows[i], screen->controls[i]);
        }
        if (current()) view->revision = revision;
    }
    if (current()) {
        const UiScreen* screen = ui_active_screen(); const uint32_t selected = screen->selected;
        const UiKind kind = selected < screen->controls.count ? screen->controls[selected].kind : UiKind::text;
        UiFooterGuide guide{screen->settings_tab, kind == UiKind::choice || kind == UiKind::integer, kind == UiKind::button, true, screen->resettable};
        const char* footer = screen->footer.get();
        const size_t footer_size = strlen(footer);
        if (footer_size >= sizeof guide.custom) { finish(view); return false; }
        memcpy(guide.custom, footer, footer_size + 1);
        ui_footer_update(view->footer, view->host, guide);
        if (current() && view->selected != selected) {
            const uint64_t host = live(view->host);
            const int32_t native_selected = selected < view->rows.count ? int32_t(selected) : -1;
            const int32_t previous = view->selected < view->rows.count ? int32_t(view->selected) : -1;
            okay = game::write<int32_t>(host + t::ui_mod_list_menu::previous, previous) &&
                game::write<int32_t>(host + t::ui_mod_list_menu::selected, native_selected);
            if (okay && native_selected >= 0 && current()) t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host));
            // Focus redraws the real selector and may restore its arrows.
            if (current() && previous >= 0) okay = row_art(*view, view->rows[uint32_t(previous)]);
            if (current() && native_selected >= 0) okay = row_art(*view, view->rows[uint32_t(native_selected)]);
            if (current()) {
                view->selected = selected;
                view->away = false;
                if (selected < view->rows.count) {
                    uint32_t reveal = selected;
                    bool trailing = true;
                    for (uint32_t i = selected + 1; i < view->rows.count; ++i) trailing = trailing && view->rows[i].kind == UiKind::text;
                    // Native scrolling owns row/category heights and its viewport.
                    if (trailing) reveal = view->rows.count - 1;
                    t::fn::ui_mod_list_reveal(reinterpret_cast<void*>(live(view->scroll)), reveal + 1);
                    if (current() && reveal != selected)
                        t::fn::ui_mod_list_reveal(reinterpret_cast<void*>(live(view->scroll)), selected + 1);
                }
            }
        }
    }
    for (uint32_t i = 0; i < view->rows.count && current(); ++i)
        if (view->rows[i].kind == UiKind::text) okay = fit_message(*view, view->rows[i]);
    if (current()) ui_fit_owned_label(view->heading_fit, live(view->scroll), child(live(view->category), t::ui_mod_list_category::title), true, current);
    for (uint32_t i = 0; i < view->rows.count && current(); ++i)
        if (view->rows[i].kind != UiKind::text)
            ui_fit_owned_label(view->rows[i].fit, live(view->scroll), live(view->rows[i].label), false, current);
    if (current()) reconcile_layout(*view);
    if (current() && alive(view)) report(*view, view->report_warm, view->build_us, view->install_us);
    okay = current() && alive(view); finish(view); return okay;
}
void report(View& view, bool warm, uint64_t build_us, uint64_t install_us) {
    bool& told = warm ? view.logged_warm : view.logged_cold;
    if (told) return;
    float geometry[2]{};
    const uint64_t scroll = live(view.scroll);
    const uint64_t at = t::bound(t::fn::ui_cached_geometry) ? reinterpret_cast<uint64_t>(t::fn::ui_cached_geometry(reinterpret_cast<void*>(scroll))) : 0;
    if (at) game::read(at, geometry);
    const float viewport = game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport);
    if (!building(view) || !view.showing || !installed(view) ||
        !(viewport > 0 && viewport < 100000 && geometry[0] > 0 && geometry[0] < 100000 && geometry[1] > 0 && geometry[1] < 100000)) return;
    told = true;
    char text[256]{}; size_t used = 0;
    const auto words = [&](const char* value) { while (*value && used + 1 < sizeof text) text[used++] = *value++; text[used] = 0; };
    const auto digits = [&](uint64_t value) { char reverse[20]; uint32_t n = 0; do { reverse[n++] = char('0' + value % 10); value /= 10; } while (value); while (n && used + 1 < sizeof text) text[used++] = reverse[--n]; text[used] = 0; };
    words("UI form screen="); digits(view.screen); words(warm ? " warm create_us=" : " cold create_us="); digits(build_us);
    words(" install_us="); digits(install_us); words(" rows="); digits(view.rows.count);
    words(" viewport=");
    digits(viewport > 0 && viewport < 100000 ? uint64_t(viewport) : 0);
    words(" geometry="); digits(geometry[0] > 0 && geometry[0] < 100000 ? uint64_t(geometry[0]) : 0);
    words("x"); digits(geometry[1] > 0 && geometry[1] < 100000 ? uint64_t(geometry[1]) : 0);
    mod_note(kFrameworkLogId, text);
}
TSUCHI_Result open(const UiScreen& screen, void** out) {
    if (opening || closing_depth) return TSUCHI_NOT_READY;
    struct Opening { Opening() { opening = true; } ~Opening() { opening = false; } } pending;
    if (!wrapped || !t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_add_child) || !t::bound(t::fn::ui_mod_list_bind_row) || !t::bound(t::fn::ui_mod_list_append_choice) ||
        !t::bound(t::fn::ui_mod_list_refresh) || !t::bound(t::fn::ui_mod_list_focus) || !t::bound(t::fn::ui_mod_list_reveal) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return TSUCHI_NOT_READY;
    const uint64_t began = cost_now();
    TSUCHI_Mod* mod = screen.mod; const uint64_t screen_id = screen.id; const uint32_t initial = screen.selected;
    struct Shape { List<Row> rows; ~Shape() { free_rows(rows); } } shape;
    if (!copy_shape(screen, shape.rows)) return TSUCHI_NO_MEMORY;
    const uint64_t host_handle = ui_mod_list_native_host(), host = live(host_handle);
    const uint64_t tree = child(host, t::ui_host::tree), parent = child(tree, t::ui_tree::root), scroll = child(host, t::ui_host::scroll);
    if (!host || game::read_or<uint8_t>(host + t::ui_host::state) != 1 || !tree || !parent || !scroll || !mod_usable(mod)) return TSUCHI_NOT_READY;
    ui_mod_list_native_footer_release(host_handle);
    for (uint32_t i = 0; i < cache_size; ++i) {
        View* kept = cache[i];
        if (kept && (!mod_usable(kept->mod) || !associated(*kept) || !live(kept->category) ||
            (kept->mod == mod && kept->screen == screen_id && !same_layout(*kept, shape.rows)))) retire(kept);
    }
    View* view = nullptr; uint32_t free_slot = cache_size, oldest = cache_size;
    for (uint32_t i = 0; i < cache_size; ++i) {
        View* kept = cache[i];
        if (!kept) { free_slot = i; continue; }
        if (kept->mod == mod && kept->screen == screen_id && kept->host == host_handle && !kept->showing && !kept->delivered && !kept->closing && same_layout(*kept, shape.rows)) view = kept;
        if (!kept->showing && !kept->delivered && !kept->closing && (oldest == cache_size || kept->used < cache[oldest]->used)) oldest = i;
    }
    const bool warm = view != nullptr;
    if (view) ++view->calls;
    else if (free_slot == cache_size) { if (oldest == cache_size) return TSUCHI_NOT_READY; retire(cache[oldest]); free_slot = oldest; }
    if (!view) {
        view = static_cast<View*>(allocate(sizeof(View))); if (!view) return TSUCHI_NO_MEMORY;
        *view = {}; view->calls = 1; view->mod = mod; view->screen = screen_id; view->host = host_handle;
        view->tree = game::to_handle(tree); view->parent = game::to_handle(parent); view->scroll = game::to_handle(scroll);
        view->group = ui_mod_list_native_group(); view->revision = UINT32_MAX; view->rows = shape.rows; shape.rows = {};
        if (!build(*view)) { retire(view); finish(view); return TSUCHI_NOT_READY; }
        const auto owned = own(mod, cache_kind, reinterpret_cast<uint64_t>(view), owner_stopped, view);
        if (owned != TSUCHI_OK) { retire(view); finish(view); return owned; }
        view->owned = true; cache[free_slot] = view;
    }
    const uint64_t built = cost_now();
    if (!building(*view)) { retire(view); finish(view); return TSUCHI_NOT_READY; }
    view->before = game::read_or<Array>(host + t::ui_mod_list_menu::source);
    if (view->before.count < 0 || view->before.capacity < view->before.count || view->before.count > int32_t(t::constant::ui_mod_list_rows_limit) ||
        (view->before.count && !game::readable(view->before.data, uint64_t(view->before.count) * 8))) { retire(view); finish(view); return TSUCHI_NOT_READY; }
    view->baseline_group = ui_mod_list_native_group();
    if (!view->baseline_group) { retire(view); finish(view); return TSUCHI_NOT_READY; }
    for (int32_t i = 0; i < view->before.count; ++i)
        if (!ui_mod_list_native_pin(view->host, view->baseline_group, game::read_or<uint64_t>(view->before.data + uint64_t(i) * 8))) {
            retire(view); finish(view); return TSUCHI_NO_MEMORY;
        }
    view->before_selected = game::read_or<int32_t>(host + t::ui_mod_list_menu::selected);
    view->before_previous = game::read_or<int32_t>(host + t::ui_mod_list_menu::previous);
    view->total = game::read_or<float>(scroll + t::ui_mod_list_scroll::total); view->viewport = game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport);
    const uint64_t panel = child(scroll, t::ui_mod_list_scroll::panel);
    view->have_scroll = panel && view->total > 0 && view->viewport > 0;
    if (view->have_scroll) { view->scroll_x = game::read_or<float>(panel + t::ui_mod_list_scroll::x); view->scroll_y = game::read_or<float>(panel + t::ui_mod_list_scroll::y); }
    if (!ui_mod_list_native_borrow(host_handle, view->before.data, view->source_data)) { retire(view); finish(view); return TSUCHI_NOT_READY; }
    view->borrowed = true; view->showing = true; view->installed = true;
    bool okay = game::write(host + t::ui_mod_list_menu::source, Array{view->source_data, 1, 1});
    if (okay) { invalidate_layout(*view); t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host)); }
    okay = okay && building(*view) && view->showing && installed(*view);
    if (!okay) { close(view); retire(view); finish(view); return TSUCHI_NOT_READY; }
    for (uint32_t i = 0; i < view->rows.count; ++i) if (view->rows[i].kind == UiKind::text) view->rows[i].message_fit = false;
    view->used = next_use++; view->reading = 0; view->away = false; view->confirm_wait = true; view->previous_buttons = 0; view->selected = UINT32_MAX - 1; view->revision = UINT32_MAX;
    // No native enabled-row scan is allowed for an all-message screen.
    game::write<int32_t>(host + t::ui_mod_list_menu::selected, initial < view->rows.count ? int32_t(initial) : -1);
    game::write<int32_t>(host + t::ui_mod_list_menu::previous, -1);
    view->report_warm = warm; view->build_us = built - began; view->install_us = cost_now() - built;
    report(*view, warm, view->build_us, view->install_us);
    if (!building(*view) || !view->showing || !installed(*view)) { close(view); retire(view); finish(view); return TSUCHI_NOT_READY; }
    view->delivered = true; *out = view; finish(view); return TSUCHI_OK;
}
// Shows the row read, which `view->reading` already holds.
void reveal_reading(View* view, uint32_t previous) {
    const uint32_t reading = view->reading;
    const uint64_t opened = view->used;
    const uint64_t target = reading < view->rows.count ? view->rows[reading].widget : 0;
    const uint64_t widget = live(target);
    t::fn::ui_mod_list_reveal(reinterpret_cast<void*>(live(view->scroll)), reading + 1);
    // Reveal has no native focus cue. Keep passive rows unfocused, and do
    // not play for a clamped movement or a view replaced by its callback.
    if (reading != previous && widget && view->used == opened && alive(view) && model(*view) && view->reading == reading &&
        reading < view->rows.count && view->rows[reading].widget == target && live(target) == widget)
        ui_feedback_focus(target);
}
bool screen_action(void* pointer, uint64_t host, uint32_t which) {
    auto* view = static_cast<View*>(pointer); ++view->calls;
    if (which >= 100 || host != view->host || !alive(view) || !model(*view)) { finish(view); return false; }
    const UiScreen* before = ui_active_screen();
    const uint32_t selected = before->selected;
    const uint64_t opened = view->used;
    if (selected == kUiNoSelection && (which == 0 || which == 1)) {
        const uint32_t previous = view->reading;
        if (which == 0 && view->reading) --view->reading;
        else if (which == 1 && view->reading + 1 < view->rows.count) ++view->reading;
        reveal_reading(view, previous);
        finish(view); return true;
    }
    // A screen with controls reads its passive rows past the last control, and before the first, the same way:
    // where focus has no control further that way, Up and Down move through those rows and focus stays.
    if (selected < before->controls.count && selected < view->rows.count && (which == 0 || which == 1)) {
        const uint32_t previous = view->away ? view->reading : selected;
        bool further = false;
        if (which == 1)
            for (uint32_t i = selected + 1; i < before->controls.count; ++i) further = further || before->controls[i].kind != UiKind::text;
        else
            for (uint32_t i = 0; i < selected; ++i) further = further || before->controls[i].kind != UiKind::text;
        uint32_t reading = previous;
        if (which == 1 && ((view->away && reading < selected) || (!further && reading + 1 < view->rows.count))) ++reading;
        if (which == 0 && ((view->away && reading > selected) || (!further && reading > 0))) --reading;
        if (reading != previous || view->away) {
            view->reading = reading, view->away = reading != selected;
            reveal_reading(view, previous);
            finish(view); return true;
        }
    }
    const bool editing = (which == 2 || which == 3 || which == 4) && selected < before->controls.count &&
        (before->controls[selected].kind == UiKind::choice || before->controls[selected].kind == UiKind::integer);
    const uint32_t old_choice = editing ? before->controls[selected].selected : 0;
    const int64_t old_integer = editing ? before->controls[selected].integer : 0;
    const bool handled = ui_action(which);
    if (handled && which <= 4 && view->used == opened && model(*view))
        ui_sync_screen(view->screen, view);
    if (handled && editing && view->used == opened && model(*view)) {
        const UiScreen* after = ui_active_screen();
        if (after->selected == selected && selected < after->controls.count && selected < view->rows.count &&
            (after->controls[selected].selected != old_choice || after->controls[selected].integer != old_integer))
            ui_feedback_arrow(view->rows[selected].selector, which != 2);
    }
    const bool consumed = handled || which == 0 || which == 1 || which == 2 || which == 3 || which == 4;
    finish(view); return consumed;
}
void screen_buttons(void* pointer, uint64_t buttons) {
    auto* view = static_cast<View*>(pointer); ++view->calls;
    if (!alive(view) || !model(*view)) { finish(view); return; }
    const uint64_t edges = buttons & ~view->previous_buttons;
    view->previous_buttons = buttons;
    if (view->confirm_wait) { if (!(buttons & t::constant::ui_confirm_button)) view->confirm_wait = false; finish(view); return; }
    if (edges & t::constant::ui_confirm_button) {
        const uint64_t host = live(view->host);
        if (host && model(*view)) t::fn::ui_settings_dispatch(reinterpret_cast<void*>(host), 4);
    }
    finish(view);
}
bool uses_host(void* pointer, uint64_t host) { return static_cast<View*>(pointer)->host == host; }
void detach_host(void* pointer, uint64_t host) {
    auto* view = static_cast<View*>(pointer); ++view->calls;
    if (view->host == host) detach(view);
    finish(view);
}
const UiRenderer renderer{open, update, alive, close, screen_action, screen_buttons, uses_host, detach_host};
void consume_buttons(uint64_t buttons) {
    if (button_listener) button_listener(buttons);
    ui_route_buttons(buttons);
}

#if TSUCHI_UI_INPUT
uint64_t hidden_held = 0;
bool service_buttons(uint32_t player, uint64_t& buttons) {
    if (!input_mod || !mod_usable(input_mod)) return false;
    TSUCHI_PadState state{};
    if (input::table.pad(input_mod, player, &state) != TSUCHI_OK || !state.connected) return false;
    TSUCHI_Buttons hidden = 0;
    if (input::table.hidden(input_mod, player, &hidden) != TSUCHI_OK) return false;
    // Releasing a hide while the physical key stays down is not a new press.
    hidden_held = (hidden_held | hidden) & state.held;
    buttons = state.held & ~hidden_held; return true;
}
#endif
void input_step() {
    if (!on_game_thread() || !buttons_provider) return;
    uint64_t buttons = 0; if (buttons_provider(0, buttons)) consume_buttons(buttons);
}
void npad(void* pointer) {
    next_npad(pointer);
    if (!on_game_thread() || input_step_registered) return;
    const uint64_t device = reinterpret_cast<uint64_t>(pointer);
    const uint32_t player = game::read_or<uint32_t>(device + t::npad_device::id, UINT32_MAX);
    if (player != 0) return;
    uint64_t buttons = 0;
    if (buttons_provider ? buttons_provider(player, buttons) : game::read(device + t::npad_device::buttons, buttons)) consume_buttons(buttons);
}

// The original dispatcher has already checked its UI contexts, input-consumed
// flag, active state and Settings ownership. No extra stock action is sent.
void action(NativeFrame& frame) {
    if (!on_game_thread()) return;
    namespace r = t::route_frame::ui_settings_dispatch;
    const uint64_t object = frame.x[r::object];
    const uint32_t which = uint32_t(frame.x[r::action]);
    if (!game::live(object) || game::read_or<uint8_t>(object + t::ui_host::state) != 1) return;
    bool handled = which < 100 && ui_route_action(game::to_handle(object), which);
    if (!handled && menu_dispatcher) {
        bool result = false;
        const uint64_t host = game::to_handle(object);
        if (host) menu_dispatcher(host, which, handled, result);
    }
    // no_action is the original dispatcher's no-op: it still reaches the normal
    // handled-input epilogue. Every unhandled action keeps its original value.
    if (handled) frame.x[r::action] = r::no_action;
}

} // namespace
void ui_native_host_detach(uint64_t host) {
    if (!on_game_thread()) return;
    for (auto* view : cache) {
        if (!view || view->host != host || !view->showing) continue;
        ++view->calls;
        detach(view);
        finish(view);
    }
}
bool ui_native_prepare_input() {
#if TSUCHI_UI_INPUT
    if (!on_game_thread()) return false;
    ui_buttons_provider(service_buttons);
    if (input_step_registered) return true;
    return ui_input_steps(after_step);
#else
    return false;
#endif
}
bool ui_native_hide_buttons(bool hide) {
#if TSUCHI_UI_INPUT
    if (!on_game_thread() || !input_mod || !mod_usable(input_mod)) return false;
    if (!hide) { input::table.release_hidden(input_mod, 0); return true; }
    return input::table.hold_hidden(input_mod, 0, TSUCHI_Buttons(0xFFFF)) == TSUCHI_OK;   // each of the sixteen buttons
#else
    (void)hide; return false;
#endif
}
bool ui_native_buttons_down(uint64_t& held) {
#if TSUCHI_UI_INPUT
    TSUCHI_PadState state{};
    if (!on_game_thread() || !input_mod || !mod_usable(input_mod) || input::table.pad(input_mod, 0, &state) != TSUCHI_OK) return false;
    held = state.held; return true;
#else
    (void)held; return false;
#endif
}
bool ui_native_start(TSUCHI_Mod* framework) {
    input_mod = framework;
    uint64_t id = 0;
    const auto& route = t::native_routes[t::native_route::ui_settings_dispatch];
    const uint32_t expected = 0x14000000u | (uint32_t((route.entry - route.address) >> 2) & 0x03ffffffu);
    const bool matching = game::global<uint64_t>(t::stub::block) == t::stub::patch_magic &&
        game::global<uint64_t>(t::stub::block_hash) == t::stub::patch_hash &&
        t::present(route.address) && game::global<uint32_t>(route.address) == expected;
    if (!layout_step_registered) layout_step_registered = each_engine_frame(layout_frame);
    wrapped = layout_step_registered && matching && native_route(t::native_route::ui_settings_dispatch, action);
    if (wrapped) hook_slot(framework, t::slot::npad_read, npad, &next_npad, 0, &id);
    ui_set_renderer(&renderer);
    return wrapped;
}
bool ui_menu_action(uint64_t host, uint32_t which) {
    const uint64_t object = game::from_handle(host);
    return on_game_thread() && wrapped && object && t::fn::ui_settings_dispatch &&
        t::fn::ui_settings_dispatch(reinterpret_cast<void*>(object), which);
}
bool ui_input_steps(bool (*register_step)(void (*)())) {
    if (!on_game_thread() || !register_step || !buttons_provider || input_step_registered) return false;
    input_step_registered = register_step(input_step); return input_step_registered;
}
void ui_buttons_provider(bool (*read)(uint32_t, uint64_t&)) { buttons_provider = read; }
void ui_menu_dispatcher(bool (*dispatch)(uint64_t, uint32_t, bool&, bool&)) { menu_dispatcher = dispatch; }
void ui_button_listener(void (*listener)(uint64_t)) { button_listener = listener; }
} // namespace tsuchi
