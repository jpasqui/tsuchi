// The combat service: life, hits and knocks.
#include "actors.h"
#include "combat_service.h"
#include "life.h"
#include "objects.h"
#include "pikmin.h"
#include "services.h"
#include "suspend.h"

namespace tsuchi {
namespace {

uint64_t actor_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_game_actor(object) ? object : 0;
}

TSUCHI_Result get_life(TSUCHI_Actor actor, TSUCHI_Life* out) {
    const uint64_t address = actor_of(actor);
    game::Life life{};
    if (!address || !game::read_life(address, life)) return TSUCHI_NOT_FOUND;
    *out = TSUCHI_Life{life.life, life.max_life, life.extra, life.max_extra};
    return TSUCHI_OK;
}

TSUCHI_Result set_life(TSUCHI_Actor actor, float life, TSUCHI_LifeFlags flags) {
    const uint64_t address = actor_of(actor);
    game::Life now{};
    if (!address || !game::read_life(address, now)) return TSUCHI_NOT_FOUND;
    if (life < 0 || life > now.max_life) return TSUCHI_INVALID;
    return game::set_life(address, life, (flags & TSUCHI_LIFE_FLAGS_SMOOTH_GAUGE) != 0) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result damage(TSUCHI_Actor victim, float amount, TSUCHI_AttackKind kind, TSUCHI_Actor attacker) {
    const uint64_t target = actor_of(victim), by = attacker.id ? actor_of(attacker) : 0;
    if (!target || (attacker.id && !by)) return TSUCHI_NOT_FOUND;
    if (amount < 0) return TSUCHI_INVALID;
    return game::send_hit(target, by, amount, uint32_t(kind));
}

TSUCHI_Result knockback(TSUCHI_Actor victim, const TSUCHI_Vec3* velocity, float damage, TSUCHI_Actor attacker) {
    const uint64_t target = actor_of(victim), by = attacker.id ? actor_of(attacker) : 0;
    if (!target || (attacker.id && !by)) return TSUCHI_NOT_FOUND;
    if (damage < 0) return TSUCHI_INVALID;
    const float thrown[3] = {velocity->x, velocity->y, velocity->z};
    return game::send_knock(target, by, thrown, damage);
}

TSUCHI_Result kill_pikmin(TSUCHI_Actor pikmin, TSUCHI_DeathCause cause, TSUCHI_Actor killer) {
    const uint64_t target = game::from_handle(pikmin.id), by = killer.id ? actor_of(killer) : 0;
    if (!game::is_pikmin(target) || (killer.id && !by)) return TSUCHI_NOT_FOUND;
    return game::remove_actor(target, false, uint8_t(cause), by);
}

// Something a suspended actor was sent, as it is taken from the actor.
void taken(const TakenMessage& message) {
    combat::emit_intercepted({{game::to_handle(message.actor)}, {message.sender ? game::to_handle(message.sender) : 0}, TSUCHI_MessageKind(uint32_t(message.kind)),
                              message.amount, message.attack, {message.vector[0], message.vector[1], message.vector[2]}});
}

const combat::Backend service{get_life, set_life, damage, knockback, kill_pikmin};

} // namespace

void combat_start() {
    combat::set_backend(&service);
    on_message_taken(taken);
}

} // namespace tsuchi
