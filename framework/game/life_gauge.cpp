// A creature's life gauge updated by itself, for a creature whose own update is held. The game runs the gauge's update
// (0x1ADEDA0) only from the creature's main update (0x1B7E710 by way of 0x1ADDAE0); the life part's own update does
// not. The gauge's update keeps the game's rules: whether the gauge is on, smoothing what it shows toward the health,
// showing it on a change or below full, the creature's AI asking it shown or hidden, placing it, and hiding it after
// its time. It reads the creature's life part and a few of its other parts, so those are checked first, as in the
// puppet gauge research (decomp/puppet-gauge).
//
// Below full health the update asks the creature's AI whether to show the gauge (TekiAI vt+0x7B0: 1 show, 2 hide)
// unless Pikmin cling to it (its StickComponent's flag at +0x2A1), which shows it. A held creature's AI keeps the
// state it was held in, often asleep, which hides the gauge; `show` holds that flag on for the one update.
#include "actors.h"
#include "families.h"
#include "game.h"
#include "life.h"
#include "objects.h"
#include "parts.h"

namespace tsuchi::game {
namespace {
namespace t = table;

ClassRef creature_ai_class{t::name::creature_ai_class};

bool is_creature(uint64_t actor) {
    const Name creatures = global<Name>(t::data::creature_group);
    Name group{};
    return creatures.index && read(actor + t::game_actor::group, group) && group.index == creatures.index && group.number == creatures.number;
}

// A part the gauge's update reads, when the creature has one: a live part of the creature's own.
bool own_part(uint64_t actor, uint64_t field) {
    uint64_t part = 0;
    if (!read(actor + field, part)) return false;
    return !part || (live(part) && readable(part, t::scene_component::owner + sizeof(uint64_t)) && read_or<uint64_t>(part + t::scene_component::owner) == actor);
}

// A list the gauge's update walks: a short one whose entries can be read.
bool short_list(uint64_t life, uint64_t field, uint64_t stride) {
    uint64_t data = 0;
    int32_t count = 0, room = 0;
    if (!read(life + field + t::array::data, data) || !read(life + field + t::array::count, count) || !read(life + field + t::array::max, room)) return false;
    return count >= 0 && count <= 8 && room >= count && room <= 64 && (!count || (data && readable(data, uint64_t(count) * stride)));
}

bool finite(float value) { return value == value && value - value == 0; }
} // namespace

TSUCHI_Result refresh_gauge(uint64_t actor, float seconds, bool show) {
    namespace l = t::life_component;
    if (family_absent(t::family::life_gauge)) return TSUCHI_UNSUPPORTED; // a build without the fields above
    if (!finite(seconds) || seconds < 0 || seconds > 1) return TSUCHI_INVALID;
    if (!game_framework()) return TSUCHI_NOT_READY;
    if (!is_creature(actor)) return TSUCHI_UNSUPPORTED;
    const uint64_t brain = read_or<uint64_t>(actor + t::game_actor::ai);
    if (!brain || !live(brain) || !is_a(brain, creature_ai_class) || read_or<uint64_t>(brain + t::scene_component::owner) != actor) return TSUCHI_UNSUPPORTED;
    if (read_or<uint8_t>(actor + t::game_actor::kill_queued) || read_or<uint8_t>(actor + t::game_actor::terminated)) return TSUCHI_NOT_FOUND;
    if (read_or<uint8_t>(actor + t::game_actor::removed) || read_or<int32_t>(actor + t::game_actor::rest)) return TSUCHI_NOT_READY;
    const uint64_t life = life_component(actor);
    if (!life) return TSUCHI_NOT_FOUND;
    if (!readable(life, l::gauge_full + sizeof(float)) || read_or<uint64_t>(life + t::scene_component::owner) != actor ||
        read_or<uint64_t>(life + l::actor) != actor) return TSUCHI_NOT_READY;
    if (!read_or<uint8_t>(life + l::gauge_enabled)) return TSUCHI_UNSUPPORTED;
    const float health = read_or<float>(life + l::current), full = read_or<float>(life + l::gauge_full);
    if (!finite(health) || !finite(full) || full <= 0) return TSUCHI_NOT_READY;
    if (health <= 0) return TSUCHI_NOT_FOUND;
    const int32_t views = read_or<int32_t>(life + l::gauge_views, -1);
    if (views < 0 || views > 8 || !short_list(life, l::gauge_list_1, sizeof(uint64_t)) || !short_list(life, l::gauge_list_2, 1) ||
        !short_list(life, l::gauge_list_3, 1)) return TSUCHI_NOT_READY;
    const uint64_t parts[] = {t::game_actor::root, t::game_actor::physics, t::gauge_actor::part_1, t::gauge_actor::part_2};
    for (uint64_t field : parts)
        if (!own_part(actor, field)) return TSUCHI_NOT_READY;
    // A collection of targets calls each entry's own functions from the update, which nothing here can check.
    if (read_or<uint32_t>(actor + t::game_actor::state_flags) & t::constant::state_target_collection) return TSUCHI_UNSUPPORTED;
    if (!t::bound(t::fn::life_refresh_gauge)) return TSUCHI_NOT_READY;
    const uint64_t handle = to_handle(actor);
    if (!handle) return TSUCHI_NOT_FOUND;
    // The creature's StickComponent, checked above as its own; its flag is put back after the update.
    const uint64_t stick = show && health < read_or<float>(life + l::max) ? read_or<uint64_t>(actor + t::gauge_actor::part_2) : 0;
    uint8_t clung = 0;
    const bool held_on = stick && read(stick + t::gauge_stick::clung, clung) && !clung && write(stick + t::gauge_stick::clung, uint8_t(1));
    t::fn::life_refresh_gauge(reinterpret_cast<void*>(life), seconds);
    // Making or removing the gauge's widget can call back into the game, which may take the creature away.
    const bool kept = from_handle(handle) == actor && life_component(actor) == life;
    if (held_on && kept && read_or<uint64_t>(actor + t::gauge_actor::part_2) == stick) write(stick + t::gauge_stick::clung, uint8_t(0));
    return kept ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

} // namespace tsuchi::game
