#include <string.h>
#include "content_series.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "reflect.h"
namespace tsuchi {
namespace {
namespace t = game::table;
struct Kept { uint64_t row; };
struct Wanted { uint64_t row; };
bool array(uint64_t object, game::ArrayMemory& out) {
    return game::read(object + t::content_series_manager::rows, out) && out.count >= 0 && out.max >= out.count &&
           uint32_t(out.max) <= t::constant::max_array_count && game::writable(object + t::content_series_manager::rows, sizeof out) &&
           (!out.count || (out.data && game::readable(out.data, uint64_t(out.count) * sizeof(uint64_t))));
}
TSUCHI_Result insert(const Hold& hold, uint64_t row) {
    const uint64_t object = game::from_handle(hold.object);
    if (!object || !game::readable(row + t::content_series_row::series, sizeof(uint32_t))) return TSUCHI_NOT_READY;
    game::ArrayMemory current;
    if (!array(object, current)) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < current.count; ++i) {
        const uint64_t pointer = game::read_or<uint64_t>(current.data + uint64_t(i) * 8);
        if (pointer == row) return TSUCHI_OK;
        uint32_t series = 0;
        if (!pointer || !game::read(pointer + t::content_series_row::series, series)) return TSUCHI_NOT_READY;
        if (series == hold.what) return TSUCHI_BUSY;
    }
    if (uint32_t(current.count) == t::constant::max_array_count) return TSUCHI_CAPACITY;
    game::ArrayMemory next = current;
    if (current.count == current.max) {
        const uint32_t more = uint32_t(current.count) + 1;
        next.data = reinterpret_cast<uint64_t>(game::engine_memory(uint64_t(more) * 8));
        if (!next.data) return TSUCHI_NO_MEMORY;
        next.max = int32_t(more);
    } else if (!game::writable(current.data + uint64_t(current.count) * 8, 8)) return TSUCHI_NOT_READY;
    game::ArrayMemory after;
    if (!hold_call_guard(hold).permits() || !array(object, after) || after.data != current.data ||
        after.count != current.count || after.max != current.max) {
        if (next.data != current.data) game::release_engine_memory(reinterpret_cast<void*>(next.data));
        return TSUCHI_NOT_READY;
    }
    if (next.data != current.data && current.count) memcpy(reinterpret_cast<void*>(next.data), reinterpret_cast<const void*>(current.data), size_t(current.count) * 8);
    hold_apply_checkpoint(hold);
    memcpy(reinterpret_cast<void*>(next.data + uint64_t(current.count) * 8), &row, sizeof row);
    ++next.count;
    if (!game::write(object + t::content_series_manager::rows, next)) {
        if (next.data != current.data) game::release_engine_memory(reinterpret_cast<void*>(next.data));
        return TSUCHI_NOT_READY;
    }
    if (next.data != current.data && current.data) game::release_engine_memory(reinterpret_cast<void*>(current.data));
    return TSUCHI_OK;
}
TSUCHI_Result apply(const Hold& hold, Kept& kept, const Wanted& wanted, bool fresh) {
    if (!fresh && kept.row != wanted.row) return TSUCHI_BUSY;
    kept.row = wanted.row;
    return insert(hold, wanted.row);
}
bool keep(const Hold& hold, Kept& kept) { return insert(hold, kept.row) != TSUCHI_BUSY; }
void restore(const Hold& hold, Kept& kept, HoldEnd why) {
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    const uint64_t object = game::from_handle(hold.object);
    game::ArrayMemory current;
    if (!object || !array(object, current) || !game::writable(current.data, uint64_t(current.count) * 8)) return;
    for (int32_t i = 0; i < current.count; ++i) {
        if (game::read_or<uint64_t>(current.data + uint64_t(i) * 8) != kept.row) continue;
        for (int32_t j = i + 1; j < current.count; ++j) {
            const uint64_t row = game::read_or<uint64_t>(current.data + uint64_t(j) * 8);
            game::write(current.data + uint64_t(j - 1) * 8, row);
        }
        --current.count;
        game::write(current.data + uint64_t(current.count) * 8, uint64_t(0));
        game::write(object + t::content_series_manager::rows, current);
        break;
    }
}
Holds<Kept, Wanted, apply, keep, restore> series_holds{"content.series_bonus", HoldOn::object, true};
}
TSUCHI_Result content_series_hold(TSUCHI_Mod* mod, uint64_t manager, uint64_t row, uint32_t series) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    uint32_t row_series = 0;
    if (!game::read(row + t::content_series_row::series, row_series)) return TSUCHI_NOT_READY;
    if (row_series != series) return TSUCHI_INVALID;
    const uint64_t handle = game::to_handle(manager);
    if (!handle) return TSUCHI_NOT_READY;
    return series_holds.take(mod, handle, series, Wanted{row});
}
}
