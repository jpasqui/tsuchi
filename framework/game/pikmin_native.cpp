#include "pikmin_native.h"
#include "game.h"
#include "names.h"
#include "reflect.h"
#include <string.h>
namespace tsuchi {
namespace t = game::table;
TSUCHI_Result native_map_put(uint64_t object, const char* path, const void* key, uint32_t key_bytes,
                            const void* value, uint32_t value_bytes) {
    game::Place at;
    if (!object || !key || !value || !t::bound(t::fn::add_map_pair)) return TSUCHI_NOT_READY;
    TSUCHI_Result result = game::find_place(game::type_of(object), object, path, at);
    if (result != TSUCHI_OK) return result;
    const uint64_t kind = game::read_or<uint64_t>(at.property + t::property::kind);
    if (!game::name_is(game::read_or<game::Name>(kind), "MapProperty")) return TSUCHI_INVALID;
    const uint64_t kp = game::read_or<uint64_t>(at.property + t::map_property::key);
    const uint64_t vp = game::read_or<uint64_t>(at.property + t::map_property::value);
    if (game::read_or<int32_t>(kp + t::property::element_bytes) != int32_t(key_bytes) ||
        game::read_or<int32_t>(vp + t::property::element_bytes) != int32_t(value_bytes) ||
        !game::writable(at.address, t::native_map::size)) return TSUCHI_INVALID;
    // The engine initializes, hashes and copies both cells using this MapProperty.
    // Its allocator owns every sparse slot and bucket it may grow.
    t::fn::add_map_pair(reinterpret_cast<void*>(at.address), reinterpret_cast<void*>(at.property), key, value);
    return TSUCHI_OK;
}
TSUCHI_Result native_map_get(uint64_t object, const char* path, const void* key, uint32_t key_bytes,
                            void* value, uint32_t value_bytes) {
    game::Place at;
    TSUCHI_Result result = game::find_place(game::type_of(object), object, path, at);
    if (result != TSUCHI_OK) return result;
    const uint64_t kind = game::read_or<uint64_t>(at.property + t::property::kind);
    if (!key || !value || !game::name_is(game::read_or<game::Name>(kind), "MapProperty")) return TSUCHI_INVALID;
    const uint64_t kp = game::read_or<uint64_t>(at.property + t::map_property::key);
    const uint64_t vp = game::read_or<uint64_t>(at.property + t::map_property::value);
    if (game::read_or<int32_t>(kp + t::property::element_bytes) != int32_t(key_bytes) ||
        game::read_or<int32_t>(vp + t::property::element_bytes) != int32_t(value_bytes)) return TSUCHI_INVALID;
    const uint32_t stride = game::read_or<uint32_t>(at.property + t::map_property::stride);
    const uint32_t vo = game::read_or<uint32_t>(at.property + t::map_property::value_offset);
    const uint64_t data = game::read_or<uint64_t>(at.address + t::row_list::slots);
    const int32_t count = game::read_or<int32_t>(at.address + t::row_list::slot_count, -1);
    const int32_t bits = game::read_or<int32_t>(at.address + t::row_list::bit_count, -1);
    const uint64_t far = game::read_or<uint64_t>(at.address + t::row_list::used_bits_far);
    const uint64_t used = far ? far : at.address + t::row_list::used_bits;
    if (count < 0 || count > int32_t(t::constant::max_array_count) || bits < count || vo > stride || value_bytes > stride - vo || stride < key_bytes)
        return TSUCHI_INVALID;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        if (!game::read(used + uint32_t(i / 32) * sizeof word, word)) return TSUCHI_NOT_READY;
        if (!(word & (uint32_t(1) << (i % 32)))) continue;
        const uint64_t cell = data + uint64_t(i) * stride;
        if (!game::readable(cell, stride)) return TSUCHI_NOT_READY;
        if (!memcmp(reinterpret_cast<void*>(cell), key, key_bytes)) {
            memcpy(value, reinterpret_cast<void*>(cell + vo), value_bytes);
            return TSUCHI_OK;
        }
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result native_pikmin_enum(uint64_t object) {
    if (!object || !t::bound(t::fn::set_enum_names)) return TSUCHI_NOT_READY;
    game::ArrayMemory names;
    if (!game::read(object + t::native_enum::names, names) || names.count < int32_t(t::constant::pikmin_stock_enum_names) ||
        names.count > int32_t(t::constant::pikmin_save_colors)) return TSUCHI_UNSUPPORTED;
    struct Pair { game::Name name; uint64_t value; };
    static_assert(sizeof(Pair) == t::enum_name::size, "native enum pair ABI");
    const char* const extra[] = {t::name::pikmin_color_first, t::name::pikmin_color_reserved, t::name::pikmin_color_second};
    Pair rows[t::most::pikmin_save_colors]{};
    if (!game::readable(names.data, uint64_t(names.count) * sizeof(Pair))) return TSUCHI_NOT_READY;
    memcpy(rows, reinterpret_cast<const void*>(names.data), uint64_t(names.count) * sizeof(Pair));
    for (uint32_t i = uint32_t(t::constant::pikmin_stock_enum_names); i < uint32_t(t::constant::pikmin_save_colors); ++i) {
        char text[96];
        const char* suffix = extra[i - t::constant::pikmin_stock_enum_names];
        const size_t prefix = strlen(t::name::pikmin_enum), bytes = strlen(suffix);
        if (prefix + bytes + 3 > sizeof text) return TSUCHI_CAPACITY;
        memcpy(text, t::name::pikmin_enum, prefix);
        text[prefix] = ':', text[prefix + 1] = ':';
        memcpy(text + prefix + 2, suffix, bytes + 1);
        if (i < uint32_t(names.count)) {
            if (rows[i].value != i || !game::name_is(rows[i].name, text)) return TSUCHI_BUSY;
        } else {
            const TSUCHI_Result result = game::name_from_utf8(text, rows[i].name);
            if (result != TSUCHI_OK) return result;
            rows[i].value = i;
        }
    }
    if (names.count == int32_t(t::constant::pikmin_save_colors)) return TSUCHI_OK;
    game::ArrayMemory supplied{reinterpret_cast<uint64_t>(rows), int32_t(t::constant::pikmin_save_colors), int32_t(t::constant::pikmin_save_colors)};
    // Native SetEnums updates its name registry as well as copying the pair array.
    // Keep its C++ form and flags. Do not add another generated MAX enumerator.
    const uint32_t form = game::read_or<uint32_t>(object + t::native_enum::cpp_form);
    const uint32_t flags = game::read_or<uint32_t>(object + t::native_enum::flags);
    return t::fn::set_enum_names(reinterpret_cast<void*>(object), &supplied, form, flags, 0) ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}
}
