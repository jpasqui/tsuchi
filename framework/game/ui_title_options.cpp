// Mod entries in the title's Options menu, the one Plus opens on the title screen (DESIGN.md, "Menu
// entries and typed text" and "The Number of Players question").
//
// The menu's widget collects four stock menu buttons into a list, and its code counts four: setup stops at
// four, and Up and Down wrap at fixed numbers (skipping the hidden Settings entry at index 1). An entry here
// is a stock menu button added to the button box and to the end of that list. The menu presenter's Up and
// Down handlers are taken so the cursor reaches the new entries and comes back; its Confirm handler is
// taken for the new entries only, before the game would act on a type it does not have.
#include "ui_title.h"
#include "ui_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
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
using Tick = decltype(t::slot::options_presenter_tick)::Function;

constexpr uint32_t kEntries = 2, kIdBytes = 64, kLabelBytes = 81;
// A released entry keeps its place until the menu is made again: its button stays in the list, where the
// cursor passes over it and a press on it is swallowed, so the game's own Confirm never sees a button it did not make.
struct Entry { TSUCHI_Mod* mod; char id[kIdBytes], label[kLabelBytes]; uint64_t button; bool changed, tried, released; };
struct Chosen { TSUCHI_Mod* mod; char id[kIdBytes]; };

bool ready = false, patched = false;
Tick next_tick = nullptr;
List<Entry> entries;
List<Chosen> chosen;
uint64_t presenter = 0, widget_handle = 0, button_class = 0;
bool class_tried = false;
game::ClassRef view_class{t::name::options_view_class}, input_class{t::name::title_input_class};
game::ClassRef box_class{t::name::title_buttons_box_class}, slot_class{t::name::title_button_slot_class};

uint64_t live(uint64_t handle) { return game::from_handle(handle); }
uint64_t child(uint64_t object, uint64_t offset) {
    const uint64_t value = object ? game::read_or<uint64_t>(object + offset) : 0;
    return game::live(value) ? value : 0;
}
bool copy(char* out, size_t room, const char* text) {
    size_t length = 0;
    while (text && length < room && text[length]) ++length;
    if (!text || length == room) return false;
    memcpy(out, text, length + 1); return true;
}
bool label(uint64_t widget, const char* text) {
    if (!game::live(widget) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return false;
    char16_t units[kLabelBytes]; size_t count = 0;
    if (!game::utf8_to_units(text, units, kLabelBytes - 1, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText value = t::fn::text_from_string(string);
    t::fn::rich_text_set(reinterpret_cast<void*>(widget), &value);
    game::drop_shared(value.counter()); return true;
}

struct Buttons { uint64_t* data; int32_t count, room; };
bool buttons_of(uint64_t widget, Buttons& out) {
    out.data = reinterpret_cast<uint64_t*>(game::read_or<uint64_t>(widget + t::options_view::buttons));
    out.count = game::read_or<int32_t>(widget + t::options_view::count, -1);
    out.room = game::read_or<int32_t>(widget + t::options_view::room, -1);
    return out.data && out.count >= 1 && out.count <= 8 && out.room >= out.count &&
        game::writable(reinterpret_cast<uint64_t>(out.data), uint64_t(out.room) * 8);
}
game::ClassRef panel_class{t::name::ui_panel_base_class};
// A named widget in a UserWidget's own tree, when it is not a bound member.
uint64_t named(uint64_t widget, const char* name) {
    const uint64_t bound = game::member_object(widget, name);
    if (game::live(bound)) return bound;
    uint64_t pending[128]{};
    pending[0] = child(child(widget, t::ui_host::tree), t::ui_tree::root);
    uint32_t count = pending[0] ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t current = pending[i];
        if (game::name_is(game::name_of(current), name)) return current;
        if (!game::is_a(current, panel_class)) continue;
        const uint64_t data = game::read_or<uint64_t>(current + t::ui_panel::slots);
        const int32_t slots = game::read_or<int32_t>(current + t::ui_panel::slots + 8, -1);
        if (!slots) continue;
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
Entry* entry_of(uint64_t button) {
    for (uint32_t i = 0; i < entries.count; ++i)
        if (entries[i].button && live(entries[i].button) == button) return &entries[i];
    return nullptr;
}
// The first index of the list that is a mod's entry: every one after the game's own.
int32_t first_ours(const Buttons& list) {
    for (int32_t i = 0; i < list.count; ++i) if (entry_of(list.data[i])) return i;
    return -1;
}

uint64_t widget_of(uint64_t object) {
    const uint64_t widget = child(object, t::options_presenter::view);
    return widget && game::is_a(widget, view_class) ? widget : 0;
}

// The presenter's handler instance for a key, when the instance's function table is the expected one.
uint64_t handler(uint64_t object, uint32_t key, uint64_t table) {
    const uint64_t input = child(object, t::options_presenter::input);
    if (!input || !game::is_a(input, input_class)) return 0;
    const uint64_t data = game::read_or<uint64_t>(input + t::title_input::data);
    const int32_t count = game::read_or<int32_t>(input + t::title_input::count, -1);
    if (!data || count < 1 || count > 16 || !game::readable(data, uint64_t(count) * 32)) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t cell = data + uint64_t(i) * 32;
        if (game::read_or<uint32_t>(cell + t::title_handler::key) != key) continue;
        const uint64_t instance = game::read_or<uint64_t>(cell + t::title_handler::instance);
        if (!instance || !game::writable(instance, 48) || game::read_or<uint64_t>(instance) != game::main_base() + table) return 0;
        return instance;
    }
    return 0;
}

bool confirm(void* object);
bool direction(void* object, bool down);

bool swap(uint64_t instance, uint64_t stock, uint64_t ours) {
    const uint64_t now = game::read_or<uint64_t>(instance + t::title_handler::function);
    return (now == stock || now == ours) && game::write<uint64_t>(instance + t::title_handler::function, ours);
}
bool patch(uint64_t object) {
    const uint64_t up = handler(object, uint32_t(t::constant::options_up_key), t::data::options_direction_delegate_table);
    const uint64_t down = handler(object, uint32_t(t::constant::options_down_key), t::data::options_direction_delegate_table);
    const uint64_t yes = handler(object, uint32_t(t::constant::title_confirm_key), t::data::options_input_delegate_table);
    if (!up || !down || !yes) return false;
    const uint64_t stock_way = reinterpret_cast<uint64_t>(t::fn::options_direction), our_way = reinterpret_cast<uint64_t>(&direction);
    const uint64_t stock_yes = reinterpret_cast<uint64_t>(t::fn::options_confirm), our_yes = reinterpret_cast<uint64_t>(&confirm);
    return swap(up, stock_way, our_way) && swap(down, stock_way, our_way) && swap(yes, stock_yes, our_yes);
}

void highlight(uint64_t widget, int32_t selected) {
    Buttons list{};
    if (!buttons_of(widget, list) || !t::bound(t::fn::options_button_highlight)) return;
    for (int32_t i = 0; i < list.count; ++i)
        if (game::live(list.data[i])) t::fn::options_button_highlight(reinterpret_cast<void*>(list.data[i]), i == selected, true);
}

// Up and Down: the game's own wrap for its four entries, and the mod entries above its first one. In the
// list the mod entries come after the game's, so Up from its first entry and Down from its last reach them.
bool direction(void* object, bool down) {
    const uint64_t at = reinterpret_cast<uint64_t>(object), widget = widget_of(at);
    Buttons list{};
    const int32_t ours = widget && buttons_of(widget, list) ? first_ours(list) : -1;
    if (ours < 0 || !t::fn::options_gate(reinterpret_cast<void*>(widget))) return t::fn::options_direction(object, down);
    const int32_t selected = game::read_or<int32_t>(widget + t::options_view::selected, -1);
    const int32_t last_stock = int32_t(t::constant::options_last_stock), last = list.count - 1;
    const auto step = [&](int32_t from) {
        if (down && from == last_stock) return ours;
        if (down && from >= ours) return from < last ? from + 1 : 0;
        if (!down && from == 0) return last;
        if (!down && from >= ours) return from > ours ? from - 1 : last_stock;
        return -1;
    };
    int32_t next = step(selected);
    // Past released entries, to the next live one.
    for (int32_t tries = 0; next >= ours && tries < list.count; ++tries) {
        const Entry* entry = entry_of(list.data[next]);
        if (!entry || !entry->released) break;
        next = step(next);
    }
    if (next < 0) return t::fn::options_direction(object, down);
    game::write<int32_t>(widget + t::options_view::selected, next);
    highlight(widget, next);
    return true;
}

bool confirm(void* object) {
    const uint64_t at = reinterpret_cast<uint64_t>(object), widget = widget_of(at);
    Buttons list{};
    if (widget && buttons_of(widget, list) && t::fn::options_gate(reinterpret_cast<void*>(widget))) {
        const int32_t selected = game::read_or<int32_t>(widget + t::options_view::selected, -1);
        if (selected >= 0 && selected < list.count)
            if (const Entry* entry = entry_of(list.data[selected])) {
                if (entry->released) return true;   // nobody's now: the press goes nowhere
                Chosen pick{entry->mod, {}};
                if (copy(pick.id, kIdBytes, entry->id) && chosen.reserve(chosen.count + 1)) chosen.add(pick);
                return true;
            }
    }
    return t::fn::options_confirm(object);
}

uint64_t prepared_class() {
    if (game::live(button_class) && game::is_class(button_class)) return button_class;
    if (class_tried || !t::bound(t::fn::name_from_text) || !t::bound(t::fn::load_soft_object)) return 0;
    class_tried = true;
    struct Soft { int32_t index, serial; uint32_t tag, padding; uint64_t asset, subpath; int32_t count, capacity; } soft{-1, 0, 0, 0, 0, 0, 0, 0};
    char16_t path[256]; size_t units = 0;
    if (!game::utf8_to_units(t::asset::options_button_class, path, 255, units)) return 0;
    path[units] = 0;
    soft.asset = t::fn::name_from_text(path, units, 1);
    const uint64_t kind = reinterpret_cast<uint64_t>(t::fn::load_soft_object(&soft));
    button_class = game::live(kind) && game::is_class(kind) ? kind : 0;
    return button_class;
}

void note_failure(const char* why) {
    char line[160] = "options menu entry not added: ";
    const size_t used = strlen(line), n = strlen(why);
    if (used + n < sizeof line) memcpy(line + used, why, n + 1);
    mod_note(kFrameworkLogId, line);
}

// A stock menu button, onto the end of the list and into the top of the box, above the game's own.
bool insert(uint64_t widget, Entry& entry) {
    entry.tried = true;
    Buttons list{};
    if (!buttons_of(widget, list) || list.count >= 8) { note_failure("the button list is not the stock one"); return false; }
    const uint64_t box = named(widget, t::name::title_buttons_child);
    if (!game::live(box) || !game::is_a(box, box_class)) { note_failure("no box of buttons"); return false; }
    const uint64_t kind = prepared_class();
    if (!kind || !t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_add_child) || !t::bound(t::fn::ui_vertical_slot_sync) ||
        !t::bound(t::fn::options_list_grow)) { note_failure("the menu button class or a function is missing"); return false; }
    const uint64_t slots = game::read_or<uint64_t>(box + t::ui_panel::slots);
    const int32_t slot_count = game::read_or<int32_t>(box + t::ui_panel::slots + 8, 0);
    if (!slots || slot_count < 1 || slot_count > 12 || !game::readable(slots, uint64_t(slot_count) * 8)) { note_failure("the box's slots"); return false; }
    if (!t::bound(t::fn::ui_remove_child_at)) { note_failure("a function is missing"); return false; }
    // The game's own entries leave the box and come back under the new one, each with its own layout.
    // A VerticalBoxSlot's size rule (+0x38), padding (+0x40) and alignments (+0x58); its Slate slot at +0x50
    // stays the new slot's own.
    struct Layout { uint8_t size[8], padding[16], align[2]; };
    constexpr int32_t kStock = 12;
    uint64_t contents[kStock]{}; Layout layouts[kStock]{};
    const int32_t ours = first_ours(list), already = ours < 0 ? 0 : list.count - ours;   // mod entries already above
    if (already >= slot_count) { note_failure("the box's slots"); return false; }
    for (int32_t i = already; i < slot_count; ++i) {
        const uint64_t stock = game::read_or<uint64_t>(slots + uint64_t(i) * 8);
        if (!game::live(stock) || !game::is_a(stock, slot_class)) { note_failure("the box's slot"); return false; }
        contents[i] = child(stock, t::ui_panel_slot::content);
        if (!contents[i]) { note_failure("the box's slot"); return false; }
        game::read_bytes(stock + 0x38, layouts[i].size, 8);
        game::read_bytes(stock + 0x40, layouts[i].padding, 16);
        game::read_bytes(stock + 0x58, layouts[i].align, 2);
    }
    const uint64_t button = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(widget), reinterpret_cast<void*>(kind), nullptr));
    // The size, text place and font the Blueprint gave its own entries, copied before the button is built on
    // screen, which is when the button applies them: the first entry's, Play Co-op's, the menu's larger one.
    const uint64_t regular = list.data[0];
    if (game::live(button) && game::live(regular)) {
        for (uint64_t i = 0; i < t::options_button::look_bytes && i < 0x40; ++i)
            game::write<uint8_t>(button + t::options_button::look + i, game::read_or<uint8_t>(regular + t::options_button::look + i));
        if (t::bound(t::fn::options_button_icon)) t::fn::options_button_icon(reinterpret_cast<void*>(button), int32_t(t::constant::options_entry_icon));
    }
    if (!game::live(button)) { note_failure("the button was not made"); return false; }
    for (int32_t i = already; i < slot_count; ++i) t::fn::ui_remove_child_at(reinterpret_cast<void*>(box), already);
    const auto place = [&](uint64_t content, const Layout& layout) {
        const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(box), reinterpret_cast<void*>(content)));
        if (!game::live(slot)) return false;
        for (uint64_t k = 0; k < 8; ++k) game::write<uint8_t>(slot + 0x38 + k, layout.size[k]);
        for (uint64_t k = 0; k < 16; ++k) game::write<uint8_t>(slot + 0x40 + k, layout.padding[k]);
        for (uint64_t k = 0; k < 2; ++k) game::write<uint8_t>(slot + 0x58 + k, layout.align[k]);
        t::fn::ui_vertical_slot_sync(reinterpret_cast<void*>(slot));
        return true;
    };
    bool placed = place(button, layouts[already]);
    for (int32_t i = already; i < slot_count; ++i) placed = place(contents[i], layouts[i]) && placed;
    if (!placed) { note_failure("the buttons did not go back in the box"); return false; }
    if (!buttons_of(widget, list)) return false;
    game::write<int32_t>(widget + t::options_view::count, list.count + 1);
    if (list.count >= list.room) t::fn::options_list_grow(reinterpret_cast<void*>(widget + t::options_view::buttons));
    if (!buttons_of(widget, list)) return false;
    list.data[list.count - 1] = button;
    if (t::bound(t::fn::options_button_icon)) t::fn::options_button_icon(reinterpret_cast<void*>(button), int32_t(t::constant::options_entry_icon));
    if (const uint64_t icon = named(button, t::name::options_button_icon_child)) {
        const auto set = game::method(icon, t::widget_table::set_visibility);
        if (set) set(reinterpret_cast<void*>(icon), uint32_t(t::constant::widget_shown));
    }
    label(named(button, t::name::host_button_text), entry.label);
    // The menu's panel grows by a row, so the new entry sits on it.
    if (const uint64_t panel = named(widget, t::name::options_panel_child)) {
        const uint64_t panel_slot = child(panel, t::ui_mod_list_tab::slot);
        struct Box { float left, top, width, height; };
        const Box box = game::read_or<Box>(panel_slot + t::ui_mod_list_tab::offsets);
        if (panel_slot && t::bound(t::fn::ui_slot_offsets) && box.height > 0 && box.height < 2000)
            t::fn::ui_slot_offsets(reinterpret_cast<void*>(panel_slot), box.left, box.top, box.width, box.height + float(t::constant::options_row_pitch));
    }
    highlight(widget, game::read_or<int32_t>(widget + t::options_view::selected, 0));   // the game's own were built again
    entry.button = game::to_handle(button); entry.changed = false;
    mod_note(kFrameworkLogId, "options menu entry added");
    return true;
}

void forget() {
    for (uint32_t i = 0; i < entries.count;)
        if (entries[i].released) entries[i] = entries[--entries.count];
        else { entries[i].button = 0; entries[i].tried = false; ++i; }
    presenter = widget_handle = 0; patched = false;
}

void tick(void* pointer, float seconds) {
    if (on_game_thread() && ready && entries.count) {
        const uint64_t object = reinterpret_cast<uint64_t>(pointer), widget = widget_of(object);
        if ((presenter && live(presenter) != object) || (widget_handle && live(widget_handle) != widget)) forget();
        Buttons list{};
        if (widget && buttons_of(widget, list)) {
            if (!patched) patched = patch(object);
            if (patched) {
                presenter = game::to_handle(object); widget_handle = game::to_handle(widget);
                for (uint32_t i = 0; i < entries.count; ++i)
                    if (!entries[i].released && !entries[i].tried && (!entries[i].button || !live(entries[i].button))) insert(widget, entries[i]);
                for (uint32_t i = 0; i < entries.count; ++i)
                    if (!entries[i].released && entries[i].changed && live(entries[i].button)) {
                        label(named(live(entries[i].button), t::name::host_button_text), entries[i].label);
                        entries[i].changed = false;
                    }
            }
        }
    }
    next_tick(pointer, seconds);
}

// Marks an entry released. One without a button in the menu goes at once.
void retire(uint32_t i) {
    Entry& entry = entries[i];
    const uint64_t button = live(entry.button);
    if (!button) { entries[i] = entries[--entries.count]; return; }
    entry.released = true; entry.mod = nullptr;
    const auto set = game::method(button, t::widget_table::set_visibility);
    if (set) set(reinterpret_cast<void*>(button), uint32_t(t::constant::widget_collapsed));
}
void deliver() {
    for (uint32_t i = entries.count; i-- > 0;)
        if (!entries[i].released && !mod_usable(entries[i].mod)) retire(i);
    if (!chosen.count) return;
    Chosen picks[4]; uint32_t count = 0;
    for (uint32_t i = 0; i < chosen.count && count < 4; ++i) picks[count++] = chosen[i];
    chosen.count = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (mod_usable(picks[i].mod)) ui::emit_entry_chosen_to(picks[i].mod, {TSUCHI_MENU_TITLE_OPTIONS, picks[i].id});
}
} // namespace

TSUCHI_Result ui_options_hold_entry(TSUCHI_Mod* mod, const char* id, const char* text) {
    if (!ready) return TSUCHI_UNSUPPORTED;
    uint32_t held = 0;
    for (uint32_t i = 0; i < entries.count; ++i) {
        if (entries[i].released) continue;
        ++held;
        if (entries[i].mod == mod && !strcmp(entries[i].id, id)) { copy(entries[i].label, kLabelBytes, text); entries[i].changed = true; return TSUCHI_OK; }
    }
    if (held >= kEntries || !entries.reserve(entries.count + 1)) return TSUCHI_CAPACITY;
    Entry entry{mod, {}, {}, 0, false, false, false};
    copy(entry.id, kIdBytes, id); copy(entry.label, kLabelBytes, text);
    entries.add(entry);
    return TSUCHI_OK;
}
TSUCHI_Result ui_options_release_entry(TSUCHI_Mod* mod, const char* id) {
    for (uint32_t i = 0; i < entries.count; ++i)
        if (!entries[i].released && entries[i].mod == mod && !strcmp(entries[i].id, id)) {
            retire(i);
            return TSUCHI_OK;
        }
    return TSUCHI_NOT_FOUND;
}

void ui_title_options_start(TSUCHI_Mod* framework) {
    static bool listening = false;
    if (!listening) listening = each_frame(deliver);
    uint64_t id = 0;
    ready = listening && hook_slot(framework, t::slot::options_presenter_tick, tick, &next_tick, kHookNeedsStock, &id) == TSUCHI_OK;
}
} // namespace tsuchi
