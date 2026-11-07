#include "pikmin_save.h"
#include <string.h>
#include "game.h"
#include "reflect.h"
namespace tsuchi {
namespace {
namespace t = game::table;
void indexed(const char* field, uint32_t index, char path[128]) {
    size_t used = strlen(field);
    memcpy(path, field, used);
    path[used++] = '[';
    char digits[10];
    uint32_t count = 0;
    do { digits[count++] = char('0' + index % 10); index /= 10; } while (index);
    while (count) path[used++] = digits[--count];
    path[used++] = ']';
    path[used] = 0;
}
TSUCHI_Result grow(const game::Place& place, uint32_t minimum, game::ArrayMemory& array) {
    TSUCHI_Result r = game::array_of(place, array);
    if (r != TSUCHI_OK || uint32_t(array.count) >= minimum) return r;
    if ((r = game::resize(place, minimum)) != TSUCHI_OK) return r;
    return game::array_of(place, array);
}
TSUCHI_Result numbers(uint64_t type, uint64_t base, const char* field, uint32_t minimum, bool clear) {
    game::Place place;
    TSUCHI_Result r = game::find_place(type, base, field, place);
    if (r != TSUCHI_OK) return r;
    game::ArrayMemory array;
    if ((r = grow(place, minimum, array)) != TSUCHI_OK) return r;
    if (!clear) return TSUCHI_OK;
    for (uint32_t i = 0; i < uint32_t(array.count); ++i) {
        char path[128]; indexed(field, i, path);
        if ((r = game::find_place(type, base, path, place)) != TSUCHI_OK) return r;
        game::Value value{}; value.kind = game::ValueKind::integer;
        if ((r = game::write_value(place, value)) != TSUCHI_OK) return r;
    }
    return TSUCHI_OK;
}
TSUCHI_Result colors(uint64_t type, uint64_t base, const char* field, const char* inner, uint32_t minimum, bool clear) {
    game::Place place;
    TSUCHI_Result r = game::find_place(type, base, field, place);
    if (r != TSUCHI_OK) return r;
    game::ArrayMemory array;
    if ((r = grow(place, uint32_t(t::constant::pikmin_save_colors), array)) != TSUCHI_OK) return r;
    for (uint32_t color = 0; color < uint32_t(array.count); ++color) {
        char path[128]; indexed(field, color, path);
        if ((r = game::find_place(type, base, path, place)) != TSUCHI_OK) return r;
        uint64_t row_type = 0, row = 0;
        if ((r = game::struct_at(place, row_type, row)) != TSUCHI_OK) return r;
        if ((r = numbers(row_type, row, inner, minimum, clear && color >= t::constant::pikmin_stock_save_colors)) != TSUCHI_OK) return r;
    }
    return TSUCHI_OK;
}
TSUCHI_Result result_at(uint64_t type, uint64_t base, const char* field, bool clear) {
    game::Place place;
    TSUCHI_Result r = game::find_place(type, base, field, place);
    if (r != TSUCHI_OK) return r;
    uint64_t row_type = 0, row = 0;
    if ((r = game::struct_at(place, row_type, row)) != TSUCHI_OK) return r;
    return pikmin_save_result(row_type, row, clear);
}
TSUCHI_Result flags(uint64_t type, uint64_t base, const char* field, bool clear) {
    game::Place place;
    TSUCHI_Result r = game::find_place(type, base, field, place);
    if (r != TSUCHI_OK) return r;
    game::ArrayMemory array;
    if ((r = grow(place, uint32_t(t::constant::pikmin_save_colors), array)) != TSUCHI_OK) return r;
    if (!clear) return TSUCHI_OK;
    for (uint32_t color = uint32_t(t::constant::pikmin_stock_save_colors); color < uint32_t(array.count); ++color) {
        char path[128]; indexed(field, color, path);
        if ((r = game::find_place(type, base, path, place)) != TSUCHI_OK) return r;
        uint64_t row_type = 0, row = 0;
        if ((r = game::struct_at(place, row_type, row)) != TSUCHI_OK) return r;
        if ((r = game::find_place(row_type, row, "bFlag", place)) != TSUCHI_OK) return r;
        game::Value value{}; value.kind = game::ValueKind::boolean;
        if ((r = game::write_value(place, value)) != TSUCHI_OK) return r;
    }
    return TSUCHI_OK;
}
}
TSUCHI_Result pikmin_save_result(uint64_t type, uint64_t base, bool clear_custom) {
    if (!type || !base) return TSUCHI_NOT_READY;
    const char* const fields[] = {"UnionPikminNum", "DeadPikminNum"};
    for (const char* field : fields) {
        const TSUCHI_Result r = colors(type, base, field, "Num", uint32_t(t::constant::pikmin_save_leaves), clear_custom);
        if (r != TSUCHI_OK) return r;
    }
    return TSUCHI_OK;
}
TSUCHI_Result pikmin_save_summary(uint64_t type, uint64_t base, bool clear_custom) {
    if (!type || !base) return TSUCHI_NOT_READY;
    const char* const leaves[] = {"FollowPikminNum0", "FollowPikminNum1", "OnyonPikminNum"};
    for (const char* field : leaves) {
        const TSUCHI_Result r = colors(type, base, field, "Num", uint32_t(t::constant::pikmin_save_leaves), clear_custom);
        if (r != TSUCHI_OK) return r;
    }
    const char* const decorations[] = {"OnyonDecoPikminNum", "OutSideOnyonDecoPikminNum"};
    for (const char* field : decorations) {
        const TSUCHI_Result r = colors(type, base, field, "DecoNum", uint32_t(t::constant::pikmin_save_decorations), clear_custom);
        if (r != TSUCHI_OK) return r;
    }
    return result_at(type, base, "TotalStoryResultInfo", clear_custom);
}
TSUCHI_Result pikmin_save_story(uint64_t story, bool clear_custom) {
    const uint64_t type = story ? game::type_of(story) : 0;
    if (!type) return TSUCHI_NOT_READY;
    const char* const summaries[] = {"StorySummary", "PreviousStorySummary"};
    for (const char* field : summaries) {
        game::Place place;
        TSUCHI_Result r = game::find_place(type, story, field, place);
        if (r != TSUCHI_OK) return r;
        uint64_t row_type = 0, row = 0;
        if ((r = game::struct_at(place, row_type, row)) != TSUCHI_OK) return r;
        if ((r = pikmin_save_summary(row_type, row, clear_custom)) != TSUCHI_OK) return r;
    }
    game::Place place;
    TSUCHI_Result r = game::find_place(type, story, "LocalStoryResultInfo", place);
    if (r != TSUCHI_OK) return r;
    game::ArrayMemory array;
    if ((r = game::array_of(place, array)) != TSUCHI_OK) return r;
    // This list records results rather than colors. Keep its loaded length.
    for (uint32_t i = 0; i < uint32_t(array.count); ++i) {
        char path[128]; indexed("LocalStoryResultInfo", i, path);
        if ((r = result_at(type, story, path, clear_custom)) != TSUCHI_OK) return r;
    }
    const char* const fields[] = {"UnionPikmin", "UniteOnyon"};
    for (const char* field : fields)
        if ((r = flags(type, story, field, clear_custom)) != TSUCHI_OK) return r;
    return TSUCHI_OK;
}
TSUCHI_Result pikmin_save_clear_follow(uint64_t story) {
    const uint64_t type = story ? game::type_of(story) : 0;
    if (!type) return TSUCHI_NOT_READY;
    game::Place place;
    TSUCHI_Result r = game::find_place(type, story, "StorySummary", place);
    if (r != TSUCHI_OK) return r;
    uint64_t summary_type = 0, summary = 0;
    if ((r = game::struct_at(place, summary_type, summary)) != TSUCHI_OK) return r;
    game::Place counters[2 * 2 * t::most::pikmin_save_leaves];
    unsigned used = 0;
    const char* const fields[] = {"FollowPikminNum0", "FollowPikminNum1"};
    const uint32_t colors[] = {uint32_t(t::constant::pikmin_color_first), uint32_t(t::constant::pikmin_color_second)};
    // Resolve every counter before clearing any. A short or malformed saved
    // array never turns a partially checked snapshot into an empty squad.
    for (const char* field : fields) {
        game::ArrayMemory outer;
        if ((r = game::find_place(summary_type, summary, field, place)) != TSUCHI_OK ||
            (r = game::array_of(place, outer)) != TSUCHI_OK) return r;
        for (uint32_t color : colors) {
            if (outer.count <= int32_t(color)) return TSUCHI_NOT_READY;
            char path[128]; indexed(field, color, path);
            if ((r = game::find_place(summary_type, summary, path, place)) != TSUCHI_OK) return r;
            uint64_t row_type = 0, row = 0;
            if ((r = game::struct_at(place, row_type, row)) != TSUCHI_OK ||
                (r = game::find_place(row_type, row, "Num", place)) != TSUCHI_OK) return r;
            game::ArrayMemory inner;
            if ((r = game::array_of(place, inner)) != TSUCHI_OK) return r;
            if (inner.count < int32_t(t::constant::pikmin_save_leaves)) return TSUCHI_NOT_READY;
            for (uint32_t leaf = 0; leaf < t::constant::pikmin_save_leaves; ++leaf) {
                indexed("Num", leaf, path);
                if ((r = game::find_place(row_type, row, path, counters[used])) != TSUCHI_OK) return r;
                game::Value value;
                if ((r = game::read_value(counters[used], value)) != TSUCHI_OK) return r;
                if (value.kind != game::ValueKind::integer) return TSUCHI_INVALID;
                ++used;
            }
        }
    }
    game::Value zero{}; zero.kind = game::ValueKind::integer;
    for (unsigned i = 0; i < used; ++i)
        if ((r = game::write_value(counters[i], zero)) != TSUCHI_OK) return r;
    return TSUCHI_OK;
}
}
