#include "pikmin_hud.h"

#include <string.h>
#include "actors.h"
#include "game.h"
#include "hooks.h"
#include "holds.h"
#include "reflect.h"
#include "objects.h"
#include "pikmin_counts.h"
#include "pikmin_native.h"
#include "registry.h"

namespace tsuchi::game {
namespace {
namespace t = table;
struct Entry { uint8_t color = 0, donor = 0, toggle = 0, icon = 0; uint64_t installed_on = 0; };
Entry added[2];
void (*icon_before)(void*) = nullptr;
decltype(t::code_site::pikmin_selected_info)::Function selected_before = nullptr;
decltype(t::code_site::pikmin_icon_number)::Function number_before = nullptr;
void (*list_before)(void*) = nullptr;
void (*cursor_before)(void*, void*, float) = nullptr;
void (*cursor_update_before)(void*, float) = nullptr;
void (*cursor_tick_before)(void*, float, int32_t, void*) = nullptr;
// The game's thread while the throw list updates.
uint64_t list_thread = 0;
bool hooked() { return icon_before && selected_before && number_before && list_before && cursor_before && cursor_update_before && cursor_tick_before; }

struct IconTexture { uint64_t texture = 0; };
TSUCHI_Result icon_admit(const Hold&, IconTexture& kept, const IconTexture& wanted, bool) {
    if (!from_handle(wanted.texture)) return TSUCHI_NOT_READY;
    kept = wanted;
    return TSUCHI_OK;
}
bool icon_keep(const Hold&, IconTexture& kept) { return from_handle(kept.texture) != 0; }
Holds<IconTexture, IconTexture, icon_admit, icon_keep, nullptr> icon_textures{
    "content.pikmin.icon", HoldOn::nothing, true};

struct IconPaint {
    uint64_t material = 0, atlas = 0, texture = 0;
    uint32_t toggle_field = 0;
    uint8_t color = 0, donor = 0, toggle = 0, cursor = 0;
};
uint64_t paint_material(uint64_t icon, bool cursor) {
    return read_or<uint64_t>(icon + (cursor ? t::cursor_toggle_icon::material : t::content_throw_icon::material));
}
uint8_t paint_toggle(uint64_t icon, const IconPaint& kept) {
    return read_or<uint8_t>(icon + kept.toggle_field);
}
bool icon_size_name(uint64_t& packed) {
    Name name{};
    if (!t::bound(t::fn::content_icon_vector) || name_from_utf8("IconSize", name) != TSUCHI_OK) return false;
    packed = 0;
    memcpy(&packed, &name, sizeof name);
    return true;
}
bool icon_size(uint64_t material, float size) {
    uint64_t packed = 0;
    if (!icon_size_name(packed)) return false;
    t::fn::content_icon_vector(reinterpret_cast<void*>(material), packed, size, size, 0.f, 1.f);
    return true;
}
bool (*foreign_icon_owned)(uint64_t) = nullptr;
void paint_restore(const Hold& hold, IconPaint& kept, HoldEnd why) {
    if (foreign_icon_owned && foreign_icon_owned(hold.object)) return;
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    const uint64_t icon = from_handle(hold.object), material = from_handle(kept.material);
    if (!icon || !material || paint_material(icon, kept.cursor) != material) return;
    // A recycled widget has already received its stock texture and atlas cell.
    // It still needs the atlas UV size after showing a full custom texture.
    if (paint_toggle(icon, kept) == kept.toggle) {
        const uint64_t atlas = from_handle(kept.atlas);
        if (atlas && t::bound(t::fn::icon_texture) && t::bound(t::fn::icon_index)) {
            t::fn::icon_texture(reinterpret_cast<void*>(material), reinterpret_cast<void*>(atlas));
            if (!from_handle(hold.object) || !from_handle(kept.material)) return;
            t::fn::icon_index(reinterpret_cast<void*>(material), kept.donor);
        }
    }
    if (from_handle(hold.object) && from_handle(kept.material)) icon_size(material, 0.25f);
}
TSUCHI_Result paint_apply(const Hold& hold, IconPaint& kept, const IconPaint& wanted, bool fresh) {
    const uint64_t material = from_handle(wanted.material), texture = from_handle(wanted.texture);
    uint64_t size_name = 0;
    if (!material || !texture || !from_handle(wanted.atlas) || !t::bound(t::fn::icon_texture) || !t::bound(t::fn::icon_index) ||
        !icon_size_name(size_name)) return TSUCHI_NOT_READY;
    if (!fresh && (kept.material != wanted.material || kept.color != wanted.color)) return TSUCHI_BUSY;
    kept = wanted;
    const auto guard = hold_call_guard(hold);
    hold_apply_checkpoint(hold);
    t::fn::icon_texture(reinterpret_cast<void*>(material), reinterpret_cast<void*>(texture));
    if (!guard.permits()) return TSUCHI_NOT_READY;
    t::fn::icon_index(reinterpret_cast<void*>(material), 0);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    t::fn::content_icon_vector(reinterpret_cast<void*>(material), size_name, 1.f, 1.f, 0.f, 1.f);
    return guard.permits() ? TSUCHI_OK : TSUCHI_NOT_READY;
}
bool paint_keep(const Hold& hold, IconPaint& kept) {
    const uint64_t icon = from_handle(hold.object);
    return icon && from_handle(kept.texture) && from_handle(kept.material) &&
        paint_material(icon, kept.cursor) == from_handle(kept.material) && paint_toggle(icon, kept) == kept.toggle &&
        icon_textures.find(hold.mod, 0, kept.color);
}
Holds<IconPaint, IconPaint, paint_apply, paint_keep, paint_restore> painted_icons{
    "content.pikmin.icon.paint", HoldOn::object, true};
void custom_icon(uint64_t icon, const Entry* entry, bool cursor, uint64_t field = t::throw_icon::current_toggle) {
    const uint64_t handle = to_handle(icon);
    if (!handle) return;
    IconPaint* before = painted_icons.find(handle, 0);
    if (before && (!entry || before->color != entry->color ||
        from_handle(before->material) != paint_material(icon, cursor))) {
        painted_icons.release(painted_icons.holder(handle, 0), handle, 0);
    }
    if (!entry) return;
    const IconTexture* texture = icon_textures.find(0, entry->color);
    TSUCHI_Mod* owner = icon_textures.holder(0, entry->color);
    if (!texture || !owner) return;
    const uint64_t material = paint_material(icon, cursor);
    const uint64_t atlases = read_or<uint64_t>(icon + (cursor ? t::cursor_toggle_icon::textures : t::content_throw_icon::textures));
    const uint64_t atlas = atlases ? read_or<uint64_t>(atlases) : 0;
    const IconPaint wanted{to_handle(material), to_handle(atlas), texture->texture,
        uint32_t(field), entry->color, entry->icon, entry->toggle, uint8_t(cursor)};
    painted_icons.take(owner, handle, 0, wanted);
}

uint64_t ui_object() {
    const uint64_t framework = game_framework();
    const uint64_t ui = framework ? read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    return ui && live(ui) ? ui : 0;
}
bool route_is_patched() {
    if (patch_found() != Patch::right || !t::conditional_site_count) return false;
    for (unsigned i = 0; i < t::conditional_site_count; ++i) {
        const auto& site = t::conditional_sites[i];
        const uint32_t branch = 0x14000000u | (uint32_t((site.entry - site.address) >> 2) & 0x03FFFFFFu);
        if (read_or<uint32_t>(main_base() + site.address) != branch ||
            read_or<uint32_t>(main_base() + site.address + 4) != site.next_instruction) return false;
    }
    return true;
}
void apply_icon(void* object) {
    const uint64_t icon = reinterpret_cast<uint64_t>(object);
    const uint8_t current = read_or<uint8_t>(icon + t::throw_icon::current_toggle);
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (const auto& entry : added) {
        if (!entry.color || entry.toggle != current || entry.installed_on != owner || !owner) continue;
        // Only this synchronous texture application borrows the donor's atlas cell.
        // Native formation colors, list entries, selected type and history stay distinct.
        if (write(icon + t::throw_icon::current_toggle, entry.icon)) {
            icon_before(object);
            write(icon + t::throw_icon::current_toggle, current);
            custom_icon(icon, &entry, false);
            return;
        }
    }
    icon_before(object);
    custom_icon(icon, nullptr, false);
}
// An added color with its throw-list rows in place on the current HUD.
bool shown(uint8_t color) {
    if (!color) return false;
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (const auto& entry : added) if (owner && entry.color == color && entry.installed_on == owner) return true;
    return false;
}
// The squad reports types 16 to 21 as no type, with the difference beside it. While the throw
// list updates, an added color with its rows in place is reported as itself, so the list selects
// its entry instead of hiding every entry. Other readers keep the game's answer.
int32_t selected(void* squad, int32_t which, void* out) {
    const int32_t count = selected_before(squad, which, out);
    const uint64_t thread = known_game_thread_token(), info = reinterpret_cast<uint64_t>(out);
    uint8_t color = 0;
    int32_t special = 0;
    if (!thread || thread != list_thread || !info || !read(info + t::selected_info::color, color) || !read(info + t::selected_info::special, special) ||
        color != t::constant::pikmin_selected_none || special <= 0 || special > 0xFF - color) return count;
    const uint8_t type = uint8_t(color + special);
    if (shown(type) && write(info + t::selected_info::color, type)) write(info + t::selected_info::special, int32_t(0));
    return count;
}
void list_update(void* widget) {
    const uint64_t outer = list_thread;
    list_thread = known_game_thread_token();
    list_before(widget);
    list_thread = outer;
}
// A throw-list icon of an added color asks for row 0, every type. It gets its own color's count.
void icon_number(void* object, int32_t second, uint64_t pending) {
    const uint64_t icon = reinterpret_cast<uint64_t>(object);
    const uint64_t field = read_or<uint8_t>(icon + t::throw_icon::alternate) ? t::throw_icon::alternate_pikmin_type :
                           (pending & 1) ? t::throw_icon::pending_pikmin_type : t::throw_icon::pikmin_type;
    uint8_t type = 0;
    if (!read(icon + field, type) || !shown(type)) { number_before(object, second, pending); return; }
    PikminCountScope scope(type);
    number_before(object, second, pending);
}
// The entry whose toggle an icon shows, with its rows in place on the current HUD.
const Entry* toggled(uint8_t toggle) {
    if (!toggle) return nullptr;
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (const auto& entry : added) if (owner && entry.color && entry.toggle == toggle && entry.installed_on == owner) return &entry;
    return nullptr;
}
// The floating selector over the throw cursor paints a toggle past 16 from the item atlas, and
// its count pass stops at the first toggle past 15. After it paints, an icon of an added color
// gets its donor's Pikmin cell and its own color's count. The icons after it get the counts the
// stop left out: the pass runs again with the donor's toggle in place of each added one.
// The cursor gives the selector the squad's total for an added color it holds, so the same
// repair follows the cursor's update too.
constexpr uint32_t kCursorIcons = 8;
uint64_t cursor_widgets[2]{};
unsigned cursor_next = 0;
void remember(uint64_t widget) {
    const uint64_t handle = to_handle(widget);
    if (!handle || cursor_widgets[0] == handle || cursor_widgets[1] == handle) return;
    cursor_widgets[cursor_next] = handle;
    cursor_next = (cursor_next + 1) % 2;
}
void repair_cursor(uint64_t widget) {
    void* const object = reinterpret_cast<void*>(widget);
    uint64_t icons = 0;
    int32_t count = 0;
    if (!read(widget + t::cursor_toggle::icons, icons) || !read(widget + t::cursor_toggle::icon_count, count) || !icons || count <= 0) return;
    const uint64_t field = read_or<uint8_t>(widget + t::cursor_toggle::alternate) ? t::cursor_toggle_icon::alternate_toggle : t::cursor_toggle_icon::toggle;
    const Entry* shows[kCursorIcons]{};
    uint64_t at[kCursorIcons]{};
    bool any = false;
    for (uint32_t i = 0; i < uint32_t(count) && i < kCursorIcons; ++i) {
        at[i] = read_or<uint64_t>(icons + i * sizeof(uint64_t));
        shows[i] = at[i] ? toggled(read_or<uint8_t>(at[i] + field)) : nullptr;
        any = any || shows[i];
        if (!shows[i] && at[i]) custom_icon(at[i], nullptr, true, field);
    }
    if (!any) return;
    if (t::bound(t::fn::cursor_toggle_counts)) {
        for (uint32_t i = 0; i < kCursorIcons; ++i) if (shows[i]) write(at[i] + field, shows[i]->icon);
        t::fn::cursor_toggle_counts(object, false, false);
        for (uint32_t i = 0; i < kCursorIcons; ++i) if (shows[i]) write(at[i] + field, shows[i]->toggle);
    }
    for (uint32_t i = 0; i < kCursorIcons; ++i) {
        if (!shows[i]) continue;
        const uint64_t material = read_or<uint64_t>(at[i] + t::cursor_toggle_icon::material);
        const uint64_t atlases = read_or<uint64_t>(at[i] + t::cursor_toggle_icon::textures);
        const uint64_t pikmin_atlas = atlases ? read_or<uint64_t>(atlases) : 0;
        if (material && pikmin_atlas && read_or<uint8_t>(at[i] + t::cursor_toggle_icon::item_atlas) && t::bound(t::fn::icon_texture) && t::bound(t::fn::icon_index)) {
            t::fn::icon_texture(reinterpret_cast<void*>(material), reinterpret_cast<void*>(pikmin_atlas));
            t::fn::icon_index(reinterpret_cast<void*>(material), shows[i]->icon);
            write(at[i] + t::cursor_toggle_icon::item_atlas, uint8_t(0));
        }
        const int64_t own = pikmin_counts_color(shows[i]->color, 0, uint32_t(t::constant::pikmin_counter_joined), uint32_t(t::constant::pikmin_counter_current));
        if (own >= 0 && t::fn::cursor_icon_count) t::fn::cursor_icon_count(reinterpret_cast<void*>(at[i]), int32_t(own), -1);
        custom_icon(at[i], shows[i], true, field);
    }
}
void cursor_tick(void* object, void* geometry, float seconds) {
    cursor_before(object, geometry, seconds);
    remember(reinterpret_cast<uint64_t>(object));
    repair_cursor(reinterpret_cast<uint64_t>(object));
}
// The cursor colours its ring for the type it holds, or for the selected type when it holds none,
// and gives every type past the stock ones a dark default ring. While the cursor ticks, an added
// color is reported as itself, and after the tick its ring takes the donor's colour row and parts.
const Entry* listed(uint8_t color) {
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (const auto& entry : added) if (owner && entry.color && entry.color == color && entry.installed_on == owner) return &entry;
    return nullptr;
}
uint8_t ring_of(uint8_t color) {
    if (color == 14) return 9;
    if (color > 7) return color == 9 ? 8 : 14;
    return color;
}
struct Ring { uint64_t cursor = 0; uint8_t bytes[t::throw_cursor_ring::most]{}; };
Ring rings[2];
void repair_ring(uint64_t cursor) {
    const uint64_t owner = read_or<uint64_t>(cursor + t::throw_cursor::owner);
    const int32_t team = owner ? read_or<int32_t>(owner + t::game_actor::team, 0) : 0;
    const Entry* entry = listed(read_or<uint8_t>(cursor + t::throw_cursor::color));
    // Teams 0 and 1 colour the ring by team, whatever the type.
    if (!entry || !owner || team == 0 || team == 1 || !t::bound(t::fn::cursor_ring_colors) || !readable(cursor + t::throw_cursor::ring, t::throw_cursor_ring::size)) return;
    const uint64_t handle = to_handle(cursor);
    Ring* slot = nullptr;
    for (auto& ring : rings) if (ring.cursor == handle) slot = &ring;
    for (auto& ring : rings) if (!slot && !ring.cursor) slot = &ring;
    if (!slot) slot = &rings[0];
    if (slot->cursor == handle && !memcmp(slot->bytes, reinterpret_cast<const void*>(cursor + t::throw_cursor::ring), t::throw_cursor_ring::size)) return;
    uint8_t ring = ring_of(entry->donor);
    t::fn::cursor_ring_colors(reinterpret_cast<void*>(cursor), &ring);
    const uint64_t materials = read_or<uint64_t>(cursor + t::throw_cursor::materials);
    const uint32_t first = ring == t::constant::cursor_ring_purple ? 3 : 0;
    for (uint32_t i = 0; materials && i < 3; ++i) {
        const uint64_t part = read_or<uint64_t>(cursor + t::throw_cursor::parts + i * sizeof(uint64_t));
        const uint64_t material = read_or<uint64_t>(materials + (first + i) * sizeof(uint64_t));
        if (const auto set = part ? method(part, t::ring_part_table::set_material) : nullptr) set(reinterpret_cast<void*>(part), 0, reinterpret_cast<void*>(material));
    }
    slot->cursor = handle;
    memcpy(slot->bytes, reinterpret_cast<const void*>(cursor + t::throw_cursor::ring), t::throw_cursor_ring::size);
}
void cursor_tick(void* cursor, float seconds, int32_t kind, void* function) {
    const uint64_t outer = list_thread;
    list_thread = known_game_thread_token();
    cursor_tick_before(cursor, seconds, kind, function);
    list_thread = outer;
    repair_ring(reinterpret_cast<uint64_t>(cursor));
}
void cursor_update(void* cursor, float seconds) {
    cursor_update_before(cursor, seconds);
    for (uint64_t handle : cursor_widgets)
        if (const uint64_t widget = handle ? from_handle(handle) : 0) repair_cursor(widget);
}
bool maps_match(const Entry& entry, uint64_t ui) {
    if (!t::bound(t::fn::throw_toggle_for_color)) return false;
    alignas(8) uint8_t row[t::throw_toggle_parameter::most];
    static const char* paths[] = {t::name::throw_parameters, t::name::throw_parameters_2p};
    for (const char* path : paths) {
        if (native_map_get(ui, path, &entry.toggle, sizeof entry.toggle, row, uint32_t(t::throw_toggle_parameter::size)) != TSUCHI_OK ||
            row[t::throw_toggle_parameter::toggle] != entry.toggle || row[t::throw_toggle_parameter::color] != entry.color) return false;
    }
    return t::fn::throw_toggle_for_color(reinterpret_cast<void*>(ui), entry.color) == entry.toggle;
}
TSUCHI_Result install(Entry& entry, uint64_t ui) {
    entry.installed_on = 0;
    if (!t::bound(t::fn::throw_toggle_for_color)) return TSUCHI_NOT_READY;
    const uint8_t donor_toggle = t::fn::throw_toggle_for_color(reinterpret_cast<void*>(ui), entry.donor);
    if (!donor_toggle || donor_toggle > 9) return TSUCHI_NOT_READY;
    alignas(8) uint8_t rows[2][t::throw_toggle_parameter::most]{}, existing[t::throw_toggle_parameter::most]{};
    static const char* paths[] = {t::name::throw_parameters, t::name::throw_parameters_2p};
    for (unsigned i = 0; i < 2; ++i) {
        TSUCHI_Result result = native_map_get(ui, paths[i], &donor_toggle, sizeof donor_toggle, rows[i], uint32_t(t::throw_toggle_parameter::size));
        if (result != TSUCHI_OK) return result;
        result = native_map_get(ui, paths[i], &entry.toggle, sizeof entry.toggle, existing, uint32_t(t::throw_toggle_parameter::size));
        if (result == TSUCHI_OK && existing[t::throw_toggle_parameter::color] != entry.color) return TSUCHI_BUSY;
        if (result != TSUCHI_OK && result != TSUCHI_NOT_FOUND) return result;
        rows[i][t::throw_toggle_parameter::toggle] = entry.toggle;
        rows[i][t::throw_toggle_parameter::color] = entry.color;
    }
    for (unsigned i = 0; i < 2; ++i) {
        TSUCHI_Result result = native_map_put(ui, paths[i], &entry.toggle, sizeof entry.toggle, rows[i], uint32_t(t::throw_toggle_parameter::size));
        if (result != TSUCHI_OK) return result;
        result = native_map_get(ui, paths[i], &entry.toggle, sizeof entry.toggle, existing, uint32_t(t::throw_toggle_parameter::size));
        if (result != TSUCHI_OK || memcmp(existing, rows[i], t::throw_toggle_parameter::size)) return TSUCHI_NOT_READY;
    }
    if (t::fn::throw_toggle_for_color(reinterpret_cast<void*>(ui), entry.color) != entry.toggle) return TSUCHI_NOT_READY;
    entry.icon = donor_toggle;
    entry.installed_on = to_handle(ui);
    return TSUCHI_OK;
}
} // namespace

TSUCHI_Result pikmin_hud_start(TSUCHI_Mod* self) {
    if (!route_is_patched()) return TSUCHI_NOT_READY;
    uint64_t id = 0;
    TSUCHI_Result r = TSUCHI_OK;
    if (!icon_before) r = hook_site(self, t::code_site::pikmin_throw_icon, &apply_icon, &icon_before, &id);
    if (r == TSUCHI_OK && !selected_before) r = hook_site(self, t::code_site::pikmin_selected_info, &selected, &selected_before, &id);
    if (r == TSUCHI_OK && !number_before) r = hook_site(self, t::code_site::pikmin_icon_number, &icon_number, &number_before, &id);
    if (r == TSUCHI_OK && !list_before) r = hook_slot(self, t::slot::throw_list_update, &list_update, &list_before, kHookNeedsStock, &id);
    if (r == TSUCHI_OK && !cursor_before) r = hook_slot(self, t::slot::cursor_toggle_tick, &cursor_tick, &cursor_before, kHookNeedsStock, &id);
    if (r == TSUCHI_OK && !cursor_update_before) r = hook_slot(self, t::slot::cursor_update, &cursor_update, &cursor_update_before, kHookNeedsStock, &id);
    if (r == TSUCHI_OK && !cursor_tick_before) r = hook_slot(self, t::slot::cursor_tick, &cursor_tick, &cursor_tick_before, kHookNeedsStock, &id);
    return r;
}
TSUCHI_Result pikmin_hud_register(uint8_t color, uint8_t donor) {
    if (!hooked() || !route_is_patched()) return TSUCHI_NOT_READY;
    unsigned slot;
    if (color == t::constant::pikmin_hud_color_a) slot = 0;
    else if (color == t::constant::pikmin_hud_color_b) slot = 1;
    else return TSUCHI_UNSUPPORTED;
    if (!(donor <= 7 || donor == 9)) return TSUCHI_INVALID;
    Entry& entry = added[slot];
    if (entry.color && entry.donor != donor) return TSUCHI_BUSY;
    entry.color = color, entry.donor = donor, entry.toggle = uint8_t(t::constant::pikmin_hud_toggle_a + slot);
    const uint64_t ui = ui_object();
    return ui ? install(entry, ui) : TSUCHI_NOT_READY;
}
TSUCHI_Result pikmin_hud_install(TSUCHI_Mod* self, uint8_t color, uint8_t donor) {
    const TSUCHI_Result result = pikmin_hud_start(self);
    return result == TSUCHI_OK ? pikmin_hud_register(color, donor) : result;
}
TSUCHI_Result pikmin_hud_icon(TSUCHI_Mod* owner, uint8_t color, uint64_t texture) {
    if (!mod_usable(owner)) return TSUCHI_INVALID;
    if (color != t::constant::pikmin_hud_color_a && color != t::constant::pikmin_hud_color_b) return TSUCHI_UNSUPPORTED;
    return icon_textures.take(owner, 0, color, {texture});
}
TSUCHI_Result pikmin_hud_icon_release(TSUCHI_Mod* owner, uint8_t color) {
    if (!mod_usable(owner)) return TSUCHI_INVALID;
    for (uint32_t i = painted_icons.count(); i > 0; --i) {
        if (i > painted_icons.count()) continue;
        const Hold hold = painted_icons.hold_at(i - 1);
        if (hold.mod == owner && painted_icons.kept_at(i - 1).color == color)
            painted_icons.release(owner, hold.object, hold.what);
    }
    return icon_textures.release(owner, 0, color);
}
void pikmin_hud_refresh() {
    if (!hooked() || !route_is_patched()) return;
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (auto& entry : added) {
        if (!entry.color) continue;
        if (!owner) entry.installed_on = 0;
        else if (entry.installed_on != owner || !maps_match(entry, ui)) install(entry, ui);
    }
}
void pikmin_hud_icon_guard(bool (*owned)(uint64_t)) { foreign_icon_owned = owned; }
bool pikmin_hud_icon_owned(uint64_t icon) {
    const auto* painted = painted_icons.find(icon, 0);
    if (!painted) return false;
    for (uint32_t i=0;i<painted_icons.count();++i)
        if (painted_icons.hold_at(i).object == icon) {
            auto copy = *painted;
            return paint_keep(painted_icons.hold_at(i), copy);
        }
    return false;
}
bool pikmin_hud_toggle_reserved(uint8_t toggle) {
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (const auto& entry : added)
        if (owner && entry.color && entry.installed_on == owner && entry.toggle == toggle) return true;
    return false;
}
bool pikmin_hud_ready(uint8_t color) {
    if (!hooked() || !route_is_patched()) return false;
    const uint64_t ui = ui_object(), owner = ui ? to_handle(ui) : 0;
    for (auto& entry : added) {
        if (!owner || entry.color != color || entry.installed_on != owner) continue;
        if (maps_match(entry, ui)) return true;
        entry.installed_on = 0;
        return false;
    }
    return false;
}
} // namespace tsuchi::game
