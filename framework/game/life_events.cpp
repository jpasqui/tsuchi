#include "life_events.h"

#include <string.h>

#include "actors.h"
#include "combat_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "ids.h"
#include "life.h"
#include "names.h"
#include "objects.h"
#include "pikmin.h"
#include "pikmin_service.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// The game's functions, as the game table gives them: (life, attacker, kind; amount, second amount),
// (life, jump the gauge, attacker; value) and (Pikmin, killer, cause) -> reported. Whole numbers and pointers
// are in one row of registers, decimal numbers in another.
decltype(t::code_site::life_apply_damage)::Function next_apply = nullptr;
decltype(t::code_site::life_set)::Function next_set = nullptr;
decltype(t::code_site::pikmin_report_kill)::Function next_report = nullptr;

struct Drop {
    uint64_t actor, attacker; // handles
    float before, after;
};
Drop* drops = nullptr;
uint32_t count = 0, capacity = 0;
// A Pikmin's death, as it was when the game counted it.
struct Death {
    uint64_t actor, killer; // handles
    game::Name kind;
    uint32_t cause;
    float location[3];
    bool typed; // the Pikmin's type was read, as pikmin.info reads it
    uint32_t type;
};
Death* deaths = nullptr;
uint32_t death_count = 0, death_capacity = 0;
LifeEventCounts counts{};

// Notes a drop in the life a part holds, to be told at the start of the next frame.
void note(uint64_t life, float before, uint64_t attacker) {
    float after = 0;
    if (!game::current_life(life, after) || !(after < before)) return;
    if (!combat::damaged_wanted() && !combat::defeated_wanted()) return;
    const uint64_t owner = game::life_owner(life);
    if (!owner) return;
    if (count == capacity) {
        const uint32_t next = capacity ? capacity * 2 : 64;
        auto* grown = static_cast<Drop*>(allocate(sizeof(Drop) * next));
        if (!grown) return;
        if (drops) memcpy(static_cast<void*>(grown), drops, sizeof(Drop) * count);
        release(drops);
        drops = grown;
        capacity = next;
    }
    const uint64_t by = game::from_weak_value(attacker);
    drops[count++] = Drop{game::to_handle(owner), by ? game::to_handle(by) : 0, before, after};
    ++counts.drops;
    counts.waiting = count;
}

void on_apply_damage(uint64_t life, uint64_t attacker, uint64_t kind, float amount, float second) {
    float before = 0;
    const bool known = game::current_life(life, before);
    next_apply(life, attacker, kind, amount, second);
    if (known) note(life, before, attacker);
}

void on_set_life(uint64_t life, uint64_t jump_gauge, uint64_t attacker, float value) {
    float before = 0;
    const bool known = game::current_life(life, before);
    next_set(life, jump_gauge, attacker, value);
    if (known) note(life, before, attacker);
}

uint64_t on_report_kill(uint64_t pikmin, uint64_t killer, uint64_t cause) {
    if (combat::pikmin_died_wanted() && game::is_game_actor(pikmin)) {
        if (death_count == death_capacity) {
            const uint32_t next = death_capacity ? death_capacity * 2 : 64;
            auto* grown = static_cast<Death*>(allocate(sizeof(Death) * next));
            if (grown) {
                if (deaths) memcpy(static_cast<void*>(grown), deaths, sizeof(Death) * death_count);
                release(deaths);
                deaths = grown;
                death_capacity = next;
            }
        }
        if (death_count < death_capacity) {
            Death death{game::to_handle(pikmin), killer && game::is_game_actor(killer) ? game::to_handle(killer) : 0, game::actor_kind(pikmin),
                        uint32_t(cause & 0xFF), {}, false, 0}; // the game's own number, a byte, whether or not it has a name here
            game::PikminInfo info{};
            if (game::describe_pikmin(pikmin, info)) death.typed = true, death.type = info.type;
            game::Transform at{};
            if (game::get_transform(pikmin, at)) memcpy(death.location, at.location, sizeof death.location);
            deaths[death_count++] = death;
            ++counts.deaths;
        }
    }
    return next_report(pikmin, killer, cause);
}

// At the start of a frame: what was seen is told, in the order it happened.
void deliver() {
    // A handler may make more happen; that waits for the next frame.
    const uint32_t ready = count;
    for (uint32_t i = 0; i < ready; ++i) {
        const Drop drop = drops[i];
        combat::emit_damaged({{drop.actor}, {drop.attacker}, drop.before, drop.after});
        if (drop.before > 0 && drop.after <= 0) combat::emit_defeated({{drop.actor}, {drop.attacker}, drop.before, drop.after});
    }
    if (count > ready) memmove(static_cast<void*>(drops), drops + ready, sizeof(Drop) * (count - ready));
    count -= ready;
    counts.waiting = count;

    const uint32_t dead = death_count;
    for (uint32_t i = 0; i < dead; ++i) {
        const Death death = deaths[i];
        char kind[game::kNameBytes];
        if (!game::name_text(death.kind, kind, sizeof kind)) kind[0] = 0;
        char type[kIdBytes] = "";
        if (death.typed) id_of(pikmin::PikminType_vocabulary, death.type, type);
        combat::emit_pikmin_died({{death.actor}, kind, type, {death.killer}, TSUCHI_DeathCause(death.cause),
                                  {death.location[0], death.location[1], death.location[2]}});
    }
    if (death_count > dead) memmove(static_cast<void*>(deaths), deaths + dead, sizeof(Death) * (death_count - dead));
    death_count -= dead;
}

} // namespace

void life_events_start(TSUCHI_Mod* self) {
    uint64_t id = 0;
    counts.sites += hook_site(self, t::code_site::life_apply_damage, &on_apply_damage, &next_apply, &id) == TSUCHI_OK;
    counts.sites += hook_site(self, t::code_site::life_set, &on_set_life, &next_set, &id) == TSUCHI_OK;
    counts.sites += hook_site(self, t::code_site::pikmin_report_kill, &on_report_kill, &next_report, &id) == TSUCHI_OK;
    each_frame(deliver);
}

const LifeEventCounts& life_event_counts() { return counts; }

} // namespace tsuchi
