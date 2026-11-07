// A saved copy put back into the game's current save of its kind, the way the game retries a day.
//
// The game instance holds its current saves, one object of each kind. The game keeps places in them
// outside them: the quest manager keeps the current area's records in the story save's maps. When
// the game puts a day back, it keeps its objects: each current object takes the copy's data with its
// own copy function and then applies itself, and applying finds those places again. Setting new
// objects in the game instance instead left the places in the old objects, which the engine freed.
#include "save_restore.h"

#include "flow.h"
#include "game.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef save_class{t::name::save_class};

using CopyFrom = void (*)(uint64_t save, uint64_t copy);
using Apply = void (*)(uint64_t save);

// An entry of an object's function table, or 0 when it is not in the game's code.
uint64_t entry(uint64_t object, uint64_t offset) {
    const uint64_t table = game::read_or<uint64_t>(object);
    const uint64_t function = table ? game::read_or<uint64_t>(table + offset) : 0;
    return function && game::executable(function) ? function : 0;
}
} // namespace

TSUCHI_Result restore_save(uint64_t copy_handle) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    const uint64_t copy = game::from_handle(copy_handle);
    if (!copy || !game::live(copy) || !game::is_a(copy, save_class)) return TSUCHI_INVALID;
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    if (!instance || !game::live(instance)) return TSUCHI_NOT_READY;
    if (game::busy_with_saves_or_scenes()) return TSUCHI_BUSY;
    // The current save of the copy's kind: the one of the game instance's saves with the copy's class.
    const uint64_t kind = game::class_of(copy);
    uint64_t current = 0;
    for (uint32_t i = 0; i < t::constant::save_kinds && !current; ++i) {
        const uint64_t save = game::read_or<uint64_t>(instance + t::game_instance::saves + 8ull * i);
        if (save && game::live(save) && game::class_of(save) == kind) current = save;
    }
    if (!current) return TSUCHI_NOT_FOUND;
    if (current == copy) return TSUCHI_INVALID;
    const uint64_t copy_from = entry(current, t::save_table::copy_from), apply = entry(current, t::save_table::apply);
    if (!copy_from || !apply) return TSUCHI_UNSUPPORTED;
    reinterpret_cast<CopyFrom>(copy_from)(current, copy);
    reinterpret_cast<Apply>(apply)(current);
    return TSUCHI_OK;
}

} // namespace tsuchi
