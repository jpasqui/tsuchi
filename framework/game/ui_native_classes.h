#pragma once
#include "game.h"
#include "objects.h"

namespace tsuchi {
namespace ui_native_class_detail {
inline uint64_t native_metaclass() {
    static game::ClassRef ref{"Class"};
    const auto valid = [&] {
        return ref.found && game::from_handle(ref.handle) == ref.found &&
            game::class_of(ref.found) == ref.found && game::is_class(ref.found) &&
            game::name_is(game::name_of(ref.found), ref.name);
    };
    if (!valid()) {
        ref.found = ref.handle = 0;
        game::find_class(ref);
        if (!valid()) { ref.found = ref.handle = 0; return 0; }
    }
    return ref.found;
}
} // namespace ui_native_class_detail
// Seed only the expected native base, never the supplied Blueprint subclass.
// This is a lookup hint: callers must retain their ordinary is_a check, which
// also provides the global lookup fallback when this bounded walk cannot help.
inline bool ui_native_seed_class(game::ClassRef& ref, uint64_t actual_class) {
    const uint64_t metaclass = ui_native_class_detail::native_metaclass();
    if (metaclass && ref.found && game::from_handle(ref.handle) == ref.found &&
        game::class_of(ref.found) == metaclass && game::is_class(ref.found) &&
        game::name_is(game::name_of(ref.found), ref.name)) return true;
    ref.found = ref.handle = 0;
    if (!metaclass) return false;
    for (uint32_t depth = 0; actual_class && depth < 64; ++depth) {
        if (!game::live(actual_class) || !game::is_class(actual_class)) return false;
        if (game::class_of(actual_class) == metaclass && game::name_is(game::name_of(actual_class), ref.name)) {
            const uint64_t handle = game::to_handle(actual_class);
            if (!handle || game::from_handle(handle) != actual_class) return false;
            ref.found = actual_class; ref.handle = handle; return true;
        }
        const uint64_t parent = game::read_or<uint64_t>(actual_class + game::table::struct_type::super);
        if (parent == actual_class) return false;
        actual_class = parent;
    }
    return false;
}
} // namespace tsuchi
