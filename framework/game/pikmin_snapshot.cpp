#include "pikmin_snapshot.h"
#include <string.h>
#include "actors.h"
#include "flow.h"
#include "game.h"
#include "objects.h"
#include "reflect.h"
namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t kMaximumNodes = 65536; // Same bounded ring policy as each_listed.
alignas(8) uint8_t scratch[t::pikmin_counter::most];
bool using_scratch = false;
struct Counts { int64_t joined[2][t::most::pikmin_live_leaves]{}, loose[t::most::pikmin_live_leaves]{}; };
void indexed(const char* field, uint32_t index, char path[64]) {
    size_t n = strlen(field); memcpy(path, field, n); path[n++] = '[';
    char digits[10]; unsigned used = 0;
    do { digits[used++] = char('0' + index % 10); index /= 10; } while (index);
    while (used) path[n++] = digits[--used];
    path[n++] = ']'; path[n] = 0;
}
TSUCHI_Result destinations(uint64_t story, game::Place (&cells)[2][2][t::most::pikmin_save_leaves]) {
    const uint64_t type = game::type_of(story);
    game::Place p;
    TSUCHI_Result r = game::find_place(type, story, "StorySummary", p);
    if (r != TSUCHI_OK) return r;
    uint64_t summary_type = 0, summary = 0;
    if ((r = game::struct_at(p, summary_type, summary)) != TSUCHI_OK) return r;
    const char* fields[] = {"FollowPikminNum0", "FollowPikminNum1"};
    const uint32_t colors[] = {uint32_t(t::constant::pikmin_color_first), uint32_t(t::constant::pikmin_color_second)};
    for (unsigned side = 0; side < 2; ++side) {
        game::ArrayMemory outer;
        if ((r = game::find_place(summary_type, summary, fields[side], p)) != TSUCHI_OK ||
            (r = game::array_of(p, outer)) != TSUCHI_OK) return r;
        for (unsigned color = 0; color < 2; ++color) {
            if (outer.count <= int32_t(colors[color])) return TSUCHI_NOT_READY;
            char path[64]; indexed(fields[side], colors[color], path);
            uint64_t row_type = 0, row = 0;
            if ((r = game::find_place(summary_type, summary, path, p)) != TSUCHI_OK ||
                (r = game::struct_at(p, row_type, row)) != TSUCHI_OK ||
                (r = game::find_place(row_type, row, "Num", p)) != TSUCHI_OK) return r;
            game::ArrayMemory inner;
            if ((r = game::array_of(p, inner)) != TSUCHI_OK) return r;
            if (inner.count < int32_t(t::constant::pikmin_save_leaves)) return TSUCHI_NOT_READY;
            for (unsigned leaf = 0; leaf < t::constant::pikmin_save_leaves; ++leaf) {
                indexed("Num", leaf, path);
                auto& cell = cells[side][color][leaf];
                game::Value value;
                if ((r = game::find_place(row_type, row, path, cell)) != TSUCHI_OK ||
                    (r = game::read_value(cell, value)) != TSUCHI_OK) return r;
                if (value.kind != game::ValueKind::integer) return TSUCHI_INVALID;
                if (!game::writable(cell.address, sizeof(int32_t))) return TSUCHI_NOT_READY;
            }
        }
    }
    return TSUCHI_OK;
}
// Classifier registration and child-removal side effects are excluded before
// invoking it. The actor's color, leader, class and state remain the engine's.
bool eligible(uint64_t actor, bool& classify) {
    classify = false;
    if (!game::readable(actor, t::game_actor::snapshot_excluded + 1)) return false;
    if (game::read_or<uint8_t>(actor + t::game_actor::removed, 1) ||
        game::read_or<uint8_t>(actor + t::game_actor::kill_queued, 1)) return true;
    const int32_t rest = game::read_or<int32_t>(actor + t::game_actor::rest, -1);
    if (rest != 0 && rest != int32_t(t::constant::pikmin_snapshot_rest_second)) return true;
    const uint64_t part = game::read_or<uint64_t>(actor + t::game_actor::pikmin);
    if (!part || !game::live(part) || !game::readable(part, t::pikmin_component::size)) return false;
    if (game::read_or<uint8_t>(part + t::pikmin_component::classify_decoration) != t::constant::spawn_decoration_none &&
        game::read_or<uint8_t>(part + t::pikmin_component::classify_skip) != 0) return true;
    const uint64_t ai = game::read_or<uint64_t>(actor + t::game_actor::ai);
    const uint64_t ai_class = game::global<uint64_t>(t::data::pikmin_ai_class);
    const uint64_t part_class = game::global<uint64_t>(t::data::pikmin_component_class);
    if (!ai || !game::live(ai) || !game::readable(ai, t::ai_component::size) ||
        !ai_class || !part_class || !game::live(ai_class) || !game::live(part_class) ||
        !game::derives_from(game::class_of(ai), ai_class) || !game::derives_from(game::class_of(part), part_class)) return false;
    const uint64_t actions = game::read_or<uint64_t>(ai + t::ai_component::actions);
    if (!actions || !game::read_or<uint64_t>(part + t::pikmin_component::classify_ready)) return true;
    if (!game::readable(actions + t::ai_action_root::state, sizeof(uint64_t))) return false;
    const uint64_t state = game::read_or<uint64_t>(actions + t::ai_action_root::state);
    if (state && !game::readable(state + t::ai_state::name, sizeof(uint64_t))) return false;
    const uint64_t child = game::from_weak_reference(ai + t::ai_component::classify_child);
    if (child && !game::read_or<uint8_t>(child + t::game_actor::removed)) return false;
    classify = true;
    return true;
}
int32_t counter(unsigned leaf, unsigned mode, unsigned player) {
    int32_t value;
    memcpy(&value, scratch + (leaf + 1) * t::pikmin_counter::leaf_stride +
           mode * t::pikmin_counter::mode_stride + player * t::pikmin_counter::player_stride, sizeof value);
    return value;
}
int64_t all(unsigned leaf, unsigned mode) {
    int64_t value = 0;
    for (unsigned player = uint32_t(t::constant::pikmin_counter_player_first); player <= uint32_t(t::constant::pikmin_counter_player_last); ++player)
        value += counter(leaf, mode, player);
    return value;
}
bool walk(uint64_t framework, Counts* out, bool subtract_attached) {
    const uint64_t head = framework + t::game_framework::pikmin_list;
    uint64_t node = 0;
    if (!game::read(head + t::actor_node::next, node)) return false;
    for (uint32_t seen = 0; node != head; ++seen) {
        if (!node || seen == kMaximumNodes || !game::readable(node, t::actor_node::size)) return false;
        uint64_t actor = 0, next = 0;
        if (!game::read(node + t::actor_node::actor, actor) || !game::read(node + t::actor_node::next, next) ||
            !actor || !game::live(actor)) return false;
        uint8_t color = 0;
        if (!game::read(actor + t::game_actor::color, color)) return false;
        unsigned slot = 2;
        if (color == t::constant::pikmin_color_first) slot = 0;
        else if (color == t::constant::pikmin_color_second) slot = 1;
        if (slot < 2) {
            bool classify = false;
            if (!eligible(actor, classify)) return false;
            if (out && classify) {
                memset(scratch, 0, sizeof scratch);
                t::fn::pikmin_counter_classify(scratch, reinterpret_cast<void*>(actor));
                for (unsigned leaf = 0; leaf < t::constant::pikmin_live_leaves; ++leaf) {
                    out[slot].joined[0][leaf] += counter(leaf, uint32_t(t::constant::pikmin_counter_joined), uint32_t(t::constant::pikmin_counter_current));
                    out[slot].joined[1][leaf] += counter(leaf, uint32_t(t::constant::pikmin_counter_joined), uint32_t(t::constant::pikmin_counter_partner));
                    out[slot].loose[leaf] += all(leaf, uint32_t(t::constant::pikmin_counter_army)) - all(leaf, uint32_t(t::constant::pikmin_counter_joined))
                        - all(leaf, uint32_t(t::constant::pikmin_counter_pending)) - all(leaf, uint32_t(t::constant::pikmin_counter_excluded))
                        - (subtract_attached ? all(leaf, uint32_t(t::constant::pikmin_counter_attached)) : 0);
                    // Seed/head entries add equally to army and pending counters.
                    // Their contribution cancels in the native loose formula.
                }
            }
        }
        node = next;
    }
    return true;
}
bool attached_is_subtracted(bool& out) {
    const uint64_t manager = game::pause_manager();
    uint64_t counters = 0;
    if (!manager || !game::read(manager + t::pause_manager::counters, counters) || !counters) return false;
    out = true;
    uint32_t mask = uint32_t(t::constant::pikmin_snapshot_pause_mask);
    for (uint32_t index = 0; mask; ++index, mask >>= 1) {
        if (!(mask & 1)) continue;
        int8_t count = 0;
        if (!game::read(counters + index, count)) return false;
        if (count < 1) out = false;
    }
    return true;
}
bool player(uint64_t actor, int32_t& id) {
    id = uint32_t(t::constant::pikmin_counter_player_missing);
    if (!actor) return true;
    uint64_t info = 0;
    return game::live(actor) && game::read(actor + t::game_actor::snapshot_playerinfo, info) && info &&
        game::read(info + t::snapshot_playerinfo::id, id);
}
unsigned side(int32_t player) { return player == int32_t(t::constant::pikmin_snapshot_secondary_player_first) || player == int32_t(t::constant::pikmin_snapshot_secondary_player_second) ? 1 : 0; }
int64_t bounded(int64_t value) { return value < 0 ? 0 : value > int64_t(t::constant::pikmin_count_max) ? int64_t(t::constant::pikmin_count_max) : value; }
}
TSUCHI_Result pikmin_snapshot_follow(uint64_t story, uint64_t include_loose, uint32_t include_photon) {
    (void)include_photon; // The native option excludes only stock color 9.
    if (using_scratch || !game::live(story) || !t::bound(t::fn::pikmin_counter_classify) ||
        !t::bound(t::fn::pikmin_snapshot_current) || !t::bound(t::fn::pikmin_snapshot_partner)) return TSUCHI_NOT_READY;
    const uint64_t context = game::game_context(), framework = game::game_framework();
    uint8_t rule = 0;
    if (!context || !framework || !game::from_weak_reference(context + t::game_context::snapshot_owner) ||
        !game::read(context + t::game_context::rule, rule) ||
        (rule != t::constant::rule_story && rule != t::constant::pikmin_snapshot_rule_second && rule != t::constant::pikmin_snapshot_rule_third)) return TSUCHI_NOT_READY;
    int32_t ids[2];
    void* players = reinterpret_cast<void*>(context + t::game_context::players);
    if (!player(reinterpret_cast<uint64_t>(t::fn::pikmin_snapshot_current(players)), ids[0]) ||
        !player(reinterpret_cast<uint64_t>(t::fn::pikmin_snapshot_partner(players)), ids[1])) return TSUCHI_NOT_READY;
    game::Place cells[2][2][t::most::pikmin_save_leaves];
    TSUCHI_Result r = destinations(story, cells);
    if (r != TSUCHI_OK) return r;
    bool subtract_attached = false;
    if ((include_loose & 1) && !attached_is_subtracted(subtract_attached)) return TSUCHI_NOT_READY;
    // No custom counter changes until the complete ring and all native calls pass.
    if (!walk(framework, nullptr, subtract_attached)) return TSUCHI_NOT_READY;
    using_scratch = true;
    Counts counts[2];
    const bool counted = walk(framework, counts, subtract_attached);
    using_scratch = false;
    if (!counted) return TSUCHI_NOT_READY;
    int64_t values[2][2][t::most::pikmin_save_leaves]{};
    for (unsigned color = 0; color < 2; ++color) for (unsigned leaf = 0; leaf < t::constant::pikmin_live_leaves; ++leaf) {
        for (unsigned player = 0; player < 2; ++player) {
            if (ids[player] == int32_t(t::constant::pikmin_counter_player_missing)) continue;
            values[side(ids[player])][color][leaf] = bounded(values[side(ids[player])][color][leaf] + counts[color].joined[player][leaf]);
        }
        if (include_loose & 1) values[side(ids[0])][color][leaf] = bounded(values[side(ids[0])][color][leaf] + bounded(counts[color].loose[leaf]));
    }
    game::Value value{}; value.kind = game::ValueKind::integer;
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned color = 0; color < 2; ++color) for (unsigned leaf = 0; leaf < t::constant::pikmin_save_leaves; ++leaf) {
            value.integer = values[side][color][leaf];
            if ((r = game::write_value(cells[side][color][leaf], value)) != TSUCHI_OK) return r;
        }
    return TSUCHI_OK;
}
}
