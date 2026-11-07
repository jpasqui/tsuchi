#include "ui_mod_list_native.h"
#include "ui_screens.h"
#include "ui_native.h"
#include "ui_feedback.h"
#include "ui_label_layout.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "names.h"
#include "objects.h"
#include "shared.h"
#include "list.h"
#include "frame.h"
#include "mods.h"
#include "cost_clock.h"
#include <string.h>
namespace tsuchi {
namespace {
namespace t = game::table;
struct Array { uint64_t data; int32_t count, capacity; };
struct Row {
    char id[kModIdBytes], name[641], status[641];
    uint64_t widget, selector;
    bool requested, has_settings;
    UiLabelFitStamp fit;
};
struct State {
    uint64_t host, category, source_data, rows_data, scope, replacement_source;
    Array before;
    int32_t selected, previous;
    uint64_t scroll;
    float scroll_x, scroll_y, total, viewport;
    bool have_scroll;
    List<Row> rows;
    uint32_t calls;
    bool retired, installed;
    bool cleaned;
    HoldEnd ending;
    State* next_loan;
    UiFooterLease footer;
    UiLabelFitStamp heading_fit;
};
struct Kept { State* state; };
struct Owned { uint64_t host, group; bool was_kept; };
struct WantOwned { uint64_t host, group; };
TSUCHI_Mod* framework_mod = nullptr;
State* source_loans = nullptr;
uint64_t next_scope = 1;
UiModListNativeCallbacks callbacks{};
bool ready = false;
uint64_t cached_host = 0;
using Lifecycle = decltype(t::slot::ui_mod_list_construct)::Function;
Lifecycle next_construct = nullptr, next_destruct = nullptr;
using Refresh = decltype(t::code_site::ui_settings_refresh)::Function;
void* next_refresh = nullptr;
using Reopen = decltype(t::code_site::ui_settings_reopen)::Function;
void* next_reopen = nullptr;
game::ClassRef category_class{t::name::ui_mod_list_category_class};
game::ClassRef row_class{t::name::ui_mod_list_row_class};
game::ClassRef label_class{t::name::ui_mod_list_label_class};
game::ClassRef panel_class{t::name::ui_mod_list_panel_class};
game::ClassRef panel_base_class{t::name::ui_panel_base_class};

uint64_t live(uint64_t handle) { return game::from_handle(handle); }
uint64_t child(uint64_t object, uint64_t offset) {
    const uint64_t value = object ? game::read_or<uint64_t>(object + offset) : 0;
    return game::live(value) ? value : 0;
}
uint64_t member(uint64_t object, const char* name) {
    const uint64_t value = game::member_object(object, name);
    return game::live(value) ? value : 0;
}
bool copy(char* out, size_t room, const char* text) {
    if (!text) return false;
    size_t length = 0; while (length < room && text[length]) ++length;
    if (length == room) return false;
    memcpy(out, text, length + 1); return true;
}
bool caption(const Row& row, char out[641]) {
    const size_t name = strlen(row.name), status = strlen(row.status);
    if (!status) return copy(out, 641, row.name);
    if (name + 3 + status > 640) return false;
    memcpy(out, row.name, name); memcpy(out + name, " - ", 3); memcpy(out + name + 3, row.status, status + 1); return true;
}
void finish(State* state) {
    if (--state->calls || !state->retired) return;
    state->rows.clear(); release(state);
}
struct Retain {
    State* state;
    explicit Retain(State* value) : state(value) { ++state->calls; }
    ~Retain() { finish(state); }
};
bool pin_baseline(const Hold& hold, bool fallback);
bool another_pin(uint64_t handle);
TSUCHI_Result own_widget(const Hold& hold, Owned& kept, const WantOwned& wanted, bool fresh) {
    const uint64_t object = live(hold.object);
    if (!object) return TSUCHI_NOT_FOUND;
    if (fresh) { kept.was_kept = pin_baseline(hold, game::object_kept(object)); kept.host = wanted.host; kept.group = wanted.group; }
    game::keep_object(object, true);
    return game::object_kept(object) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
bool keep_widget(const Hold& hold, Owned&) { const uint64_t object = live(hold.object); if (!object) return false; game::keep_object(object, true); return game::object_kept(object); }
void restore_widget(const Hold& hold, Owned& kept, HoldEnd) {
    const uint64_t object = live(hold.object);
    if (object && !kept.was_kept && !another_pin(hold.object)) game::keep_object(object, false);
}
Holds<Owned, WantOwned, own_widget, keep_widget, restore_widget> widgets("ui.mod_list.widgets", HoldOn::object, false);
bool pin_baseline(const Hold& hold, bool fallback) {
    for (uint32_t i = 0; i < widgets.count(); ++i) {
        const Hold& other = widgets.hold_at(i);
        if (other.id != hold.id && other.object == hold.object) return widgets.kept_at(i).was_kept;
    }
    return fallback;
}
bool another_pin(uint64_t handle) {
    for (uint32_t i = 0; i < widgets.count(); ++i) if (widgets.hold_at(i).object == handle) return true;
    return false;
}
TSUCHI_Result apply(const Hold&, Kept&, const HoldNothing&, bool);
bool keep(const Hold&, Kept&);
void restore(const Hold&, Kept&, HoldEnd);
Holds<Kept, HoldNothing, apply, keep, restore> hosts("ui.mod_list.host", HoldOn::object, true);
State* state_for(uint64_t host) { const Kept* kept = hosts.find(framework_mod, host, 0); return kept ? kept->state : nullptr; }
bool usable(const State& state) { return !state.retired && live(state.host) && mod_usable(framework_mod) && state_for(state.host) == &state; }
void fit_labels(State& state) {
    Retain retained(&state);
    const auto current = [&] {
        const uint64_t host = live(state.host);
        return usable(state) && state.installed && !state.replacement_source &&
            game::read_or<uint8_t>(host + t::ui_host::state) == 1 &&
            game::read_or<Array>(host + t::ui_mod_list_menu::source).data == state.source_data &&
            child(host, t::ui_host::scroll) == live(state.scroll);
    };
    if (!current()) return;
    ui_fit_owned_label(state.heading_fit, live(state.scroll), child(live(state.category), t::ui_mod_list_category::title), true, current);
    for (uint32_t i = 0; i < state.rows.count && current(); ++i)
        ui_fit_owned_label(state.rows[i].fit, live(state.scroll), child(live(state.rows[i].widget), t::ui_mod_list_row::title), false, current);
}
void update_footer(State& state) {
    if (!usable(state)) return;
    Retain retained(&state);
    if (ui_active_screen()) { ui_footer_release(state.footer); return; }
    const int32_t selected = game::read_or<int32_t>(live(state.host) + t::ui_mod_list_menu::selected, -1);
    const bool selected_row = selected >= 0 && uint32_t(selected) < state.rows.count;
    UiFooterGuide guide;
    guide.tabs = true;
    guide.edit = selected_row;
    guide.settings = selected_row && state.rows[uint32_t(selected)].has_settings;
    guide.reset = state.rows.count != 0;
    guide.details = selected_row;
    guide.back = true; // Back returns to Game, after the save question when there is a draft
    ui_footer_update(state.footer, state.host, guide);
}
void release_widgets(uint64_t host, uint64_t group) {
    for (uint32_t i = widgets.count(); i--;) {
        if (i >= widgets.count()) continue;
        const Hold hold = widgets.hold_at(i);
        const Owned kept = widgets.kept_at(i);
        if (hold.mod == framework_mod && kept.host == host && kept.group == group) widgets.release(framework_mod, hold.object, hold.what);
    }
}
bool pin(uint64_t object, uint64_t host, uint64_t group) {
    const uint64_t handle = game::to_handle(object);
    return handle && widgets.take(framework_mod, handle, group, {host, group}) == TSUCHI_OK;
}
uint64_t named(uint64_t widget, const char* name) {
    uint64_t bound = 0;
    if (!strcmp(name, t::name::ui_mod_list_title_child)) bound = child(widget, t::ui_mod_list_category::title);
    if (!strcmp(name, t::name::ui_mod_list_panel_child)) bound = child(widget, t::ui_mod_list_category::panel);
    if (bound) return bound;
    // CreateWidget builds the tree before Settings binds these native members.
    // This lookup is local to the new category and never scans global objects.
    uint64_t pending[128]{};
    const uint64_t tree = child(widget, t::ui_host::tree);
    pending[0] = child(tree, t::ui_tree::root);
    uint32_t count = pending[0] ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t current = pending[i];
        if (game::name_is(game::name_of(current), name)) return current;
        if (!game::is_a(current, panel_base_class)) continue;
        Array slots{};
        if (!game::read(current + t::ui_panel::slots, slots) || slots.count < 0 ||
            slots.count > slots.capacity || slots.count > 128 ||
            (slots.count && !game::readable(slots.data, uint64_t(slots.count) * 8))) return 0;
        for (int32_t j = 0; j < slots.count; ++j) {
            const uint64_t next = child(game::read_or<uint64_t>(slots.data + uint64_t(j) * 8), t::ui_panel_slot::content);
            bool seen = !next;
            for (uint32_t k = 0; k < count && !seen; ++k) seen = pending[k] == next;
            if (!seen) { if (count == 128) return 0; pending[count++] = next; }
        }
    }
    return 0;
}

bool label(uint64_t widget, const char* text) {
    if (!game::live(widget)) return false;
    char16_t units[641]; size_t count = 0;
    if (!game::utf8_to_units(text, units, 640, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText text_value = t::fn::text_from_string(string);
    t::fn::rich_text_set(reinterpret_cast<void*>(widget), &text_value);
    game::drop_shared(text_value.counter()); return true;
}
bool choice(uint64_t widget, const char* text) {
    char16_t units[4]; size_t count = 0;
    if (!game::utf8_to_units(text, units, 3, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText text_value = t::fn::text_from_string(string);
    t::fn::ui_mod_list_append_choice(reinterpret_cast<void*>(widget), &text_value);
    game::drop_shared(text_value.counter()); return true;
}
bool set_choice(uint64_t widget, uint64_t selector, bool requested, game::CallGuard guard = {}) {
    const auto valid = game::method(selector, t::ui_mod_list_selector::valid);
    const auto redraw = game::method(selector, t::ui_mod_list_selector::redraw);
    if (!valid || !redraw || !valid(reinterpret_cast<void*>(selector), requested ? 1 : 0) || !guard.permits()) return false;
    if (!game::write<uint32_t>(selector + t::ui_mod_list_selector::choice, requested ? 1 : 0) ||
        !game::write<uint32_t>(widget + t::ui_mod_list_row::shadow, requested ? 1 : 0)) return false;
    redraw(reinterpret_cast<void*>(selector)); return guard.permits();
}
void tab_highlight(uint64_t host, bool mods);
void cleanup_source(State& state) {
    if (state.cleaned || state.replacement_source) return;
    state.cleaned = true;
    const HoldEnd why = state.ending;
    ui_footer_release(state.footer);
    tab_highlight(state.host, false);
    const uint64_t host = live(state.host);
    if (host && state.installed) {
        const Array current = game::read_or<Array>(host + t::ui_mod_list_menu::source);
        if (current.data == state.source_data) {
            game::write(host + t::ui_mod_list_menu::source, state.before);
            // Refresh deselects against the old active array before replacing it.
            if (why != HoldEnd::destroyed && why != HoldEnd::destroying) {
                t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host));
                if (live(state.host) && game::read_or<Array>(host + t::ui_mod_list_menu::source).data == state.before.data) {
                    game::write<int32_t>(host + t::ui_mod_list_menu::selected, state.selected);
                    game::write<int32_t>(host + t::ui_mod_list_menu::previous, -1);
                    t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host));
                    if (live(state.host) && game::read_or<Array>(host + t::ui_mod_list_menu::source).data == state.before.data) {
                        game::write<int32_t>(host + t::ui_mod_list_menu::previous, state.previous);
                        const uint64_t scroll = live(state.scroll), panel = child(scroll, t::ui_mod_list_scroll::panel);
                        if (state.have_scroll && panel && t::bound(t::fn::ui_mod_list_transform) && t::fn::ui_mod_list_scrollbar &&
                            game::read_or<float>(scroll + t::ui_mod_list_scroll::total) == state.total &&
                            game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport) == state.viewport) {
                            game::write<float>(panel + t::ui_mod_list_scroll::x, state.scroll_x);
                            game::write<float>(panel + t::ui_mod_list_scroll::y, state.scroll_y);
                            t::fn::ui_mod_list_transform(reinterpret_cast<void*>(panel));
                            if (live(state.scroll) && game::read_or<Array>(host + t::ui_mod_list_menu::source).data == state.before.data)
                                t::fn::ui_mod_list_scrollbar(reinterpret_cast<void*>(scroll));
                        }
                    }
                }
            }
        }
    }
    if (const uint64_t category = live(state.category)) {
        const Array current = game::read_or<Array>(category + t::ui_mod_list_category::rows);
        if (current.data == state.rows_data) game::write(category + t::ui_mod_list_category::rows, Array{});
        game::write<uint64_t>(category + t::ui_mod_list_category::owner, 0);
    }
    if (state.source_data) game::release_engine_memory(reinterpret_cast<void*>(state.source_data));
    if (state.rows_data) game::release_engine_memory(reinterpret_cast<void*>(state.rows_data));
    state.source_data = state.rows_data = 0;
    release_widgets(state.host, state.scope);
}
void cleanup(State& state, HoldEnd why) {
    state.retired = true; state.ending = why;
    // Closing can be deferred while a renderer update is on the stack. Its
    // source loan retains this state until the captured close returns it.
    ui_native_host_detach(state.host);
    ui_host_lost(state.host);
    cleanup_source(state);
}
TSUCHI_Result apply(const Hold& hold, Kept& kept, const HoldNothing&, bool fresh) {
    if (!fresh) return TSUCHI_OK;
    const uint64_t host = live(hold.object);
    const auto guard = hold_call_guard(hold);
    if (!host || !ready || !callbacks.count || !callbacks.row || !t::bound(t::fn::ui_create_widget) ||
        !t::bound(t::fn::ui_add_child) || !t::bound(t::fn::ui_mod_list_refresh) || !t::bound(t::fn::ui_mod_list_focus) ||
        !t::bound(t::fn::ui_mod_list_bind_row) || !t::bound(t::fn::ui_mod_list_append_choice) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set) ||
        !game::read_or<uint8_t>(host + t::ui_mod_list_menu::new_mode)) return TSUCHI_NOT_READY;
    const uint32_t count = callbacks.count();
    if (!guard.permits()) return TSUCHI_NOT_READY;
    // Never give stock navigation an empty list: its enabled-row scan is unbounded.
    if (!count || count > t::constant::ui_mod_list_rows_limit) return TSUCHI_CAPACITY;
    auto* state = static_cast<State*>(allocate(sizeof(State)));
    if (!state) return TSUCHI_NO_MEMORY;
    *state = {}; state->calls = 1; state->host = hold.object; state->scope = next_scope++;
    state->before = game::read_or<Array>(host + t::ui_mod_list_menu::source);
    state->selected = game::read_or<int32_t>(host + t::ui_mod_list_menu::selected);
    state->previous = game::read_or<int32_t>(host + t::ui_mod_list_menu::previous);
    const uint64_t scroll = child(host, t::ui_host::scroll), scroll_panel = child(scroll, t::ui_mod_list_scroll::panel);
    state->scroll = game::to_handle(scroll);
    state->total = game::read_or<float>(scroll + t::ui_mod_list_scroll::total);
    state->viewport = game::read_or<float>(scroll + t::ui_mod_list_scroll::viewport);
    state->have_scroll = scroll_panel && state->total > 0 && state->viewport > 0;
    if (state->have_scroll) {
        state->scroll_x = game::read_or<float>(scroll_panel + t::ui_mod_list_scroll::x);
        state->scroll_y = game::read_or<float>(scroll_panel + t::ui_mod_list_scroll::y);
    }
    kept.state = state; hold_apply_checkpoint(hold);
    TSUCHI_Result result = TSUCHI_NOT_READY;
    const char* problem = "Mods menu: source categories unavailable";
    bool okay = state->before.count >= 0 && state->before.count <= int32_t(t::constant::ui_mod_list_rows_limit) &&
        (!state->before.count || game::readable(state->before.data, uint64_t(state->before.count) * 8)) && state->rows.reserve(count);
    for (int32_t i = 0; i < state->before.count && okay; ++i) okay = pin(game::read_or<uint64_t>(state->before.data + uint64_t(i) * 8), hold.object, state->scope);
    for (uint32_t i = 0; i < count && okay && guard.permits(); ++i) {
        UiModListRow input{}; Row row{};
        okay = callbacks.row(i, input) && guard.permits() && valid_mod_id(input.id) && copy(row.id, sizeof row.id, input.id) &&
            copy(row.name, sizeof row.name, input.name) && copy(row.status, sizeof row.status, input.status ? input.status : "");
        row.requested = input.requested;
        row.has_settings = input.has_settings;
        if (okay) okay = state->rows.add(row) != nullptr;
    }
    if (okay && guard.permits()) {
        problem = "Mods menu: category creation unavailable";
        const uint64_t kind = child(host, t::ui_mod_list_menu::category_class);
        const uint64_t category = kind ? reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(host), reinterpret_cast<void*>(kind), nullptr)) : 0;
        state->category = game::to_handle(category);
        okay = guard.permits() && category && game::is_a(category, category_class) && pin(category, hold.object, state->scope);
        if (okay) {
            problem = "Mods menu: category children unavailable";
            const uint64_t title = named(category, t::name::ui_mod_list_title_child), panel = named(category, t::name::ui_mod_list_panel_child);
            okay = title && panel && game::is_a(title, label_class) && game::is_a(panel, panel_class) &&
                game::write<uint64_t>(category + t::ui_mod_list_category::title, title) &&
                game::write<uint64_t>(category + t::ui_mod_list_category::panel, panel) && label(title, "Mods") && guard.permits();
            const Array empty = game::read_or<Array>(category + t::ui_mod_list_category::rows);
            if (okay) problem = "Mods menu: new category already has rows";
            okay = okay && !empty.data && !empty.count && !empty.capacity;
            if (okay) {
                problem = "Mods menu: row array allocation failed";
                state->rows_data = reinterpret_cast<uint64_t>(game::engine_memory(uint64_t(count) * 8));
                state->source_data = reinterpret_cast<uint64_t>(game::engine_memory(8));
                okay = state->rows_data && state->source_data && guard.permits();
                if (!okay) result = TSUCHI_NO_MEMORY;
            }
            const uint64_t row_kind = child(category, t::ui_mod_list_category::row_class);
            for (uint32_t i = 0; i < count && okay && guard.permits(); ++i) {
                problem = "Mods menu: row creation unavailable";
                const uint64_t widget = row_kind ? reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(host), reinterpret_cast<void*>(row_kind), nullptr)) : 0;
                Row& row = state->rows[i]; row.widget = game::to_handle(widget);
                okay = guard.permits() && widget && game::is_a(widget, row_class) && pin(widget, hold.object, state->scope);
                if (!okay) break;
                t::fn::ui_mod_list_bind_row(reinterpret_cast<void*>(widget));
                problem = "Mods menu: row controls unavailable";
                const uint64_t title_widget = child(widget, t::ui_mod_list_row::title), selector = child(widget, t::ui_mod_list_row::selector);
                row.selector = game::to_handle(selector);
                char text[641];
                okay = guard.permits() && title_widget && selector && caption(row, text) && label(title_widget, text) && guard.permits() &&
                    choice(widget, "Off") && guard.permits() && choice(widget, "On") && guard.permits() &&
                    game::write<uint8_t>(widget + t::ui_mod_list_row::kind, uint8_t(t::constant::ui_mod_list_item)) &&
                    game::write<uint8_t>(widget + t::ui_mod_list_row::parent, 0) && game::write<int32_t>(widget + t::ui_mod_list_row::count, -1) &&
                    game::write<uint8_t>(widget + t::ui_mod_list_row::enabled, 1) && game::write<uint8_t>(selector + t::ui_mod_list_selector::wrap, 0) &&
                    game::write<uint8_t>(selector + t::ui_mod_list_selector::enabled, 1) && set_choice(widget, selector, row.requested, guard) && guard.permits();
                if (okay) {
                    const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(panel), reinterpret_cast<void*>(widget)));
                    okay = guard.permits() && game::live(slot) && game::write<uint64_t>(state->rows_data + uint64_t(i) * 8, widget);
                }
            }
            if (okay && guard.permits()) {
                problem = "Mods menu: category installation failed";
                okay = game::write(category + t::ui_mod_list_category::rows, Array{state->rows_data, int32_t(count), int32_t(count)}) &&
                    game::write<uint8_t>(category + t::ui_mod_list_category::kind, uint8_t(t::constant::ui_mod_list_category)) &&
                    game::write<int32_t>(category + t::ui_mod_list_category::span, int32_t(count)) &&
                    game::write<uint64_t>(category + t::ui_mod_list_category::owner, host) && game::write<uint64_t>(state->source_data, category) &&
                    game::write(host + t::ui_mod_list_menu::source, Array{state->source_data, 1, 1});
                if (okay) {
                    state->installed = true;
                    t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host));
                    okay = guard.permits() && game::write<int32_t>(host + t::ui_mod_list_menu::selected, 0) &&
                        game::write<int32_t>(host + t::ui_mod_list_menu::previous, -1);
                    if (okay) { t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host)); okay = guard.permits(); }
                }
            }
        }
    }
    if (okay && guard.permits()) { tab_highlight(hold.object, true); finish(state); return guard.permits() ? TSUCHI_OK : TSUCHI_NOT_READY; }
    // An ended apply is restored once by HoldTable after this call returns.
    if (guard.permits()) { cleanup(*state, HoldEnd::released); kept.state = nullptr; mod_note(kFrameworkLogId, problem); }
    finish(state); return result;
}
bool keep(const Hold& hold, Kept& kept) {
    if (!kept.state) return false;
    const uint64_t host = live(hold.object);
    if (!host || game::read_or<uint8_t>(host + t::ui_host::state) == 4 || !game::read_or<uint8_t>(host + t::ui_mod_list_menu::new_mode)) return false;
    const Array current = game::read_or<Array>(host + t::ui_mod_list_menu::source);
    return (current.data == kept.state->source_data ||
        (kept.state->replacement_source && current.data == kept.state->replacement_source)) && live(kept.state->category);
}
void restore(const Hold& hold, Kept& kept, HoldEnd why) {
    if (!kept.state) return;
    State* state = kept.state; Retain retained(state);
    cleanup(*state, why); kept.state = nullptr;
    (void)hold;
}

struct Margin { float left, top, width, height; };
struct Tabs {
    uint64_t host, scroll, scroll_slot, scrollbar, bar_slot, bar_parent, scope;
    List<uint64_t> buttons;
    uint32_t revision, selected;
    Margin before, installed, rendered;
    float bar_before, bar_rendered;
    uint32_t calls, refresh_depth, cycle_traces;
    uint8_t clipping_before;
    bool retired, changed, clipping_changed, announced;
    // Made by the menu's own construction or reopening: until it accepts input, a short while at most, its
    // Game guide is written already. A bar found on a menu that is not opening never writes it before state 1.
    uint64_t opening_until;
    UiFooterLease footer;
};
struct TabKept { Tabs* tabs; };
game::ClassRef tab_class{t::name::ui_mod_list_tab_class}, slot_class{t::name::ui_mod_list_slot_class};
uint64_t cached_tab_class() {
    return tab_class.found && live(tab_class.handle) == tab_class.found ? tab_class.found : 0;
}
game::ClassRef canvas_class{t::name::ui_canvas_class}, host_class{t::name::ui_host_class};
bool active_host(uint64_t object);
bool layout_host(uint64_t object);
void finish_tabs(Tabs* tabs) { if (!--tabs->calls && tabs->retired) { tabs->buttons.clear(); release(tabs); } }
struct RetainTabs {
    Tabs* tabs;
    explicit RetainTabs(Tabs* value) : tabs(value) { ++tabs->calls; }
    ~RetainTabs() { finish_tabs(tabs); }
};
uint64_t tab_cycle_attempt = 0;
uint32_t tab_cycle_traces = 0, tab_unbound_cycle_traces = 0;
struct TabCycleTrace {
    uint64_t at, attempt, host;
    uint32_t old = UINT32_MAX, next = UINT32_MAX;
    bool previous, ready;
};
void tab_cycle_trace(const TabCycleTrace& trace, uint64_t completed, TSUCHI_Result result) {
    char line[320]; size_t used = 0;
    const auto text = [&](const char* part) {
        const size_t size = strlen(part);
        if (used + size >= sizeof line) return false;
        memcpy(line + used, part, size); used += size; return true;
    };
    const auto number = [&](const char* name, uint64_t value) {
        char digits[21]; char* at = digits + sizeof digits; *--at = 0;
        do { *--at = char('0' + value % 10); value /= 10; } while (value);
        return text(name) && text(at);
    };
    const uint64_t elapsed = trace.at && completed >= trace.at ? completed - trace.at : 0;
    if (!text("Settings tab cycle") || !number(" at_us=", trace.at) || !number(" attempt=", trace.attempt) ||
        !number(" host=", trace.host) || !number(" previous=", trace.previous) || !number(" ready=", trace.ready) ||
        !number(" old=", trace.old) || !number(" next=", trace.next) || !number(" result=", result) ||
        !number(" completed_us=", completed) || !number(" cost_us=", elapsed)) return;
    line[used] = 0; mod_note(kFrameworkLogId, line);
}
bool visibility(uint64_t object, uint32_t value) {
    const auto set = object ? game::method(object, t::widget_table::set_visibility) : nullptr;
    if (!set) return false;
    set(reinterpret_cast<void*>(object), value); return true;
}
void offsets(uint64_t slot, Margin value) {
    t::fn::ui_slot_offsets(reinterpret_cast<void*>(slot), value.left, value.top, value.width, value.height);
}
void detach_widget(uint64_t handle) {
    const uint64_t widget = live(handle), slot = child(widget, t::ui_mod_list_tab::slot), parent = child(slot, t::ui_mod_list_tab::parent);
    if (!parent || !t::bound(t::fn::ui_remove_child_at)) return;
    const Array slots = game::read_or<Array>(parent + t::ui_panel::slots);
    if (slots.count < 0 || slots.count > 256 || slots.capacity < slots.count || !game::readable(slots.data, uint64_t(slots.count) * 8)) return;
    for (int32_t i = 0; i < slots.count; ++i) {
        const uint64_t candidate = game::read_or<uint64_t>(slots.data + uint64_t(i) * 8);
        if (child(candidate, t::ui_panel_slot::content) == widget) {
            t::fn::ui_remove_child_at(reinterpret_cast<void*>(parent), i); return;
        }
    }
}
bool tabs_associated(const Tabs& tabs) {
    const uint64_t host = live(tabs.host), scroll = live(tabs.scroll), slot = live(tabs.scroll_slot);
    return host && scroll && slot && child(host, t::ui_host::scroll) == scroll && child(scroll, t::ui_mod_list_tab::slot) == slot;
}
bool tabs_current(const Tabs& tabs);
bool tabs_superseded(const Tabs& tabs);
bool owned_layout(const Tabs& tabs, const Margin& current) {
    return !memcmp(&current, &tabs.installed, sizeof current) || !memcmp(&current, &tabs.rendered, sizeof current);
}
bool bar_associated(const Tabs& tabs) {
    return tabs_associated(tabs) && live(tabs.scrollbar) && live(tabs.bar_slot) &&
        child(live(tabs.scrollbar), t::ui_mod_list_tab::slot) == live(tabs.bar_slot) &&
        child(live(tabs.bar_slot), t::ui_mod_list_tab::parent) == live(tabs.bar_parent);
}
void bar_height(Tabs& tabs, bool rebuilding) {
    if (!tabs.bar_slot || tabs_superseded(tabs) || !bar_associated(tabs)) return;
    const uint64_t slot = live(tabs.bar_slot);
    Margin current = game::read_or<Margin>(slot + t::ui_mod_list_tab::offsets);
    if (current.height != tabs.bar_before && current.height != tabs.bar_rendered) return;
    const float height = rebuilding ? tabs.bar_before : tabs.bar_rendered;
    if (current.height != height) { current.height = height; offsets(slot, current); }
}
bool list_height(Tabs& tabs, bool rebuilding) {
    if (!tabs_current(tabs) || !tabs_associated(tabs)) return false;
    const uint64_t slot = live(tabs.scroll_slot);
    const Margin current = game::read_or<Margin>(slot + t::ui_mod_list_tab::offsets);
    if (!owned_layout(tabs, current)) return false;
    const Margin& wanted = rebuilding ? tabs.installed : tabs.rendered;
    if (memcmp(&current, &wanted, sizeof current)) offsets(slot, wanted);
    const Margin installed = game::read_or<Margin>(slot + t::ui_mod_list_tab::offsets);
    if (!tabs_current(tabs) || !tabs_associated(tabs) || memcmp(&installed, &wanted, sizeof wanted)) return false;
    bar_height(tabs, rebuilding);
    return tabs_current(tabs) && tabs_associated(tabs);
}
void restore_tabs(const Hold& hold, TabKept& kept, HoldEnd why) {
    if (!kept.tabs) return;
    Tabs* tabs = kept.tabs; RetainTabs retained(tabs); tabs->retired = true;
    ui_footer_release(tabs->footer);
    for (uint32_t i = 0; i < tabs->buttons.count; ++i) { const uint64_t handle = tabs->buttons[i]; visibility(live(handle), uint32_t(t::constant::widget_hidden)); detach_widget(handle); }
    bool restored = false;
    const uint64_t slot = live(tabs->scroll_slot);
    if (tabs_associated(*tabs) && !tabs_superseded(*tabs)) {
        if (tabs->changed) {
            const Margin current = game::read_or<Margin>(slot + t::ui_mod_list_tab::offsets);
            if (owned_layout(*tabs, current)) { offsets(slot, tabs->before); restored = true; }
        }
        bar_height(*tabs, true);
        const uint64_t scroll = live(tabs->scroll);
        if (tabs_associated(*tabs) && !tabs_superseded(*tabs) && tabs->clipping_changed && game::read_or<uint8_t>(scroll + t::ui_widget::clipping) == 1)
            t::fn::ui_widget_set_clipping(reinterpret_cast<void*>(scroll), tabs->clipping_before);
        const uint64_t host = live(hold.object);
        if (restored && tabs_associated(*tabs) && !tabs_superseded(*tabs) && host && t::bound(t::fn::ui_mod_list_refresh) && t::bound(t::fn::ui_mod_list_focus)) {
            t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host));
            if (tabs_associated(*tabs) && !tabs_superseded(*tabs) && active_host(host) &&
                (!ui_active_screen() || game::read_or<int32_t>(host + t::ui_mod_list_menu::selected, -1) >= 0))
                t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host));
        }
    }
    release_widgets(hold.object, tabs->scope); kept.tabs = nullptr;
    if (why != HoldEnd::released && callbacks.host_lost) callbacks.host_lost(hold.object);
}
TSUCHI_Result apply_tabs(const Hold& hold, TabKept& kept, const HoldNothing&, bool fresh) {
    if (!fresh) return TSUCHI_OK;
    const auto guard = hold_call_guard(hold);
    const uint64_t host = live(hold.object), kind = cached_tab_class();
    const uint64_t tree = child(host, t::ui_host::tree), root = child(tree, t::ui_tree::root), scroll = child(host, t::ui_host::scroll);
    const uint64_t scroll_slot = child(scroll, t::ui_mod_list_tab::slot);
    if (!layout_host(host) || !root || !kind || !scroll_slot || !game::is_a(root, canvas_class) || !game::is_a(scroll_slot, slot_class) ||
        !t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_add_child) || !t::bound(t::fn::ui_remove_child_at) || !t::bound(t::fn::ui_slot_offsets) || !t::bound(t::fn::ui_widget_set_clipping) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return TSUCHI_NOT_READY;
    const Margin layout = game::read_or<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
    const Margin anchors = game::read_or<Margin>(scroll_slot + t::ui_mod_list_tab::anchors);
    if (anchors.left || anchors.top || anchors.width || anchors.height || !(layout.width >= 600 && layout.width <= 2000 && layout.height >= 400 && layout.height <= 1400)) return TSUCHI_UNSUPPORTED;
    const float scale = game::read_or<float>(host + t::ui_widget::render_scale_y);
    if (!(scale >= .25f && scale <= 1)) return TSUCHI_UNSUPPORTED;
    const uint64_t scrollbar = named(scroll, "PA_ScrollBar"), bar_slot = child(scrollbar, t::ui_mod_list_tab::slot);
    uint64_t stretched_bar = 0;
    if (scrollbar) {
        if (!bar_slot || !game::is_a(bar_slot, slot_class)) return TSUCHI_UNSUPPORTED;
        const Margin bar_anchors = game::read_or<Margin>(bar_slot + t::ui_mod_list_tab::anchors);
        if (bar_anchors.top != bar_anchors.height) {
            if (bar_anchors.top || bar_anchors.height != 1) return TSUCHI_UNSUPPORTED;
            stretched_bar = bar_slot;
        }
    }
    auto* tabs = static_cast<Tabs*>(allocate(sizeof(Tabs))); if (!tabs) return TSUCHI_NO_MEMORY;
    *tabs = {}; tabs->calls = 1; tabs->host = hold.object; tabs->scope = next_scope++;
    tabs->scroll = game::to_handle(scroll); tabs->scroll_slot = game::to_handle(scroll_slot); tabs->before = layout;
    tabs->clipping_before = game::read_or<uint8_t>(scroll + t::ui_widget::clipping);
    tabs->installed = {layout.left, layout.top + 80, layout.width, layout.height - 80};
    tabs->rendered = tabs->installed; tabs->rendered.height /= scale;
    if (stretched_bar) {
        tabs->scrollbar = game::to_handle(scrollbar); tabs->bar_slot = game::to_handle(stretched_bar);
        tabs->bar_parent = game::to_handle(child(stretched_bar, t::ui_mod_list_tab::parent));
        if (!tabs->bar_parent) { release(tabs); return TSUCHI_UNSUPPORTED; }
        tabs->bar_before = game::read_or<Margin>(stretched_bar + t::ui_mod_list_tab::offsets).height;
        if (!(tabs->bar_before > -10000 && tabs->bar_before < 10000)) { release(tabs); return TSUCHI_UNSUPPORTED; }
        tabs->bar_rendered = tabs->bar_before + tabs->rendered.height - tabs->installed.height;
    }
    kept.tabs = tabs; hold_apply_checkpoint(hold);
    // Retain the host until its Slate teardown or closed state hands its
    // original arrays back. Polling after collection would be too late.
    const uint32_t tab_count = ui_settings_tab_count() + 2;
    tabs->revision = ui_settings_tab_revision();
    const auto current = [&] { return guard.permits() && tabs->revision == ui_settings_tab_revision(); };
    bool okay = tab_count <= 8 && tabs->buttons.reserve(tab_count) && pin(host, hold.object, tabs->scope);
    for (uint32_t i = 0; i < tab_count && okay && current(); ++i) {
        const UiScreen* screen = i && i + 1 < tab_count ? ui_settings_tab(i - 1) : nullptr;
        if (i && i + 1 < tab_count && !screen) { okay = false; break; }
        char caption[641];
        const char* title = i == 0 ? "Game" : i + 1 == tab_count ? "Mods" : screen->title.get();
        const size_t title_bytes = strlen(title);
        if (title_bytes >= sizeof caption) { okay = false; break; }
        memcpy(caption, title, title_bytes + 1);
        const uint64_t button = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(host), reinterpret_cast<void*>(kind), nullptr));
        tabs->buttons.add(game::to_handle(button));
        const uint64_t text = member(button, t::name::ui_mod_list_tab_label_member);
        okay = current() && button && text && pin(button, hold.object, tabs->scope) && label(text, caption) && current() && visibility(button, uint32_t(t::constant::widget_shown)) && current();
        if (!okay) break;
        const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(root), reinterpret_cast<void*>(button)));
        okay = current() && game::live(slot); if (!okay) break;
        const float width = layout.width / float(tab_count);
        offsets(slot, {layout.left + i * width, layout.top, width - 12, 64});
        const uint64_t line = member(button, t::name::ui_mod_list_tab_underline_member), line_slot = child(line, t::ui_mod_list_tab::slot);
        const uint64_t panel = child(line_slot, t::ui_mod_list_tab::parent), panel_slot = child(panel, t::ui_mod_list_tab::slot);
        okay = current() && line_slot && panel_slot && game::is_a(line_slot, slot_class) && game::is_a(panel, canvas_class) && game::is_a(panel_slot, slot_class);
        if (okay) {
            Margin inner = game::read_or<Margin>(panel_slot + t::ui_mod_list_tab::offsets); inner.width = width - 12; offsets(panel_slot, inner);
            if (current()) { Margin underline = game::read_or<Margin>(line_slot + t::ui_mod_list_tab::offsets); underline.width = width - 12; offsets(line_slot, underline); }
            okay = current();
        }
    }
    okay = okay && current();
    if (okay) {
        const Margin current = game::read_or<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
        okay = tabs_current(*tabs) && tabs_associated(*tabs) && !memcmp(&current, &tabs->before, sizeof current) &&
            game::read_or<uint8_t>(scroll + t::ui_widget::clipping) == tabs->clipping_before;
        if (okay && tabs->bar_slot) okay = bar_associated(*tabs) &&
            game::read_or<Margin>(live(tabs->bar_slot) + t::ui_mod_list_tab::offsets).height == tabs->bar_before;
    }
    if (okay && current()) {
        tabs->changed = true; offsets(scroll_slot, tabs->installed); okay = current() && tabs_current(*tabs) && tabs_associated(*tabs);
        if (okay) {
            tabs->clipping_changed = true;
            t::fn::ui_widget_set_clipping(reinterpret_cast<void*>(scroll), 1);
            okay = current() && tabs_current(*tabs) && tabs_associated(*tabs);
        }
        if (okay && t::bound(t::fn::ui_mod_list_refresh) && t::bound(t::fn::ui_mod_list_focus)) {
            t::fn::ui_mod_list_refresh(reinterpret_cast<void*>(host));
            okay = current() && tabs_current(*tabs) && tabs_associated(*tabs);
            if (okay && active_host(host) && (!ui_active_screen() || game::read_or<int32_t>(host + t::ui_mod_list_menu::selected, -1) >= 0)) {
                t::fn::ui_mod_list_focus(reinterpret_cast<void*>(host)); okay = current() && tabs_current(*tabs) && tabs_associated(*tabs);
            }
        }
    }
    if (okay) { finish_tabs(tabs); return TSUCHI_OK; }
    if (guard.permits()) { restore_tabs(hold, kept, HoldEnd::released); }
    finish_tabs(tabs); return TSUCHI_NOT_READY;
}
bool keep_tabs(const Hold& hold, TabKept& kept) {
    // Declaration changes are reconciled by prepare_host on engine frames.
    // A world pass must not remove an otherwise live bar before that work.
    return kept.tabs && live(hold.object) && game::read_or<uint8_t>(live(hold.object) + t::ui_host::state) != 4 && tabs_associated(*kept.tabs) && kept.tabs->buttons.count >= 2 && live(kept.tabs->buttons[0]) && live(kept.tabs->buttons[kept.tabs->buttons.count - 1]);
}
Holds<TabKept, HoldNothing, apply_tabs, keep_tabs, restore_tabs> bars("ui.mod_list.tabs", HoldOn::object, true);
bool tabs_current(const Tabs& tabs) {
    const TabKept* kept = bars.find(framework_mod, tabs.host, 0);
    return !tabs.retired && mod_usable(framework_mod) && kept && kept->tabs == &tabs;
}
bool tabs_superseded(const Tabs& tabs) {
    const TabKept* kept = bars.find(framework_mod, tabs.host, 0);
    return kept && kept->tabs && kept->tabs != &tabs;
}
void refresh(void* pointer) {
    const Refresh original = reinterpret_cast<Refresh>(next_refresh);
    if (!on_game_thread()) { original(pointer); return; }
    const uint64_t host = game::to_handle(reinterpret_cast<uint64_t>(pointer));
    const TabKept* kept = host ? bars.find(framework_mod, host, 0) : nullptr;
    Tabs* tabs = kept ? kept->tabs : nullptr;
    if (!tabs || !tabs->changed || !tabs_current(*tabs) || !tabs_associated(*tabs)) { original(pointer); return; }
    RetainTabs retained(tabs);
    ++tabs->refresh_depth;
    const bool owned = tabs->refresh_depth != 1 || list_height(*tabs, true);
    original(pointer);
    if (!--tabs->refresh_depth && owned && tabs_current(*tabs) && tabs_associated(*tabs) &&
        game::read_or<uint8_t>(live(tabs->scroll) + t::ui_widget::clipping) == 1) list_height(*tabs, false);
}
void update_game_footer(Tabs& tabs) {
    RetainTabs retained(&tabs);
    // The guide is written while the menu opens as well (state 0 just after its construction or reopening), so it
    // is complete in its first picture. The footer is shared with other menus: a closed Settings never writes it.
    const uint64_t object = live(tabs.host);
    const uint8_t state = game::read_or<uint8_t>(object + t::ui_host::state);
    if (state == 1) tabs.opening_until = 0;
    const bool showing = layout_host(object) && (state == 1 || (state == 0 && tabs.opening_until && cost_now() < tabs.opening_until));
    if (tabs.retired || tabs.selected || !showing || ui_active_screen() || state_for(tabs.host)) {
        ui_footer_release(tabs.footer);
        return;
    }
    UiFooterGuide guide;
    guide.tabs = true;
    guide.preserve_stock = true;
    ui_footer_update(tabs.footer, tabs.host, guide);
}
void tab_highlight(uint64_t host, bool mods) {
    const TabKept* kept = bars.find(framework_mod, host, 0);
    if (!kept || !kept->tabs || !t::bound(t::fn::ui_mod_list_play_animation)) return;
    Tabs* tabs = kept->tabs; RetainTabs retained(tabs);
    if (mods) tabs->selected = tabs->buttons.count - 1;
    else if (!ui_active_screen()) tabs->selected = 0;
    for (uint32_t i = 0; i < tabs->buttons.count && !tabs->retired; ++i) {
        const uint64_t button = live(tabs->buttons[i]);
        const uint64_t animation = member(button, i == (mods ? tabs->buttons.count - 1 : tabs->selected) ? t::name::ui_mod_list_tab_selected_member : t::name::ui_mod_list_tab_idle_member);
        if (animation) t::fn::ui_mod_list_play_animation(reinterpret_cast<void*>(button), reinterpret_cast<void*>(animation), 0, 1, 0, 1, false);
    }
    if (!tabs->retired) update_game_footer(*tabs);
}
bool tab_load_attempted = false;
bool first_bar_told = false;
List<uint64_t> candidates;
bool inventoried = false;
void startup_trace(const char* what, uint64_t began) {
    if (!cost_clock) return;
    const uint64_t now = cost_now();
    char at[21], elapsed[21], line[160];
    const auto digits = [](uint64_t value, char (&out)[21]) {
        char* end = out + sizeof out; *--end = 0;
        do { *--end = char('0' + value % 10); value /= 10; } while (value);
        return end;
    };
    const char* parts[] = {what, " at_us=", digits(now, at), " cost_us=", digits(now - began, elapsed)};
    size_t count = 0;
    for (const char* part : parts) {
        const size_t bytes = strlen(part);
        if (count + bytes >= sizeof line) return;
        memcpy(line + count, part, bytes); count += bytes;
    }
    line[count] = 0; mod_note(kFrameworkLogId, line);
}
TSUCHI_Result prepare_tab_class() {
    if (!on_game_thread() || !mod_usable(framework_mod) || !ready) return TSUCHI_NOT_READY;
    if (cached_tab_class()) return TSUCHI_OK;
    // A failed preparation must not cause an object-table scan on every menu
    // step or construction. The table and stock asset path do not change.
    if (tab_load_attempted) return TSUCHI_NOT_READY;
    tab_load_attempted = true;
    const uint64_t began = cost_now();
    uint64_t kind = game::find_class(tab_class);
    if (!kind && t::bound(t::fn::name_from_text) && t::bound(t::fn::load_soft_object)) {
        struct Soft { int32_t index, serial; uint32_t tag, padding; uint64_t asset, subpath; int32_t count, capacity; } soft{-1, 0, 0, 0, 0, 0, 0, 0};
        char16_t path[256]; size_t units = 0;
        if (game::utf8_to_units(t::asset::ui_mod_list_tab_class, path, 255, units)) {
            path[units] = 0;
            soft.asset = t::fn::name_from_text(path, units, 1);
            kind = reinterpret_cast<uint64_t>(t::fn::load_soft_object(&soft));
        }
    }
    if (!game::live(kind) || !game::is_class(kind) || !game::name_is(game::name_of(kind), t::name::ui_mod_list_tab_class) || !pin(kind, 0, 0)) {
        tab_class.found = tab_class.handle = 0;
        startup_trace("Settings tab class unavailable", began);
        return TSUCHI_NOT_READY;
    }
    // LoadSynchronous returned the class itself; no second inventory is needed.
    tab_class.found = kind; tab_class.handle = game::to_handle(kind);
    startup_trace("Settings tab class prepared", began);
    return TSUCHI_OK;
}
void tell_first_bar(uint64_t began) {
    if (first_bar_told) return;
    first_bar_told = true;
    startup_trace("Settings tabs first ready", began);
}
bool layout_host(uint64_t object) {
    if (!game::live(object) || !game::is_a(object, host_class) ||
        (game::read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags) ||
        game::read_or<uint8_t>(object + t::ui_host::state) == 4 || !game::read_or<uint8_t>(object + t::ui_mod_list_menu::new_mode)) return false;
    const uint64_t tree = child(object, t::ui_host::tree), root = child(tree, t::ui_tree::root), scroll = child(object, t::ui_host::scroll);
    const uint64_t slot = child(scroll, t::ui_mod_list_tab::slot);
    const Array source = game::read_or<Array>(object + t::ui_mod_list_menu::source);
    return root && scroll && slot && game::is_a(root, canvas_class) && game::is_a(slot, slot_class) &&
           source.count > 0 && source.capacity >= source.count && source.count <= 256 && game::readable(source.data, uint64_t(source.count) * 8);
}
bool active_host(uint64_t object) {
    return layout_host(object) && game::read_or<uint8_t>(object + t::ui_host::state) == 1;
}
void remember(uint64_t object) {
    const uint64_t handle = game::to_handle(object);
    if (!handle || (game::read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags)) return;
    for (uint32_t i = 0; i < candidates.count; ++i) if (candidates[i] == handle) return;
    candidates.add(handle);
}
bool inventory(void*, uint64_t object) {
    if (game::name_is(game::name_of(game::class_of(object)), t::name::ui_host_class)) remember(object);
    return true;
}
uint32_t current_tab(uint64_t host, uint32_t count) {
    State* state = state_for(host);
    if (state && usable(*state)) return count - 1;
    if (host != cached_host || !active_host(live(host))) return 0;
    const UiScreen* screen = ui_active_screen();
    if (!screen) return 0;
    for (uint32_t i = 0; i + 2 < count; ++i) {
        const UiScreen* tab = ui_settings_tab(i);
        if (tab && tab->id == screen->id && tab->mod == screen->mod) return i + 1;
    }
    return 0;
}
struct TabReplacement;
TabReplacement* replacing_tabs = nullptr;
struct TabReplacement {
    Tabs& before;
    bool announced;
    TabReplacement* outer;
    TabReplacement(Tabs& value, bool told) : before(value), announced(told), outer(replacing_tabs) { replacing_tabs = this; }
    ~TabReplacement() { replacing_tabs = outer; }
};
bool replacement_announced(uint64_t host) {
    for (TabReplacement* at = replacing_tabs; at; at = at->outer)
        if (at->before.host == host && tabs_associated(at->before)) return at->announced;
    return false;
}
void replacement_lost(uint64_t host) {
    for (TabReplacement* at = replacing_tabs; at; at = at->outer)
        if (at->before.host == host) at->announced = false;
}
void prepare_host(uint64_t object, const char* path, bool opening = false) {
    const uint64_t host = game::to_handle(object);
    if (!host || !cached_tab_class() || !layout_host(object)) return;
    bool announced = replacement_announced(host);
    const TabKept* before = bars.find(framework_mod, host, 0);
    if (before && before->tabs && before->tabs->revision != ui_settings_tab_revision()) {
        Tabs* old = before->tabs; RetainTabs retained(old);
        announced = old->announced;
        TabReplacement replacement(*old, announced);
        bars.release(framework_mod, host, 0);
        announced = replacement_announced(host);
        // Native restoration can close/reconstruct the menu and install a
        // successor. Never retire that successor or reuse the old layout.
        if (!mod_usable(framework_mod) || !ready || live(host) != object || !layout_host(object) ||
            bars.find(framework_mod, host, 0)) return;
    }
    if (!bars.find(framework_mod, host, 0)) {
        const uint64_t began = cost_now();
        if (bars.take(framework_mod, host, 0, {}) != TSUCHI_OK) return;
        const TabKept* made = bars.find(framework_mod, host, 0);
        Tabs* tabs = made ? made->tabs : nullptr;
        if (!tabs) return;
        RetainTabs retained(tabs);
        const auto current = [&] { return tabs_current(*tabs) && tabs_associated(*tabs) &&
            tabs->revision == ui_settings_tab_revision() && live(host) == object && layout_host(object); };
        if (!current()) return;
        tabs->announced = announced;
        tabs->opening_until = opening && cost_clock ? cost_now() + 2000000 : 0;
        tabs->selected = current_tab(host, tabs->buttons.count);
        tell_first_bar(began);
        if (!current()) return;
        startup_trace(path, began);
        if (!current()) return;
        tab_highlight(host, tabs->selected + 1 == tabs->buttons.count);
    }
    const TabKept* kept = bars.find(framework_mod, host, 0);
    if (!kept || !kept->tabs || kept->tabs->revision != ui_settings_tab_revision() || live(host) != object || !active_host(object)) return;
    Tabs* tabs = kept->tabs; RetainTabs retained(tabs);
    cached_host = host;
    if (!tabs->announced) {
        tabs->announced = true;
        if (callbacks.host_ready) callbacks.host_ready(host);
    }
}
void discover() {
    if (!mod_usable(framework_mod) || !ready) return;
    // Once for menus constructed before this module registered its hooks.
    // Later instances arrive through NativeConstruct, including inactive ones.
    if (!inventoried) { inventoried = true; game::each_object(inventory, nullptr); }
    bool layout = false;
    cached_host = 0;
    for (uint32_t i = candidates.count; i--;) {
        const uint64_t object = live(candidates[i]);
        if (!object) { candidates[i] = candidates[--candidates.count]; continue; }
        layout = layout || layout_host(object);
        if (!cached_host && active_host(object)) cached_host = candidates[i];
    }
    if (!layout) return;
    for (uint32_t i = 0; i < bars.count(); ++i) {
        const auto& kept = bars.kept_at(i);
        if (kept.tabs && kept.tabs->selected && !ui_active_screen() && !state_for(bars.hold_at(i).object))
            tab_highlight(bars.hold_at(i).object, false);
    }
    if (prepare_tab_class() != TSUCHI_OK) return;
    for (uint32_t i = 0; i < candidates.count; ++i) {
        const uint64_t object = live(candidates[i]);
        prepare_host(object, "Settings tabs installed via discovery");
    }
    for (uint32_t i = 0; i < hosts.count(); ++i) {
        State* state = hosts.kept_at(i).state;
        if (state) { Retain retained(state); update_footer(*state); if (usable(*state)) fit_labels(*state); }
    }
    for (uint32_t i = 0; i < bars.count(); ++i) {
        Tabs* tabs = bars.kept_at(i).tabs;
        if (tabs) update_game_footer(*tabs);
    }
}

void construct(void* pointer) {
    const uint64_t host = game::to_handle(reinterpret_cast<uint64_t>(pointer));
    if (on_game_thread() && host) replacement_lost(host);
    if (on_game_thread() && host) { ui_native_host_detach(host); ui_host_lost(host); }
    if (on_game_thread() && host) { if (state_for(host)) hosts.release(framework_mod, host, 0); bars.release(framework_mod, host, 0); }
    next_construct(pointer);
    if (on_game_thread()) remember(reinterpret_cast<uint64_t>(pointer));
    if (on_game_thread() && mod_usable(framework_mod)) {
        // Class preparation runs at safe startup or the post-construction step,
        // never recursively inside the stock NativeConstruct chain.
        // Layout is constructed while stock input state is still zero. Waiting
        // for state one here would defer visible headers to a later world step.
        prepare_host(reinterpret_cast<uint64_t>(pointer), "Settings tabs installed via construct", true);
    }
}
void reopen(void* pointer) {
    const Reopen original = reinterpret_cast<Reopen>(next_reopen);
    const uint64_t host = game::to_handle(reinterpret_cast<uint64_t>(pointer));
    if (on_game_thread() && host) replacement_lost(host);
    if (on_game_thread() && host) {
        ui_native_host_detach(host); ui_host_lost(host);
        if (state_for(host)) hosts.release(framework_mod, host, 0);
        bars.release(framework_mod, host, 0);
        if (cached_host == host) cached_host = 0;
    }
    original(pointer);
    if (on_game_thread() && mod_usable(framework_mod)) {
        remember(reinterpret_cast<uint64_t>(pointer));
        prepare_host(reinterpret_cast<uint64_t>(pointer), "Settings tabs installed via reopen", true);
    }
}
void destruct(void* pointer) {
    const uint64_t host = game::to_handle(reinterpret_cast<uint64_t>(pointer));
    if (on_game_thread() && host) replacement_lost(host);
    if (on_game_thread() && host) { ui_native_host_detach(host); ui_host_lost(host); }
    if (cached_host == host) cached_host = 0;
    if (on_game_thread() && host) { if (state_for(host)) hosts.release(framework_mod, host, 0); bars.release(framework_mod, host, 0); }
    if (on_game_thread() && host && callbacks.host_lost) callbacks.host_lost(host);
    next_destruct(pointer);
}
}
void ui_mod_list_native_callbacks(const UiModListNativeCallbacks* value) { callbacks = value ? *value : UiModListNativeCallbacks{}; }
void ui_mod_list_native_start(TSUCHI_Mod* framework) {
    static bool listening = false;
    framework_mod = framework; hosts.start(); widgets.start(); bars.start();
    if (!listening) listening = each_engine_frame(discover);
    uint64_t id = 0;
    const Refresh wrapper = refresh;
    const Reopen reopen_wrapper = reopen;
    ready = code_site_patched(t::code_site::ui_settings_refresh.number) &&
        code_site_patched(t::code_site::ui_settings_reopen.number) &&
        hook_code_site(framework, t::code_site::ui_settings_refresh.number, reinterpret_cast<void*>(wrapper), &id, &next_refresh) == TSUCHI_OK &&
        hook_code_site(framework, t::code_site::ui_settings_reopen.number, reinterpret_cast<void*>(reopen_wrapper), &id, &next_reopen) == TSUCHI_OK &&
        hook_slot(framework, t::slot::ui_mod_list_destruct, destruct, &next_destruct, kHookNeedsStock, &id) == TSUCHI_OK &&
        hook_slot(framework, t::slot::ui_mod_list_construct, construct, &next_construct, kHookNeedsStock, &id) == TSUCHI_OK;
}
TSUCHI_Result ui_mod_list_native_prepare() { return prepare_tab_class(); }
uint64_t ui_mod_list_native_host() {
    if (!on_game_thread() || !mod_usable(framework_mod) || !ready) return 0;
    return active_host(live(cached_host)) ? cached_host : 0;
}
uint64_t ui_mod_list_native_group() {
    return on_game_thread() && mod_usable(framework_mod) ? next_scope++ : 0;
}
bool ui_mod_list_native_pin(uint64_t host, uint64_t group, uint64_t object) {
    return on_game_thread() && mod_usable(framework_mod) && group && live(host) && pin(object, host, group);
}
void ui_mod_list_native_unpin(uint64_t host, uint64_t group) {
    if (on_game_thread() && mod_usable(framework_mod) && group) release_widgets(host, group);
}
bool ui_mod_list_native_borrow(uint64_t host, uint64_t source, uint64_t replacement) {
    if (!on_game_thread() || !mod_usable(framework_mod) || !replacement) return false;
    State* state = state_for(host);
    if (!state) return true;
    if (!usable(*state) || state->source_data != source || state->replacement_source) return false;
    state->replacement_source = replacement;
    ++state->calls;
    state->next_loan = source_loans; source_loans = state;
    return true;
}
void ui_mod_list_native_return_source(uint64_t host, uint64_t replacement) {
    if (!on_game_thread()) return;
    for (State** at = &source_loans; *at; at = &(*at)->next_loan) {
        State* state = *at;
        if (state->host != host || state->replacement_source != replacement) continue;
        *at = state->next_loan; state->next_loan = nullptr;
        state->replacement_source = 0;
        if (state->retired) cleanup_source(*state);
        finish(state);
        return;
    }
}
bool ui_mod_list_native_content(uint64_t host, UiContentRect& out) {
    if (!on_game_thread() || !mod_usable(framework_mod) || !active_host(live(host))) return false;
    const uint64_t scroll = child(live(host), t::ui_host::scroll), slot = child(scroll, t::ui_mod_list_tab::slot);
    const TabKept* kept = bars.find(framework_mod, host, 0);
    if (!slot || !kept || !kept->tabs || kept->tabs->retired || !game::is_a(slot, slot_class)) return false;
    Margin bounds{};
    if (!game::read(slot + t::ui_mod_list_tab::offsets, bounds) ||
        !(bounds.left > -10000 && bounds.left < 10000 && bounds.top > -10000 && bounds.top < 10000 &&
          bounds.width > 0 && bounds.width <= 10000 && bounds.height > 0 && bounds.height <= 10000)) return false;
    out = {bounds.left, bounds.top, bounds.width, bounds.height};
    return true;
}
void ui_mod_list_native_footer_release(uint64_t host) {
    if (!on_game_thread() || !mod_usable(framework_mod)) return;
    const TabKept* kept = bars.find(framework_mod, host, 0);
    if (kept && kept->tabs) {
        Tabs* tabs = kept->tabs; RetainTabs retained(tabs);
        ui_footer_release(tabs->footer);
    }
    if (State* state = state_for(host)) { Retain retained(state); ui_footer_release(state->footer); }
}
TSUCHI_Result ui_mod_list_native_hold(uint64_t host) {
    if (!mod_usable(framework_mod)) return TSUCHI_INVALID;
    if (!on_game_thread() || !ready || !active_host(live(host))) return TSUCHI_NOT_READY;
    if (!bars.find(framework_mod, host, 0)) { const auto result = bars.take(framework_mod, host, 0, {}); if (result != TSUCHI_OK) return result; }
    ui_mod_list_native_footer_release(host);
    return hosts.take(framework_mod, host, 0, {});
}
TSUCHI_Result ui_mod_list_native_release(uint64_t host) {
    if (!mod_usable(framework_mod)) return TSUCHI_INVALID;
    return hosts.release(framework_mod, host, 0);
}
TSUCHI_Result ui_mod_list_native_cycle(uint64_t host, bool previous) {
    const TabKept* kept = bars.find(framework_mod, host, 0);
    Tabs* candidate = kept ? kept->tabs : nullptr;
    const bool ready = candidate && candidate->revision == ui_settings_tab_revision() && active_host(live(host));
    TabCycleTrace trace{cost_now(), ++tab_cycle_attempt, host, candidate ? candidate->selected : UINT32_MAX,
        UINT32_MAX, previous, ready};
    uint32_t& budget = candidate ? candidate->cycle_traces : tab_unbound_cycle_traces;
    const bool tracing = tab_cycle_traces < 48 && budget < 12;
    if (tracing) { ++budget; ++tab_cycle_traces; }
    const auto done = [&](TSUCHI_Result result) {
        if (tracing) tab_cycle_trace(trace, cost_now(), result);
        return result;
    };
    if (!ready) {
        if (candidate) { RetainTabs retained(candidate); return done(TSUCHI_NOT_READY); }
        return done(TSUCHI_NOT_READY);
    }
    Tabs* tabs = kept->tabs; RetainTabs retained(tabs);
    const uint32_t count = tabs->buttons.count;
    const uint32_t next = (tabs->selected + (previous ? count - 1 : 1)) % count;
    trace.next = next;
    ui_mod_list_native_footer_release(host);
    ui_settings_tab_close();
    if (state_for(host)) hosts.release(framework_mod, host, 0);
    if (tabs->retired) return done(TSUCHI_NOT_READY);
    TSUCHI_Result result = TSUCHI_OK;
    if (next + 1 == count) result = hosts.take(framework_mod, host, 0, {});
    else if (next) result = ui_settings_tab_open(next - 1);
    if (result == TSUCHI_OK && !tabs->retired) {
        tabs->selected = next; tab_highlight(host, next + 1 == count);
        if (!tabs->retired && next < tabs->buttons.count) ui_feedback_focus(tabs->buttons[next]);
    }
    return done(result);
}
bool ui_mod_list_native_active(uint64_t host) { State* state = state_for(host); return state && usable(*state); }
TSUCHI_Result ui_mod_list_native_selected(uint64_t host, uint32_t& index) {
    if (!mod_usable(framework_mod)) return TSUCHI_INVALID;
    State* state = state_for(host); if (!state || !usable(*state)) return TSUCHI_NOT_FOUND;
    const int32_t selected = game::read_or<int32_t>(live(host) + t::ui_mod_list_menu::selected, -1);
    if (selected < 0 || uint32_t(selected) >= state->rows.count) return TSUCHI_NOT_FOUND;
    index = uint32_t(selected); return TSUCHI_OK;
}
TSUCHI_Result ui_mod_list_native_sync(uint64_t host) {
    if (!mod_usable(framework_mod)) return TSUCHI_INVALID;
    State* state = state_for(host); if (!state || !usable(*state)) return TSUCHI_NOT_FOUND;
    Retain retained(state);
    for (uint32_t i = 0; i < state->rows.count && usable(*state); ++i) {
        UiModListRow input{};
        if (!callbacks.row || !callbacks.row(i, input) || !usable(*state) || !input.id || strcmp(input.id, state->rows[i].id)) return TSUCHI_NOT_READY;
        Row row = state->rows[i];
        if (!copy(row.name, sizeof row.name, input.name) || !copy(row.status, sizeof row.status, input.status ? input.status : "")) return TSUCHI_INVALID;
        row.requested = input.requested;
        row.has_settings = input.has_settings;
        const uint64_t widget = live(row.widget), selector = live(row.selector);
        if (!widget || !selector) return TSUCHI_NOT_READY;
        if (strcmp(row.name, state->rows[i].name) || strcmp(row.status, state->rows[i].status)) {
            char text[641]; const uint64_t title = child(widget, t::ui_mod_list_row::title);
            if (!caption(row, text) || !label(title, text) || !usable(*state)) return TSUCHI_NOT_READY;
        }
        if (game::read_or<uint32_t>(selector + t::ui_mod_list_selector::choice) != uint32_t(input.requested)) {
            if (!set_choice(widget, selector, input.requested, {[](void* pointer) { return usable(*static_cast<State*>(pointer)); }, state}) || !usable(*state)) return TSUCHI_NOT_READY;
        }
        state->rows[i] = row;
    }
    if (usable(*state)) update_footer(*state);
    return usable(*state) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
TSUCHI_Result ui_mod_list_native_focus(uint64_t host, uint32_t index) {
    if (!mod_usable(framework_mod)) return TSUCHI_INVALID;
    State* state = state_for(host); if (!state || !usable(*state)) return TSUCHI_NOT_FOUND;
    if (index >= state->rows.count) return TSUCHI_NOT_FOUND;
    Retain retained(state);
    const uint64_t menu = live(host);
    if (!game::write<int32_t>(menu + t::ui_mod_list_menu::previous, game::read_or<int32_t>(menu + t::ui_mod_list_menu::selected)) ||
        !game::write<int32_t>(menu + t::ui_mod_list_menu::selected, int32_t(index))) return TSUCHI_NOT_READY;
    t::fn::ui_mod_list_focus(reinterpret_cast<void*>(menu));
    if (usable(*state)) update_footer(*state);
    return usable(*state) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
bool ui_mod_list_native_dispatch(uint64_t host, uint32_t action, bool& handled, bool& result) {
    handled = false; result = false;
    if (!mod_usable(framework_mod)) return false;
    State* state = state_for(host); if (!state || !usable(*state)) return false;
    if (action < 2) return true;
    Retain retained(state);
    uint32_t index = 0; if (ui_mod_list_native_selected(host, index) != TSUCHI_OK) return false;
    handled = true;
    bool requested = state->rows[index].requested;
    if (action == 2 || action == 3) {
        if (!t::bound(t::fn::ui_mod_list_adjust) || !t::fn::ui_mod_list_adjust(reinterpret_cast<void*>(live(host)), action == 3 ? 1 : 0, int32_t(index)) || !usable(*state)) return true;
        const uint64_t selector = live(state->rows[index].selector);
        const uint32_t selected = selector ? game::read_or<uint32_t>(selector + t::ui_mod_list_selector::choice, UINT32_MAX) : UINT32_MAX;
        if (selected > 1) return true;
        requested = selected == 1;
    }
    // A settings/details view must save the stock guide, not this Mods lease.
    if (action == 4 || action == 102) ui_footer_release(state->footer);
    result = callbacks.action && callbacks.action(host, index, action, requested);
    if (usable(*state)) ui_mod_list_native_sync(host);
    return true;
}
}
