// Mod entries in the game's title menu (DESIGN.md, "Menu entries and typed text").
//
// The title menu is a box of stock buttons that the title widget collects into a list, and the title
// presenter walks by index. An entry here is one more stock button, added to the box and the list the
// way the widget's setup adds its own, in front of an entry the game hides. The presenter's Confirm and
// Back handlers are taken for these entries only; every other press goes to the game's own handlers.
#include "ui_title.h"
#include "ui_service.h"
#include "ui_screens.h"
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
using Tick = decltype(t::slot::title_presenter_tick)::Function;

constexpr uint32_t kEntries = 4, kIdBytes = 64, kLabelBytes = 81, kDescriptionBytes = 301;

struct Entry {
    TSUCHI_Mod* mod;
    char id[kIdBytes], label[kLabelBytes], description[kDescriptionBytes];
    uint64_t button;     // the stock button made for it, as a handle; 0 until it is in the menu
    bool changed;        // its texts changed since they were written
    bool tried;          // it was tried in this title screen's menu: a failure is not retried every frame
};
struct Chosen { TSUCHI_Mod* mod; char id[kIdBytes]; };

TSUCHI_Mod* framework_mod = nullptr;
bool ready = false;
Tick next_tick = nullptr;
List<Entry> entries;
List<Chosen> chosen;
uint64_t presenter = 0, view = 0;    // handles of the title presenter and widget the entries are in
bool patched = false, described = false, listed = false;
List<TSUCHI_Mod*> lockers;
bool dimmed = false;
uint64_t button_class = 0;
bool class_tried = false;
game::ClassRef view_class{t::name::title_view_class}, input_class{t::name::title_input_class};
game::ClassRef box_class{t::name::title_buttons_box_class}, slot_class{t::name::title_button_slot_class};
game::ClassRef panel_class{t::name::ui_panel_base_class};

bool told_map = false;
void note_numbers(const char* what, const uint64_t* values, uint32_t count) {
    char line[200]; size_t used = 0;
    const size_t n = strlen(what);
    if (n >= sizeof line) return;
    memcpy(line, what, n); used = n;
    for (uint32_t i = 0; i < count && used + 20 < sizeof line; ++i) {
        line[used++] = ' ';
        char digits[17]; uint32_t d = 0; uint64_t v = values[i];
        do { const uint32_t nibble = uint32_t(v & 15); digits[d++] = char(nibble < 10 ? '0' + nibble : 'a' + nibble - 10); v >>= 4; } while (v && d < 16);
        while (d) line[used++] = digits[--d];
    }
    line[used] = 0; mod_note(kFrameworkLogId, line);
}

// Why an entry did not go in, once for each title screen.
bool insert_failed(const char* why) {
    char line[160] = "title menu entry not added: ";
    const size_t used = strlen(line), n = strlen(why);
    if (used + n < sizeof line) memcpy(line + used, why, n + 1);
    mod_note(kFrameworkLogId, line);
    return false;
}

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
    char16_t units[kDescriptionBytes]; size_t count = 0;
    if (!game::utf8_to_units(text, units, kDescriptionBytes - 1, count)) return false;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText value = t::fn::text_from_string(string);
    t::fn::rich_text_set(reinterpret_cast<void*>(widget), &value);
    game::drop_shared(value.counter()); return true;
}
bool visibility(uint64_t object, uint32_t value) {
    const auto set = object ? game::method(object, t::widget_table::set_visibility) : nullptr;
    if (!set) return false;
    set(reinterpret_cast<void*>(object), value); return true;
}
bool valid(const char* text) {
    char16_t units[kDescriptionBytes]; size_t count = 0;
    return game::utf8_to_units(text, units, kDescriptionBytes - 1, count);
}
// A named widget in a UserWidget's own tree, when it is not a bound member.
uint64_t named(uint64_t widget, const char* name) {
    uint64_t pending[128]{};
    pending[0] = child(child(widget, t::ui_host::tree), t::ui_tree::root);
    uint32_t count = pending[0] ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t current = pending[i];
        if (game::name_is(game::name_of(current), name)) return current;
        if (!game::is_a(current, panel_class)) continue;
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
Entry* entry_of(uint64_t button) {
    for (uint32_t i = 0; i < entries.count; ++i)
        if (entries[i].button && live(entries[i].button) == button) return &entries[i];
    return nullptr;
}

// The title widget's list of buttons, checked before it is read or written.
struct Buttons { uint64_t* data; int32_t count, room; };
bool buttons_of(uint64_t widget, Buttons& out) {
    out.data = reinterpret_cast<uint64_t*>(game::read_or<uint64_t>(widget + t::title_view::buttons));
    out.count = game::read_or<int32_t>(widget + t::title_view::count, -1);
    out.room = game::read_or<int32_t>(widget + t::title_view::room, -1);
    return out.data && out.count >= 2 && out.count <= 8 && out.room >= out.count &&
        game::writable(reinterpret_cast<uint64_t>(out.data), uint64_t(out.room) * 8);
}

// The presenter's handler for a key, when its map is the stock one of four entries.
uint64_t handler(uint64_t object, uint32_t key) {
    const uint64_t input = child(object, t::title_presenter::input);
    const uint64_t data = input ? game::read_or<uint64_t>(input + t::title_input::data) : 0;
    const int32_t count = input ? game::read_or<int32_t>(input + t::title_input::count, -1) : -1;
    const int32_t room = input ? game::read_or<int32_t>(input + t::title_input::capacity, -1) : -1;
    if (!told_map) {
        told_map = true;
        const uint64_t values[] = {input ? 1u : 0u, input && game::is_a(input, input_class) ? 1u : 0u, uint64_t(uint32_t(count)), uint64_t(uint32_t(room)),
            data && count > 0 && count <= 16 && game::readable(data, uint64_t(count) * 32) ? game::read_or<uint32_t>(data) : 0,
            data && count > 0 && count <= 16 && game::readable(data, uint64_t(count) * 32) ? game::read_or<uint32_t>(data + 32) : 0,
            data && count > 0 && count <= 16 && game::readable(data, uint64_t(count) * 32) ? game::read_or<uint32_t>(data + 16) : 0};
        note_numbers("title presenter input map: input is-input count room key0 key1 kind0", values, 7);
        if (data && count > 0 && count <= 16 && game::readable(data, uint64_t(count) * 32)) {
            const uint64_t instance = game::read_or<uint64_t>(data + 8);
            const uint64_t table = instance && game::readable(instance, 48) ? game::read_or<uint64_t>(instance) : 0;
            const uint64_t more[] = {table ? table - game::main_base() : 0, instance && game::readable(instance, 48) ? game::read_or<uint64_t>(instance + 32) - game::main_base() : 0,
                instance && game::readable(instance, 48) ? game::read_or<uint64_t>(instance + 40) : 1};
            note_numbers("title presenter first handler: table function zero", more, 3);
        }
    }
    if (!input || !game::is_a(input, input_class)) return 0;
    if (!data || count != 4 || room < count || room > 16 || !game::readable(data, uint64_t(count) * 32)) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t cell = data + uint64_t(i) * 32;
        if (game::read_or<uint32_t>(cell + t::title_handler::key) != key) continue;
        const uint64_t instance = game::read_or<uint64_t>(cell + t::title_handler::instance);
        if (!instance || game::read_or<uint32_t>(cell + t::title_handler::kind) != 3 || !game::writable(instance, 48)) return 0;
        if (game::read_or<uint64_t>(instance) != game::main_base() + t::data::title_input_delegate_table ||
            game::read_or<uint64_t>(instance + t::title_handler::zero) != 0) return 0;
        return instance;
    }
    return 0;
}

bool confirm(void* object);
bool back(void* object);
bool direction(void* object, int32_t delta);

// Puts our Confirm and Back in the presenter's map, or the game's back.
bool patch(uint64_t object, bool ours) {
    const uint64_t yes = handler(object, uint32_t(t::constant::title_confirm_key)), no = handler(object, uint32_t(t::constant::title_back_key));
    if (!yes || !no) return false;
    const uint64_t stock_yes = reinterpret_cast<uint64_t>(t::fn::title_confirm), stock_no = reinterpret_cast<uint64_t>(t::fn::title_back);
    const uint64_t our_yes = reinterpret_cast<uint64_t>(&confirm), our_no = reinterpret_cast<uint64_t>(&back);
    const uint64_t now_yes = game::read_or<uint64_t>(yes + t::title_handler::function), now_no = game::read_or<uint64_t>(no + t::title_handler::function);
    if ((now_yes != stock_yes && now_yes != our_yes) || (now_no != stock_no && now_no != our_no)) return false;
    if (!game::write<uint64_t>(yes + t::title_handler::function, ours ? our_yes : stock_yes) ||
        !game::write<uint64_t>(no + t::title_handler::function, ours ? our_no : stock_no)) return false;
    // Up and Down: the other entries of the map, whose handler instances hold the direction handler.
    const uint64_t input = child(object, t::title_presenter::input);
    const uint64_t data = input ? game::read_or<uint64_t>(input + t::title_input::data) : 0;
    const uint64_t stock_way = reinterpret_cast<uint64_t>(t::fn::title_direction), our_way = reinterpret_cast<uint64_t>(&direction);
    for (int32_t i = 0; data && i < 4; ++i) {
        const uint64_t instance = game::read_or<uint64_t>(data + uint64_t(i) * 32 + t::title_handler::instance);
        if (!instance || !game::writable(instance, 48) || game::read_or<uint64_t>(instance) != game::main_base() + t::data::title_direction_delegate_table) continue;
        const uint64_t now = game::read_or<uint64_t>(instance + t::title_handler::function);
        if (now == stock_way || now == our_way) game::write<uint64_t>(instance + t::title_handler::function, ours ? our_way : stock_way);
    }
    return true;
}

// While a screen is open in Tsuchi's host over this menu, the menu's presses are the host's.
bool hosted(void* object) { return reinterpret_cast<uint64_t>(object) == live(presenter) && ui_active_over_menu(); }
bool direction(void* object, int32_t delta) {
    if (hosted(object)) { ui_route_action(0, delta < 0 ? 0u : 1u); return false; }
    return t::fn::title_direction(object, delta);
}
bool confirm(void* object) {
    if (hosted(object)) { ui_route_action(0, 4); return false; }
    const uint64_t at = reinterpret_cast<uint64_t>(object), widget = live(view);
    if (at == live(presenter) && widget && game::read_or<int32_t>(at + t::title_presenter::state) == int32_t(t::constant::title_menu_state)) {
        Buttons list{};
        const int32_t index = game::read_or<int32_t>(at + t::title_presenter::cursor, -1);
        if (buttons_of(widget, list) && index >= 0 && index < list.count) {
            if (const Entry* entry = entry_of(list.data[index])) {
                Chosen pick{entry->mod, {}};
                if (copy(pick.id, kIdBytes, entry->id) && chosen.reserve(chosen.count + 1)) chosen.add(pick);
                return false;   // the press is ours: the game does nothing with it
            }
            if (lockers.count) return false;   // a mod keeps the game's own entries from being chosen
        }
    }
    return t::fn::title_confirm(object);
}
bool back(void* object) {
    if (hosted(object)) { ui_route_action(0, 5); return false; }
    return t::fn::title_back(object);
}

uint64_t prepared_class() {
    if (game::live(button_class) && game::is_class(button_class)) return button_class;
    if (class_tried) return 0;
    class_tried = true;
    if (!t::bound(t::fn::name_from_text) || !t::bound(t::fn::load_soft_object)) return 0;
    struct Soft { int32_t index, serial; uint32_t tag, padding; uint64_t asset, subpath; int32_t count, capacity; } soft{-1, 0, 0, 0, 0, 0, 0, 0};
    char16_t path[256]; size_t units = 0;
    if (!game::utf8_to_units(t::asset::title_button_class, path, 255, units)) return 0;
    path[units] = 0;
    soft.asset = t::fn::name_from_text(path, units, 1);
    const uint64_t kind = reinterpret_cast<uint64_t>(t::fn::load_soft_object(&soft));
    button_class = game::live(kind) && game::is_class(kind) ? kind : 0;
    return button_class;
}

// The game's own entries in the log, once for each title screen: what a mod can name them by later.
void list_stock(uint64_t widget) {
    if (listed) return;
    listed = true;
    Buttons list{};
    if (!buttons_of(widget, list)) return;
    for (int32_t i = 0; i < list.count; ++i) {
        const uint64_t button = list.data[i];
        char name[96] = "?";
        if (game::live(button)) game::name_text(game::read_or<game::Name>(button + t::title_button::id), name, sizeof name);
        char line[160]; size_t used = 0;
        const char* parts[] = {"title menu entry ", nullptr, " ", name, entry_of(button) ? " (mod)" : ""};
        char number[4] = {char('0' + i), 0};
        parts[1] = number;
        for (const char* part : parts) { const size_t n = strlen(part); if (used + n >= sizeof line) break; memcpy(line + used, part, n); used += n; }
        line[used] = 0; mod_note(kFrameworkLogId, line);
    }
}

// Moves a mod button in front of the entry the game hides, which must stay last (the cursor stops before it).
void order(uint64_t object, uint64_t widget, uint64_t button) {
    Buttons list{};
    if (!buttons_of(widget, list) || !(game::read_or<uint8_t>(widget + t::title_view::hide) & 1)) return;
    int32_t ours = -1;
    for (int32_t i = 0; i < list.count; ++i) if (list.data[i] == button) ours = i;
    const uint64_t hidden_at = widget + (game::read_or<uint8_t>(widget + t::title_view::hidden_select) ? t::title_view::hidden_other : t::title_view::hidden);
    const int32_t hidden = game::read_or<int32_t>(hidden_at, -1);
    if (ours < 0 || hidden < 2 || hidden >= list.count || hidden > ours) return;
    for (int32_t i = ours; i > hidden; --i) list.data[i] = list.data[i - 1];
    list.data[hidden] = button;
    const uint64_t fields[2] = {t::title_view::hidden, t::title_view::hidden_other};
    for (uint64_t field : fields) {
        const int32_t index = game::read_or<int32_t>(widget + field, -1);
        if (index >= hidden && index < ours) game::write<int32_t>(widget + field, index + 1);
    }
    const int32_t cursor = game::read_or<int32_t>(object + t::title_presenter::cursor, -1);
    if (cursor >= hidden && cursor < ours) game::write<int32_t>(object + t::title_presenter::cursor, cursor + 1);
    else if (cursor == ours) game::write<int32_t>(object + t::title_presenter::cursor, hidden);
}

// One more stock button, into the box after the game's own and into the list as the setup adds them.
bool insert(uint64_t object, uint64_t widget, Entry& entry) {
    Buttons list{};
    entry.tried = true;
    if (!buttons_of(widget, list) || list.count >= 8) return insert_failed("the button list is not the stock one");
    uint64_t box = game::member_object(widget, t::name::title_buttons_child);
    if (!game::live(box)) box = named(widget, t::name::title_buttons_child);
    if (!game::live(box) || !game::is_a(box, box_class)) return insert_failed("no box of buttons");
    const uint64_t kind = prepared_class();
    if (!kind) return insert_failed("the title button class did not load");
    if (!t::bound(t::fn::ui_create_widget) || !t::bound(t::fn::ui_add_child) || !t::bound(t::fn::ui_vertical_slot_sync)) return insert_failed("a widget function is missing");
    // The spacing of the entry above it.
    const uint64_t slots = game::read_or<uint64_t>(box + t::ui_panel::slots);
    const int32_t slot_count = game::read_or<int32_t>(box + t::ui_panel::slots + 8, 0);
    if (!slots || slot_count < 2 || slot_count > 12 || !game::readable(slots, uint64_t(slot_count) * 8)) return insert_failed("the box's slots are not the stock ones");
    const uint64_t above = game::read_or<uint64_t>(slots + 8);
    if (!game::live(above) || !game::is_a(above, slot_class)) return insert_failed("the box's slot is not a vertical box slot");
    struct Margin { float left, top, right, bottom; };
    const Margin padding = game::read_or<Margin>(above + 0x40);
    const uint64_t button = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(reinterpret_cast<void*>(widget), reinterpret_cast<void*>(kind), nullptr));
    if (!game::live(button)) return insert_failed("the button was not created");
    const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(box), reinterpret_cast<void*>(button)));
    if (!game::live(slot)) return insert_failed("the button did not go in the box");
    game::write<Margin>(slot + 0x40, padding);
    t::fn::ui_vertical_slot_sync(reinterpret_cast<void*>(slot));
    if (!buttons_of(widget, list)) return insert_failed("the button list changed");
    game::write<int32_t>(widget + t::title_view::count, list.count + 1);
    if (list.count >= list.room) t::fn::title_list_grow(reinterpret_cast<void*>(widget + t::title_view::buttons));
    if (!buttons_of(widget, list)) return false;
    list.data[list.count - 1] = button;
    // No mark of new content, and a name of its own for the game's bookkeeping.
    visibility(child(button, t::title_button::mark), uint32_t(t::constant::widget_collapsed));
    label(child(button, t::title_button::label), entry.label);
    const uint64_t normal = game::member_object(button, t::name::title_button_normal);
    if (game::live(normal) && t::bound(t::fn::ui_mod_list_play_animation))
        t::fn::ui_mod_list_play_animation(reinterpret_cast<void*>(button), reinterpret_cast<void*>(normal), 0.f, 1, 0, 1.f, false);
    entry.button = game::to_handle(button); entry.changed = false;
    order(object, widget, button);
    mod_note(kFrameworkLogId, "title menu entry added");
    return true;
}

void forget_menu() {
    for (uint32_t i = 0; i < entries.count; ++i) { entries[i].button = 0; entries[i].tried = false; }
    presenter = view = 0; patched = described = listed = dimmed = false;
}

void tick(void* pointer, float seconds) {
    if (on_game_thread() && ready && mod_usable(framework_mod)) {
        const uint64_t object = reinterpret_cast<uint64_t>(pointer);
        const uint64_t widget = child(object, t::title_presenter::view);
        if (presenter && live(presenter) != object) forget_menu();
        const int32_t state = game::read_or<int32_t>(object + t::title_presenter::state, -1);
        // Buttons go in before the menu shows (0 to 3) or into a menu already showing (5), never while it moves in.
        if (widget && game::is_a(widget, view_class) && state >= 0 && (state <= 3 || state == int32_t(t::constant::title_menu_state))) {
            bool wanted = false;
            for (uint32_t i = 0; i < entries.count; ++i) wanted = wanted || (!entries[i].tried && (!entries[i].button || !live(entries[i].button)));
            // A lock needs the game's Confirm too, even with no entries of ours in the menu.
            if ((wanted || lockers.count > 0) && !patched && patch(object, true)) patched = true;
            else if (wanted && !patched) {
                for (uint32_t i = 0; i < entries.count; ++i) entries[i].tried = true;
                insert_failed("the title presenter's Confirm and Back are not the stock ones");
            }
            if (wanted && patched) {
                presenter = game::to_handle(object); view = game::to_handle(widget);
                for (uint32_t i = 0; i < entries.count; ++i)
                    if (!entries[i].tried && (!entries[i].button || !live(entries[i].button))) insert(object, widget, entries[i]);
            }
            if (state == int32_t(t::constant::title_menu_state)) list_stock(widget);
            // The menu showing now, for the note and the lock, whether or not a mod has entries in it.
            if (!presenter) { presenter = game::to_handle(object); view = game::to_handle(widget); }
        }
        // The game's own entries dimmed while a mod locks them.
        if (live(presenter) == object && widget && dimmed != (lockers.count > 0) && t::bound(t::fn::ui_set_render_opacity)) {
            Buttons list{};
            if (buttons_of(widget, list)) {
                dimmed = lockers.count > 0;
                for (int32_t i = 0; i < list.count; ++i)
                    if (game::live(list.data[i]) && !entry_of(list.data[i])) t::fn::ui_set_render_opacity(reinterpret_cast<void*>(list.data[i]), dimmed ? 0.4f : 1.f);
            }
        }
        ui_title_note_tick(live(presenter) == object ? widget : 0, state == int32_t(t::constant::title_menu_state));
        // Texts that changed, and the line under the menu for an entry of ours.
        if (live(presenter) == object && widget) {
            Buttons list{};
            for (uint32_t i = 0; i < entries.count; ++i)
                if (entries[i].changed && live(entries[i].button)) { label(child(live(entries[i].button), t::title_button::label), entries[i].label); entries[i].changed = false; }
            const int32_t cursor = game::read_or<int32_t>(object + t::title_presenter::cursor, -1);
            const Entry* on = buttons_of(widget, list) && cursor >= 0 && cursor < list.count ? entry_of(list.data[cursor]) : nullptr;
            if (on && !described && state == int32_t(t::constant::title_menu_state)) described = label(child(widget, t::title_view::description), on->description);
            if (!on) described = false;
        }
    }
    next_tick(pointer, seconds);
}

void deliver() {
    for (uint32_t i = 0; i < lockers.count;)
        if (!mod_usable(lockers[i])) lockers[i] = lockers[--lockers.count]; else ++i;
    for (uint32_t i = 0; i < entries.count;)
        if (!mod_usable(entries[i].mod)) {
            if (const uint64_t button = live(entries[i].button)) visibility(button, uint32_t(t::constant::widget_collapsed));
            entries[i] = entries[--entries.count];
        } else ++i;
    if (!chosen.count) return;
    Chosen picks[8]; uint32_t count = 0;
    for (uint32_t i = 0; i < chosen.count && count < 8; ++i) picks[count++] = chosen[i];
    chosen.count = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (mod_usable(picks[i].mod)) ui::emit_entry_chosen_to(picks[i].mod, {TSUCHI_MENU_TITLE, picks[i].id});
}

TSUCHI_Result hold_entry(TSUCHI_Mod* mod, TSUCHI_Menu menu, const char* id, const char* text, const char* description) {
    if (!mod || !id || !text || !description) return TSUCHI_INVALID;
    if (menu != TSUCHI_MENU_TITLE && menu != TSUCHI_MENU_TITLE_OPTIONS) return TSUCHI_INVALID;
    const size_t id_bytes = strlen(id), label_bytes = strlen(text), description_bytes = strlen(description);
    if (!id_bytes || id_bytes >= kIdBytes || !label_bytes || label_bytes >= kLabelBytes || description_bytes >= kDescriptionBytes ||
        !valid(id) || !valid(text) || !valid(description)) return TSUCHI_INVALID;
    if (menu == TSUCHI_MENU_TITLE_OPTIONS) return ui_options_hold_entry(mod, id, text);   // that menu shows no descriptions
    if (!ready) return TSUCHI_UNSUPPORTED;
    for (uint32_t i = 0; i < entries.count; ++i)
        if (entries[i].mod == mod && !strcmp(entries[i].id, id)) {
            copy(entries[i].label, kLabelBytes, text); copy(entries[i].description, kDescriptionBytes, description);
            entries[i].changed = true; described = false; return TSUCHI_OK;
        }
    if (entries.count >= kEntries || !entries.reserve(entries.count + 1)) return TSUCHI_CAPACITY;
    Entry entry{mod, {}, {}, {}, 0, false, false};
    copy(entry.id, kIdBytes, id); copy(entry.label, kLabelBytes, text); copy(entry.description, kDescriptionBytes, description);
    entries.add(entry);
    return TSUCHI_OK;
}

TSUCHI_Result release_entry(TSUCHI_Mod* mod, TSUCHI_Menu menu, const char* id) {
    if (!mod || !id || (menu != TSUCHI_MENU_TITLE && menu != TSUCHI_MENU_TITLE_OPTIONS)) return TSUCHI_INVALID;
    if (menu == TSUCHI_MENU_TITLE_OPTIONS) return ui_options_release_entry(mod, id);
    for (uint32_t i = 0; i < entries.count; ++i) {
        if (entries[i].mod != mod || strcmp(entries[i].id, id)) continue;
        // Out of the list, so the cursor never stops on it; the button stays in the box, collapsed.
        const uint64_t button = live(entries[i].button), widget = live(view), object = live(presenter);
        Buttons list{};
        if (button && widget && buttons_of(widget, list)) {
            for (int32_t j = 0; j < list.count; ++j) {
                if (list.data[j] != button) continue;
                for (int32_t k = j; k + 1 < list.count; ++k) list.data[k] = list.data[k + 1];
                game::write<int32_t>(widget + t::title_view::count, list.count - 1);
                const int32_t cursor = object ? game::read_or<int32_t>(object + t::title_presenter::cursor, -1) : -1;
                if (object && cursor >= j && cursor > 0) game::write<int32_t>(object + t::title_presenter::cursor, cursor - 1);
                break;
            }
            visibility(button, uint32_t(t::constant::widget_collapsed));
        }
        entries[i] = entries[--entries.count];
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result hold_lock(TSUCHI_Mod* mod, TSUCHI_Menu menu) {
    if (!mod || menu != TSUCHI_MENU_TITLE) return TSUCHI_INVALID;
    if (!ready) return TSUCHI_UNSUPPORTED;
    for (uint32_t i = 0; i < lockers.count; ++i) if (lockers[i] == mod) return TSUCHI_OK;
    if (!lockers.reserve(lockers.count + 1)) return TSUCHI_NO_MEMORY;
    lockers.add(mod);
    return TSUCHI_OK;
}
TSUCHI_Result release_lock(TSUCHI_Mod* mod, TSUCHI_Menu menu) {
    if (!mod || menu != TSUCHI_MENU_TITLE) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < lockers.count; ++i)
        if (lockers[i] == mod) { lockers[i] = lockers[--lockers.count]; return TSUCHI_OK; }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result hold_note(TSUCHI_Mod* mod, TSUCHI_Menu menu, const char* text) {
    if (!mod || !text || menu != TSUCHI_MENU_TITLE) return TSUCHI_INVALID;
    if (!ready) return TSUCHI_UNSUPPORTED;
    return ui_title_hold_note(mod, text);
}
TSUCHI_Result release_note(TSUCHI_Mod* mod, TSUCHI_Menu menu) {
    if (!mod || menu != TSUCHI_MENU_TITLE) return TSUCHI_INVALID;
    return ui_title_release_note(mod);
}
} // namespace

uint64_t ui_title_view() { return ready && patched ? live(view) : 0; }
bool ui_title_menu_ready() {
    const uint64_t object = live(presenter);
    return object && patched && game::read_or<int32_t>(object + t::title_presenter::state, -1) == int32_t(t::constant::title_menu_state);
}

void ui_title_start(TSUCHI_Mod* framework) {
    framework_mod = framework;
    static bool listening = false;
    if (!listening) listening = each_frame(deliver);
    uint64_t id = 0;
    ready = listening && hook_slot(framework, t::slot::title_presenter_tick, tick, &next_tick, kHookNeedsStock, &id) == TSUCHI_OK;
    ui_title_options_start(framework);
    auto& backend = ui_screen_backend();
    backend.hold_menu_entry = hold_entry;
    backend.release_menu_entry = release_entry;
    backend.hold_menu_lock = hold_lock;
    backend.release_menu_lock = release_lock;
    backend.hold_menu_note = hold_note;
    backend.release_menu_note = release_note;
    if (ready) ui_title_host_start();
    ui::set_backend(&backend);
}
} // namespace tsuchi
