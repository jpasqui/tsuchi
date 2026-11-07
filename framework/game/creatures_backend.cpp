// The game side of the creatures service.

#include "actors.h"
#include "creature_service.h"
#include "game.h"
#include "objects.h"
#include "reflect.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// True for an actor of the group the game keeps its enemies in.
bool is_creature(uint64_t actor) {
    const game::Name creatures = game::global<game::Name>(t::data::creature_group);
    game::Name group{};
    return creatures.index && game::read(actor + t::game_actor::group, group) && group.index == creatures.index && group.number == creatures.number;
}

struct Collector {
    TSUCHI_Actor* buffer;
    uint64_t capacity, count;
};
bool collect(void* context, uint64_t actor) {
    auto& c = *static_cast<Collector*>(context);
    const uint64_t handle = is_creature(actor) ? game::to_handle(actor) : 0;
    if (!handle) return true;
    if (c.count < c.capacity) c.buffer[c.count].id = handle;
    ++c.count;
    return true;
}

// A list into a caller's buffer, whole or not at all.
TSUCHI_Result list(TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Collector counter{nullptr, 0, 0};
    game::each_actor(collect, &counter);
    *out = counter.count;
    if (counter.count > capacity) return TSUCHI_CAPACITY;
    Collector writer{buffer, counter.count, 0};
    game::each_actor(collect, &writer);
    return TSUCHI_OK;
}

TSUCHI_Result defeated(TSUCHI_Actor actor, bool* out) {
    const uint64_t creature = game::from_handle(actor.id);
    if (!creature || !game::is_game_actor(creature) || !is_creature(creature)) return TSUCHI_NOT_FOUND;
    *out = game::read_or<uint8_t>(creature + t::game_actor::removed) != 0;
    return TSUCHI_OK;
}

TSUCHI_Result carcass(TSUCHI_Actor actor, bool* out) {
    const uint64_t creature = game::from_handle(actor.id);
    if (!creature || !game::is_game_actor(creature) || !is_creature(creature)) return TSUCHI_NOT_FOUND;
    *out = (game::read_or<uint32_t>(creature + t::game_actor::state_flags) & t::constant::state_carcass) != 0;
    return TSUCHI_OK;
}

TSUCHI_Result revive(TSUCHI_Actor actor) {
    const uint64_t creature = game::from_handle(actor.id);
    if (!creature || !game::is_game_actor(creature) || !is_creature(creature)) return TSUCHI_NOT_FOUND;
    // A build that inlines the request's constructor into its one caller has it made from the request's table.
    const bool made_here = !t::bound(t::fn::revive_request);
    if (made_here && !(t::present(t::data::revive_request_table) && game::can_build_message(t::revive_request::size))) return TSUCHI_UNSUPPORTED;
    if (!game::read_or<uint8_t>(creature + t::game_actor::removed) || !t::bound(t::fn::is_held_or_carried) || !t::bound(t::fn::post_message))
        return TSUCHI_NOT_READY;
    if (game::read_or<uint8_t>(creature + t::game_actor::kill_queued) || t::fn::is_held_or_carried(reinterpret_cast<void*>(creature))) return TSUCHI_BUSY;
    // The game frees a request when its queue is done with it, so the memory is the engine's.
    void* request = game::engine_memory(t::revive_request::size);
    if (!request) return TSUCHI_NO_MEMORY;
    // Sent by the creature to itself. Its kind's name stays none, as the game's own constructor leaves it.
    if (made_here) game::build_message(static_cast<uint8_t*>(request), t::revive_request::size, t::data::revive_request_table, creature);
    else t::fn::revive_request(request, reinterpret_cast<void*>(creature));
    return t::fn::post_message(reinterpret_cast<void*>(creature), request) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

const creature::Backend service{list, defeated, carcass, revive};

} // namespace

void creatures_start() { creature::set_backend(&service); }

} // namespace tsuchi
