#include "fake_game.h"
#include "ui_mod_list_native.h"
#include "ui_screens.h"
#include "hooks.h"
#include "holds.h"
#include "objects.h"
#include "shared.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include "cost_clock.h"
#include "ui_members.h"
#include <cassert>
#include <cmath>
#include <array>
#include <map>
#include <string>
#include <vector>
using namespace tsuchi;
namespace t = game::table;
namespace {
struct Array { uint64_t data; int32_t count, capacity; };
uint64_t root_class, host_type, category_type, row_type, label_type, panel_type, canvas_type, slot_type, tab_type, host, host_handle, methods, scroll_slot;
struct Margin { float left, top, width, height; };
std::map<uint64_t, uint32_t> visibility;
unsigned animations = 0, transformed = 0, scrollbar_updates = 0;
uint64_t scroll_object, scroll_panel, root_panel;
std::map<uint64_t, std::vector<uint64_t>> panel_slots;
unsigned detached_tabs = 0;
uint64_t old_category, old_data;
TSUCHI_Mod* framework;
void (*construct_hook)(void*) = nullptr;
void (*destruct_hook)(void*) = nullptr;
void (*refresh_hook)(void*) = nullptr;
void (*reopen_hook)(void*) = nullptr;
unsigned refreshed = 0, focused = 0, constructed = 0, destructed = 0, ready_calls = 0, lost_calls = 0, action_calls = 0;
unsigned reopened = 0, world_frames = 0;
unsigned fail_create = 0, creates = 0;
bool requested[8]{};
bool pending_status = false;
uint64_t thread = 1;
std::vector<void (*)()> steps;
std::vector<void (*)()> engine_frames;
unsigned tab_loads = 0, tab_names = 0;
bool load_fails = false;
bool eager_prepare = false, reentered_prepare = false;
std::vector<std::string> startup_traces;
std::vector<std::string> cycle_traces;
void (*cycle_note_callback)() = nullptr;
struct Cover {
    uint64_t host = 0;
    Array before{}, source{};
    bool active = false, deferred = false;
} cover;
bool reconstruct_covered = false;
unsigned cover_loss_calls = 0;
void close_cover();
uint32_t custom_tabs = 0, tab_revision = 1;
bool custom_open = false;
TSUCHI_Result custom_open_result = TSUCHI_OK;
UiScreen video_screen{};
UiScreen earlier_screen{};
const UiScreen* custom_screen = &video_screen;
std::vector<uint64_t> released;
std::map<uint64_t, std::u16string> labels;
std::map<uint64_t, std::vector<std::u16string>> choices;
std::u16string temporary;
uint64_t footer_label = 0;
unsigned footer_lookups = 0, footer_sets = 0;
void (*refresh_callback)() = nullptr;
void (*row_callback)() = nullptr;
uint64_t scrollbar_widget = 0, scrollbar_slot = 0, unknown_host = 0;
bool refresh_mode = false, replace_layout_on_construct = false;
bool fit_mode = false;
std::map<uint64_t, uint64_t> cached_geometry;
struct LabelWrite { uint64_t label; char kind; };
std::vector<LabelWrite> label_writes;
void (*label_setter_callback)() = nullptr;
char label_callback_at = 'p';
struct RefreshGeometry { uint64_t target; Margin content; float bar_bottom; uint8_t clipping; };
std::vector<RefreshGeometry> refresh_entries, refresh_exits;
unsigned unknown_refreshes = 0;
bool near(float value, float expected) { return std::fabs(value - expected) < 0.002f; }
uint64_t cycle_field(const std::string& line, const char* field) {
    const std::string key = std::string(" ") + field + "=";
    size_t at = line.find(key); assert(at != std::string::npos); at += key.size();
    assert(at < line.size() && line[at] >= '0' && line[at] <= '9');
    uint64_t value = 0;
    for (; at < line.size() && line[at] != ' '; ++at) {
        assert(line[at] >= '0' && line[at] <= '9');
        value = value * 10 + uint64_t(line[at] - '0');
    }
    return value;
}
void cycle_record(size_t index, uint64_t attempt, bool previous, bool ready,
                  uint32_t old_tab, uint32_t next_tab, TSUCHI_Result result) {
    assert(index < cycle_traces.size());
    const std::string& line = cycle_traces[index];
    assert(line.find("Settings tab cycle at_us=") == 0);
    assert(cycle_field(line, "attempt") == attempt && cycle_field(line, "host") == host_handle);
    assert(cycle_field(line, "previous") == uint64_t(previous) && cycle_field(line, "ready") == uint64_t(ready));
    assert(cycle_field(line, "old") == old_tab && cycle_field(line, "next") == next_tab);
    assert(cycle_field(line, "result") == uint64_t(result));
    const uint64_t started = cycle_field(line, "at_us"), completed = cycle_field(line, "completed_us");
    assert(completed >= started && cycle_field(line, "cost_us") == completed - started);
    if (started) assert(completed > started); // This fixture's enabled clock advances on every read.
}
uint64_t type(const char* name) {
    std::u16string wide; while (*name) wide.push_back(char16_t(*name++)); return fake::object(wide, root_class, 0x100);
}
uint64_t object(uint64_t type_id, const char* name = "widget", uint64_t outer = 0) {
    std::u16string wide; while (*name) wide.push_back(char16_t(*name++));
    const uint64_t made = fake::object(wide, type_id, 0x900); fake::put<uint64_t>(made, methods); fake::put<uint64_t>(made + t::object::outer, outer);
    fake::put<float>(made + t::ui_widget::render_scale_x, 1.0f);
    fake::put<float>(made + t::ui_widget::render_scale_y, 1.0f); return made;
}
uint64_t child(uint64_t object_id, uint32_t offset) { return fake::get<uint64_t>(object_id + offset); }
void* add_child(void*, void*);
void* create_widget(void* pointer, void* kind, void* name) {
    assert(reinterpret_cast<uint64_t>(pointer) == host && !name);
    if (++creates == fail_create) return nullptr;
    const uint64_t type_id = reinterpret_cast<uint64_t>(kind);
    const uint64_t made = object(type_id);
    if (type_id == category_type) {
        const uint64_t tree = object(root_class);
        fake::put<uint64_t>(made + t::ui_host::tree, tree);
        // Like CreateWidget in game: the tree exists, native members are unbound.
        // Observed template names; keep these independent of the lookup table.
        const uint64_t panel = object(panel_type, "N_Vrt_Headline", tree);
        const uint64_t title = object(label_type, "F_HeadlineS", tree);
        fake::put<uint64_t>(tree + t::ui_tree::root, panel);
        add_child(reinterpret_cast<void*>(panel), reinterpret_cast<void*>(title));
        fake::put<uint64_t>(made + t::ui_mod_list_category::row_class, row_type);
    } else if (type_id == tab_type) {
        fake::put<uint64_t>(made + ui_members::tab_label, object(label_type));
        const uint64_t line = object(root_class), line_slot = object(slot_type), panel = object(canvas_type), panel_slot = object(slot_type);
        fake::put<uint64_t>(made + ui_members::tab_underline, line);
        fake::put<uint64_t>(line + t::ui_mod_list_tab::slot, line_slot);
        fake::put<uint64_t>(line_slot + t::ui_mod_list_tab::parent, panel);
        fake::put<uint64_t>(panel + t::ui_mod_list_tab::slot, panel_slot);
        fake::put<uint64_t>(made + ui_members::tab_selected, object(root_class));
        fake::put<uint64_t>(made + ui_members::tab_idle, object(root_class));
    } else assert(type_id == row_type);
    return reinterpret_cast<void*>(made);
}
void bind_row(void* pointer) {
    const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    const uint64_t selector = object(root_class);
    fake::put<uint64_t>(widget + t::ui_mod_list_row::title, object(label_type));
    fake::put<uint64_t>(widget + t::ui_mod_list_row::selector, selector);
}
game::EngineText make_text(const std::u16string& value) {
    const uint64_t data = fake::memory((value.size() + 1) * 2), string = fake::memory(t::array::size);
    memcpy(reinterpret_cast<void*>(data), value.c_str(), (value.size() + 1) * 2);
    fake::put<uint64_t>(string + t::array::data, data);
    fake::put<int32_t>(string + t::array::count, int32_t(value.size() + 1));
    fake::put<int32_t>(string + t::array::max, int32_t(value.size() + 1));
    game::EngineText text;
    memcpy(text.bytes, &string, 8);
    return text;
}
std::u16string string_at(const void* pointer) {
    const uint64_t at = reinterpret_cast<uint64_t>(pointer);
    const uint64_t data = fake::get<uint64_t>(at + t::array::data);
    const int32_t count = fake::get<int32_t>(at + t::array::count);
    return {reinterpret_cast<const char16_t*>(data), size_t(count - 1)};
}
game::EngineText text_from_string(const void* pointer) { temporary = string_at(pointer); return make_text(temporary); }
void* text_string(void* pointer) { uint64_t value = 0; memcpy(&value, pointer, 8); return reinterpret_cast<void*>(value); }
game::EngineText footer_lookup(const void* space, const void* key, uint32_t flags, uint32_t fallback, uint32_t) {
    assert(string_at(space) == u"FooterGuide" && !flags && !fallback); ++footer_lookups;
    return make_text(u"localized " + string_at(key));
}
void label(void* pointer, void* value) {
    const uint64_t object_id = reinterpret_cast<uint64_t>(pointer);
    labels[object_id] = string_at(text_string(value));
    fake::put(object_id + t::ui_footer_label::text, *static_cast<game::EngineText*>(value));
    if (object_id == footer_label) ++footer_sets;
}
void footer_caption(const char16_t* caption) { auto text = make_text(caption); label(reinterpret_cast<void*>(footer_label), &text); }
void append_choice(void* pointer, void*) { choices[child(reinterpret_cast<uint64_t>(pointer), t::ui_mod_list_row::selector)].push_back(temporary); }
bool valid(void* pointer, int32_t choice) { return choice >= 0 && uint32_t(choice) < choices[reinterpret_cast<uint64_t>(pointer)].size(); }
void redraw(void* pointer) { assert(valid(pointer, int32_t(fake::get<uint32_t>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_selector::choice)))); }
void set_visible(void* pointer, uint32_t value) { visibility[reinterpret_cast<uint64_t>(pointer)] = value; }
void set_clipping(void* pointer, uint32_t value) { fake::put<uint8_t>(reinterpret_cast<uint64_t>(pointer) + t::ui_widget::clipping, uint8_t(value)); }
void offsets(void* pointer, float left, float top, float width, float height) { fake::put<Margin>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_tab::offsets, {left, top, width, height}); }
void transform(void* pointer) {
    const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    if (cached_geometry.count(widget)) { label_writes.push_back({widget, 't'}); return; }
    assert(widget == scroll_panel); ++transformed;
}
void label_setter_return(char kind) {
    if (!label_setter_callback || kind != label_callback_at) return;
    const auto callback = label_setter_callback; label_setter_callback = nullptr; callback();
}
void scale_widget(float x, float y, void* pointer) {
    const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    label_writes.push_back({widget, 's'});
    fake::put<float>(widget + t::ui_widget::render_scale_x, x);
    fake::put<float>(widget + t::ui_widget::render_scale_y, y);
    label_setter_return('s');
}
void pivot_widget(float x, float y, void* pointer) {
    const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    label_writes.push_back({widget, 'p'});
    fake::put<float>(widget + t::ui_widget::render_pivot_x, x);
    fake::put<float>(widget + t::ui_widget::render_pivot_y, y);
    label_setter_return('p');
}
void* geometry(void* pointer) {
    const auto at = cached_geometry.find(reinterpret_cast<uint64_t>(pointer));
    return at == cached_geometry.end() ? nullptr : reinterpret_cast<void*>(at->second);
}
void arrangement(uint64_t widget, float width, float height, float scale_x, float scale_y, float left, float top) {
    uint64_t& at = cached_geometry[widget]; if (!at) at = fake::memory(13 * sizeof(float));
    const std::array<float, 13> value{width, height, 0, 0, 0, 0, 0, scale_x, 0, 0, scale_y, left, top};
    memcpy(reinterpret_cast<void*>(at), value.data(), sizeof value);
}
void scrollbar(void* pointer) { assert(reinterpret_cast<uint64_t>(pointer) == scroll_object); ++scrollbar_updates; }
void* play(void*, void*, float, int32_t, uint32_t, float, bool) { ++animations; return nullptr; }
void slots_for(uint64_t parent) {
    const auto& list = panel_slots[parent]; const uint64_t data = list.empty() ? 0 : fake::memory(list.size() * 8);
    for (size_t i = 0; i < list.size(); ++i) fake::put<uint64_t>(data + i * 8, list[i]);
    fake::put<Array>(parent + t::ui_panel::slots, {data, int32_t(list.size()), int32_t(list.size())});
}
void* add_child(void* pointer, void* child_pointer) {
    const uint64_t parent = reinterpret_cast<uint64_t>(pointer), widget = reinterpret_cast<uint64_t>(child_pointer), slot = object(slot_type);
    panel_slots[parent].push_back(slot); slots_for(parent);
    fake::put<uint64_t>(slot + t::ui_panel_slot::content, widget); fake::put<uint64_t>(slot + t::ui_mod_list_tab::parent, parent);
    fake::put<uint64_t>(widget + t::ui_mod_list_tab::slot, slot); return reinterpret_cast<void*>(slot);
}
bool remove_child_at(void* pointer, int32_t index) {
    const uint64_t parent = reinterpret_cast<uint64_t>(pointer); auto& list = panel_slots[parent];
    assert(index >= 0 && uint32_t(index) < list.size());
    const uint64_t widget = child(list[size_t(index)], t::ui_panel_slot::content);
    fake::put<uint64_t>(widget + t::ui_mod_list_tab::slot, 0); list.erase(list.begin() + index); slots_for(parent);
    if (parent == root_panel) ++detached_tabs;
    return true;
}
void install_scrollbar(uint64_t scroll) {
    const uint64_t tree = object(root_class), root = object(canvas_type, "ScrollRoot", tree);
    fake::put<uint64_t>(scroll + t::ui_host::tree, tree);
    fake::put<uint64_t>(tree + t::ui_tree::root, root);
    scrollbar_widget = object(root_class, "PA_ScrollBar", tree);
    scrollbar_slot = reinterpret_cast<uint64_t>(add_child(reinterpret_cast<void*>(root), reinterpret_cast<void*>(scrollbar_widget)));
    fake::put<Margin>(scrollbar_slot + t::ui_mod_list_tab::anchors, {0, 0, 1, 1});
    fake::put<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets, {2, 3, 18, 11});
}
RefreshGeometry refresh_geometry(uint64_t target) {
    const uint64_t scroll = child(target, t::ui_host::scroll), slot = child(scroll, t::ui_mod_list_tab::slot);
    return {target, fake::get<Margin>(slot + t::ui_mod_list_tab::offsets),
        target == host ? fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height : 0,
        fake::get<uint8_t>(scroll + t::ui_widget::clipping)};
}
void original_refresh(void* pointer) {
    const uint64_t target = reinterpret_cast<uint64_t>(pointer);
    if (refresh_mode) refresh_entries.push_back(refresh_geometry(target));
    if (target != host) {
        assert(target == unknown_host); ++unknown_refreshes;
        if (refresh_mode) refresh_exits.push_back(refresh_geometry(target));
        return;
    }
    ++refreshed;
    const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    assert(source.count == 1 && source.data != 0);
    const uint64_t category = fake::get<uint64_t>(source.data);
    assert(game::live(category));
    if (category != old_category) assert(fake::get<int32_t>(category + t::ui_mod_list_category::span) == 8);
    // Model the native range cache consuming the slot during Refresh. This is
    // deliberately the value observed at the original entry, not the expected
    // adapter height computed by the test.
    if (refresh_mode) fake::put<float>(scroll_object + t::ui_mod_list_scroll::viewport, refresh_entries.back().content.height);
    if (refresh_callback) { const auto callback = refresh_callback; refresh_callback = nullptr; callback(); }
    if (refresh_mode) refresh_exits.push_back(refresh_geometry(target));
}
void refresh(void* pointer) { assert(refresh_hook); refresh_hook(pointer); }
void focus(void* pointer) {
    assert(reinterpret_cast<uint64_t>(pointer) == host);
    assert(!custom_open || video_screen.selected != kUiNoSelection);
    ++focused;
}
uint32_t adjust(void* pointer, uint32_t increment, int32_t row_index) {
    assert(reinterpret_cast<uint64_t>(pointer) == host && row_index >= 0 && row_index < 8);
    const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    const uint64_t category = fake::get<uint64_t>(source.data);
    const Array rows = fake::get<Array>(category + t::ui_mod_list_category::rows);
    const uint64_t row = fake::get<uint64_t>(rows.data + uint64_t(row_index) * 8), selector = child(row, t::ui_mod_list_row::selector);
    fake::put<uint32_t>(selector + t::ui_mod_list_selector::choice, increment ? 1 : 0);
    fake::put<uint32_t>(row + t::ui_mod_list_row::shadow, increment ? 1 : 0); return 1;
}
uint32_t count() { return 8; }
bool row(uint32_t index, UiModListRow& out) {
    static const char* ids[] = {"mod.a", "mod.b", "mod.c", "mod.d", "mod.e", "mod.f", "mod.g", "mod.h"};
    if (row_callback) { const auto callback = row_callback; row_callback = nullptr; callback(); }
    out = {ids[index], fit_mode && !index ? "A very long installed mod caption crossing the native clipping boundary" : ids[index], requested[index], pending_status ? "Restart pending" : "", index == 0}; return true;
}
bool action(uint64_t handle, uint32_t index, uint32_t which, bool choice) { assert(handle == host_handle); ++action_calls; if (which == 2 || which == 3) requested[index] = choice; return true; }
void host_ready(uint64_t handle) { assert(handle == host_handle); ++ready_calls; }
void host_lost(uint64_t handle) { assert(handle == host_handle); ++lost_calls; }
void original_construct(void*) {
    ++constructed;
    if (replace_layout_on_construct) {
        scroll_object = object(root_class); scroll_slot = object(slot_type);
        fake::put<uint64_t>(host + t::ui_host::scroll, scroll_object);
        fake::put<uint64_t>(scroll_object + t::ui_mod_list_tab::slot, scroll_slot);
        fake::put<Margin>(scroll_slot + t::ui_mod_list_tab::offsets, {30, 40, 1468, 900});
        fake::put<uint8_t>(scroll_object + t::ui_widget::clipping, 4);
        install_scrollbar(scroll_object);
    }
    if (reconstruct_covered) {
        assert(cover.active);
        fake::put<Array>(host + t::ui_mod_list_menu::source, {old_data, 1, 1});
    }
}
void original_destruct(void*) {
    const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    if (cover.active) {
        assert(cover.deferred && source.data == cover.source.data && game::readable(cover.before.data, 8));
        assert(game::live(fake::get<uint64_t>(source.data)));
    } else assert(source.data == old_data);
    ++destructed;
}
void original_reopen(void* pointer) {
    assert(reinterpret_cast<uint64_t>(pointer) == host); ++reopened;
    // The retained host must surrender our headers and geometry before the
    // stock rebuild reads its own tree. Reopen does not call NativeConstruct.
    assert(panel_slots[root_panel].empty());
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    const Margin stock = fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
    assert(stock.left == 10 && stock.top == 20 && stock.width == 1468 && stock.height == 868);
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 0);
    assert(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height == 11);
    fake::put<uint8_t>(host + t::ui_host::state, 0);
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2);
    fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
}
void stop() { mod_stop(framework); }
void stop_from_cycle_note() {
    // The trace is delivered after the successful Mods source installation.
    assert(ui_mod_list_native_active(host_handle));
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data != old_data);
    stop();
    assert(!ui_mod_list_native_active(host_handle) && panel_slots[root_panel].empty());
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    assert(!game::object_kept(host));
}
void reopen() { assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK); }
void engine_frame() { for (auto work : engine_frames) work(); }
void frame() { ++world_frames; for (auto step : steps) step(); engine_frame(); }
void nested_refresh() {
    refresh(reinterpret_cast<void*>(host));
    assert(near(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height, 788));
    assert(near(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height, 11));
}
void nested_stop() {
    refresh_callback = stop;
    refresh(reinterpret_cast<void*>(host));
    assert(near(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height, 868));
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 3);
}
void reconstruct_layout() {
    replace_layout_on_construct = true;
    construct_hook(reinterpret_cast<void*>(host));
    replace_layout_on_construct = false;
}
void release_fitting_list() { assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK); }
void reconstruct_fitting_list() { construct_hook(reinterpret_cast<void*>(host)); assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK); }
uint64_t tab_name(void* pointer, uint64_t count, uint64_t flag) {
    assert(std::u16string(static_cast<char16_t*>(pointer), count) == u"/Game/Carrot4/UI/InGame/Tablet/Parts/WBP_ui_Tablet_PA_Tab_Item.WBP_ui_Tablet_PA_Tab_Item_C");
    assert(flag == 1); ++tab_names; return 0xC1FFEE;
}
void* load_tab(void* pointer) {
    struct Soft { int32_t index, serial; uint32_t tag, padding; uint64_t asset, subpath; int32_t count, capacity; };
    static_assert(sizeof(Soft) == 40);
    const auto& soft = *static_cast<Soft*>(pointer);
    assert(soft.index == -1 && !soft.serial && !soft.tag && !soft.padding && soft.asset == 0xC1FFEE && !soft.subpath && !soft.count && !soft.capacity);
    assert((eager_prepare || fake::get<uint8_t>(host + t::ui_host::state) == 1) && !(fake::get<uint32_t>(host + t::object::flags) & t::constant::template_flags));
    if (eager_prepare) {
        assert(ui_mod_list_native_prepare() == TSUCHI_NOT_READY);
        reentered_prepare = true;
    }
    ++tab_loads; if (load_fails) return nullptr;
    tab_type = type(t::name::ui_mod_list_tab_class); ui_members::tab(tab_type); return reinterpret_cast<void*>(tab_type);
}
struct Borrowed {
    Array source, rows;
    uint64_t category;
    std::vector<uint64_t> widgets;
    size_t releases;
};
Borrowed begin_borrow(bool deferred) {
    assert(!cover.active && fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    Borrowed borrowed{};
    borrowed.source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    borrowed.category = fake::get<uint64_t>(borrowed.source.data);
    borrowed.rows = fake::get<Array>(borrowed.category + t::ui_mod_list_category::rows);
    assert(borrowed.source.count == 1 && borrowed.rows.count == 8);
    for (int32_t i = 0; i < borrowed.rows.count; ++i) borrowed.widgets.push_back(fake::get<uint64_t>(borrowed.rows.data + uint64_t(i) * 8));
    borrowed.releases = released.size();
    const uint64_t replacement_category = object(category_type, "generated settings");
    fake::put<int32_t>(replacement_category + t::ui_mod_list_category::span, 8);
    cover = {host_handle, borrowed.source, {fake::memory(8), 1, 1}, true, deferred};
    fake::put<uint64_t>(cover.source.data, replacement_category);
    assert(!ui_mod_list_native_borrow(host_handle, borrowed.source.data + 8, cover.source.data));
    assert(ui_mod_list_native_borrow(host_handle, borrowed.source.data, cover.source.data));
    assert(!ui_mod_list_native_borrow(host_handle, borrowed.source.data, cover.source.data));
    fake::put<Array>(host + t::ui_mod_list_menu::source, cover.source);
    custom_open = true;
    return borrowed;
}
void loan_alive(const Borrowed& borrowed, size_t other_releases = 0) {
    assert(released.size() == borrowed.releases + other_releases);
    assert(game::readable(borrowed.source.data, 8) && game::readable(borrowed.rows.data, uint64_t(borrowed.rows.count) * 8));
    assert(fake::get<uint64_t>(borrowed.source.data) == borrowed.category);
    assert(fake::get<Array>(borrowed.category + t::ui_mod_list_category::rows).data == borrowed.rows.data);
    assert(game::object_kept(borrowed.category) && game::object_kept(old_category));
    for (const uint64_t widget : borrowed.widgets) assert(game::object_kept(widget));
}
void loan_cleaned(const Borrowed& borrowed, size_t other_releases = 0, bool stock_kept = false) {
    assert(released.size() == borrowed.releases + other_releases + 2);
    assert(!game::readable(borrowed.source.data, 8) && !game::readable(borrowed.rows.data, 8));
    assert(!game::object_kept(borrowed.category) && game::object_kept(old_category) == stock_kept);
    assert(!fake::get<Array>(borrowed.category + t::ui_mod_list_category::rows).data);
    assert(!fake::get<uint64_t>(borrowed.category + t::ui_mod_list_category::owner));
    for (const uint64_t widget : borrowed.widgets) assert(!game::object_kept(widget));
}
void close_cover() {
    assert(cover.active);
    const Cover closing = cover;
    cover.active = false; custom_open = false;
    const uint64_t owner = game::from_handle(closing.host);
    if (owner && fake::get<Array>(owner + t::ui_mod_list_menu::source).data == closing.source.data) {
        fake::put<Array>(owner + t::ui_mod_list_menu::source, closing.before);
        // The View returns only after stock refresh/focus have consumed the
        // restored source. The raw native arrays must still be live here.
        refresh(reinterpret_cast<void*>(owner)); focus(reinterpret_cast<void*>(owner));
        assert(game::readable(closing.before.data, 8));
    }
    ui_mod_list_native_return_source(closing.host, closing.source.data);
    fake::unmap(closing.source.data);
}
void borrow_scenarios() {
    // A generated page may cover the real Mods source across maintenance steps.
    const auto normal = begin_borrow(false);
    for (unsigned i = 0; i < 5; ++i) frame();
    assert(ui_mod_list_native_active(host_handle)); loan_alive(normal);
    const uint64_t normal_replacement = cover.source.data;
    ui_mod_list_native_return_source(host_handle, normal_replacement + 8);
    loan_alive(normal);
    close_cover();
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == normal.source.data);
    loan_alive(normal);
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK); loan_cleaned(normal);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);

    // The immediate host-loss callback restores and returns before release ends.
    const auto immediate = begin_borrow(false);
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK);
    assert(!cover.active && !ui_mod_list_native_active(host_handle)); loan_cleaned(immediate);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);

    // An update on the stack can defer the View close. Retiring the Mods hold
    // must retain both native arrays and every category/row root until return.
    const auto deferred = begin_borrow(true);
    const unsigned before_loss = cover_loss_calls;
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK && cover_loss_calls == before_loss + 1);
    assert(cover.active && !ui_mod_list_native_active(host_handle)); loan_alive(deferred);
    thread = 2; ui_mod_list_native_return_source(host_handle, cover.source.data); thread = 1;
    ui_mod_list_native_return_source(host_handle, cover.source.data + 8);
    loan_alive(deferred);
    frame(); loan_alive(deferred);
    close_cover(); loan_cleaned(deferred);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);

    const auto destructed_loan = begin_borrow(true);
    const unsigned before_destruct = destructed;
    destruct_hook(reinterpret_cast<void*>(host));
    assert(destructed == before_destruct + 1 && cover.active && !ui_mod_list_native_active(host_handle));
    loan_alive(destructed_loan);
    close_cover(); loan_cleaned(destructed_loan);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);

    // Reconstruction can install a new stock source before the old View gets
    // to close; neither its close nor the returned loan may overwrite it.
    const auto reconstructed = begin_borrow(true);
    reconstruct_covered = true; construct_hook(reinterpret_cast<void*>(host)); reconstruct_covered = false;
    assert(cover.active && !ui_mod_list_native_active(host_handle)); loan_alive(reconstructed);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 42);
    close_cover(); loan_cleaned(reconstructed);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 42);

    // Returning after native identity death still frees the retained tail,
    // without writing through the same address reused for a new generation.
    const auto dead = begin_borrow(true);
    const Cover dead_cover = cover;
    const uint64_t dead_handle = host_handle;
    const int32_t host_index = fake::get<int32_t>(host + t::object::index);
    fake::destroy(host); frame();
    assert(!game::from_handle(dead_handle) && cover.active && !ui_mod_list_native_active(dead_handle)); loan_alive(dead);
    fake::place(host, host_index); host_handle = game::to_handle(host);
    assert(host_handle != dead_handle);
    fake::put<Array>(host + t::ui_mod_list_menu::source, {old_data, 1, 1});
    cover.active = false; custom_open = false;
    const auto revived = begin_borrow(true);
    const Cover revived_cover = cover;
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 77);
    const unsigned before_refresh = refreshed, before_focus = focused;
    ui_mod_list_native_return_source(host_handle, dead_cover.source.data);
    ui_mod_list_native_return_source(dead_handle, revived_cover.source.data);
    loan_alive(revived);
    // Generation one's loan is now a non-head ledger entry. Its return must
    // leave generation two's outstanding source, selection and roots alone.
    cover = dead_cover; close_cover(); loan_cleaned(dead, 0, true);
    cover = revived_cover; custom_open = true;
    loan_alive(revived, 2);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == revived_cover.source.data);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 77 && refreshed == before_refresh && focused == before_focus);
    ui_mod_list_native_return_source(dead_handle, dead_cover.source.data); // duplicate return is inert
    loan_alive(revived, 2);
    close_cover(); loan_alive(revived, 2);
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK); loan_cleaned(revived, 2);
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
}
void refresh_scenarios() {
    assert(refresh_hook && ui_mod_list_native_prepare() == TSUCHI_OK);
    fake::put<float>(host + t::ui_widget::render_scale_y, .75f);
    fake::put<uint8_t>(scroll_object + t::ui_widget::clipping, 3);
    fake::put<uint8_t>(host + t::ui_host::state, 1);
    construct_hook(reinterpret_cast<void*>(host));
    auto displayed = [](float height, float bottom) {
        const Margin content = fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
        assert(near(content.height, height));
        assert(near(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height, bottom));
        assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 1);
    };
    displayed(1050.666667f, 273.666667f);
    UiContentRect rect{};
    assert(ui_mod_list_native_content(host_handle, rect));
    assert(rect.left == 10 && rect.top == 100 && rect.width == 1468 && near(rect.height, 1050.666667f));
    const unsigned stable_creates = creates;
    // These calls enter through the captured code-site wrapper, just as a
    // stock Game reset or Refresh call does. No maintenance step repairs it.
    for (unsigned repeat = 0; repeat < 4; ++repeat) {
        const size_t entries = refresh_entries.size(), exits = refresh_exits.size();
        refresh(reinterpret_cast<void*>(host));
        assert(refresh_entries.size() == entries + 1 && refresh_exits.size() == exits + 1);
        assert(near(refresh_entries.back().content.height, 788) && near(refresh_exits.back().content.height, 788));
        assert(near(refresh_entries.back().bar_bottom, 11) && near(refresh_exits.back().bar_bottom, 11));
        assert(near(fake::get<float>(scroll_object + t::ui_mod_list_scroll::viewport), 788));
        displayed(1050.666667f, 273.666667f);
        assert(creates == stable_creates);
    }
    const size_t nested_entries = refresh_entries.size(), nested_exits = refresh_exits.size();
    refresh_callback = nested_refresh;
    refresh(reinterpret_cast<void*>(host));
    assert(refresh_entries.size() == nested_entries + 2 && refresh_exits.size() == nested_exits + 2);
    for (size_t i = nested_entries; i < refresh_entries.size(); ++i) {
        assert(near(refresh_entries[i].content.height, 788) && near(refresh_entries[i].bar_bottom, 11));
        assert(refresh_entries[i].clipping == 1);
    }
    for (size_t i = nested_exits; i < refresh_exits.size(); ++i)
        assert(near(refresh_exits[i].content.height, 788) && near(refresh_exits[i].bar_bottom, 11));
    displayed(1050.666667f, 273.666667f);

    // An unrelated receiver reaches the original once, without adopting its
    // layout or changing even its clipping property.
    unknown_host = object(root_class);
    const uint64_t unknown_scroll = object(root_class), unknown_slot = object(slot_type);
    fake::put<uint64_t>(unknown_host + t::ui_host::scroll, unknown_scroll);
    fake::put<uint64_t>(unknown_scroll + t::ui_mod_list_tab::slot, unknown_slot);
    const Margin unknown_before{7, 8, 1000, 432};
    fake::put<Margin>(unknown_slot + t::ui_mod_list_tab::offsets, unknown_before);
    fake::put<float>(unknown_host + t::ui_widget::render_scale_y, .75f);
    fake::put<uint8_t>(unknown_scroll + t::ui_widget::clipping, 2);
    refresh(reinterpret_cast<void*>(unknown_host));
    const Margin unknown_after = fake::get<Margin>(unknown_slot + t::ui_mod_list_tab::offsets);
    assert(unknown_refreshes == 1 && !memcmp(&unknown_before, &unknown_after, sizeof unknown_before));
    assert(fake::get<uint8_t>(unknown_scroll + t::ui_widget::clipping) == 2);
    displayed(1050.666667f, 273.666667f);

    // Native Refresh still rebuilds range geometry while an all-message form
    // is active, but no native row may receive focus. Retiring and reapplying
    // the tab lease exercises both restoration and installation boundaries.
    UiControl message{}; message.kind = UiKind::text;
    assert(video_screen.controls.add(message));
    video_screen.selected = kUiNoSelection; custom_open = true;
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, -1);
    const unsigned before_message_focus = focused, before_message_refresh = refreshed;
    ++tab_revision; frame();
    assert(custom_open && focused == before_message_focus && refreshed > before_message_refresh);
    assert(panel_slots[root_panel].size() == 2);
    displayed(1050.666667f, 273.666667f);
    custom_open = false; video_screen.controls.clear(); video_screen.selected = 0;
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2);

    // Retirement inside the original must restore the baseline and prevent
    // the outer wrapper from reinstalling display values after mod stop.
    refresh_callback = nested_stop;
    refresh(reinterpret_cast<void*>(host));
    assert(panel_slots[root_panel].empty());
    assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).top == 20);
    assert(near(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height, 868));
    assert(near(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height, 11));
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 3);

    framework = mod_create("framework.refresh-reconstruct"); ui_mod_list_native_start(framework);
    construct_hook(reinterpret_cast<void*>(host));
    const uint64_t old_scroll = scroll_object, old_slot = scroll_slot, old_bar_slot = scrollbar_slot;
    refresh_callback = reconstruct_layout;
    refresh(reinterpret_cast<void*>(host));
    assert(scroll_object != old_scroll && scroll_slot != old_slot && panel_slots[root_panel].size() == 2);
    assert(near(fake::get<Margin>(old_slot + t::ui_mod_list_tab::offsets).height, 868));
    assert(near(fake::get<Margin>(old_bar_slot + t::ui_mod_list_tab::offsets).height, 11));
    assert(fake::get<uint8_t>(old_scroll + t::ui_widget::clipping) == 3);
    displayed(1093.333333f, 284.333333f);
    assert(ui_mod_list_native_content(host_handle, rect) && rect.top == 120 && near(rect.height, 1093.333333f));
    mod_stop(framework);
    assert(panel_slots[root_panel].empty());
    assert(near(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height, 900));
    assert(near(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height, 11));
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 4);

    // Cleanup owns only values it installed. Another writer's clipping and
    // slot margins must survive rather than being replaced with old baselines.
    framework = mod_create("framework.refresh-foreign"); ui_mod_list_native_start(framework);
    construct_hook(reinterpret_cast<void*>(host));
    fake::put<uint8_t>(scroll_object + t::ui_widget::clipping, 2);
    fake::put<Margin>(scroll_slot + t::ui_mod_list_tab::offsets, {31, 41, 1469, 1234});
    fake::put<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets, {2, 3, 18, 321});
    mod_stop(framework);
    assert(panel_slots[root_panel].empty());
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 2);
    assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height == 1234);
    assert(fake::get<Margin>(scrollbar_slot + t::ui_mod_list_tab::offsets).height == 321);
}
struct FitLabels { uint64_t heading, row; };
FitLabels fitting_labels() {
    const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    const uint64_t category = fake::get<uint64_t>(source.data);
    const Array rows = fake::get<Array>(category + t::ui_mod_list_category::rows);
    assert(rows.count == 8);
    return {child(category, t::ui_mod_list_category::title), child(fake::get<uint64_t>(rows.data), t::ui_mod_list_row::title)};
}
void arrange_labels(FitLabels value) {
    arrangement(scroll_object, 1000, 788, .75f, .75f, 100, 200);
    arrangement(value.heading, 400, 64, .75f, .75f, 20, 220);
    arrangement(value.row, 800, 64, .6f, .675f, 20, 300);
    fake::put<float>(value.heading + t::ui_widget::render_x, 5);
    fake::put<float>(value.heading + t::ui_widget::render_pivot_x, .5f);
    fake::put<float>(value.row + t::ui_widget::render_x, 10);
    fake::put<float>(value.row + t::ui_widget::render_scale_x, .8f);
    fake::put<float>(value.row + t::ui_widget::render_scale_y, .9f);
    fake::put<float>(value.row + t::ui_widget::render_pivot_x, .25f);
    fake::put<float>(value.row + t::ui_widget::render_pivot_y, .4f);
}
void fit_scenarios() {
    assert(ui_mod_list_native_prepare() == TSUCHI_OK);
    fake::put<uint8_t>(host + t::ui_host::state, 1);
    construct_hook(reinterpret_cast<void*>(host));
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    const FitLabels fitted = fitting_labels(); arrange_labels(fitted);
    frame();
    assert(labels[fitted.row] == u"A very long installed mod caption crossing the native clipping boundary");
    assert(label_writes.size() == 6);
    assert(fake::get<float>(fitted.row + t::ui_widget::render_pivot_x) == 1);
    assert(near(fake::get<float>(fitted.row + t::ui_widget::render_scale_x), .636666667f));
    assert(near(fake::get<float>(fitted.row + t::ui_widget::render_x), -110));
    assert(near(fake::get<float>(fitted.row + t::ui_widget::render_scale_y), .9f));
    assert(near(fake::get<float>(fitted.row + t::ui_widget::render_pivot_y), .4f));
    // Independent rendered edge checks use the fixture's known parent origin
    // and native pivot transform. The original right edge is 500 pixels.
    const auto row_edges = [&](float wanted_left) {
        const float scale = fake::get<float>(fitted.row + t::ui_widget::render_scale_x);
        const float x = fake::get<float>(fitted.row + t::ui_widget::render_x);
        const float pivot = fake::get<float>(fitted.row + t::ui_widget::render_pivot_x);
        const float left = -17.5f + .75f * (x + (1 - scale) * pivot * 800);
        assert(near(left, wanted_left) && near(left + .75f * scale * 800, 500));
    };
    row_edges(118);
    assert(fake::get<float>(fitted.heading + t::ui_widget::render_pivot_x) == 0);
    assert(near(fake::get<float>(fitted.heading + t::ui_widget::render_x), 135.666667f));
    assert(near(20 + .75f * (fake::get<float>(fitted.heading + t::ui_widget::render_x) - 5), 118));
    assert(118 + 400 * .75f * fake::get<float>(fitted.heading + t::ui_widget::render_scale_x) <= 832);

    // Cached Slate geometry deliberately stays stale after the native setters.
    // Neither vertical scrolling nor a changed clip alone permits compounding.
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        arrangement(scroll_object, 1000, 788, .75f, .75f, 150, 200 + float(repeat));
        frame();
        assert(label_writes.size() == 6);
    }
    arrangement(scroll_object, 1000, 788, .75f, .75f, 100, 200);
    arrangement(fitted.heading, 400, 64, .75f, .75f, 118, 220);
    arrangement(fitted.row, 800, 64, .4775f, .675f, 118, 300);
    frame(); frame();
    assert(label_writes.size() == 6); row_edges(118);
    // A later arranged clip change may now trigger a fresh fit. The caption
    // follows the actual left boundary while retaining its same right edge.
    arrangement(scroll_object, 1000, 788, .75f, .75f, 250, 200);
    frame();
    assert(label_writes.size() == 12); row_edges(268);
    assert(near(fake::get<float>(fitted.heading + t::ui_widget::render_x), 335.666667f));

    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK);
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    const FitLabels released_fit = fitting_labels(); arrange_labels(released_fit);
    const size_t before_release = label_writes.size();
    label_setter_callback = release_fitting_list;
    frame();
    assert(!ui_mod_list_native_active(host_handle) && label_writes.size() == before_release + 1);
    assert(label_writes.back().label == released_fit.heading && label_writes.back().kind == 'p');
    assert(fake::get<float>(released_fit.heading + t::ui_widget::render_x) == 5);
    assert(fake::get<float>(released_fit.row + t::ui_widget::render_pivot_x) == .25f);

    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    const FitLabels rebuilt_fit = fitting_labels(); arrange_labels(rebuilt_fit);
    const size_t before_reconstruct = label_writes.size();
    label_callback_at = 's';
    label_setter_callback = reconstruct_fitting_list;
    frame();
    assert(ui_mod_list_native_active(host_handle) && label_writes.size() == before_reconstruct + 2);
    assert(label_writes.back().label == rebuilt_fit.heading && label_writes.back().kind == 's');
    assert(fake::get<float>(rebuilt_fit.heading + t::ui_widget::render_x) == 5);
    assert(fake::get<float>(rebuilt_fit.row + t::ui_widget::render_pivot_x) == .25f);
    const FitLabels current = fitting_labels();
    assert(current.heading != rebuilt_fit.heading && current.row != rebuilt_fit.row);
    arrange_labels(current); frame();
    assert(label_writes.size() == before_reconstruct + 8);
    mod_stop(framework);
    assert(panel_slots[root_panel].empty());
}
void construct_before_activation() {
    const unsigned before_construct = constructed, before_ready = ready_calls;
    const unsigned before_focus = focused, before_actions = action_calls;
    const Array before_source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    // A real Settings NativeConstruct may finish with bound rows and state0.
    // A prepared tab class is not permission to focus or accept input yet.
    fake::put<uint8_t>(host + t::ui_host::state, 0);
    construct_hook(reinterpret_cast<void*>(host));
    assert(constructed == before_construct + 1 && ready_calls == before_ready);
    assert(focused == before_focus && action_calls == before_actions);
    assert(panel_slots[root_panel].size() == 2 && game::object_kept(host));
    bool game_header = false, mods_header = false;
    for (const uint64_t slot : panel_slots[root_panel]) {
        const uint64_t widget = child(slot, t::ui_panel_slot::content);
        const auto& caption = labels[child(widget, ui_members::tab_label)];
        game_header |= caption == u"Game"; mods_header |= caption == u"Mods";
    }
    assert(game_header && mods_header);
    const Margin prepared = fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
    assert(prepared.top == 100 && prepared.height == 788);
    assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 1);
    assert(!ui_mod_list_native_host());
    UiContentRect unavailable{1, 2, 3, 4};
    assert(!ui_mod_list_native_content(host_handle, unavailable));
    assert(unavailable.left == 1 && unavailable.top == 2 && unavailable.width == 3 && unavailable.height == 4);
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_NOT_READY);
    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_NOT_READY);
    assert(ui_mod_list_native_focus(host_handle, 0) == TSUCHI_NOT_FOUND);
    bool handled = false, result = false;
    assert(!ui_mod_list_native_dispatch(host_handle, 4, handled, result));
    const Array after_source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    assert(!memcmp(&before_source, &after_source, sizeof before_source));
    assert(focused == before_focus && action_calls == before_actions);
}
void engine_only_tab_revisions() {
    assert(!world_frames && !custom_tabs && !custom_open);
    const unsigned announced = ready_calls;
    const Array stock_source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    const uint64_t foreign = object(root_class), foreign_slot = reinterpret_cast<uint64_t>(
        add_child(reinterpret_cast<void*>(root_panel), reinterpret_cast<void*>(foreign)));
    const Margin foreign_offsets{7, 9, 311, 61};
    fake::put(foreign_slot + t::ui_mod_list_tab::offsets, foreign_offsets);
    const auto headers = [&](std::initializer_list<const char16_t*> captions) {
        std::vector<uint64_t> result;
        auto caption = captions.begin();
        assert(panel_slots[root_panel].size() == captions.size() + 1);
        for (const uint64_t slot : panel_slots[root_panel]) {
            if (slot == foreign_slot) continue;
            const uint64_t widget = child(slot, t::ui_panel_slot::content);
            assert(caption != captions.end() && game::class_of(widget) == tab_type);
            assert(child(widget, t::ui_mod_list_tab::slot) == slot && child(slot, t::ui_mod_list_tab::parent) == root_panel);
            assert(labels[child(widget, ui_members::tab_label)] == *caption++ && game::object_kept(widget));
            result.push_back(widget);
        }
        assert(caption == captions.end() && child(foreign, t::ui_mod_list_tab::slot) == foreign_slot);
        const Margin current = fake::get<Margin>(foreign_slot + t::ui_mod_list_tab::offsets);
        assert(!memcmp(&current, &foreign_offsets, sizeof current));
        return result;
    };
    const auto retired = [](const std::vector<uint64_t>& widgets) {
        for (const uint64_t widget : widgets)
            assert(!child(widget, t::ui_mod_list_tab::slot) && !game::object_kept(widget));
    };
    const auto stable = [&] {
        const unsigned before_creates = creates, before_refresh = refreshed, before_focus = focused;
        const unsigned before_ready = ready_calls, before_animation = animations, before_footer = footer_sets;
        const auto slots = panel_slots[root_panel];
        const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
        for (unsigned i = 0; i < 3; ++i) engine_frame();
        const Array after = fake::get<Array>(host + t::ui_mod_list_menu::source);
        assert(!world_frames && creates == before_creates && refreshed == before_refresh && focused == before_focus);
        assert(ready_calls == before_ready && animations == before_animation && footer_sets == before_footer);
        assert(panel_slots[root_panel] == slots && !memcmp(&source, &after, sizeof source));
    };
    const auto baseline = [&] {
        const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
        assert(!memcmp(&source, &stock_source, sizeof source));
    };
    auto previous = headers({u"Game", u"Mods"});
    video_screen.id = 71; video_screen.settings_tab = true; assert(video_screen.title.set("Video"));
    earlier_screen.id = 72; earlier_screen.settings_tab = true; assert(earlier_screen.title.set("Earlier"));
    unsigned before_creates = creates;
    custom_tabs = 1; ++tab_revision; engine_frame();
    assert(creates == before_creates + 3 && ready_calls == announced && !world_frames);
    retired(previous); previous = headers({u"Game", u"Video", u"Mods"}); baseline(); stable();

    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_OK && custom_open && custom_screen == &video_screen);
    before_creates = creates;
    custom_tabs = 2; ++tab_revision;
    // A declaration change after maintenance must not dispatch using stale
    // header indices while waiting for the next engine-only repair.
    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_NOT_READY);
    assert(custom_open && custom_screen == &video_screen && creates == before_creates); baseline();
    engine_frame();
    assert(creates == before_creates + 4 && ready_calls == announced && custom_open && custom_screen == &video_screen);
    retired(previous); previous = headers({u"Game", u"Earlier", u"Video", u"Mods"}); baseline(); stable();
    // Video moved from header1 to header2. The next page must be Mods.
    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_OK && !custom_open && ui_mod_list_native_active(host_handle));
    assert(cycle_field(cycle_traces.back(), "old") == 2 && cycle_field(cycle_traces.back(), "next") == 3);
    assert(ui_mod_list_native_cycle(host_handle, true) == TSUCHI_OK && custom_open && custom_screen == &video_screen);
    before_creates = creates;
    custom_tabs = 1; ++tab_revision; engine_frame();
    assert(creates == before_creates + 3 && ready_calls == announced && custom_open && custom_screen == &video_screen);
    retired(previous); previous = headers({u"Game", u"Video", u"Mods"}); baseline(); stable();
    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_OK && !custom_open && ui_mod_list_native_active(host_handle));
    assert(cycle_field(cycle_traces.back(), "old") == 1 && cycle_field(cycle_traces.back(), "next") == 2);
    const Array mods_source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    const uint64_t mods_category = fake::get<uint64_t>(mods_source.data);
    before_creates = creates;
    custom_tabs = 0; ++tab_revision; engine_frame();
    const Array after_removal = fake::get<Array>(host + t::ui_mod_list_menu::source);
    assert(creates == before_creates + 2 && ready_calls == announced && ui_mod_list_native_active(host_handle));
    assert(!memcmp(&mods_source, &after_removal, sizeof mods_source) && game::object_kept(mods_category));
    retired(previous); previous = headers({u"Game", u"Mods"}); stable();
    assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_OK && !ui_mod_list_native_active(host_handle));
    assert(cycle_field(cycle_traces.back(), "old") == 1 && cycle_field(cycle_traces.back(), "next") == 0); baseline();

    // Nested discovery may repair this same native lifecycle from inside the
    // restoring Refresh. It inherits the announcement and owns the new bar.
    before_creates = creates; custom_tabs = 1; ++tab_revision;
    refresh_callback = engine_frame;
    engine_frame();
    assert(!refresh_callback && creates == before_creates + 3 && ready_calls == announced);
    retired(previous); previous = headers({u"Game", u"Video", u"Mods"}); baseline(); stable();
    custom_tabs = 0; ++tab_revision; engine_frame();
    assert(ready_calls == announced); retired(previous); previous = headers({u"Game", u"Mods"}); baseline(); stable();

    // NativeConstruct can install a successor during the old bar's restoring
    // Refresh. The outer revision repair must leave that successor untouched.
    const unsigned before_construct = constructed, before_ready = ready_calls;
    before_creates = creates; custom_tabs = 1; ++tab_revision;
    refresh_callback = [] { construct_hook(reinterpret_cast<void*>(host)); };
    engine_frame();
    assert(!refresh_callback && constructed == before_construct + 1 && ready_calls == before_ready + 1);
    assert(creates == before_creates + 3 && ui_mod_list_native_host() == host_handle && !world_frames);
    retired(previous); previous = headers({u"Game", u"Video", u"Mods"}); baseline(); stable();
    custom_tabs = 0; ++tab_revision; engine_frame();
    assert(ready_calls == before_ready + 1); retired(previous); headers({u"Game", u"Mods"}); baseline(); stable();
    for (size_t i = 0; i < panel_slots[root_panel].size(); ++i)
        if (panel_slots[root_panel][i] == foreign_slot) { remove_child_at(reinterpret_cast<void*>(root_panel), int32_t(i)); break; }
    assert(!child(foreign, t::ui_mod_list_tab::slot) && panel_slots[root_panel].size() == 2);
    video_screen.title.clear(); video_screen.id = 0; video_screen.settings_tab = false;
    earlier_screen.title.clear();
}
}
namespace tsuchi {
void ui_native_host_detach(uint64_t) {}
void mod_note(const char*, const char* text) {
    if (!strncmp(text, "Settings tab cycle ", sizeof("Settings tab cycle ") - 1)) {
        cycle_traces.emplace_back(text);
        if (cycle_note_callback) {
            const auto callback = cycle_note_callback; cycle_note_callback = nullptr; callback();
        }
    } else startup_traces.emplace_back(text);
}
void ui_host_lost(uint64_t handle) {
    if (!cover.active || cover.host != handle) return;
    ++cover_loss_calls;
    if (!cover.deferred) close_cover();
}
uint32_t ui_settings_tab_count() { return custom_tabs; }
uint32_t ui_settings_tab_revision() { return tab_revision; }
const UiScreen* ui_settings_tab(uint32_t index) {
    if (custom_tabs == 2) return index == 0 ? &earlier_screen : index == 1 ? &video_screen : nullptr;
    return custom_tabs && !index ? &video_screen : nullptr;
}
const UiScreen* ui_active_screen() { return custom_open ? custom_screen : nullptr; }
void ui_settings_tab_close() { custom_open=false; }
TSUCHI_Result ui_settings_tab_open(uint32_t index) {
    const UiScreen* screen = ui_settings_tab(index);
    if (!screen) return TSUCHI_NOT_FOUND;
    if (custom_open_result != TSUCHI_OK) return custom_open_result;
    custom_open=true; custom_screen=screen; return TSUCHI_OK;
}

bool each_frame(void (*step)()) { steps.push_back(step); return true; }
bool each_engine_frame(void (*work)()) { engine_frames.push_back(work); return true; }
bool on_actor_left(void (*)(uint64_t,uint64_t,ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle, uint64_t) { return handle; }
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** next) {
    assert(flags == kHookNeedsStock);
    if (entry == fake::main_image() + t::slot::ui_mod_list_construct.table + t::slot::ui_mod_list_construct.offset) {
        assert(stock == fake::main_image() + t::slot::ui_mod_list_construct.stock); construct_hook = reinterpret_cast<void (*)(void*)>(function); *next = reinterpret_cast<void*>(original_construct);
    } else {
        assert(entry == fake::main_image() + t::slot::ui_mod_list_destruct.table + t::slot::ui_mod_list_destruct.offset && stock == fake::main_image() + t::slot::ui_mod_list_destruct.stock);
        destruct_hook = reinterpret_cast<void (*)(void*)>(function); *next = reinterpret_cast<void*>(original_destruct);
    }
    *id = 1; return TSUCHI_OK;
}
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t site, void* function, uint64_t* id, void** next) {
    assert(function && id && next);
    if (site == t::code_site::ui_settings_refresh.number) {
        refresh_hook = reinterpret_cast<void (*)(void*)>(function);
        *next = reinterpret_cast<void*>(original_refresh); *id = 3;
    } else {
        assert(site == t::code_site::ui_settings_reopen.number);
        reopen_hook = reinterpret_cast<void (*)(void*)>(function);
        *next = reinterpret_cast<void*>(original_reopen); *id = 4;
    }
    return TSUCHI_OK;
}
bool code_site_patched(uint32_t site) { assert(site == t::code_site::ui_settings_refresh.number || site == t::code_site::ui_settings_reopen.number); return true; }
}
namespace tsuchi::game {
uint64_t part_owner(uint64_t handle) { return handle; }
bool is_game_actor(uint64_t) { return true; }
void* engine_memory(uint64_t bytes) { return reinterpret_cast<void*>(fake::memory(size_t(bytes))); }
void release_engine_memory(void* pointer) { const uint64_t address = reinterpret_cast<uint64_t>(pointer); released.push_back(address); fake::unmap(address); }
void drop_shared(uint64_t counter) { assert(!counter); }
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    count = std::strlen(text); if (count > capacity) return false; for (size_t i = 0; i < count; ++i) out[i] = char16_t(text[i]); return true;
}
}
int main(int argc, char** argv) {
    const bool borrow_mode = argc > 1 && argv[1][0] == 'b';
    refresh_mode = argc > 1 && argv[1][0] == 'r';
    fit_mode = argc > 1 && !strcmp(argv[1], "fit");
    const bool probe_load = argc > 1 && !borrow_mode && !refresh_mode && !fit_mode; load_fails = probe_load && argv[1][0] == 'f';
    eager_prepare = probe_load && argv[1][0] == 'p';
    fake::start(); set_thread_probe([] { return thread; }); note_game_thread();
    root_class = fake::object(u"Class", 0, 0x100); methods = fake::memory(0x800);
    fake::put<uint64_t>(methods + t::ui_mod_list_selector::valid, reinterpret_cast<uint64_t>(valid));
    fake::put<uint64_t>(methods + t::ui_mod_list_selector::redraw, reinterpret_cast<uint64_t>(redraw));
    fake::put<uint64_t>(methods + t::widget_table::set_visibility, reinterpret_cast<uint64_t>(set_visible));
    fake::code().push_back(reinterpret_cast<uint64_t>(set_visible));
    fake::code().push_back(reinterpret_cast<uint64_t>(valid)); fake::code().push_back(reinterpret_cast<uint64_t>(redraw));
    category_type = type(t::name::ui_mod_list_category_class); row_type = type(t::name::ui_mod_list_row_class);
    label_type = type(t::name::ui_mod_list_label_class); panel_type = type(t::name::ui_mod_list_panel_class);
    fake::put<uint64_t>(panel_type + t::struct_type::super, type(t::name::ui_panel_base_class));
    canvas_type = type(t::name::ui_canvas_class); slot_type = type(t::name::ui_mod_list_slot_class); if (!probe_load) { tab_type = type(t::name::ui_mod_list_tab_class); ui_members::tab(tab_type); }
    fake::put<uint64_t>(canvas_type + t::struct_type::super, fake::get<uint64_t>(panel_type + t::struct_type::super));
    host_type = type(t::name::ui_host_class); host = object(host_type); host_handle = game::to_handle(host);
    const uint64_t option_type = type(t::name::ui_option_owner_class), footer_type = type(t::name::ui_footer_class);
    ui_members::option_owner(option_type); ui_members::footers(footer_type, type(t::name::ui_footer_ingame_class));
    const uint64_t option_owner = object(option_type), footer_widget = object(footer_type);
    footer_label = object(label_type);
    fake::put<uint64_t>(host + t::object::outer, option_owner);
    fake::put<uint64_t>(option_owner + ui_members::settings, host);
    fake::put<uint64_t>(option_owner + ui_members::footer, footer_widget);
    fake::put<uint64_t>(footer_widget + ui_members::footer_text, footer_label);
    footer_caption(u"stock-only context guide");
    const uint64_t tree = object(root_class), root = object(canvas_type), scroll = object(root_class); scroll_slot = object(slot_type);
    root_panel = root;
    fake::put<uint64_t>(host + t::ui_host::tree, tree); fake::put<uint64_t>(tree + t::ui_tree::root, root);
    fake::put<uint64_t>(host + t::ui_host::scroll, scroll); fake::put<uint64_t>(scroll + t::ui_mod_list_tab::slot, scroll_slot);
    fake::put<Margin>(scroll_slot + t::ui_mod_list_tab::offsets, {10, 20, 1468, 868});
    scroll_object = scroll; scroll_panel = object(root_class);
    install_scrollbar(scroll);
    fake::put<uint64_t>(scroll + t::ui_mod_list_scroll::panel, scroll_panel);
    fake::put<float>(scroll + t::ui_mod_list_scroll::total, 1000); fake::put<float>(scroll + t::ui_mod_list_scroll::viewport, 600);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 4.5f); fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -85);
    old_category = object(category_type); old_data = fake::memory(8); fake::put<uint64_t>(old_data, old_category);
    fake::put<Array>(host + t::ui_mod_list_menu::source, {old_data, 1, 1}); fake::put<uint8_t>(host + t::ui_mod_list_menu::new_mode, 1);
    fake::put<uint64_t>(host + t::ui_mod_list_menu::category_class, category_type);
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2); fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
    t::fn::ui_remove_child_at = remove_child_at;
    t::fn::ui_widget_set_clipping = set_clipping;
    t::fn::ui_mod_list_transform = transform; t::fn::ui_mod_list_scrollbar = scrollbar;
    t::fn::ui_slot_offsets = offsets; t::fn::ui_mod_list_play_animation = play;
    t::fn::ui_create_widget = create_widget; t::fn::ui_add_child = add_child; t::fn::text_from_string = text_from_string; t::fn::rich_text_set = label;
    t::fn::ui_footer_lookup = footer_lookup; t::fn::text_display_string = text_string;
    t::fn::ui_mod_list_bind_row = bind_row; t::fn::ui_mod_list_append_choice = append_choice;
    t::fn::ui_mod_list_refresh = refresh; t::fn::ui_mod_list_focus = focus; t::fn::ui_mod_list_adjust = adjust;
    if (fit_mode) {
        t::fn::ui_cached_geometry = geometry;
        t::fn::ui_widget_scale = scale_widget; t::fn::ui_widget_pivot = pivot_widget;
    }
    UiModListNativeCallbacks callbacks{count, row, action, host_ready, host_lost}; ui_mod_list_native_callbacks(&callbacks);
    framework = mod_create("framework.mods"); ui_mod_list_native_start(framework); assert(construct_hook && destruct_hook && refresh_hook && reopen_hook);
    UiContentRect content_rect{-1, -2, -3, -4};
    assert(!ui_mod_list_native_host() && !ui_mod_list_native_content(host_handle, content_rect));
    assert(content_rect.left == -1 && content_rect.height == -4);
    t::fn::name_from_text = tab_name; t::fn::load_soft_object = load_tab;
    // Independent category caches share roots without releasing a sibling's pin.
    const uint64_t pinned = object(root_class), kept = object(root_class);
    assert(game::keep_object(kept, true));
    const uint64_t group_a = ui_mod_list_native_group(), group_b = ui_mod_list_native_group();
    assert(group_a && group_b && group_a != group_b);
    for (const uint64_t raw : {pinned, kept}) {
        assert(ui_mod_list_native_pin(host_handle, group_a, raw));
        assert(ui_mod_list_native_pin(host_handle, group_b, raw));
    }
    ui_mod_list_native_unpin(host_handle, group_a);
    assert(game::object_kept(pinned) && game::object_kept(kept));
    ui_mod_list_native_unpin(host_handle, group_b);
    assert(!game::object_kept(pinned) && game::object_kept(kept));
    assert(game::keep_object(kept, false));
    if (fit_mode) {
        fit_scenarios();
        return 0;
    }
    if (refresh_mode) {
        refresh_scenarios();
        return 0;
    }
    if (borrow_mode) {
        assert(ui_mod_list_native_prepare() == TSUCHI_OK);
        fake::put<uint8_t>(host + t::ui_host::state, 1); construct_hook(reinterpret_cast<void*>(host));
        borrow_scenarios();
        mod_stop(framework); assert(panel_slots[root_panel].empty());
        return 0;
    }
    if (eager_prepare) {
        static uint64_t ticks = 100;
        cost_clock = [] { ticks += 10; return ticks; };
        // Safe startup can prepare the template without opening Settings.
        assert(ui_mod_list_native_prepare() == TSUCHI_OK);
        assert(reentered_prepare && tab_loads == 1 && tab_names == 1 && !creates && !ready_calls);
        assert(startup_traces.size() == 1 && startup_traces[0].find("Settings tab class prepared at_us=") == 0);
        const uint64_t sentinel = type("NoClassInventoryAfterPreparation");
        fake::measured_read_address = sentinel + t::object::name; fake::measured_reads = 0;
        for (unsigned i = 0; i < 30; ++i) assert(ui_mod_list_native_prepare() == TSUCHI_OK);
        construct_before_activation();
        assert(constructed == 1 && !ready_calls && creates == 2 && !world_frames && !focused);
        assert(engine_frames.size() == 1);
        fake::put<uint8_t>(host + t::ui_host::state, 1); engine_frame();
        assert(constructed == 1 && ready_calls == 1 && creates == 2 && !fake::measured_reads);
        assert(!world_frames && ui_mod_list_native_host() == host_handle);
        engine_frame();
        assert(ready_calls == 1 && creates == 2 && !world_frames);
        // Native retained reopen happens while world steps remain paused.
        // Its wrapper must restore before stock and rebuild before returning.
        fake::put<uint8_t>(host + t::ui_host::state, 4);
        const unsigned focus_before_reopen = focused;
        reopen_hook(reinterpret_cast<void*>(host));
        assert(reopened == 1 && constructed == 1 && creates == 4 && ready_calls == 1);
        assert(panel_slots[root_panel].size() == 2 && !world_frames && focused == focus_before_reopen);
        assert(fake::get<uint8_t>(host + t::ui_host::state) == 0 && !ui_mod_list_native_host());
        UiContentRect closed_rect{1, 2, 3, 4};
        assert(!ui_mod_list_native_content(host_handle, closed_rect));
        assert(ui_mod_list_native_cycle(host_handle, false) == TSUCHI_NOT_READY);
        const Margin reopened_layout = fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets);
        assert(reopened_layout.top == 100 && reopened_layout.height == 788);
        assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 1);
        fake::put<uint8_t>(host + t::ui_host::state, 1); engine_frame(); engine_frame();
        assert(ready_calls == 2 && creates == 4 && !world_frames && ui_mod_list_native_host() == host_handle);
        fake::measured_read_address = 0;
        assert(startup_traces.size() == 4 && startup_traces[1].find("Settings tabs first ready at_us=") == 0);
        assert(startup_traces[2].find("Settings tabs installed via construct at_us=") == 0);
        assert(startup_traces[3].find("Settings tabs installed via reopen at_us=") == 0);
        engine_only_tab_revisions();
        const size_t revision_traces = startup_traces.size();
        for (unsigned i = 0; i < 30; ++i) frame();
        assert(startup_traces.size() == revision_traces && tab_loads == 1);
        fake::put<uint8_t>(host + t::ui_host::state, 4);
        const unsigned focus_before_close = focused;
        mod_stop(framework); assert(panel_slots[root_panel].empty());
        assert(focused == focus_before_close);
        assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).top == 20);
        assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).height == 868);
        assert(fake::get<uint8_t>(scroll_object + t::ui_widget::clipping) == 0);
        assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
        assert(labels[footer_label] == u"stock-only context guide");
        assert(!game::object_kept(tab_type));
        return 0;
    }
    if (probe_load) {
        fake::put<uint64_t>(host + t::object::class_, root_class);
        for (unsigned i = 0; i < 80; ++i) frame(); assert(!tab_loads && !tab_names && !creates);
        // No active or remembered Settings host: another gameplay interval
        // must not rediscover the global object table.
        fake::measured_read_address=fake::main_image()+t::data::object_count;
        fake::measured_reads=0;
        for (unsigned i = 0; i < 300; ++i) frame();
        assert(fake::measured_reads==0);
        fake::measured_read_address=0;
        fake::put<uint64_t>(host + t::object::class_, host_type);
        for (unsigned i = 0; i < 80; ++i) frame(); assert(!tab_loads && !creates);
        construct_hook(reinterpret_cast<void*>(host)); assert(constructed == 1 && !ready_calls && !creates);
        fake::put<uint8_t>(host + t::ui_host::state, 4); for (unsigned i = 0; i < 80; ++i) frame(); assert(!tab_loads && !creates);
        fake::put<uint8_t>(host + t::ui_host::state, 1); fake::put<uint8_t>(host + t::ui_mod_list_menu::new_mode, 0); frame(); assert(!tab_loads);
        fake::put<uint8_t>(host + t::ui_mod_list_menu::new_mode, 1); fake::put<uint32_t>(host + t::object::flags, t::constant::template_flags); frame(); assert(!tab_loads);
        fake::put<uint32_t>(host + t::object::flags, 0); fake::put<uint64_t>(tree + t::ui_tree::root, 0); frame(); assert(!tab_loads);
        fake::put<uint64_t>(tree + t::ui_tree::root, root); fake::put<uint64_t>(scroll + t::ui_mod_list_tab::slot, 0); frame(); assert(!tab_loads);
        fake::put<uint64_t>(scroll + t::ui_mod_list_tab::slot, scroll_slot);
        frame(); assert(tab_loads == 1 && tab_names == 1);
        const uint64_t sentinel = type("NoClassInventoryAfterFailedPreparation");
        fake::measured_read_address = sentinel + t::object::name; fake::measured_reads = 0;
        for (unsigned i = 0; i < 80; ++i) frame(); assert(tab_loads == 1);
        assert(!fake::measured_reads); fake::measured_read_address = 0;
        assert(creates == (load_fails ? 0u : 2u)); assert(ready_calls == (load_fails ? 0u : 1u));
        mod_stop(framework); assert(panel_slots[root_panel].empty()); return 0;
    }
    assert(ui_mod_list_native_prepare() == TSUCHI_OK && !tab_loads);
    fake::put<uint8_t>(host + t::ui_host::state, 1);
    construct_hook(reinterpret_cast<void*>(host)); assert(constructed == 1 && ready_calls == 1);
    assert(ui_mod_list_native_host() == host_handle);
    assert(labels[footer_label] == u"stock-only context guide    localized ZLZR_MovePages");
    const unsigned stable_footer_sets = footer_sets, stable_footer_lookups = footer_lookups;
    for (unsigned i = 0; i < 30; ++i) frame();
    assert(footer_sets == stable_footer_sets && footer_lookups == stable_footer_lookups);
    ui_mod_list_native_footer_release(host_handle);
    assert(labels[footer_label] == u"stock-only context guide");
    custom_open = true; footer_caption(u"custom active guide"); frame();
    assert(labels[footer_label] == u"custom active guide");
    custom_open = false; footer_caption(u"stock-only context guide"); frame();
    assert(labels[footer_label] == u"stock-only context guide    localized ZLZR_MovePages");
    assert(ui_mod_list_native_content(host_handle, content_rect));
    assert(content_rect.left == 10 && content_rect.top == 100 && content_rect.width == 1468 && content_rect.height == 788);
    const unsigned cached_creates = creates, cached_ready = ready_calls;
    for (unsigned i = 0; i < 100; ++i) assert(ui_mod_list_native_host() == host_handle && ui_mod_list_native_content(host_handle, content_rect));
    assert(creates == cached_creates && ready_calls == cached_ready);
    thread = 2;
    assert(!ui_mod_list_native_host() && !ui_mod_list_native_content(host_handle, content_rect));
    thread = 1;
    assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).top == 100 && animations == 2);
    bool game_label = false, mods_label = false; for (const auto& pair : labels) { game_label |= pair.second == u"Game"; mods_label |= pair.second == u"Mods"; }
    assert(game_label && mods_label && game::object_kept(host));
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK && ui_mod_list_native_active(host_handle));
    frame();
    assert(labels[footer_label].find(u"stock-only") == std::u16string::npos);
    assert(labels[footer_label].find(u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Settings") != std::u16string::npos &&
        labels[footer_label].find(u"[PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] Details") != std::u16string::npos);
    const Array source = fake::get<Array>(host + t::ui_mod_list_menu::source);
    const uint64_t category = fake::get<uint64_t>(source.data); const Array rows = fake::get<Array>(category + t::ui_mod_list_category::rows);
    assert(rows.count == 8 && rows.capacity == 8 && game::object_kept(category) && game::object_kept(old_category));
    for (unsigned i = 0; i < 8; ++i) {
        const uint64_t widget = fake::get<uint64_t>(rows.data + i * 8), selector = child(widget, t::ui_mod_list_row::selector);
        assert(game::object_kept(widget) && choices[selector].size() == 2 && choices[selector][0] == u"Off" && choices[selector][1] == u"On");
    }
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 7); uint32_t index = 0;
    assert(ui_mod_list_native_selected(host_handle, index) == TSUCHI_OK && index == 7);
    frame();
    assert(labels[footer_label].find(u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Settings") == std::u16string::npos);
    assert(labels[footer_label].find(u"[PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] Details") != std::u16string::npos);
    bool handled = true, result = false; assert(ui_mod_list_native_dispatch(host_handle, 1, handled, result) && !handled);
    assert(ui_mod_list_native_dispatch(host_handle, 3, handled, result) && handled && result && requested[7] && action_calls == 1);
    assert(ui_mod_list_native_dispatch(host_handle, 4, handled, result) && handled && result && action_calls == 2);
    pending_status = true; assert(ui_mod_list_native_sync(host_handle) == TSUCHI_OK);
    const uint64_t last_row = fake::get<uint64_t>(rows.data + 7 * 8);
    assert(labels[child(last_row, t::ui_mod_list_row::title)] == u"mod.h - Restart pending"); pending_status = false;
    assert(ui_mod_list_native_focus(host_handle, 3) == TSUCHI_OK && fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 3);
    assert(ui_mod_list_native_focus(host_handle, 99) == TSUCHI_NOT_FOUND);
    thread = 2; assert(ui_mod_list_native_release(host_handle) == TSUCHI_INVALID && ui_mod_list_native_focus(host_handle, 0) == TSUCHI_INVALID); thread = 1;
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 9); fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -777);
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK && lost_calls == 0);
    assert(fake::get<float>(scroll_panel + t::ui_mod_list_scroll::x) == 4.5f && fake::get<float>(scroll_panel + t::ui_mod_list_scroll::y) == -85 && transformed == 1 && scrollbar_updates == 1);
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    destruct_hook(reinterpret_cast<void*>(host)); assert(destructed == 1 && lost_calls == 1 && !ui_mod_list_native_active(host_handle));
    assert(labels[footer_label] == u"stock-only context guide");
    assert(!ui_mod_list_native_host() && !ui_mod_list_native_content(host_handle, content_rect));
    assert(!game::object_kept(category) && !game::object_kept(old_category) && fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 2);
    assert(released.size() == 4);
    assert(fake::get<Margin>(scroll_slot + t::ui_mod_list_tab::offsets).top == 20 && panel_slots[root_panel].empty() && detached_tabs == 2 && !game::object_kept(host));
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK); refresh_callback = reopen;
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK && ui_mod_list_native_active(host_handle));
    const Array reopened = fake::get<Array>(host + t::ui_mod_list_menu::source);
    assert(reopened.data != old_data && game::object_kept(fake::get<uint64_t>(reopened.data)) && game::object_kept(old_category));
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK && !game::object_kept(old_category));
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK);
    const unsigned transform_before = transformed;
    fake::put<float>(scroll_object + t::ui_mod_list_scroll::viewport, 601);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 11); fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -600);
    assert(ui_mod_list_native_release(host_handle) == TSUCHI_OK && transformed == transform_before);
    assert(fake::get<float>(scroll_panel + t::ui_mod_list_scroll::x) == 11 && fake::get<float>(scroll_panel + t::ui_mod_list_scroll::y) == -600);
    fake::put<float>(scroll_object + t::ui_mod_list_scroll::viewport, 600);
    fail_create = creates + 2; assert(ui_mod_list_native_hold(host_handle) == TSUCHI_NOT_READY && fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data); fail_create = 0;
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK); fake::destroy(host); frame(); assert(lost_calls == 2 && !ui_mod_list_native_active(host_handle));
    fake::place(host, fake::get<int32_t>(host + t::object::index)); host_handle = game::to_handle(host);
    fake::put<Array>(host + t::ui_mod_list_menu::source, {old_data, 1, 1});
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK); mod_stop(framework); assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    framework = mod_create("framework.reentrant"); ui_mod_list_native_start(framework); refresh_callback = stop;
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_NOT_READY && fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    framework = mod_create("framework.snapshot"); ui_mod_list_native_start(framework); row_callback = stop;
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_NOT_READY && fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    assert(panel_slots[root_panel].empty());
    framework = mod_create("framework.closed"); ui_mod_list_native_start(framework);
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_OK && game::object_kept(host));
    fake::put<uint8_t>(host + t::ui_host::state, 4); frame();
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data && !ui_mod_list_native_active(host_handle) && !game::object_kept(host) && panel_slots[root_panel].empty());
    assert(ui_mod_list_native_hold(host_handle) == TSUCHI_NOT_READY); mod_stop(framework);
    assert(cycle_traces.empty() && !cost_clock);
    uint64_t cycle_attempt = 1;
    const auto traced_cycle = [&](bool previous, bool ready, uint32_t old_tab, uint32_t next_tab,
                                  TSUCHI_Result expected) {
        const size_t before = cycle_traces.size();
        assert(ui_mod_list_native_cycle(host_handle, previous) == expected);
        assert(cycle_traces.size() == before + 1);
        cycle_record(before, cycle_attempt++, previous, ready, old_tab, next_tab, expected);
    };
    const auto silent_cycle = [&](bool previous, TSUCHI_Result expected) {
        const size_t before = cycle_traces.size();
        assert(ui_mod_list_native_cycle(host_handle, previous) == expected);
        ++cycle_attempt;
        assert(cycle_traces.size() == before);
    };
    // Missing Tabs has its own twelve-record allowance, even without a clock.
    for (unsigned i = 0; i < 12; ++i) {
        traced_cycle(bool(i % 2), false, UINT32_MAX, UINT32_MAX, TSUCHI_NOT_READY);
        assert(cycle_field(cycle_traces.back(), "at_us") == 0);
        assert(cycle_field(cycle_traces.back(), "completed_us") == 0 && cycle_field(cycle_traces.back(), "cost_us") == 0);
    }
    silent_cycle(false, TSUCHI_NOT_READY);
    assert(cycle_traces.size() == 12);
    // A registered screen becomes the middle tab, with the native Mods list
    // and original Game source restored at each boundary.
    framework=mod_create("framework.video");custom_tabs=1;++tab_revision;
    video_screen.title.set("Video");video_screen.settings_tab=true;
    fake::put<uint8_t>(host+t::ui_host::state,1);
    ui_mod_list_native_start(framework);construct_hook(reinterpret_cast<void*>(host));
    assert(panel_slots[root_panel].size()==3);
    traced_cycle(false, true, 0, 1, TSUCHI_OK);
    assert(custom_open&&!ui_mod_list_native_active(host_handle));
    traced_cycle(false, true, 1, 2, TSUCHI_OK);
    assert(!custom_open&&ui_mod_list_native_active(host_handle));
    traced_cycle(false, true, 2, 0, TSUCHI_OK);
    assert(!custom_open&&!ui_mod_list_native_active(host_handle));
    assert(fake::get<Array>(host+t::ui_mod_list_menu::source).data==old_data);
    custom_open_result = TSUCHI_NOT_READY;
    traced_cycle(false, true, 0, 1, TSUCHI_NOT_READY);
    assert(!custom_open && !ui_mod_list_native_active(host_handle));
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    custom_open_result = TSUCHI_OK;
    // An inactive native host reports its known old tab, but no target.
    fake::put<uint8_t>(host + t::ui_host::state, 0);
    traced_cycle(true, false, 0, UINT32_MAX, TSUCHI_NOT_READY);
    assert(!custom_open && fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    fake::put<uint8_t>(host + t::ui_host::state, 1);
    traced_cycle(false, true, 0, 1, TSUCHI_OK); // Failed opening did not advance selection.
    assert(custom_open && !ui_mod_list_native_active(host_handle));
    traced_cycle(true, true, 1, 0, TSUCHI_OK);
    assert(!custom_open && !ui_mod_list_native_active(host_handle));
    for (unsigned i = 0; i < 5; ++i) traced_cycle(false, true, i % 3, (i + 1) % 3, TSUCHI_OK);
    assert(cycle_traces.size() == 24 && ui_mod_list_native_active(host_handle));
    silent_cycle(false, TSUCHI_OK); // Per-Tabs cap leaves Mods -> Game unchanged.
    assert(!custom_open && !ui_mod_list_native_active(host_handle));
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);

    construct_hook(reinterpret_cast<void*>(host)); // Fresh Tabs resets only its local cap.
    static uint64_t cycle_ticks = 1000;
    cost_clock = [] { cycle_ticks += 10; return cycle_ticks; };
    cycle_note_callback = stop_from_cycle_note;
    traced_cycle(true, true, 0, 2, TSUCHI_OK);
    assert(!cycle_note_callback && !mod_usable(framework));
    assert(cycle_field(cycle_traces.back(), "at_us") > 0);
    assert(cycle_traces.size() == 25 && cycle_field(cycle_traces.back(), "attempt") == 27);

    framework = mod_create("framework.video.trace");
    ui_mod_list_native_start(framework); construct_hook(reinterpret_cast<void*>(host));
    for (unsigned i = 0; i < 12; ++i) traced_cycle(false, true, i % 3, (i + 1) % 3, TSUCHI_OK);
    assert(cycle_traces.size() == 37);
    for (unsigned i = 0; i < 3; ++i) silent_cycle(false, TSUCHI_OK);
    assert(!custom_open && !ui_mod_list_native_active(host_handle));
    construct_hook(reinterpret_cast<void*>(host));
    for (unsigned i = 0; i < 11; ++i) traced_cycle(false, true, i % 3, (i + 1) % 3, TSUCHI_OK);
    assert(cycle_traces.size() == 48 && ui_mod_list_native_active(host_handle));
    silent_cycle(false, TSUCHI_OK); // Global cap wins while this Tabs has used only eleven.
    assert(!custom_open && !ui_mod_list_native_active(host_handle));
    assert(fake::get<Array>(host + t::ui_mod_list_menu::source).data == old_data);
    construct_hook(reinterpret_cast<void*>(host));
    for (unsigned i = 0; i < 3; ++i) silent_cycle(false, TSUCHI_OK);
    assert(cycle_traces.size() == 48 && !custom_open && !ui_mod_list_native_active(host_handle));
    custom_tabs=0;++tab_revision;frame();
    assert(panel_slots[root_panel].size()==2);
    mod_stop(framework);assert(panel_slots[root_panel].empty());video_screen.title.clear();
}
