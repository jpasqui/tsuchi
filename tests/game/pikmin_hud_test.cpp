// Native HUD admission against explicit host stand-ins, never a game result.
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "pikmin_counts.h"
#include "pikmin_hud.h"
#include "pikmin_native.h"
#include "actors.h"
#include "game.h"
#include "hooks.h"
#include "registry.h"
#include "actor_events.h"
#include "reflect.h"
namespace t = tsuchi::game::table;
using Row = std::array<uint8_t, t::throw_toggle_parameter::most>;
static std::array<uint8_t, 8> code{};
static std::array<uint8_t, 4096> framework{}, ui_a{}, ui_b{}, icon{};
static std::array<uint8_t, t::selected_info::most> info{};
static uint64_t current_ui = reinterpret_cast<uint64_t>(ui_a.data());
static std::map<std::string, std::map<uint8_t, Row>> rows;
static void (*wrapped)(void*) = nullptr;
static decltype(t::code_site::pikmin_selected_info)::Function wrapped_selected = nullptr;
static decltype(t::code_site::pikmin_icon_number)::Function wrapped_number = nullptr;
static void (*wrapped_list)(void*) = nullptr;
static uint8_t seen_toggle;
static void original_icon(void* object) { seen_toggle = tsuchi::game::read_or<uint8_t>(reinterpret_cast<uint64_t>(object) + t::throw_icon::current_toggle); }
// The squad's own answer: types 16 to 21 as type 16 with the difference beside it.
static int32_t squad_type = 0;
static int32_t original_selected(void*, int32_t, void* out) {
    const uint8_t color = squad_type < 16 ? uint8_t(squad_type) : uint8_t(t::constant::pikmin_selected_none);
    const int32_t special = squad_type < 16 ? 0 : squad_type - 16;
    memcpy(static_cast<uint8_t*>(out) + t::selected_info::color, &color, 1);
    memcpy(static_cast<uint8_t*>(out) + t::selected_info::special, &special, 4);
    return 7;
}
// The throw list asks the squad for its selected type while it updates.
static uint8_t list_color = 0;
static int32_t list_special = -1;
static uint64_t thread_now = 1, reader_thread = 0;
static void original_list(void*) {
    info.fill(0xEE);
    const uint64_t list = thread_now;
    if (reader_thread) thread_now = reader_thread;
    const int32_t count = wrapped_selected(nullptr, 0, info.data());
    thread_now = list;
    if (count != 7) list_special = -2;
    list_color = info[t::selected_info::color];
    memcpy(&list_special, info.data() + t::selected_info::special, 4);
}
static uint8_t scoped = 0, scoped_during_number = 0xFF;
static void original_number(void*, int32_t, uint64_t) { scoped_during_number = scoped; }
// The floating selector over the throw cursor, laid out in one block: the widget, its list of
// icons, three icons, a material for each, and its two atlases.
static std::array<uint8_t, 0x4000> cursor{};
static void (*wrapped_cursor)(void*, void*, float) = nullptr;
static int count_passes = 0, count_writes = 0;
static uint64_t cursor_at(uint64_t offset) { return reinterpret_cast<uint64_t>(cursor.data()) + offset; }
static uint64_t cursor_icon(unsigned i) { return cursor_at(0x1000 + i * 0x800); }
static uint64_t cursor_material(unsigned i) { return cursor_at(0x2800 + i * 0x20); }
constexpr uint64_t kPikminAtlas = 0xA0, kItemAtlas = 0xA1;
constexpr uint64_t kCustomTexture = 0xA2, kOtherTexture = 0xA3;
static bool custom_alive = true;
static bool icon_alive = true, name_ready = true;
static TSUCHI_Mod* stop_on_texture = nullptr;
template <class T> static T peek(uint64_t at) { T v{}; memcpy(&v, reinterpret_cast<void*>(at), sizeof v); return v; }
template <class T> static void poke(uint64_t at, T v) { memcpy(reinterpret_cast<void*>(at), &v, sizeof v); }
static uint32_t toggle_field() { return cursor[t::cursor_toggle::alternate] ? t::cursor_toggle_icon::alternate_toggle : t::cursor_toggle_icon::toggle; }
static void game_texture(void* material, void* atlas) {
    poke(reinterpret_cast<uint64_t>(material), reinterpret_cast<uint64_t>(atlas));
    if (stop_on_texture && reinterpret_cast<uint64_t>(atlas) == kCustomTexture) {
        TSUCHI_Mod* mod = stop_on_texture; stop_on_texture = nullptr; tsuchi::mod_stop(mod);
    }
}
static void game_index(void* material, int32_t cell) { poke(reinterpret_cast<uint64_t>(material) + 8, cell); }
static void game_vector(void* material, uint64_t name, float x, float y, float z, float w) {
    assert(name == 77 && x == y && z == 0.f && w == 1.f);
    poke(reinterpret_cast<uint64_t>(material) + 12, x);
}
static void game_icon_count(void* icon, int32_t count, int32_t) { poke(reinterpret_cast<uint64_t>(icon) + 0x528, count); ++count_writes; }
// The game's count pass: each icon in order, stopping at the first toggle past 15.
static void game_counts(void*, bool, bool) {
    ++count_passes;
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t toggle = peek<uint8_t>(cursor_icon(i) + toggle_field());
        if (toggle < 1 || toggle > 15) return;
        game_icon_count(reinterpret_cast<void*>(cursor_icon(i)), 10 * toggle, -1);
    }
}
// The game's tick: a toggle past 16 is painted from the item atlas, then the count pass.
static void original_cursor(void* widget, void*, float seconds) {
    assert(seconds == 0.5f);
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t toggle = peek<uint8_t>(cursor_icon(i) + toggle_field());
        const bool item = toggle > 16;
        game_texture(reinterpret_cast<void*>(cursor_material(i)), reinterpret_cast<void*>(item ? kItemAtlas : kPikminAtlas));
        game_index(reinterpret_cast<void*>(cursor_material(i)), item ? toggle - 16 : toggle);
        cursor[0x1000 + i * 0x800 + t::cursor_toggle_icon::item_atlas] = item;
    }
    game_counts(widget, false, false);
}
// The throw cursor's update: it gives the selector's current icon the squad's total for what it holds.
static void (*wrapped_update)(void*, float) = nullptr;
static void original_update(void*, float seconds) {
    assert(seconds == 0.25f);
    game_icon_count(reinterpret_cast<void*>(cursor_icon(1)), 65, -1);
}
// The throw cursor and its ring: its actor, three ring parts with a function table, and its materials.
static std::array<uint8_t, 0x2000> thrown{};
static std::array<uint8_t, 0x1300> captain{};
static void (*wrapped_tick)(void*, float, int32_t, void*) = nullptr;
static int ring_copies = 0;
static uint8_t held = 0;                           // the type of the Pikmin the cursor holds; 0 for none
static uint64_t thrown_at(uint64_t offset) { return reinterpret_cast<uint64_t>(thrown.data()) + offset; }
static uint64_t part_material[3]{};
static void set_material(void* part, int32_t slot, void* material) {
    assert(slot == 0);
    for (unsigned i = 0; i < 3; ++i) if (reinterpret_cast<uint64_t>(part) == thrown_at(0x1000 + i * 0x100)) part_material[i] = reinterpret_cast<uint64_t>(material);
}
// The game's colour copy: the ring's colours and sizes from the row of that colour.
static void game_ring(void* cursor, void* index) {
    ++ring_copies;
    memset(static_cast<uint8_t*>(cursor) + t::throw_cursor::ring, *static_cast<uint8_t*>(index) + 1, t::throw_cursor_ring::size);
}
// The game's tick: the held type, or the squad's selected one; a change recolours the ring, with
// every type past the stock ones on the dark default row.
static void original_tick(void* cursor, float seconds, int32_t kind, void* function) {
    assert(seconds == 0.5f && kind == 2 && function == nullptr);
    uint8_t color = held;
    if (!color) { info.fill(0); wrapped_selected(nullptr, 0, info.data()); color = info[t::selected_info::color]; }
    if (color == 16 || color == thrown[t::throw_cursor::color]) return;
    thrown[t::throw_cursor::color] = color;
    uint8_t ring = color == 14 ? 9 : color > 7 ? (color == 9 ? 8 : 14) : color;
    game_ring(cursor, &ring);
    const unsigned first = ring == 5 ? 3 : 0;
    for (unsigned i = 0; i < 3; ++i) set_material(reinterpret_cast<void*>(thrown_at(0x1000 + i * 0x100)), 0, reinterpret_cast<void*>(uint64_t(0xE0 + first + i)));
}
static uint8_t ring_row() { return uint8_t(thrown[t::throw_cursor::ring] - 1); }
static void cursor_toggles(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t toggles[3] = {a, b, c};
    for (unsigned i = 0; i < 3; ++i) {
        cursor[0x1000 + i * 0x800 + t::cursor_toggle_icon::toggle] = cursor[0x1000 + i * 0x800 + t::cursor_toggle_icon::alternate_toggle] = 0;
        cursor[0x1000 + i * 0x800 + toggle_field()] = toggles[i];
        poke(cursor_icon(i) + 0x528, int32_t(-1));
    }
}
namespace tsuchi::game {
TSUCHI_Result name_from_utf8(const char* text, Name& out) {
    if (!name_ready) return TSUCHI_NOT_READY;
    assert(!strcmp(text, "IconSize")); out = {77, 0}; return TSUCHI_OK;
}
bool is_game_actor(uint64_t) { return false; }
uint64_t part_owner(uint64_t) { return 0; }
uint64_t main_base() { return reinterpret_cast<uint64_t>(code.data()) - t::conditional_sites[0].address; }
uint64_t game_framework() { return reinterpret_cast<uint64_t>(framework.data()); }
static bool cursor_alive = true;
bool live(uint64_t object) {
    if (object == kPikminAtlas || object == kItemAtlas || object == kOtherTexture) return true;
    if (object == kCustomTexture) return custom_alive;
    if (object == reinterpret_cast<uint64_t>(icon.data())) return icon_alive;
    if (object == reinterpret_cast<uint64_t>(icon.data()) + 0x800) return true;
    for (unsigned i = 0; i < 3; ++i)
        if (cursor_alive && (object == cursor_icon(i) || object == cursor_material(i))) return true;
    return object == current_ui || object == reinterpret_cast<uint64_t>(thrown.data()) ||
           (cursor_alive && object == reinterpret_cast<uint64_t>(cursor.data()));
}
uint64_t to_handle(uint64_t object) { return live(object) ? object : 0; }
uint64_t from_handle(uint64_t handle) { return live(handle) ? handle : 0; }
bool readable(uint64_t address, uint64_t bytes) {
    const std::pair<uint64_t, uint64_t> spans[] = {
        {reinterpret_cast<uint64_t>(code.data()), code.size()},
        {reinterpret_cast<uint64_t>(framework.data()), framework.size()},
        {reinterpret_cast<uint64_t>(ui_a.data()), ui_a.size()},
        {reinterpret_cast<uint64_t>(ui_b.data()), ui_b.size()},
        {reinterpret_cast<uint64_t>(icon.data()), icon.size()},
        {reinterpret_cast<uint64_t>(info.data()), info.size()},
        {reinterpret_cast<uint64_t>(cursor.data()), cursor.size()},
        {reinterpret_cast<uint64_t>(thrown.data()), thrown.size()},
        {reinterpret_cast<uint64_t>(captain.data()), captain.size()}};
    for (const auto& span : spans) if (address >= span.first && bytes <= span.second && address - span.first <= span.second - bytes) return true;
    return false;
}
bool writable(uint64_t address, uint64_t bytes) { return readable(address, bytes); }
bool executable(uint64_t address) { return address == reinterpret_cast<uint64_t>(&set_material); }
// One added color's count, as the counter keeps it: two of color 19 and one of color 21 follow player 2.
int64_t pikmin_counts_color(uint8_t color, int32_t leaf, int32_t state, int32_t player) {
    assert(leaf == 0 && state == int32_t(t::constant::pikmin_counter_joined) && player == int32_t(t::constant::pikmin_counter_current));
    return color == 19 ? 2 : color == 21 ? 1 : -1;
}
// The count for one added color is the counter's business; here only which color is asked for.
PikminCountScope::PikminCountScope(uint8_t color) : outer_color_(scoped), outer_thread_(0) { scoped = color; }
PikminCountScope::~PikminCountScope() { (void)outer_thread_; scoped = outer_color_; }
}
namespace tsuchi {
Patch patch_found() { return Patch::right; }
static std::vector<void (*)()> hold_steps;
bool each_frame(void (*step)()) { hold_steps.push_back(step); return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle, uint64_t) { return game::from_handle(handle); }
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t site, void* function, uint64_t* id, void** before) {
    *id = site + 1;
    if (site == t::code_site::pikmin_throw_icon.number)
        wrapped = reinterpret_cast<void (*)(void*)>(function), *before = reinterpret_cast<void*>(&original_icon);
    else if (site == t::code_site::pikmin_selected_info.number)
        wrapped_selected = reinterpret_cast<decltype(wrapped_selected)>(function), *before = reinterpret_cast<void*>(&original_selected);
    else if (site == t::code_site::pikmin_icon_number.number)
        wrapped_number = reinterpret_cast<decltype(wrapped_number)>(function), *before = reinterpret_cast<void*>(&original_number);
    else assert(false);
    return TSUCHI_OK;
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** original) {
    assert(flags & kHookNeedsStock);
    if (entry == game::main_base() + t::slot::throw_list_update.table + t::slot::throw_list_update.offset) {
        assert(stock == game::main_base() + t::slot::throw_list_update.stock);
        wrapped_list = reinterpret_cast<void (*)(void*)>(function);
        *original = reinterpret_cast<void*>(&original_list), *id = 99;
        return TSUCHI_OK;
    }
    if (entry == game::main_base() + t::slot::cursor_update.table + t::slot::cursor_update.offset) {
        assert(stock == game::main_base() + t::slot::cursor_update.stock);
        wrapped_update = reinterpret_cast<void (*)(void*, float)>(function);
        *original = reinterpret_cast<void*>(&original_update), *id = 101;
        return TSUCHI_OK;
    }
    if (entry == game::main_base() + t::slot::cursor_tick.table + t::slot::cursor_tick.offset) {
        assert(stock == game::main_base() + t::slot::cursor_tick.stock);
        wrapped_tick = reinterpret_cast<void (*)(void*, float, int32_t, void*)>(function);
        *original = reinterpret_cast<void*>(&original_tick), *id = 102;
        return TSUCHI_OK;
    }
    assert(entry == game::main_base() + t::slot::cursor_toggle_tick.table + t::slot::cursor_toggle_tick.offset);
    assert(stock == game::main_base() + t::slot::cursor_toggle_tick.stock);
    wrapped_cursor = reinterpret_cast<void (*)(void*, void*, float)>(function);
    *original = reinterpret_cast<void*>(&original_cursor), *id = 100;
    return TSUCHI_OK;
}
TSUCHI_Result native_map_get(uint64_t object, const char* path, const void* key, uint32_t keybytes, void* value, uint32_t valuebytes) {
    assert(object == current_ui && keybytes == 1 && valuebytes == sizeof(Row));
    auto& table = rows[path];
    auto at = table.find(*static_cast<const uint8_t*>(key));
    if (at == table.end()) return TSUCHI_NOT_FOUND;
    memcpy(value, at->second.data(), sizeof(Row));
    return TSUCHI_OK;
}
TSUCHI_Result native_map_put(uint64_t object, const char* path, const void* key, uint32_t keybytes, const void* value, uint32_t valuebytes) {
    assert(object == current_ui && keybytes == 1 && valuebytes == sizeof(Row));
    memcpy(rows[path][*static_cast<const uint8_t*>(key)].data(), value, sizeof(Row));
    return TSUCHI_OK;
}
}
static uint8_t lookup(void*, uint8_t color) {
    for (const auto& pair : rows[t::name::throw_parameters]) if (pair.second[t::throw_toggle_parameter::color] == color) return pair.first;
    return 0;
}
static void donors(bool second) {
    rows.clear();
    for (const char* path : {t::name::throw_parameters, t::name::throw_parameters_2p}) {
        if (!second && !strcmp(path, t::name::throw_parameters_2p)) continue;
        Row row{}; row.fill(42), row[t::throw_toggle_parameter::toggle] = 1, row[t::throw_toggle_parameter::color] = 0;
        rows[path][1] = row;
    }
}
int main() {
    tsuchi::set_thread_probe([] { return thread_now; });
    tsuchi::note_game_thread();
    static_assert(t::conditional_site_count == 1);
    const auto& site = t::conditional_sites[0];
    const uint32_t branch = 0x14000000u | (uint32_t((site.entry - site.address) >> 2) & 0x03FFFFFFu);
    const uint32_t next_word = site.next_instruction;
    memcpy(code.data(), &branch, 4), memcpy(code.data() + 4, &next_word, 4);
    memcpy(framework.data() + t::game_framework::ui, &current_ui, 8);
    t::fn::throw_toggle_for_color = lookup;
    t::fn::cursor_ring_colors = game_ring;
    poke(thrown_at(t::throw_cursor::owner), reinterpret_cast<uint64_t>(captain.data()));
    poke(reinterpret_cast<uint64_t>(captain.data()) + t::game_actor::team, int32_t(4));
    poke(thrown_at(t::throw_cursor::materials), thrown_at(0x1800));
    poke(thrown_at(0x1900), reinterpret_cast<uint64_t>(&set_material));
    for (unsigned i = 0; i < 6; ++i) poke(thrown_at(0x1800 + i * 8), uint64_t(0xE0 + i));
    for (unsigned i = 0; i < 3; ++i) {
        poke(thrown_at(t::throw_cursor::parts + i * 8), thrown_at(0x1000 + i * 0x100));
        poke(thrown_at(0x1000 + i * 0x100), thrown_at(0x1900) - t::ring_part_table::set_material.offset);
    }
    t::fn::cursor_toggle_counts = game_counts, t::fn::cursor_icon_count = game_icon_count;
    t::fn::icon_texture = game_texture, t::fn::icon_index = game_index;
    t::fn::content_icon_vector = game_vector;
    poke(cursor_at(t::cursor_toggle::icons), cursor_at(0x800));
    poke(cursor_at(t::cursor_toggle::icon_count), int32_t(3));
    poke(cursor_at(0x2900), kPikminAtlas), poke(cursor_at(0x2908), kItemAtlas);
    for (unsigned i = 0; i < 3; ++i) {
        poke(cursor_at(0x800 + i * 8), cursor_icon(i));
        poke(cursor_icon(i) + t::cursor_toggle_icon::material, cursor_material(i));
        poke(cursor_icon(i) + t::cursor_toggle_icon::textures, cursor_at(0x2900));
    }
    auto* owner = reinterpret_cast<TSUCHI_Mod*>(1);
    donors(false);
    assert(tsuchi::game::pikmin_hud_install(owner, 19, 0) == TSUCHI_NOT_FOUND);
    assert(!tsuchi::game::pikmin_hud_ready(19) && rows[t::name::throw_parameters].count(32) == 0);
    assert(wrapped && wrapped_selected && wrapped_number && wrapped_list && wrapped_cursor && wrapped_update && wrapped_tick);
    // Before a color has its rows, the throw list hears the squad's own answer.
    squad_type = 19;
    wrapped_list(nullptr);
    assert(list_color == t::constant::pikmin_selected_none && list_special == 3);
    donors(true);
    assert(tsuchi::game::pikmin_hud_register(19, 0) == TSUCHI_OK);
    assert(tsuchi::game::pikmin_hud_register(21, 0) == TSUCHI_OK);
    for (const char* path : {t::name::throw_parameters, t::name::throw_parameters_2p}) {
        assert(rows[path].at(32)[t::throw_toggle_parameter::color] == 19);
        assert(rows[path].at(33)[t::throw_toggle_parameter::color] == 21);
        for (unsigned i = 2; i < sizeof(Row); ++i) assert(rows[path].at(32)[i] == 42 && rows[path].at(33)[i] == 42);
    }
    for (uint8_t toggle : {uint8_t(32), uint8_t(33), uint8_t(12), uint8_t(27)}) {
        icon.fill(67), icon[t::throw_icon::current_toggle] = toggle;
        const auto before = icon;
        wrapped(icon.data());
        assert(seen_toggle == (toggle >= 32 ? 1 : toggle) && icon == before);
    }
    // While the throw list updates, an added color with its rows is the selected type itself,
    // so the list selects its entry. Its count stays the squad's.
    for (int32_t type : {19, 21}) {
        squad_type = type;
        wrapped_list(nullptr);
        assert(list_color == type && list_special == 0);
    }
    // Stock types, no type, and the carried and item types keep the squad's answer.
    for (int32_t type : {0, 2, 16, 17, 18, 20}) {
        squad_type = type;
        wrapped_list(nullptr);
        assert(list_color == (type < 16 ? type : 16) && list_special == (type < 16 ? 0 : type - 16));
    }
    // The floating selector: an added color's icon gets its donor's Pikmin cell and its own count,
    // and the stock icons after it get the counts the game's pass stopped before.
    for (uint8_t alternate : {uint8_t(0), uint8_t(1)}) {
        cursor[t::cursor_toggle::alternate] = alternate;
        cursor_toggles(3, 33, 1);
        count_passes = 0;
        wrapped_cursor(cursor.data(), nullptr, 0.5f);
        assert(count_passes == 2);
        assert(peek<uint64_t>(cursor_material(1)) == kPikminAtlas && peek<int32_t>(cursor_material(1) + 8) == 1);
        assert(cursor[0x1000 + 0x800 + t::cursor_toggle_icon::item_atlas] == 0);
        assert(peek<int32_t>(cursor_icon(0) + 0x528) == 30 && peek<int32_t>(cursor_icon(1) + 0x528) == 1 && peek<int32_t>(cursor_icon(2) + 0x528) == 10);
        assert(cursor[0x1000 + 0x800 + toggle_field()] == 33);
        assert(peek<int32_t>(cursor_material(0) + 8) == 3 && peek<int32_t>(cursor_material(2) + 8) == 1);
        cursor_toggles(32, 2, 33);
        wrapped_cursor(cursor.data(), nullptr, 0.5f);
        assert(peek<int32_t>(cursor_icon(0) + 0x528) == 2 && peek<int32_t>(cursor_icon(1) + 0x528) == 20 && peek<int32_t>(cursor_icon(2) + 0x528) == 1);
        assert(peek<uint64_t>(cursor_material(0)) == kPikminAtlas && peek<uint64_t>(cursor_material(2)) == kPikminAtlas);
    }
    cursor[t::cursor_toggle::alternate] = 0;
    // The cursor's own update writes the squad's total into the selector it knows; the count is repaired after it.
    cursor_toggles(3, 33, 1);
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    wrapped_update(nullptr, 0.25f);
    assert(peek<int32_t>(cursor_icon(1) + 0x528) == 1 && peek<int32_t>(cursor_icon(2) + 0x528) == 10);
    tsuchi::game::cursor_alive = false;                         // a selector whose life is over is left alone
    wrapped_update(nullptr, 0.25f);
    assert(peek<int32_t>(cursor_icon(1) + 0x528) == 65);
    tsuchi::game::cursor_alive = true;
    // The cursor's ring: an added type it holds, or an added type selected while it holds none,
    // takes its donor's colour row and ring parts. Stock types keep the game's ring.
    thrown[t::throw_cursor::color] = 0;
    held = 21, ring_copies = 0;
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(thrown[t::throw_cursor::color] == 21 && ring_row() == 0 && ring_copies == 2 && part_material[0] == 0xE0 && part_material[2] == 0xE2);
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(ring_copies == 2);                     // nothing changed, nothing is copied again
    game_ring(thrown.data(), &held);              // the game recolours the ring by itself
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(ring_row() == 0 && ring_copies == 4);
    held = 2;
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(thrown[t::throw_cursor::color] == 2 && ring_row() == 2);
    held = 0, squad_type = 19;
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(thrown[t::throw_cursor::color] == 19 && ring_row() == 0);
    // Teams 0 and 1 colour the ring by team: the framework leaves it to the game.
    poke(reinterpret_cast<uint64_t>(captain.data()) + t::game_actor::team, int32_t(0));
    held = 21;
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(ring_row() == 14);
    poke(reinterpret_cast<uint64_t>(captain.data()) + t::game_actor::team, int32_t(4));
    held = 0;
    // Stock icons alone, and an item cell that is no added color's, are the game's.
    cursor_toggles(3, 2, 1);
    count_passes = count_writes = 0;
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    assert(count_passes == 1 && count_writes == 3);
    cursor_toggles(3, 34, 1);
    count_passes = 0;
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    assert(count_passes == 1 && peek<uint64_t>(cursor_material(1)) == kItemAtlas && peek<int32_t>(cursor_icon(2) + 0x528) == -1);
    // Any other reader, and a reader on another thread while the list updates, keep the squad's answer.
    squad_type = 19;
    info.fill(0);
    assert(wrapped_selected(nullptr, 0, info.data()) == 7);
    assert(info[t::selected_info::color] == 16 && info[t::selected_info::special] == 3);
    reader_thread = 2;
    wrapped_list(nullptr);
    assert(list_color == 16 && list_special == 3);
    reader_thread = 0;
    thread_now = 0;                               // before the game's first frame
    wrapped_list(nullptr);
    assert(list_color == 16 && list_special == 3);
    thread_now = 1;
    // A throw-list icon of an added color asks for that color's count; a stock icon asks as the game does.
    struct Case { uint8_t type, pending, alternate, shows_alternate, flags, expect; };
    for (const Case& c : {Case{19, 0, 0, 0, 0, 19}, Case{2, 21, 0, 0, 1, 21}, Case{21, 2, 0, 0, 1, 0}, Case{2, 2, 19, 1, 0, 19},
                          Case{19, 19, 2, 1, 1, 0}, Case{2, 0, 19, 0, 0, 0}, Case{3, 0, 0, 0, 0, 0}, Case{20, 0, 0, 0, 0, 0}}) {
        icon.fill(0);
        icon[t::throw_icon::pikmin_type] = c.type, icon[t::throw_icon::pending_pikmin_type] = c.pending;
        icon[t::throw_icon::alternate_pikmin_type] = c.alternate, icon[t::throw_icon::alternate] = c.shows_alternate;
        scoped_during_number = 0xFF;
        wrapped_number(icon.data(), 0, c.flags);
        assert(scoped_during_number == c.expect && scoped == 0);
    }
    // Native table loading can clear the maps while keeping the same UI object.
    rows.clear();
    assert(!tsuchi::game::pikmin_hud_ready(19));
    tsuchi::game::pikmin_hud_refresh();
    assert(!tsuchi::game::pikmin_hud_ready(19) && !tsuchi::game::pikmin_hud_ready(21));
    donors(true);
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(19) && tsuchi::game::pikmin_hud_ready(21));
    // Refresh also notices loss before anyone asks whether that color is ready.
    rows[t::name::throw_parameters_2p].erase(33);
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(21));
    // A conflicting color is refused rather than overwritten.
    rows[t::name::throw_parameters_2p].at(33)[t::throw_toggle_parameter::color] = 0;
    assert(!tsuchi::game::pikmin_hud_ready(21) && tsuchi::game::pikmin_hud_ready(19));
    tsuchi::game::pikmin_hud_refresh();
    assert(!tsuchi::game::pikmin_hud_ready(21));
    assert(rows[t::name::throw_parameters_2p].at(33)[t::throw_toggle_parameter::color] == 0);
    rows[t::name::throw_parameters_2p].erase(33);
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(21));
    // The payload toggle and the native color lookup must agree with the key.
    rows[t::name::throw_parameters].at(32)[t::throw_toggle_parameter::toggle] = 5;
    assert(!tsuchi::game::pikmin_hud_ready(19));
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(19));
    Row duplicate = rows[t::name::throw_parameters].at(32);
    duplicate[t::throw_toggle_parameter::toggle] = 3;
    rows[t::name::throw_parameters][3] = duplicate;
    assert(!tsuchi::game::pikmin_hud_ready(19));
    tsuchi::game::pikmin_hud_refresh();
    assert(!tsuchi::game::pikmin_hud_ready(19));
    rows[t::name::throw_parameters].erase(3);
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(19));
    current_ui = reinterpret_cast<uint64_t>(ui_b.data());
    memcpy(framework.data() + t::game_framework::ui, &current_ui, 8);
    donors(true);
    assert(!tsuchi::game::pikmin_hud_ready(19));
    tsuchi::game::pikmin_hud_refresh();
    assert(tsuchi::game::pikmin_hud_ready(19) && tsuchi::game::pikmin_hud_ready(21));
    assert(tsuchi::game::pikmin_hud_register(19, 1) == TSUCHI_BUSY);
    assert(tsuchi::game::pikmin_hud_toggle_reserved(32));
    assert(!tsuchi::game::pikmin_hud_toggle_reserved(2));
    // Custom textures use the full image in both selectors. The same shared holds
    // handle replacement, recycled widgets, stop, lost textures, and interrupted applies.
    TSUCHI_Mod* custom = tsuchi::mod_create("icons.custom");
    TSUCHI_Mod* rival = tsuchi::mod_create("icons.rival");
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    assert(tsuchi::game::pikmin_hud_icon(rival, 19, kOtherTexture) == TSUCHI_BUSY);
    assert(tsuchi::game::pikmin_hud_icon(custom, 0, kCustomTexture) == TSUCHI_UNSUPPORTED);
    assert(tsuchi::game::pikmin_hud_icon(custom, 21, 0) == TSUCHI_NOT_READY);
    thread_now = 2;
    assert(tsuchi::game::pikmin_hud_icon(custom, 21, kCustomTexture) == TSUCHI_INVALID);
    assert(tsuchi::game::pikmin_hud_icon_release(custom, 19) == TSUCHI_INVALID);
    thread_now = 1;
    for (uint8_t alternate : {uint8_t(0), uint8_t(1)}) {
        cursor[t::cursor_toggle::alternate] = alternate;
        cursor_toggles(3, 32, 1);
        wrapped_cursor(cursor.data(), nullptr, 0.5f);
        assert(peek<uint64_t>(cursor_material(1)) == kCustomTexture);
        assert(peek<int32_t>(cursor_material(1) + 8) == 0);
        assert(peek<float>(cursor_material(1) + 12) == 1.f);
        cursor_toggles(3, 2, 1);
        wrapped_cursor(cursor.data(), nullptr, 0.5f);
        assert(peek<uint64_t>(cursor_material(1)) == kPikminAtlas);
        assert(peek<int32_t>(cursor_material(1) + 8) == 2);
        assert(peek<float>(cursor_material(1) + 12) == 0.25f);
    }
    cursor[t::cursor_toggle::alternate] = 0;
    cursor_toggles(3, 32, 1);
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kOtherTexture) == TSUCHI_OK);
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    assert(peek<uint64_t>(cursor_material(1)) == kOtherTexture);
    assert(tsuchi::game::pikmin_hud_icon_release(custom, 19) == TSUCHI_OK);
    assert(peek<uint64_t>(cursor_material(1)) == kPikminAtlas);
    assert(peek<int32_t>(cursor_material(1) + 8) == 1);
    assert(peek<float>(cursor_material(1) + 12) == 0.25f);
    assert(tsuchi::game::pikmin_hud_icon_release(custom, 19) == TSUCHI_NOT_FOUND);
    // The throw selector shares the same full-image rendering and stop cleanup.
    icon.fill(0); icon[t::throw_icon::current_toggle] = 32;
    const uint64_t icon_at = reinterpret_cast<uint64_t>(icon.data()), material_at = icon_at + 0x800;
    poke(icon_at + t::content_throw_icon::material, material_at);
    poke(icon_at + t::content_throw_icon::textures, icon_at + 0x700);
    poke(icon_at + 0x700, kPikminAtlas);
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    game_texture(reinterpret_cast<void*>(material_at), reinterpret_cast<void*>(kPikminAtlas));
    game_index(reinterpret_cast<void*>(material_at), 1);
    game_vector(reinterpret_cast<void*>(material_at), 77, 0.25f, 0.25f, 0.f, 1.f);
    name_ready = false;
    wrapped(icon.data());
    assert(peek<uint64_t>(material_at) == kPikminAtlas && peek<int32_t>(material_at + 8) == 1);
    assert(peek<float>(material_at + 12) == 0.25f);
    name_ready = true;
    wrapped(icon.data());
    assert(seen_toggle == 1 && icon[t::throw_icon::current_toggle] == 32);
    assert(peek<uint64_t>(material_at) == kCustomTexture && peek<float>(material_at + 12) == 1.f);
    assert(tsuchi::game::pikmin_hud_icon_owned(icon_at));
    tsuchi::mod_stop(custom);
    assert(!tsuchi::game::pikmin_hud_icon_owned(icon_at));
    assert(peek<uint64_t>(material_at) == kPikminAtlas && peek<int32_t>(material_at + 8) == 1);
    assert(peek<float>(material_at + 12) == 0.25f);
    custom = tsuchi::mod_create("icons.lost");
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    wrapped(icon.data());
    custom_alive = false;
    for (auto step : tsuchi::hold_steps) step();
    assert(peek<uint64_t>(material_at) == kPikminAtlas && peek<float>(material_at + 12) == 0.25f);
    custom_alive = true;
    tsuchi::mod_stop(custom);
    custom = tsuchi::mod_create("icons.destroyed");
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    wrapped(icon.data());
    icon_alive = false;
    for (auto step : tsuchi::hold_steps) step();
    assert(peek<uint64_t>(material_at) == kCustomTexture && peek<float>(material_at + 12) == 1.f);
    tsuchi::mod_stop(custom);
    icon_alive = true;
    custom = tsuchi::mod_create("icons.reentrant");
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    stop_on_texture = custom;
    wrapped(icon.data());
    assert(!tsuchi::mod_running(custom));
    assert(peek<uint64_t>(material_at) == kPikminAtlas && peek<float>(material_at + 12) == 0.25f);
    tsuchi::mod_stop(rival);
    custom = tsuchi::mod_create("icons.foreign-owner");
    assert(tsuchi::game::pikmin_hud_icon(custom, 19, kCustomTexture) == TSUCHI_OK);
    wrapped(icon.data());
    tsuchi::game::pikmin_hud_icon_guard([](uint64_t) { return true; });
    game_texture(reinterpret_cast<void*>(material_at), reinterpret_cast<void*>(kOtherTexture));
    game_index(reinterpret_cast<void*>(material_at), 7);
    game_vector(reinterpret_cast<void*>(material_at), 77, 1.f, 1.f, 0.f, 1.f);
    tsuchi::mod_stop(custom);
    assert(peek<uint64_t>(material_at) == kOtherTexture && peek<int32_t>(material_at + 8) == 7);
    assert(peek<float>(material_at + 12) == 1.f);
    tsuchi::game::pikmin_hud_icon_guard(nullptr);
    current_ui = 0;
    memcpy(framework.data() + t::game_framework::ui, &current_ui, 8);
    tsuchi::game::pikmin_hud_refresh();
    assert(!tsuchi::game::pikmin_hud_ready(19));
    // With no HUD to show it, the color is no longer reported as itself, and the selector is the game's.
    squad_type = 19;
    wrapped_list(nullptr);
    assert(list_color == 16 && list_special == 3);
    cursor_toggles(3, 33, 1);
    count_passes = 0;
    wrapped_cursor(cursor.data(), nullptr, 0.5f);
    assert(count_passes == 1 && peek<uint64_t>(cursor_material(1)) == kItemAtlas);
    thrown[t::throw_cursor::color] = 0;
    held = 21;
    wrapped_tick(thrown.data(), 0.5f, 2, nullptr);
    assert(ring_row() == 14);
}
