#include "ui_icons.h"
#include "ui_control_hints.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "ui_feedback.h"
#include "frame.h"
#include "object_retention.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include "holds.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef icon_class{"InTextIconData"}, texture_class{"Texture2D"};
game::ClassRef rich_class{t::name::ui_footer_text_class};
bool dirty = false, registered = false;
bool same_text(const game::EngineText& a, const game::EngineText& b) {
    // FText assignment leaves its final four padding bytes unspecified.
    return !memcmp(a.bytes + t::text_value::data, b.bytes + t::text_value::data, 8) &&
        a.counter() == b.counter() && !memcmp(a.bytes + t::text_value::flags, b.bytes + t::text_value::flags, 4);
}
bool refresh(void*, uint64_t object) {
    if (!game::is_a(object, rich_class) || !game::read_or<uint64_t>(object + t::ui_rich_text::slate)) return true;
    game::Place place{}; char text[4096]{};
    if (game::find_place(game::type_of(object), object, "Text", place) != TSUCHI_OK ||
        game::read_engine_text(place, text, sizeof text, nullptr) != TSUCHI_OK) return true;
    bool has_icon = false;
    constexpr char marker[] = "PictureFont:IconData";
    const size_t length = strlen(text);
    for (size_t i = 0; i + sizeof marker - 1 <= length; ++i)
        if (!memcmp(text + i, marker, sizeof marker - 1)) { has_icon = true; break; }
    if (!has_icon) return true;
    const uint64_t handle = game::to_handle(object);
    if (!handle || !retain_game_object(handle)) return true;
    game::EngineText shown{}, blank{};
    const bool kept = game::read(place.address, shown) && ui_retain_text(shown);
    if (kept) {
        const auto current = [&](const game::EngineText& expected) {
            game::EngineText now{};
            return game::from_handle(handle) == object && game::read(place.address, now) && same_text(now, expected);
        };
        // Sync rebuilds the decorators' copied brushes. Reapply the retained
        // localized FText so existing image runs use those decorators too.
        ui_control_hints_refreshing(true);
        t::fn::ui_rich_text_sync(reinterpret_cast<void*>(object));
        if (current(shown)) {
            char16_t empty = 0; alignas(8) uint8_t string[t::array::size]{};
            const uint64_t data = reinterpret_cast<uint64_t>(&empty); const int32_t count = 1;
            memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &count, 4); memcpy(string + t::array::max, &count, 4);
            blank = t::fn::text_from_string(string);
            if (current(shown)) {
                t::fn::rich_text_set(reinterpret_cast<void*>(object), &blank);
                if (current(blank)) t::fn::rich_text_set(reinterpret_cast<void*>(object), &shown);
            }
            game::drop_shared(blank.counter());
        }
        ui_control_hints_refreshing(false);
        game::drop_shared(shown.counter());
    }
    release_game_object(handle); return true;
}
void refresh_changed() {
    if (!dirty) return;
    dirty = false;
    // Once per batch of changed brushes, never as idle per-frame work.
    int32_t next = 0; game::each_object_from(next, refresh, nullptr);
}
struct Pins {
    uint64_t table = 0, before = 0, texture = 0;
    ~Pins() { release_game_object(texture); release_game_object(before); release_game_object(table); }
};
struct Icon { uint64_t before = 0, applied = 0; };
struct Cache { uint64_t data = 0, counter = 0; };
void row_path(char out[80], uint32_t index, const char* tail) {
    char digits[11]; uint32_t count = 0;
    do { digits[count++] = char('0' + index % 10); index /= 10; } while (index);
    memcpy(out, "Icons[", 6); uint32_t at = 6;
    while (count) out[at++] = digits[--count];
    out[at++] = ']'; memcpy(out + at, tail, strlen(tail) + 1);
}
TSUCHI_Result resource(const Hold& hold, game::Place& out, uint64_t& current, uint64_t& cache) {
    const uint64_t object = game::from_handle(hold.object);
    if (!object) return TSUCHI_NOT_FOUND;
    const uint64_t type = game::type_of(object);
    game::Place array{}; game::Value value{};
    auto result = game::find_place(type, object, "Icons", array);
    if (result != TSUCHI_OK) return result;
    result = game::read_value(array, value);
    if (result != TSUCHI_OK) return result;
    if (value.kind != game::ValueKind::array || value.count > 4096) return TSUCHI_UNSUPPORTED;
    int32_t found = -1;
    for (uint32_t i = 0; i < value.count; ++i) {
        char path[80]; row_path(path, i, ".Key");
        game::Place place{}; game::Name name{};
        if (game::find_place(type, object, path, place) != TSUCHI_OK || !game::read(place.address, name)) return TSUCHI_NOT_FOUND;
        if (memcmp(&name, &hold.what, sizeof name)) continue;
        if (found >= 0) return TSUCHI_BUSY;
        found = int32_t(i);
    }
    if (found < 0) return TSUCHI_NOT_FOUND;
    char path[80]; row_path(path, uint32_t(found), ".Icon");
    game::Place brush{}; uint64_t brush_type = 0, brush_base = 0;
    result = game::find_place(type, object, path, brush);
    if (result != TSUCHI_OK) return result;
    if (game::struct_at(brush, brush_type, brush_base) != TSUCHI_OK ||
        !game::name_is(game::name_of(brush_type), "SlateBrush") ||
        game::read_or<int32_t>(brush.property + t::property::element_bytes) != int32_t(t::slate_brush::size)) return TSUCHI_UNSUPPORTED;
    row_path(path, uint32_t(found), ".Icon.ResourceObject");
    result = game::find_place(type, object, path, out);
    if (result != TSUCHI_OK) return result;
    result = game::read_value(out, value);
    if (result != TSUCHI_OK) return result;
    if (value.kind != game::ValueKind::object && value.kind != game::ValueKind::none) return TSUCHI_UNSUPPORTED;
    cache = brush_base + t::slate_brush::resource_handle;
    Cache handle{};
    if (out.address != brush_base + t::slate_brush::resource || !game::read(cache, handle) || !game::writable(cache, sizeof handle) ||
        (handle.data && !handle.counter) || (handle.counter && !game::writable(handle.counter, t::shared_count::size))) return TSUCHI_UNSUPPORTED;
    current = value.kind == game::ValueKind::object ? value.object : 0;
    return TSUCHI_OK;
}
TSUCHI_Result write(const game::Place& place, uint64_t handle) {
    game::Value value{};
    value.kind = handle ? game::ValueKind::object : game::ValueKind::none; value.object = handle;
    return game::write_value(place, value);
}
TSUCHI_Result apply(const Hold& hold, Icon& kept, const uint64_t& texture, bool fresh) {
    game::Place place{}; uint64_t current = 0, cache = 0;
    auto result = resource(hold, place, current, cache);
    if (result != TSUCHI_OK) return result;
    if (!fresh && current != kept.applied) return TSUCHI_BUSY;
    if (!fresh && texture == kept.applied) return TSUCHI_OK;
    Pins pins;
    if (fresh) {
        if (!retain_game_object(hold.object)) return TSUCHI_NO_MEMORY;
        pins.table = hold.object;
        if (!retain_game_object(current)) return TSUCHI_NO_MEMORY;
        pins.before = current;
    }
    if (!retain_game_object(texture)) return TSUCHI_NO_MEMORY;
    pins.texture = texture;
    if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
    // Pinning can call the engine. Resolve the named row again before writing.
    uint64_t check = 0;
    result = resource(hold, place, check, cache);
    if (result != TSUCHI_OK) return result;
    if (check != current) return TSUCHI_BUSY;
    result = write(place, texture);
    if (result != TSUCHI_OK) return result;
    const uint64_t previous = fresh ? 0 : kept.applied;
    kept = {fresh ? current : kept.before, texture};
    pins.table = pins.before = pins.texture = 0;
    hold_apply_checkpoint(hold);
    const Cache old_cache = game::read_or<Cache>(cache);
    game::write(cache, Cache{});
    dirty = true;
    game::drop_shared(old_cache.counter);
    release_game_object(previous);
    return hold_call_guard(hold).permits() ? TSUCHI_OK : TSUCHI_NOT_READY;
}
void restore(const Hold& hold, Icon& kept, HoldEnd why) {
    const Icon old = kept; kept = {};
    game::Place place{}; uint64_t current = 0, cache = 0;
    if (why != HoldEnd::destroyed && resource(hold, place, current, cache) == TSUCHI_OK && current == old.applied) {
        if (write(place, old.before) == TSUCHI_OK) {
            const Cache old_cache = game::read_or<Cache>(cache);
            game::write(cache, Cache{}); game::drop_shared(old_cache.counter);
            dirty = true;
        }
    }
    release_game_object(old.applied); release_game_object(old.before); release_game_object(hold.object);
}
Holds<Icon, uint64_t, apply, nullptr, restore> icons{"ui.icon_texture", HoldOn::object, true};
TSUCHI_Result key_of(const char* key, uint64_t& packed) {
    if (!key || !*key || strlen(key) > 127) return TSUCHI_INVALID;
    game::Name name{}; const auto result = game::existing_name_from_utf8(key, name);
    if (result == TSUCHI_OK) memcpy(&packed, &name, sizeof name);
    return result;
}
TSUCHI_Result hold_texture(TSUCHI_Mod* mod, TSUCHI_Object table, const char* key, TSUCHI_Object texture) {
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!registered || !t::bound(t::fn::ui_rich_text_sync) || !t::bound(t::fn::rich_text_set) || !t::bound(t::fn::text_from_string)) return TSUCHI_UNSUPPORTED;
    const uint64_t object = game::from_handle(table.id), image = game::from_handle(texture.id);
    if (!object || !image) return TSUCHI_NOT_FOUND;
    if (!game::is_a(object, icon_class) || !game::is_a(image, texture_class)) return TSUCHI_INVALID;
    uint64_t packed = 0; const auto result = key_of(key, packed);
    return result == TSUCHI_OK ? icons.take(mod, table.id, packed, texture.id) : result;
}
TSUCHI_Result release_texture(TSUCHI_Mod* mod, TSUCHI_Object table, const char* key) {
    uint64_t packed = 0; const auto result = key_of(key, packed);
    return result == TSUCHI_OK ? icons.release(mod, table.id, packed) : result;
}

} // namespace
void ui_icons_changed() { dirty = true; }
void ui_icons_start(TSUCHI_Mod* self) {
    if (!registered) registered = each_engine_frame(refresh_changed);
    auto& backend = ui_screen_backend();
    backend.hold_icon_texture = hold_texture; backend.release_icon_texture = release_texture;
    ui_control_hints_start(self);
}
}
