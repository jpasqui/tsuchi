#include "presenter_scene.h"
#include "actor_events.h"

#include <string.h>

#include "actors.h"
#include "content.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "entity_service.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// The game's functions, as the game table gives them.
using Register = decltype(t::slot_group::actor_register)::Function;
using EndPlay = decltype(t::slot_group::actor_end_play)::Function;
using PoolTake = decltype(t::code_site::actor_pool_take)::Function;
using PoolReturn = decltype(t::code_site::actor_pool_return)::Function;
using Park = decltype(t::code_site::actor_park)::Function;

// One of the game's actor function tables, and what its two entries lead to now.
struct Wrapped {
    uint64_t table;
    Register next_register;
    EndPlay next_end_play;
};
Wrapped* wrapped = nullptr;
PoolTake next_take = nullptr;
PoolReturn next_return = nullptr;
Park next_park = nullptr;
// The actor a pool is being asked to take back right now, and whether the game has parked it for that.
uint64_t returning = 0;
bool returning_parked = false;
ActorEventCounts counts{};

// Which objects are actors in play, by the object's number. An actor can
// register more than once, and be put away more than once, so the events
// follow the changes of this and not the calls.
uint8_t* in_play = nullptr;
bool mark(uint64_t actor, bool now) {
    const uint32_t index = uint32_t(game::read_or<int32_t>(actor + t::object::index, -1));
    if (!in_play || index >= t::constant::max_objects) return false;
    const uint8_t bit = uint8_t(1u << (index & 7));
    const bool was = in_play[index >> 3] & bit;
    if (was == now) return false;
    in_play[index >> 3] = now ? in_play[index >> 3] | bit : in_play[index >> 3] & ~bit;
    counts.in_play += now ? 1 : -1;
    return true;
}
bool marked(uint64_t actor) {
    const uint32_t index = uint32_t(game::read_or<int32_t>(actor + t::object::index, -1));
    return in_play && index < t::constant::max_objects && (in_play[index >> 3] & (1u << (index & 7)));
}

// What an event will say, taken while the actor can still be read.
struct Pending {
    bool spawned;
    uint64_t handle;
    game::Name kind;
    int32_t team;
    uint64_t placement;
    float location[3];
};
using LeftListener = void (*)(uint64_t actor, uint64_t handle, ActorLeft how);
LeftListener* left_listeners = nullptr;
uint32_t left_count = 0, left_capacity = 0;
void tell_left(uint64_t actor, ActorLeft how) {
    const uint64_t handle = game::to_handle(actor);
    for (uint32_t i = 0; i < left_count; ++i) left_listeners[i](actor, handle, how);
}
using EnteredListener = void (*)(uint64_t actor, uint64_t handle);
EnteredListener* entered_listeners = nullptr;
uint32_t entered_count = 0, entered_capacity = 0;
Pending* pending = nullptr;
uint32_t pending_count = 0, pending_capacity = 0;

void describe(uint64_t actor, Pending& out) {
    out.kind = game::actor_kind(actor);
    out.team = game::actor_team(actor);
    out.placement = game::actor_placement(actor);
    game::Transform at{};
    game::get_transform(actor, at);
    memcpy(out.location, at.location, sizeof out.location);
}

// False when nothing was queued.
bool queue(bool spawned, uint64_t actor) {
    if (pending_count == pending_capacity) {
        const uint32_t next = pending_capacity ? pending_capacity * 2 : 256;
        auto* grown = static_cast<Pending*>(allocate(sizeof(Pending) * next));
        if (!grown) return false;
        if (pending) memcpy(static_cast<void*>(grown), pending, sizeof(Pending) * pending_count);
        release(pending);
        pending = grown;
        pending_capacity = next;
    }
    Pending& event = pending[pending_count];
    event = Pending{};
    event.spawned = spawned;
    event.handle = game::to_handle(actor);
    if (!event.handle) return false;
    if (!spawned) describe(actor, event); // one that has just arrived is described when the event is delivered
    ++pending_count;
    return true;
}

void entered(uint64_t actor) {
    if (!game::is_game_actor(actor) || !mark(actor, true)) return;
    ++counts.entered;
    if (entity::spawned_wanted()) queue(true, actor);
    for (uint32_t i = 0; i < entered_count; ++i) entered_listeners[i](actor, game::to_handle(actor));
}

// Call while the actor is still as it was in play.
bool leaving(uint64_t actor, ActorLeft how) {
    if (!game::live(actor) || !mark(actor, false)) return false;
    if (entity::destroyed_wanted()) queue(false, actor);
    tell_left(actor, how);
    return true;
}

const Wrapped* table_of(uint64_t actor) {
    const uint64_t table = game::read_or<uint64_t>(actor);
    for (uint32_t i = 0; i < counts.tables; ++i)
        if (wrapped[i].table == table) return &wrapped[i];
    return nullptr;
}

void on_register(uint64_t actor, bool with_children) {
    if (const Wrapped* table = table_of(actor)) table->next_register(actor, with_children);
    entered(actor);
    presenter_cut(actor); // it comes into play where it is, with no slide from where its parts were
}

void on_end_play(uint64_t actor, uint8_t reason) {
    // An actor whose level is taken out of the world is not destroyed: the object lives on as it is left.
    counts.destroyed += leaving(actor, reason == t::constant::end_play_removed ? ActorLeft::put_away : ActorLeft::destroyed);
    if (const Wrapped* table = table_of(actor)) table->next_end_play(actor, reason);
}

uint64_t on_pool_take(uint64_t pool, uint64_t request) {
    const uint64_t actor = next_take(pool, request);
    if (actor) entered(actor), presenter_cut(actor);
    return actor;
}

void on_park(uint64_t actor, uint64_t unused) {
    if (actor && actor == returning) {
        // Its pool is taking it back. Whoever changed something on it puts that back now, before the
        // game switches it off and hides it, and not after, which would undo what the game does here.
        if (!returning_parked && marked(actor)) tell_left(actor, ActorLeft::put_away);
        returning_parked = true;
    }
    next_park(actor, unused);
    presenter_cut(actor);
}

void on_pool_return(uint64_t pool, uint64_t actor) {
    const bool was_in_play = actor && game::live(actor) && marked(actor);
    const uint32_t place = pending_count;
    const bool queued = was_in_play && entity::destroyed_wanted() && queue(false, actor); // while it is as it was in play
    // The game parks the actor only when it is this pool's; asked to take back another's, it does
    // nothing and the actor plays on. Parking can lead to other actors being taken back, so what
    // is being watched is kept across the call.
    const uint64_t outer = returning;
    const bool outer_parked = returning_parked;
    returning = was_in_play ? actor : 0;
    returning_parked = false;
    next_return(pool, actor);
    // Without the hook on parking, the actor's own word for it has to do.
    const bool parked = next_park ? returning_parked : game::read_or<uint32_t>(actor + t::game_actor::rest) == t::constant::rest_parked;
    returning = outer;
    returning_parked = outer_parked;
    if (!was_in_play) return;
    if (!parked || !marked(actor)) { // not taken back; or taken back already, from inside this very call
        if (queued) pending[place].handle = 0; // other events may have been queued behind it meanwhile
        return;
    }
    mark(actor, false);
    ++counts.put_away;
    if (!next_park) tell_left(actor, ActorLeft::put_away); // too late to be before the game parked it, and better than never
    // The object lives on in the pool. Its use is over, and so is every handle to it.
    game::end_life(actor);
}

// Once a frame, before mods hear the frame: what happened since the last one.
void deliver() {
    // A handler may make more happen; that waits for the next frame.
    const uint32_t count = pending_count;
    for (uint32_t i = 0; i < count; ++i) {
        Pending event = pending[i];
        if (!event.handle) continue; // taken back
        if (event.spawned) {
            const uint64_t actor = game::from_handle(event.handle);
            if (actor) describe(actor, event); // gone again already: nothing more is known of it
        }
        char kind[game::kNameBytes];
        uint64_t kind_bytes = 0;
        if (game::kind_into(event.kind, kind, sizeof kind, &kind_bytes) != TSUCHI_OK) kind[0] = 0; // a mod's thing by its ID
        const TSUCHI_Vec3 location{event.location[0], event.location[1], event.location[2]};
        if (event.spawned) entity::emit_spawned({{event.handle}, kind, event.team, event.placement, location});
        else entity::emit_destroyed({{event.handle}, kind, event.team, event.placement, location});
    }
    if (pending_count > count) memmove(static_cast<void*>(pending), pending + count, sizeof(Pending) * (pending_count - count));
    pending_count -= count;
}

bool one_of(uint64_t value, const game::table::Cell<uint64_t>* offsets, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (value == game::main_base() + offsets[i]) return true;
    return false;
}

} // namespace

void actor_events_start(TSUCHI_Mod* self) {
    const auto& enter = t::slot_group::actor_register;
    const auto& leave = t::slot_group::actor_end_play;
    wrapped = static_cast<Wrapped*>(allocate(sizeof(Wrapped) * enter.table_count));
    in_play = static_cast<uint8_t*>(allocate(t::constant::max_objects / 8 + 1));
    if (!wrapped || !in_play) return;
    memset(in_play, 0, t::constant::max_objects / 8 + 1);

    for (unsigned i = 0; i < enter.table_count; ++i) {
        const uint64_t table = game::main_base() + enter.tables[i];
        const uint64_t register_entry = table + enter.offset, end_entry = table + leave.offset;
        // A table whose entries are not the game's own is not one this knows how to wrap.
        if (!one_of(hook_entry_base(register_entry), enter.stock, enter.stock_count) ||
            !one_of(hook_entry_base(end_entry), leave.stock, leave.stock_count))
            continue;
        uint64_t id = 0;
        Register next_register = nullptr;
        EndPlay next_end = nullptr;
        if (hook_group_entry(self, enter, i, &on_register, &next_register, &id) != TSUCHI_OK) continue;
        if (hook_group_entry(self, leave, i, &on_end_play, &next_end, &id) != TSUCHI_OK) {
            hook_remove(self, id);
            continue;
        }
        wrapped[counts.tables++] = Wrapped{table, next_register, next_end};
    }

    uint64_t id = 0;
    counts.pool_sites += hook_site(self, t::code_site::actor_pool_take, &on_pool_take, &next_take, &id) == TSUCHI_OK;
    counts.pool_sites += hook_site(self, t::code_site::actor_pool_return, &on_pool_return, &next_return, &id) == TSUCHI_OK;
    counts.pool_sites += hook_site(self, t::code_site::actor_park, &on_park, &next_park, &id) == TSUCHI_OK;
    each_frame(deliver);
}

bool on_actor_left(void (*listener)(uint64_t, uint64_t, ActorLeft)) {
    for (uint32_t i = 0; i < left_count; ++i)
        if (left_listeners[i] == listener) return true;
    if (left_count == left_capacity) {
        const uint32_t next = left_capacity ? left_capacity * 2 : 8;
        auto* grown = static_cast<LeftListener*>(allocate(sizeof(LeftListener) * next));
        if (!grown) return false;
        if (left_listeners) memcpy(static_cast<void*>(grown), left_listeners, sizeof(LeftListener) * left_count);
        release(left_listeners);
        left_listeners = grown;
        left_capacity = next;
    }
    left_listeners[left_count++] = listener;
    return true;
}
bool on_actor_entered(void (*listener)(uint64_t, uint64_t)) {
    for (uint32_t i = 0; i < entered_count; ++i)
        if (entered_listeners[i] == listener) return true;
    if (entered_count == entered_capacity) {
        const uint32_t next = entered_capacity ? entered_capacity * 2 : 8;
        auto* grown = static_cast<EnteredListener*>(allocate(sizeof(EnteredListener) * next));
        if (!grown) return false;
        if (entered_listeners) memcpy(static_cast<void*>(grown), entered_listeners, sizeof(EnteredListener) * entered_count);
        release(entered_listeners);
        entered_listeners = grown;
        entered_capacity = next;
    }
    entered_listeners[entered_count++] = listener;
    return true;
}
const ActorEventCounts& actor_event_counts() { return counts; }
bool actor_seen_in_play(uint64_t actor) { return marked(actor); }

} // namespace tsuchi
