#include "pikmin.h"
#include "pikmin_content.h"

#include <string.h>

#include "actors.h"
#include "captain.h"
#include "game.h"
#include "objects.h"
#include "reflect.h"

namespace tsuchi::game {
namespace {

namespace t = table;

void* pointer(uint64_t address) { return reinterpret_cast<void*>(address); }

struct Each {
    bool (*visit)(void*, uint64_t);
    void* context;
};

// The part of a dog that is steered, or 0 for an actor that is no dog.
uint64_t dog_part(uint64_t actor) {
    if (!(read_or<uint8_t>(actor + t::game_actor::sort) & t::constant::sort_dog)) return 0;
    const uint64_t part = read_or<uint64_t>(actor + t::game_actor::player);
    return part && live(part) ? part : 0;
}

} // namespace

bool is_pikmin(uint64_t actor) {
    if (!actor || !is_game_actor(actor)) return false;
    const uint64_t part = read_or<uint64_t>(actor + t::game_actor::pikmin);
    return part && live(part) && !read_or<uint8_t>(actor + t::game_actor::removed, 1) && !read_or<uint8_t>(actor + t::game_actor::kill_queued, 1);
}

void each_pikmin(bool (*visit)(void*, uint64_t), void* context) {
    Each each{visit, context};
    // The game's list holds the dying too.
    each_listed(
        t::game_framework::pikmin_list,
        [](void* context, uint64_t actor) {
            const auto& each = *static_cast<Each*>(context);
            return !is_pikmin(actor) || each.visit(each.context, actor);
        },
        &each);
}

bool describe_pikmin(uint64_t pikmin, PikminInfo& out) {
    const uint64_t part = read_or<uint64_t>(pikmin + t::game_actor::pikmin);
    uint8_t in_army = 0;
    if (!read(pikmin + t::game_actor::color, out.type) || !read(part + t::pikmin_component::leaf, out.leaf) || !read(part + t::pikmin_component::in_army, in_army))
        return false;
    out.in_army = in_army != 0;
    out.team = actor_team(pikmin);
    return true;
}

bool is_dog(uint64_t actor) { return actor && is_game_actor(actor) && dog_part(actor); }
bool is_leader(uint64_t actor) { return actor && is_game_actor(actor) && (captain_part(actor) || dog_part(actor)); }

bool can_ask_leader() { return t::fn::pikmin_leader != nullptr; }

uint64_t pikmin_leader(uint64_t pikmin) {
    if (!t::bound(t::fn::pikmin_leader)) return 0;
    const auto leader = reinterpret_cast<uint64_t>(t::fn::pikmin_leader(pointer(pikmin)));
    return leader && is_game_actor(leader) ? leader : 0;
}

TSUCHI_Result add_pikmin(uint64_t leader, uint8_t type, uint8_t leaf, uint32_t count) {
    namespace x = t::engine_transform;
    const TSUCHI_Result ready = added_pikmin_ready(type);
    if (ready != TSUCHI_OK) return ready;
    Transform at;
    if (!t::bound(t::fn::eject_pikmin) || !get_transform(leader, at)) return TSUCHI_NOT_READY;
    // The engine's transform: no turn, the leader's place, and a scale of 1. The game makes the Pikmin with it, scale
    // and all: one written a float late made every new Pikmin flat (a scale of 0, 1, 1). It is 48 bytes on every
    // build; the table says where each part is.
    alignas(16) uint8_t where[48] = {};
    const float turn[4] = {0, 0, 0, 1}, size[3] = {1, 1, 1};
    const uint64_t turn_at = x::rotation, place_at = x::translation, size_at = x::scale;
    if (!t::present(x::rotation) || !t::present(x::translation) || !t::present(x::scale) || turn_at > sizeof where - sizeof turn ||
        place_at > sizeof where - sizeof at.location || size_at > sizeof where - sizeof size)
        return TSUCHI_NOT_READY;
    memcpy(where + turn_at, turn, sizeof turn);
    memcpy(where + place_at, at.location, sizeof at.location);
    memcpy(where + size_at, size, sizeof size);
    t::fn::eject_pikmin(pointer(leader), where, type, leaf, int32_t(count), read_or<uint64_t>(leader + t::game_actor::sublevel), false,
                        int32_t(t::constant::spawn_decoration_none));
    return TSUCHI_OK;
}

namespace {

// Whether this build can make an order: with the game's constructor, or by the words it would write. Either way it
// needs the order's own layout.
bool can_build_charge_order() {
    namespace c = t::charge_order;
    if (!t::present(c::size) || !t::present(c::place) || !t::present(c::leader) || !t::present(c::target) || !t::present(c::flag))
        return false;
    return t::bound(t::fn::charge_order) || (t::present(t::data::charge_order_table) && can_build_message(c::size));
}

// Where a captain's charge sends Pikmin: an actor with physics is where its physics last put it, as the game's own
// location of an actor says; any other is where its root is.
bool work_place(uint64_t target, float place[3]) {
    if (t::present(t::game_actor::physics_place) && read_or<uint64_t>(target + t::game_actor::physics)) {
        for (int i = 0; i < 3; ++i)
            if (!read(target + t::game_actor::physics_place + 4 * i, place[i])) return false;
        return true;
    }
    Transform at;
    if (!get_transform(target, at)) return false;
    memcpy(place, at.location, sizeof at.location);
    return true;
}

} // namespace

TSUCHI_Result send_to_work(uint64_t pikmin, uint64_t target) {
    namespace c = t::charge_order;
    float place[3];
    // A build with neither the game's charge order nor what it is made of cannot send a Pikmin to work this way.
    if (!can_build_charge_order()) return TSUCHI_UNSUPPORTED;
    if (!t::bound(t::fn::post_message) || !work_place(target, place)) return TSUCHI_NOT_READY;
    // The game frees an order when its queue is done with it, so the memory is the engine's.
    auto* order = static_cast<uint8_t*>(engine_memory(c::size));
    if (!order) return TSUCHI_NO_MEMORY;
    const uint64_t leader = pikmin_leader(pikmin);
    if (t::bound(t::fn::charge_order)) t::fn::charge_order(order, pointer(leader ? leader : pikmin));
    else build_message(order, c::size, t::data::charge_order_table, leader ? leader : pikmin); // as the charge does where it is inlined
    const uint64_t in_whose_name = to_weak_value(leader), on_what = to_weak_value(target);
    // The flag is what the charge stores: the game's test of a carried target, asked the same way.
    const uint8_t flag = t::bound(t::fn::charge_carry_test) && t::fn::charge_carry_test(pointer(target), true) ? 1 : 0;
    memcpy(order + c::place, place, sizeof place), memcpy(order + c::leader, &in_whose_name, 8);
    memcpy(order + c::target, &on_what, 8), memcpy(order + c::flag, &flag, 1);
    // The game takes the order or frees it, either way.
    return t::fn::post_message(pointer(pikmin), order) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result join_squad(uint64_t pikmin, uint64_t leader) {
    if (!t::bound(t::fn::pikmin_join)) return TSUCHI_NOT_READY;
    t::fn::pikmin_join(pointer(pikmin), pointer(leader), 0, 0, 1); // at once, not at its next update
    return TSUCHI_OK;
}

TSUCHI_Result leave_squad(uint64_t pikmin) {
    if (!t::bound(t::fn::pikmin_free)) return TSUCHI_NOT_READY;
    t::fn::pikmin_free(pointer(pikmin));
    return TSUCHI_OK;
}

TSUCHI_Result dismiss_squad(uint64_t leader) {
    namespace o = t::dismiss_order;
    alignas(8) uint8_t order[o::most] = {};
    const uint64_t whose = to_weak_value(leader);
    const uint8_t keep = uint8_t(t::constant::dismiss_keep_none), yes = 1;
    memcpy(order + o::leader, &whose, sizeof whose);
    memcpy(order + o::keep_type, &keep, 1), memcpy(order + o::sound, &yes, 1), memcpy(order + o::motion, &yes, 1);
    if (const uint64_t part = dog_part(leader)) {
        if (!t::bound(t::fn::dismiss_dog_squad)) return TSUCHI_NOT_READY;
        t::fn::dismiss_dog_squad(pointer(part + t::dog_component::dismiss_settings), order);
        return TSUCHI_OK;
    }
    if (!t::bound(t::fn::dismiss_squad)) return TSUCHI_NOT_READY;
    t::fn::dismiss_squad(order);
    return TSUCHI_OK;
}

} // namespace tsuchi::game
