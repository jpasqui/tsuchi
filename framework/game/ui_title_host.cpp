// Tsuchi's own host for generated screens over a game menu (DESIGN.md, "Menu entries and typed text").
//
// Made of the game's stock parts, found by their reflected names: the Information page is the frame,
// with its background, header and footer; its article, scroll bar and pager are hidden. Buttons and
// choices are the tablet's menu buttons in a vertical box on the page's canvas, and text lines are the
// Information page's text blocks. The title adapter hands it the menu's Confirm, Back, Up and Down.
#include "ui_title.h"
#include "ui_screens.h"
#include "ui_rich_text.h"
#include "game.h"
#include "list.h"
#include "mods.h"
#include "names.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "shared.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;

constexpr uint32_t kRows = 24;
struct Row { uint64_t widget, text, slot; UiKind kind; bool focused; };
constexpr float kRowWidth = 1200.f, kButtonHeight = 72.f, kLineHeight = 52.f, kGap = 14.f;
struct View {
    uint64_t frame, box;
    bool texts;          // the header and footer were written after the page went on the screen
    Row rows[kRows];
    uint32_t count, revision, selected;
    bool fresh;
};

uint64_t page_class = 0, button_class = 0, line_class = 0, box_class = 0, note_class = 0;
game::ClassRef canvas_kind{t::name::ui_canvas_class};

uint64_t live(uint64_t handle) { return game::from_handle(handle); }
game::ClassRef panel_base{t::name::ui_panel_base_class};
uint64_t child(uint64_t object, uint64_t offset) {
    const uint64_t value = object ? game::read_or<uint64_t>(object + offset) : 0;
    return game::live(value) ? value : 0;
}
// A part of a widget by its name: a bound member, or else a widget of that name in its own tree.
uint64_t member(uint64_t object, const char* name) {
    const uint64_t value = object ? game::member_object(object, name) : 0;
    if (game::live(value)) return value;
    if (!object) return 0;
    uint64_t pending[128]{};
    pending[0] = child(child(object, t::ui_host::tree), t::ui_tree::root);
    uint32_t count = pending[0] ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t current = pending[i];
        if (game::name_is(game::name_of(current), name)) return current;
        if (!game::is_a(current, panel_base)) continue;
        const uint64_t data = game::read_or<uint64_t>(current + t::ui_panel::slots);
        const int32_t slots = game::read_or<int32_t>(current + t::ui_panel::slots + 8, -1);
        if (!slots) continue;   // an empty panel, such as a background blur
        if (!data || slots < 0 || slots > 128 || !game::readable(data, uint64_t(slots) * 8)) return 0;
        for (int32_t j = 0; j < slots; ++j) {
            const uint64_t next = child(game::read_or<uint64_t>(data + uint64_t(j) * 8), t::ui_panel_slot::content);
            bool seen = !next;
            for (uint32_t k = 0; k < count && !seen; ++k) seen = pending[k] == next;
            if (!seen) { if (count == 128) return 0; pending[count++] = next; }
        }
    }
    return 0;
}
uint64_t load(const char* path) {
    if (!t::bound(t::fn::name_from_text) || !t::bound(t::fn::load_soft_object)) return 0;
    struct Soft { int32_t index, serial; uint32_t tag, padding; uint64_t asset, subpath; int32_t count, capacity; } soft{-1, 0, 0, 0, 0, 0, 0, 0};
    char16_t units[256]; size_t count = 0;
    if (!game::utf8_to_units(path, units, 255, count)) return 0;
    units[count] = 0;
    soft.asset = t::fn::name_from_text(units, count, 1);
    const uint64_t kind = reinterpret_cast<uint64_t>(t::fn::load_soft_object(&soft));
    return game::live(kind) && game::is_class(kind) ? kind : 0;
}
bool classes() {
    if (!game::live(page_class)) page_class = load(t::asset::host_page_class);
    if (!game::live(button_class)) button_class = load(t::asset::title_button_class);
    if (!game::live(line_class)) line_class = load(t::asset::host_line_class);
    if (!game::live(box_class)) box_class = game::find_class(canvas_kind);
    return page_class && button_class && line_class && box_class;
}
bool text(uint64_t widget, const char* value) {
    if (!game::live(widget) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return false;
    char16_t units[1024]; size_t count = 0;
    if (!game::utf8_to_units(value ? value : "", units, 1023, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText made = t::fn::text_from_string(string);
    t::fn::rich_text_set(reinterpret_cast<void*>(widget), &made);
    game::drop_shared(made.counter()); return true;
}
void visibility(uint64_t object, uint32_t value) {
    const auto set = object ? game::method(object, t::widget_table::set_visibility) : nullptr;
    if (set) set(reinterpret_cast<void*>(object), value);
}
uint64_t construct(uint64_t frame, uint64_t kind) {
    const uint64_t tree = child(frame, t::ui_host::tree);
    if (!tree || !t::bound(t::fn::ui_construct)) return 0;
    uint64_t parameters[8]{}; parameters[0] = kind; parameters[1] = tree;
    const uint64_t made = reinterpret_cast<uint64_t>(t::fn::ui_construct(parameters));
    return game::live(made) ? made : 0;
}
uint64_t add(uint64_t panel, uint64_t widget) {
    if (!panel || !widget || !t::bound(t::fn::ui_add_child)) return 0;
    const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(panel), reinterpret_cast<void*>(widget)));
    return game::live(slot) ? slot : 0;
}

// A tablet button as the earlier framework's Mods page set it up: its canvas shown and sized, its text
// placed, its icon and new mark gone, and the corners only on the selected one.
// A title menu button, as the title menu shows its own: its new mark gone, and the game's own animations
// between its normal look and its selected one.
void button_look(uint64_t button, bool selected, bool first) {
    visibility(member(button, t::name::host_button_new), uint32_t(t::constant::widget_collapsed));
    const uint64_t animation = member(button, first ? (selected ? t::name::host_button_selected : t::name::title_button_normal)
                                                    : (selected ? t::name::host_button_focus : t::name::host_button_unfocus));
    if (animation && t::bound(t::fn::ui_mod_list_play_animation))
        t::fn::ui_mod_list_play_animation(reinterpret_cast<void*>(button), reinterpret_cast<void*>(animation), 0.f, 1, 0, 1.f, false);
}

// A row's caption: a button's label, a choice's label and value, or a text line's text.
void caption(const UiControl& control, char* out, size_t room) {
    out[0] = 0;
    const char* label = control.label.get() ? control.label.get() : "";
    size_t used = 0;
    auto put = [&](const char* part) { const size_t n = strlen(part); if (used + n < room) { memcpy(out + used, part, n + 1); used += n; } };
    if (control.kind == UiKind::text) { put(label); if (label[0] && control.text.get() && control.text.get()[0]) put("\n"); put(control.text.get() ? control.text.get() : ""); return; }
    put(label);
    if (control.kind == UiKind::choice && control.selected != kUiNoSelection) {
        const char* value = control.labels.first();
        for (uint32_t i = 0; value && i < control.selected; ++i) value = control.labels.after(value);
        if (value) { put(":  "); put(value); }
    }
}

bool build_rows(View& view, const UiScreen& screen) {
    const uint64_t frame = live(view.frame), box = live(view.box);
    if (!frame || !box) return false;
    view.count = 0;
    for (uint32_t i = 0; i < screen.controls.count && i < kRows; ++i) {
        const UiControl& control = screen.controls[i];
        const bool line = control.kind == UiKind::text;
        if (control.kind == UiKind::integer) continue;   // numbers wait for Left and Right in this host
        const uint64_t widget = t::fn::ui_create_widget ? reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(frame), reinterpret_cast<void*>(line ? line_class : button_class), nullptr)) : 0;
        const uint64_t slot = game::live(widget) ? add(box, widget) : 0;
        if (!slot) return false;
        view.rows[view.count].slot = game::to_handle(slot);
        const uint64_t label = member(widget, line ? t::name::host_line_text : t::name::host_button_text);
        if (!line) button_look(widget, false, true);
        view.rows[view.count].widget = game::to_handle(widget); view.rows[view.count].text = game::to_handle(label);
        view.rows[view.count].kind = control.kind; view.rows[view.count].focused = false; ++view.count;
    }
    return true;
}

void sync(View& view, const UiScreen& screen) {
    char line[1024];
    uint32_t row = 0;
    float top = 0;
    for (uint32_t i = 0; i < screen.controls.count && row < view.count; ++i) {
        const UiControl& control = screen.controls[i];
        if (control.kind == UiKind::integer) continue;
        caption(control, line, sizeof line);
        text(live(view.rows[row].text), line);
        uint32_t lines = 1;
        for (const char* c = line; *c; ++c) lines += *c == '\n';
        const bool line_row = view.rows[row].kind == UiKind::text;
        if (line_row && row && view.rows[row - 1].kind != UiKind::text) top += kLineHeight;   // a space between the buttons and the text
        const float height = line_row ? kLineHeight * float(lines) : kButtonHeight;
        if (const uint64_t slot = live(view.rows[row].slot)) t::fn::ui_slot_offsets(reinterpret_cast<void*>(slot), 0.f, top, kRowWidth, height);
        top += height + kGap;
        const bool focus = i == screen.selected;
        if (view.rows[row].kind != UiKind::text && focus != view.rows[row].focused) {
            button_look(live(view.rows[row].widget), focus, false);
            view.rows[row].focused = focus;
        }
        ++row;
    }
}

uint32_t rows_wanted(const UiScreen& screen) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < screen.controls.count && i < kRows; ++i) if (screen.controls[i].kind != UiKind::integer) ++n;
    return n;
}

uint32_t told = 0;
TSUCHI_Result refused(uint32_t which, const char* why) {
    if (which < 32 && !(told & (1u << which))) {
        told |= 1u << which;
        char line[160] = "screen host did not open: ";
        const size_t used = strlen(line), n = strlen(why);
        if (used + n < sizeof line) memcpy(line + used, why, n + 1);
        mod_note(kFrameworkLogId, line);
    }
    return TSUCHI_NOT_READY;
}
TSUCHI_Result finish_open(View* view, const UiScreen& screen, void** out);
TSUCHI_Result open(const UiScreen& screen, void** out) {
    const uint64_t owner = ui_title_view();
    if (!owner) return refused(0, "no title widget");
    if (!ui_title_menu_ready()) return refused(1, "the title menu is not taking input");
    if (!classes()) return refused(2, page_class ? button_class ? line_class ? "no canvas class" : "the text block class did not load" : "the button class did not load" : "the page class did not load");
    if (!t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_widget_viewport)) return refused(3, "a widget function is missing");
    const uint64_t frame = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(page_class), nullptr));
    if (!game::live(frame)) return refused(4, "the page was not created");
    auto* view = static_cast<View*>(allocate(sizeof(View)));
    if (!view) return TSUCHI_NO_MEMORY;
    *view = {};
    view->frame = game::to_handle(frame); view->fresh = true;
    // The page's own article, scroll bar and pager are not ours to show.
    const char* const hidden[] = {t::name::host_page_article, t::name::host_page_scroll, t::name::host_page_scrollbar, t::name::host_page_pager};
    for (const char* name : hidden)
        visibility(member(frame, name), uint32_t(t::constant::widget_collapsed));
    const uint64_t canvas = member(frame, t::name::host_page_canvas);
    if (!canvas) { release(view); return refused(5, "the page has no canvas"); }
    const uint64_t box = construct(frame, box_class);
    if (!box) { release(view); return refused(6, "the rows' canvas was not constructed"); }
    const uint64_t slot = add(canvas, box);
    if (!slot || !t::bound(t::fn::ui_slot_offsets)) { release(view); return refused(7, "the rows' canvas did not go on the page"); }
    t::fn::ui_slot_offsets(reinterpret_cast<void*>(slot), 360.f, 240.f, kRowWidth, 640.f);
    view->box = game::to_handle(box);
    return finish_open(view, screen, out);
}
void chrome(View& view, const UiScreen& screen) {
    const uint64_t frame = live(view.frame);
    if (!frame) return;
    text(member(member(frame, t::name::host_page_header), t::name::host_header_text), screen.title.get());
    char footer[256]; size_t used = 0;
    const char* a = ui_glyph_token(TSUCHI_GLYPH_BUTTON_A); const char* b = ui_glyph_token(TSUCHI_GLYPH_BUTTON_B);
    const char* const parts[] = {a ? a : "A", " Select   ", b ? b : "B", " Back"};
    for (const char* part : parts) { const size_t n = strlen(part); if (used + n < sizeof footer) { memcpy(footer + used, part, n); used += n; } }
    footer[used] = 0;
    text(member(member(frame, t::name::host_page_footer), t::name::ui_footer_text_member), screen.footer.get() && screen.footer.get()[0] ? screen.footer.get() : footer);
}
TSUCHI_Result finish_open(View* view, const UiScreen& screen, void** out) {
    const uint64_t frame = live(view->frame);
    if (!build_rows(*view, screen)) { if (const auto remove = game::method(frame, t::ui_widget::remove_from_parent)) remove(reinterpret_cast<void*>(frame)); release(view); return refused(8, "a row was not made"); }
    t::fn::ui_widget_viewport(reinterpret_cast<void*>(frame), 500, false);
    view->revision = screen.revision - 1;
    *out = view;
    return TSUCHI_OK;
}
bool update(void* pointer, const UiScreen& screen) {
    auto* view = static_cast<View*>(pointer);
    if (!view || !live(view->frame)) { refused(9, "the page went away before its first update"); return false; }
    if (rows_wanted(screen) != view->count) {
        // Controls came or went: the rows are made again.
        for (uint32_t i = 0; i < view->count; ++i)
            if (const uint64_t widget = live(view->rows[i].widget))
                if (const auto remove = game::method(widget, t::ui_widget::remove_from_parent)) remove(reinterpret_cast<void*>(widget));
        if (!build_rows(*view, screen)) return false;
        view->revision = screen.revision - 1;
    }
    if (!view->texts) { chrome(*view, screen); view->texts = true; }
    if (view->revision != screen.revision || view->selected != screen.selected) {
        sync(*view, screen);
        view->revision = screen.revision; view->selected = screen.selected;
    }
    return true;
}
bool alive(void* pointer) {
    const auto* view = static_cast<View*>(pointer);
    return view && live(view->frame) && ui_title_view();
}
void close(void* pointer) {
    auto* view = static_cast<View*>(pointer);
    if (!view) return;
    if (const uint64_t frame = live(view->frame))
        if (const auto remove = game::method(frame, t::ui_widget::remove_from_parent)) remove(reinterpret_cast<void*>(frame));
    release(view);
}
bool action(void*, uint64_t, uint32_t which) { return ui_action(which); }

struct Note { TSUCHI_Mod* mod; char text[301]; uint64_t widget, owner; bool changed; };
Note note{};
} // namespace

TSUCHI_Result ui_title_hold_note(TSUCHI_Mod* mod, const char* value) {
    const size_t bytes = strlen(value);
    char16_t units[301]; size_t count = 0;
    if (!bytes || bytes > 300 || !game::utf8_to_units(value, units, 300, count)) return TSUCHI_INVALID;
    if (note.mod && note.mod != mod && mod_usable(note.mod)) return TSUCHI_BUSY;
    if (note.mod != mod) { note = {}; note.mod = mod; }
    memcpy(note.text, value, bytes + 1); note.changed = true;
    return TSUCHI_OK;
}
TSUCHI_Result ui_title_release_note(TSUCHI_Mod* mod) {
    if (note.mod != mod) return TSUCHI_NOT_FOUND;
    if (const uint64_t widget = live(note.widget)) visibility(widget, uint32_t(t::constant::widget_collapsed));
    note = {};
    return TSUCHI_OK;
}
void ui_title_note_tick(uint64_t title_widget, bool menu_shown) {
    static const char* failed = nullptr;
    const auto fail = [](const char* why) {
        if (failed == why) return;
        failed = why;
        char line[120] = "title note not shown: "; const size_t used = strlen(line), n = strlen(why);
        if (used + n < sizeof line) memcpy(line + used, why, n + 1);
        mod_note(kFrameworkLogId, line);
    };
    if (note.mod && !mod_usable(note.mod)) ui_title_release_note(note.mod);
    if (!note.mod || !title_widget) return;
    uint64_t widget = live(note.widget);
    if (!widget || live(note.owner) != title_widget) {
        if (!menu_shown) return;
        // The note is another of the title's own description texts, placed as that one is and lifted above it.
        if (!game::live(note_class)) note_class = load(t::asset::title_text_class);
        if (!note_class || !t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_slot_offsets) ||
            !t::bound(t::fn::ui_slot_anchors) || !t::bound(t::fn::ui_slot_alignment) || !t::bound(t::fn::ui_slot_auto_size))
            return fail("its widget class or a function is missing");
        const uint64_t canvas = member(title_widget, t::name::title_canvas_child);
        if (!canvas) return fail("the title has no canvas");
        const uint64_t stock = member(title_widget, t::name::title_text_part);
        const uint64_t stock_slot = child(stock, t::ui_mod_list_tab::slot);
        if (!stock_slot) return fail("the title has no description text");
        struct Layout { float left, top, right, bottom, min_x, min_y, max_x, max_y, align_x, align_y; };
        const Layout layout = game::read_or<Layout>(stock_slot + t::ui_mod_list_tab::offsets);
        const bool auto_size = game::read_or<uint8_t>(stock_slot + t::ui_mod_list_tab::offsets + sizeof(Layout)) != 0;
        widget = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(title_widget), reinterpret_cast<void*>(note_class), nullptr));
        const uint64_t slot = game::live(widget) ? add(canvas, widget) : 0;
        if (!slot) return fail("it did not go on the canvas");
        void* const at = reinterpret_cast<void*>(slot);
        t::fn::ui_slot_anchors(at, layout.min_x, layout.min_y, layout.max_x, layout.max_y);
        t::fn::ui_slot_alignment(at, layout.align_x, layout.align_y);
        t::fn::ui_slot_auto_size(at, auto_size);
        t::fn::ui_slot_offsets(at, layout.left, layout.top - float(t::constant::title_note_lift), layout.right, layout.bottom);
        mod_note(kFrameworkLogId, "title note shown");
        note.widget = game::to_handle(widget); note.owner = game::to_handle(title_widget); note.changed = true;
    }
    if (note.changed) { text(member(widget, t::name::title_text_text), note.text); note.changed = false; }
}

namespace {
const UiRenderer renderer{open, update, alive, close, action, nullptr, nullptr, nullptr};
} // namespace

void ui_title_host_start() { ui_set_title_renderer(&renderer); }
} // namespace tsuchi
