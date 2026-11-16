// The game side of the pikmin and squad services.

#include "actors.h"
#include "ids.h"
#include "objects.h"
#include "pikmin.h"
#include "pikmin_service.h"
#include "content_squad.h"
#include "services.h"
#include "squad_service.h"

namespace tsuchi {
namespace {

// The Pikmin an actor handle names, or 0.
uint64_t pikmin_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return game::is_pikmin(object) ? object : 0;
}
uint64_t leader_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return game::is_leader(object) ? object : 0;
}

// Collects the handles of Pikmin, all of them or those that follow one leader.
// Counts every one and writes those that fit.
struct Collector {
    uint64_t leader; // 0 for every Pikmin
    TSUCHI_Actor* buffer;
    uint64_t capacity, count;
};
bool collect(void* context, uint64_t pikmin) {
    auto& c = *static_cast<Collector*>(context);
    if (c.leader && game::pikmin_leader(pikmin) != c.leader) return true;
    const uint64_t handle = game::to_handle(pikmin);
    if (!handle) return true;
    if (c.count < c.capacity) c.buffer[c.count].id = handle;
    ++c.count;
    return true;
}

// A list into a caller's buffer, whole or not at all.
TSUCHI_Result collect_all(uint64_t leader, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Collector counter{leader, nullptr, 0, 0};
    game::each_pikmin(collect, &counter);
    *out = counter.count;
    if (counter.count > capacity) return TSUCHI_CAPACITY;
    Collector writer{leader, buffer, counter.count, 0};
    game::each_pikmin(collect, &writer);
    return TSUCHI_OK;
}

// ---- pikmin ----------------------------------------------------------------------

TSUCHI_Result list(TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) { return collect_all(0, buffer, capacity, out); }

// A Pikmin is told whatever the game has it as. A type nobody has named still has an ID, its number's
// placeholder, and a leaf the framework has no name for is the game's number as it is.
TSUCHI_Result describe(TSUCHI_Actor actor, TSUCHI_PikminInfo* out) {
    const uint64_t pikmin = pikmin_of(actor);
    game::PikminInfo info;
    if (!pikmin || !game::describe_pikmin(pikmin, info)) return TSUCHI_NOT_FOUND;
    *out = TSUCHI_PikminInfo{};
    id_of(pikmin::PikminType_vocabulary, info.type, out->kind);
    out->leaf = info.leaf, out->team = info.team, out->in_army = info.in_army;
    return TSUCHI_OK;
}

TSUCHI_Result add(TSUCHI_Actor actor, const char* kind, TSUCHI_Leaf leaf, uint32_t count) {
    const uint64_t leader = leader_of(actor);
    if (!leader) return TSUCHI_NOT_FOUND;
    if (!count) return TSUCHI_INVALID;
    uint32_t type = 0;
    const TSUCHI_Result named = id_value(pikmin::PikminType_vocabulary, kind, &type);
    if (named != TSUCHI_OK) return named;
    // The game is asked only for a type somebody vouches for: one of its own that has a name, or one a mod added.
    if (!id_named(pikmin::PikminType_vocabulary, type)) return TSUCHI_UNSUPPORTED;
    return game::add_pikmin(leader, uint8_t(type), uint8_t(leaf), count);
}

TSUCHI_Result work_on(TSUCHI_Actor actor, TSUCHI_Actor on) {
    const uint64_t pikmin = pikmin_of(actor), target = game::from_handle(on.id);
    if (!pikmin || !target || !game::is_game_actor(target)) return TSUCHI_NOT_FOUND;
    return game::send_to_work(pikmin, target);
}

TSUCHI_Result type_count(uint32_t* out) {
    const auto& types = pikmin::PikminType_vocabulary;
    *out = types.own_count + types.added.count;
    return TSUCHI_OK;
}
const pikmin::Backend pikmin_service{list, describe, add, work_on, type_count};

// ---- squad -----------------------------------------------------------------------

TSUCHI_Result leader(TSUCHI_Actor actor, TSUCHI_Actor* out) {
    const uint64_t pikmin = pikmin_of(actor);
    if (!pikmin) return TSUCHI_NOT_FOUND;
    if (!game::can_ask_leader()) return TSUCHI_NOT_READY;
    const uint64_t followed = game::pikmin_leader(pikmin);
    out->id = followed ? game::to_handle(followed) : 0; // a Pikmin that follows nobody has none
    return TSUCHI_OK;
}

TSUCHI_Result members(TSUCHI_Actor actor, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t whose = leader_of(actor);
    if (!whose) return TSUCHI_NOT_FOUND;
    if (!game::can_ask_leader()) return TSUCHI_NOT_READY;
    return collect_all(whose, buffer, capacity, out);
}

TSUCHI_Result join(TSUCHI_Actor actor, TSUCHI_Actor to) {
    const uint64_t pikmin = pikmin_of(actor), whose = leader_of(to);
    return pikmin && whose ? game::join_squad(pikmin, whose) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result leave(TSUCHI_Actor actor) {
    const uint64_t pikmin = pikmin_of(actor);
    return pikmin ? game::leave_squad(pikmin) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result dismiss(TSUCHI_Actor actor) {
    const uint64_t whose = leader_of(actor);
    return whose ? game::dismiss_squad(whose) : TSUCHI_NOT_FOUND;
}

squad::Backend squad_service{};

} // namespace

void pikmin_start() { pikmin::set_backend(&pikmin_service); }
void squad_start() {
    squad_service.leader = leader; squad_service.members = members; squad_service.join = join;
    squad_service.leave = leave; squad_service.dismiss = dismiss;
    squad_content_backend(squad_service);
    squad::set_backend(&squad_service);
}

} // namespace tsuchi
