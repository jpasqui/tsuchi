#include "pikmin_counts.h"
#include <string.h>
#include "actors.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi::game {
namespace {
namespace t = table;
// One row of the counter: four leaves (0 is unused), each with its states and players.
uint32_t cells() { return uint32_t(t::pikmin_counter_row::size / sizeof(int32_t)); }
uint32_t leaf_cells() { return uint32_t(t::pikmin_counter::leaf_stride / sizeof(int32_t)); }
uint32_t state_cells() { return uint32_t(t::pikmin_counter::mode_stride / sizeof(int32_t)); }
uint32_t state_count() { return uint32_t(t::pikmin_counter::leaf_stride / t::pikmin_counter::mode_stride); }
uint32_t player_count() { return uint32_t(t::pikmin_counter::mode_stride / t::pikmin_counter::player_stride); }
uint32_t leaf_count() { return cells() / leaf_cells(); }
// The rows have to add up as this code reads them. A build whose rows do not is left uncounted.
bool layout_holds() {
    return t::pikmin_counter::leaf_stride && t::pikmin_counter::mode_stride && t::pikmin_counter::player_stride &&
           cells() == leaf_count() * leaf_cells() && leaf_cells() == state_count() * state_cells() && state_cells() == player_count() &&
           t::pikmin_counter_row::size * 17 == t::pikmin_counter::bank_stride;
}
uint8_t color_at(int index) { return uint8_t(index ? t::constant::pikmin_color_second : t::constant::pikmin_color_first); }
struct Row { int32_t cell[t::pikmin_counter_row::most / sizeof(int32_t)]; };

bool enabled = false;
// Each added color's share of row 0, in each of the counter's two banks. The game writes one bank
// while the HUD reads the other, and swaps them as it recounts.
Row own[2][2]{};
// What the game added during the recount that is running.
Row pending[2][2]{};
bool counted[2]{};
uint64_t recounting = 0, recount_thread = 0;
uint8_t scope_color = 0;
uint64_t scope_thread = 0;

decltype(t::code_site::pikmin_counter_update)::Function update_before = nullptr;
decltype(t::code_site::pikmin_counter_add)::Function add_before = nullptr;
decltype(t::code_site::pikmin_counter_query)::Function query_before = nullptr;
uint64_t hook_ids[3]{};

int index_of(uint8_t color) {
    for (int i = 0; i < 2; ++i) if (color_at(i) == color) return i;
    return -1;
}
uint64_t counters() {
    const uint64_t framework = game_framework();
    return framework ? read_or<uint64_t>(framework + t::game_framework::pikmin_counters) : 0;
}
uint64_t row_zero(uint64_t object, unsigned bank) { return object + bank * uint64_t(t::pikmin_counter::bank_stride); }
bool copy_row(uint64_t address, Row& out) {
    if (!readable(address, t::pikmin_counter_row::size)) return false;
    memset(out.cell, 0, sizeof out.cell);
    memcpy(out.cell, reinterpret_cast<const void*>(address), t::pikmin_counter_row::size);
    return true;
}
// The Onion's state never comes from the per-Pikmin count, so a change there is not a recount.
bool same_outside_onion(const Row& a, const Row& b) {
    for (uint32_t cell = 0; cell < cells(); ++cell) {
        if ((cell % leaf_cells()) / state_cells() == t::constant::pikmin_counter_onion) continue;
        if (a.cell[cell] != b.cell[cell]) return false;
    }
    return true;
}
// The counter query's own rule: 0 for a leaf means leaves 1 to 3, 0 for a state means the
// field and the Onion, 0 for a player means players 1 to 5.
bool in_range(int32_t leaf, int32_t state, int32_t player) {
    return leaf >= 0 && uint32_t(leaf) < leaf_count() && state >= 0 && uint32_t(state) < state_count() && player >= 0 && uint32_t(player) < player_count();
}
int64_t sum(const Row& row, int32_t leaf, int32_t state, int32_t player) {
    int64_t total = 0;
    const uint32_t counted[] = {uint32_t(t::constant::pikmin_counter_field), uint32_t(t::constant::pikmin_counter_onion)};
    for (uint32_t l = leaf ? uint32_t(leaf) : 1; l <= (leaf ? uint32_t(leaf) : leaf_count() - 1); ++l)
        for (unsigned s = 0; s < (state ? 1u : 2u); ++s)
            for (uint32_t p = player ? uint32_t(player) : 1; p <= (player ? uint32_t(player) : player_count() - 1); ++p)
                total += row.cell[l * leaf_cells() + (state ? uint32_t(state) : counted[s]) * state_cells() + p];
    return total;
}
// The added color's Onion stock, from the story's own rows, where the game keeps the
// stock colors' stock that it copies into the Onion state of the counter.
int64_t onion(uint8_t color, int32_t leaf, int32_t state, int32_t player) {
    if ((state && uint32_t(state) != t::constant::pikmin_counter_onion) ||
        (player && uint32_t(player) != t::constant::pikmin_counter_player_first)) return 0;
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    const uint64_t story = instance ? read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    if (!story || !live(story)) return 0;
    const uint64_t rows = story + t::story_save::onyon_leaves;
    uint64_t data = 0, leaves = 0;
    int32_t count = 0, inner = 0;
    if (!read(rows + t::array::data, data) || !read(rows + t::array::count, count) || count <= int32_t(color) ||
        count > int32_t(t::constant::max_array_count) ||
        !read(data + uint64_t(color) * t::array::size + t::array::data, leaves) ||
        !read(data + uint64_t(color) * t::array::size + t::array::count, inner) || inner < int32_t(leaf_count() - 1)) return 0;
    int64_t total = 0;
    for (uint32_t l = leaf ? uint32_t(leaf) : 1; l <= (leaf ? uint32_t(leaf) : leaf_count() - 1); ++l) {
        int32_t value = 0;
        if (read(leaves + (l - 1) * sizeof(int32_t), value) && value > 0) total += value;
    }
    return total;
}
int32_t bounded(int64_t value) {
    return value < 0 ? 0 : value > int64_t(0x7FFFFFFF) ? int32_t(0x7FFFFFFF) : int32_t(value);
}

void update(void* counter, uint64_t flags) {
    const uint64_t object = reinterpret_cast<uint64_t>(counter);
    Row before[2];
    if (!enabled || !object || object != counters() || !copy_row(row_zero(object, 0), before[0]) || !copy_row(row_zero(object, 1), before[1])) {
        update_before(counter, flags);
        return;
    }
    memset(pending, 0, sizeof pending);
    counted[0] = counted[1] = false;
    recounting = object, recount_thread = known_game_thread_token();
    update_before(counter, flags);
    recounting = 0, recount_thread = 0;
    for (unsigned bank = 0; bank < 2; ++bank) {
        Row after;
        // The game clears a bank before it counts into it, so a bank it counted holds this pass alone.
        if (counted[bank]) memcpy(own[bank], pending[bank], sizeof own[bank]);
        else if (!copy_row(row_zero(object, bank), after) || !same_outside_onion(before[bank], after)) memset(own[bank], 0, sizeof own[bank]);
    }
}
void add(void* counter, void* actor) {
    const uint64_t object = reinterpret_cast<uint64_t>(counter);
    uint8_t color = 0;
    int which = -1;
    if (!enabled || !object || object != counters() || !read(reinterpret_cast<uint64_t>(actor) + t::game_actor::color, color) ||
        (which = index_of(color)) < 0) {
        add_before(counter, actor);
        return;
    }
    const unsigned bank = read_or<int32_t>(object + t::pikmin_counter::bank) ? 1 : 0;
    Row before, after;
    const bool seen = copy_row(row_zero(object, bank), before);
    add_before(counter, actor);
    if (!seen || !copy_row(row_zero(object, bank), after)) return;
    const uint64_t thread = known_game_thread_token();
    const bool in_recount = recounting == object && recount_thread == thread;
    Row& into = in_recount ? pending[bank][which] : own[bank][which];
    if (in_recount) counted[bank] = true;
    for (uint32_t cell = 0; cell < cells(); ++cell) into.cell[cell] += after.cell[cell] - before.cell[cell];
}
int32_t query(void* counter, int32_t row, int32_t leaf, int32_t state, int32_t player) {
    const int32_t stock = query_before(counter, row, leaf, state, player);
    const uint64_t object = reinterpret_cast<uint64_t>(counter);
    if (!enabled || row != 0 || !in_range(leaf, state, player) || !object || object != counters()) return stock;
    const unsigned bank = read_or<int32_t>(object + t::pikmin_counter::read_bank) == 1 ? 1 : 0;
    const uint64_t thread = known_game_thread_token();
    const int scoped = scope_color && thread && scope_thread == thread ? index_of(scope_color) : -1;
    if (scoped >= 0) return bounded(sum(own[bank][scoped], leaf, state, player) + onion(scope_color, leaf, state, player));
    int64_t total = stock;
    for (int i = 0; i < 2; ++i) total += sum(own[bank][i], leaf, state, player) + onion(color_at(i), leaf, state, player);
    return bounded(total);
}
} // namespace

TSUCHI_Result pikmin_counts_start(TSUCHI_Mod* self) {
    if (!layout_holds()) return TSUCHI_UNSUPPORTED;
    TSUCHI_Result r = TSUCHI_OK;
    if (!update_before) r = hook_site(self, t::code_site::pikmin_counter_update, &update, &update_before, &hook_ids[0]);
    if (r == TSUCHI_OK && !add_before) r = hook_site(self, t::code_site::pikmin_counter_add, &add, &add_before, &hook_ids[1]);
    if (r == TSUCHI_OK && !query_before) r = hook_site(self, t::code_site::pikmin_counter_query, &query, &query_before, &hook_ids[2]);
    return r;
}
void pikmin_counts_enable(bool on) {
    if (on == enabled) return;
    memset(own, 0, sizeof own);
    enabled = on;
}
int64_t pikmin_counts_color(uint8_t color, int32_t leaf, int32_t state, int32_t player) {
    const int which = index_of(color);
    const uint64_t object = counters();
    if (!enabled || which < 0 || !in_range(leaf, state, player) || !object) return -1;
    const unsigned bank = read_or<int32_t>(object + t::pikmin_counter::read_bank) == 1 ? 1 : 0;
    return sum(own[bank][which], leaf, state, player) + onion(color, leaf, state, player);
}
PikminCountScope::PikminCountScope(uint8_t color) : outer_color_(scope_color), outer_thread_(scope_thread) {
    scope_color = color, scope_thread = known_game_thread_token();
}
PikminCountScope::~PikminCountScope() { scope_color = outer_color_, scope_thread = outer_thread_; }
} // namespace tsuchi::game
