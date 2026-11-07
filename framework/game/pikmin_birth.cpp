#include "pikmin_birth.h"
#include "pikmin_content.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include <string.h>
namespace tsuchi::game {
namespace {
namespace t = table;
decltype(t::code_site::pikmin_restore_batch)::Function next_batch = nullptr;
decltype(t::code_site::pikmin_restore_step)::Function next_step = nullptr;
struct Scope { uint64_t state; int32_t primary_flowers[2]{}; };
Scope* active = nullptr;
uint64_t batch_hook = 0, step_hook = 0;

bool array(uint64_t header, uint32_t minimum, uint32_t stride, uint64_t& data) {
    int32_t count = -1, capacity = -1;
    if (!read(header + t::array::data, data) || !read(header + t::array::count, count) || !read(header + t::array::max, capacity) ||
        count < int32_t(minimum) || count > int32_t(t::constant::max_array_count) || capacity < count ||
        !readable(data, uint64_t(count) * stride)) return false;
    return true;
}
bool row(uint64_t header, uint32_t color, uint32_t minimum, uint64_t& data) {
    uint64_t rows = 0;
    return array(header, uint32_t(t::constant::pikmin_save_colors), t::array::size, rows) &&
           array(rows + uint64_t(color) * t::array::size, minimum, sizeof(int32_t), data);
}
bool counts(uint64_t data) {
    for (uint32_t leaf = 0; leaf < 3; ++leaf) {
        int32_t count = -1;
        if (!read(data + leaf * sizeof(int32_t), count) || count < 0 || count > int32_t(t::constant::pikmin_count_max)) return false;
    }
    return true;
}
bool finite(float value) { uint32_t bits = 0; memcpy(&bits, &value, sizeof bits); return (bits & 0x7F800000u) != 0x7F800000u; }
uint32_t draw(uint32_t rng[4]) {
    const uint32_t first = rng[0] ^ (rng[0] << 11);
    const uint32_t next = rng[3] ^ (rng[3] >> 19) ^ first ^ (first >> 8);
    rng[0] = rng[1]; rng[1] = rng[2]; rng[2] = rng[3]; rng[3] = next;
    return next;
}
float unit(uint32_t draw) {
    const uint32_t bits = 0x3F800000u | (draw >> 9);
    float value = 0; memcpy(&value, &bits, sizeof value);
    return value - 1.0f;
}
struct Scatter { uint32_t rng[4]; float position[3]; uint64_t rng_at; };
bool scatter(uint64_t state, Scatter& out) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    if (!t::bound(t::fn::pikmin_scatter_cos) || !t::bound(t::fn::pikmin_scatter_sin)) return false;
    uint64_t config = global<uint64_t>(t::data::pikmin_scatter_override);
    if (!live(config)) config = global<uint64_t>(t::data::pikmin_scatter_config);
    float minimum = 0, maximum = 0, origin[3]{};
    if (!live(config) || !read(config + t::pikmin_scatter::minimum, minimum) || !read(config + t::pikmin_scatter::maximum, maximum) ||
        !finite(minimum) || !finite(maximum) || minimum < 0 || maximum < minimum ||
        !readable(state + t::pikmin_birth::origin, sizeof origin) || !writable(state + t::pikmin_birth::position, sizeof origin)) return false;
    memcpy(origin, reinterpret_cast<const void*>(state + t::pikmin_birth::origin), sizeof origin);
    for (float value : origin) if (!finite(value)) return false;
    out.rng_at = main_base() + t::data::random_stream;
    if (!readable(out.rng_at, sizeof out.rng) || !writable(out.rng_at, sizeof out.rng)) return false;
    memcpy(out.rng, reinterpret_cast<const void*>(out.rng_at), sizeof out.rng);
    const float angle_unit = unit(draw(out.rng));
    const uint32_t tau_bits = uint32_t(t::constant::pikmin_scatter_tau_bits);
    float tau = 0; memcpy(&tau, &tau_bits, sizeof tau);
    const float angle = angle_unit * tau;
    const float radius_unit = unit(draw(out.rng));
    const float radius_range = maximum - minimum;
    const float radius_part = radius_unit * radius_range;
    const float radius = minimum + radius_part;
    const float x = t::fn::pikmin_scatter_cos(angle) * radius;
    const float y = t::fn::pikmin_scatter_sin(angle) * radius;
    out.position[0] = origin[0] + x; out.position[1] = origin[1] + y; out.position[2] = origin[2];
    return finite(out.position[0]) && finite(out.position[1]);
}
int32_t clamp(int64_t value) {
    return value < 0 ? 0 : value > int64_t(t::constant::pikmin_count_max) ? int32_t(t::constant::pikmin_count_max) : int32_t(value);
}
int32_t custom(uint64_t state, uint64_t remaining[2], uint64_t initial[2]) {
    if (!active || active->state != state || !t::bound(t::fn::eject_pikmin)) return 0;
    const uint64_t context = global<uint64_t>(t::data::game_context);
    uint8_t rule = 0xFF;
    if (!from_weak_reference(context + t::game_context::snapshot_owner) || !read(context + t::game_context::rule, rule) ||
        rule == uint8_t(t::constant::pikmin_restore_refused_rule_first) || rule == uint8_t(t::constant::pikmin_restore_refused_rule_second)) return 0;
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    const uint64_t story = instance ? read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    uint8_t onion = 0, alone = 0;
    uint64_t sublevel = 0;
    if (!live(story) || !readable(state + t::pikmin_birth::transform, sizeof(float) * 12) || !read(state + t::pikmin_birth::onion, onion) || !read(state + t::pikmin_birth::alone, alone) ||
        !read(state + t::pikmin_birth::sublevel, sublevel)) return 0;
    const uint8_t colors[] = {uint8_t(t::constant::pikmin_color_first), uint8_t(t::constant::pikmin_color_second)};
    const uint64_t leaders[] = {t::pikmin_birth::leader_first, t::pikmin_birth::leader_second};
    for (uint32_t side = 0; side < 2; ++side) {
        const uint64_t leader = read_or<uint64_t>(state + leaders[side]);
        if (!live(leader)) continue;
        for (uint32_t index = 0; index < 2; ++index) {
            const uint8_t color = colors[index];
            uint64_t left = 0, start = 0;
            if (!row(remaining[side], color, 3, left) || !row(initial[side], color, 3, start) || !counts(left) || !counts(start)) continue;
            int32_t leaf = 2;
            for (; leaf >= 0 && read_or<int32_t>(left + uint32_t(leaf) * sizeof(int32_t)) == 0; --leaf) {}
            if (leaf < 0) continue;
            const uint64_t source = left + uint32_t(leaf) * sizeof(int32_t);
            if (!writable(source, sizeof(int32_t)) || tsuchi::added_pikmin_native_ready(color) != TSUCHI_OK) continue;
            uint64_t stored = 0, outside = 0, leaves = 0;
            if (!row(story + t::story_save::onyon_deco, color, uint32_t(t::constant::spawn_decoration_none), stored) ||
                !row(story + t::story_save::outside_deco, color, uint32_t(t::constant::spawn_decoration_none), outside)) continue;
            if (onion && (!row(story + t::story_save::onyon_leaves, color, 3, leaves) ||
                          !writable(leaves + uint32_t(leaf) * sizeof(int32_t), sizeof(int32_t)) ||
                          !writable(stored, t::constant::spawn_decoration_none * sizeof(int32_t)) ||
                          !writable(outside, t::constant::spawn_decoration_none * sizeof(int32_t)))) continue;
            Scatter position{};
            if (!scatter(state, position)) continue;
            uint32_t decoration = uint32_t(t::constant::spawn_decoration_none);
            if (onion) {
                if (!t::bound(t::fn::pikmin_deco_stored)) continue;
                decoration = t::fn::pikmin_deco_stored(reinterpret_cast<void*>(story + t::story_save::summary), color, 0);
                if (decoration > t::constant::spawn_decoration_none) continue;
            } else if (leaf == 2) {
                if (!t::bound(t::fn::pikmin_deco_field)) continue;
                int64_t ordinal = int64_t(read_or<int32_t>(start + uint32_t(leaf) * sizeof(int32_t))) - read_or<int32_t>(source);
                if (side) ordinal += active->primary_flowers[index];
                if (ordinal < 0 || ordinal > int64_t(t::constant::pikmin_count_max)) continue;
                decoration = t::fn::pikmin_deco_field(reinterpret_cast<void*>(story + t::story_save::summary), color, int32_t(ordinal));
                if (decoration > t::constant::spawn_decoration_none) continue;
            }
            // All custom storage and readiness checks precede consumption.
            if (onion && decoration < t::constant::spawn_decoration_none) {
                const uint64_t saved = stored + decoration * sizeof(int32_t), field = outside + decoration * sizeof(int32_t);
                const int32_t count = read_or<int32_t>(saved);
                if (count > 0) {
                    write(saved, clamp(int64_t(count) - 1));
                    write(field, clamp(int64_t(read_or<int32_t>(field)) + 1));
                }
            }
            memcpy(reinterpret_cast<void*>(position.rng_at), position.rng, sizeof position.rng);
            memcpy(reinterpret_cast<void*>(state + t::pikmin_birth::position), position.position, sizeof position.position);
            write(source, read_or<int32_t>(source) - 1);
            if (!onion && side == 0 && leaf == 2) ++active->primary_flowers[index];
            t::fn::eject_pikmin(reinterpret_cast<void*>(leader), reinterpret_cast<void*>(state + t::pikmin_birth::transform), color, leaf, 1, sublevel, alone != 0, int32_t(decoration));
            // The eject can reach a story repair that grows these arrays, so
            // the Onion's row is found again rather than kept from before.
            if (onion && live(story) && row(story + t::story_save::onyon_leaves, color, 3, leaves) &&
                writable(leaves + uint32_t(leaf) * sizeof(int32_t), sizeof(int32_t))) {
                const uint64_t saved = leaves + uint32_t(leaf) * sizeof(int32_t);
                write(saved, clamp(int64_t(read_or<int32_t>(saved)) - 1));
            }
            return 1;
        }
    }
    return 0;
}
int32_t step(void* state, void* remaining_first, const void* initial_first, void* remaining_second, const void* initial_second, void* cursor) {
    const int32_t stock = next_step(state, remaining_first, initial_first, remaining_second, initial_second, cursor);
    if (stock) return stock;
    uint64_t left[] = {reinterpret_cast<uint64_t>(remaining_first), reinterpret_cast<uint64_t>(remaining_second)};
    uint64_t start[] = {reinterpret_cast<uint64_t>(initial_first), reinterpret_cast<uint64_t>(initial_second)};
    return custom(reinterpret_cast<uint64_t>(state), left, start);
}
int32_t batch(void* state) {
    Scope scope{reinterpret_cast<uint64_t>(state), {}};
    Scope* before = active;
    active = &scope;
    const int32_t result = next_batch(state);
    active = before;
    return result;
}
}
TSUCHI_Result pikmin_birth_start(TSUCHI_Mod* owner) {
    if (batch_hook && step_hook) return TSUCHI_OK;
    TSUCHI_Result result = hook_site(owner, t::code_site::pikmin_restore_step, &step, &next_step, &step_hook);
    if (result != TSUCHI_OK) return result;
    result = hook_site(owner, t::code_site::pikmin_restore_batch, &batch, &next_batch, &batch_hook);
    if (result != TSUCHI_OK) { hook_remove(owner, step_hook); step_hook = 0; next_step = nullptr; }
    return result;
}
}
