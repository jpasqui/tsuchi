#include "life.h"

#include "actors.h"
#include "game.h"
#include "objects.h"
#include "parts.h"
#include "reflect.h"

namespace tsuchi::game {
namespace {

namespace t = table;

ClassRef life_class{t::name::life_class};

// Memory for a message, made into one by the game's own function. The game
// frees a message when its queue is done with it, so the memory is the engine's.
uint64_t make_message(void (*make)(void*, void*), uint64_t bytes, uint64_t attacker) {
    void* message = engine_memory(bytes);
    if (message) make(message, reinterpret_cast<void*>(attacker));
    return reinterpret_cast<uint64_t>(message);
}

// Hands a message to the actor. The game takes it or frees it, either way.
TSUCHI_Result post(uint64_t victim, uint64_t message) {
    return t::fn::post_message(reinterpret_cast<void*>(victim), reinterpret_cast<void*>(message)) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

} // namespace

uint64_t life_component(uint64_t actor) {
    const uint64_t component = read_or<uint64_t>(actor + t::game_actor::life);
    return component && is_a(component, life_class) ? component : 0;
}

uint64_t life_owner(uint64_t component) {
    const uint64_t owner = part_owner(component);
    return owner && is_game_actor(owner) && life_component(owner) == component ? owner : 0;
}

bool current_life(uint64_t component, float& life) { return read(component + t::life_component::current, life); }

bool read_life(uint64_t actor, Life& out) {
    const uint64_t component = life_component(actor);
    namespace l = t::life_component;
    return component && read(component + l::current, out.life) && read(component + l::max, out.max_life) && read(component + l::extra, out.extra) &&
           read(component + l::max_extra, out.max_extra);
}

bool set_life(uint64_t actor, float life, bool smooth) {
    const uint64_t component = life_component(actor);
    if (!component || !t::bound(t::fn::set_life)) return false;
    // Nobody did it: a weak reference to nothing is the number -1 with no serial.
    t::fn::set_life(reinterpret_cast<void*>(component), !smooth, 0xFFFFFFFFull, life);
    return true;
}

TSUCHI_Result send_hit(uint64_t victim, uint64_t attacker, float amount, uint32_t kind) {
    namespace m = t::damage_message;
    if (!life_component(victim)) return TSUCHI_NOT_FOUND;
    if (!t::bound(t::fn::damage_message) || !t::bound(t::fn::post_message)) return TSUCHI_NOT_READY;
    const uint64_t message = make_message(t::fn::damage_message, m::size, attacker);
    if (!message) return TSUCHI_NO_MEMORY;
    write(message + m::amount, amount);
    write(message + m::attack, uint8_t(kind));
    write(message + m::riding_applies, uint8_t(1));
    if (!attacker) { // a hit that is nobody's comes from where the victim stands
        Transform at{};
        if (get_transform(victim, at))
            for (int i = 0; i < 3; ++i) write(message + m::origin + 4 * i, at.location[i]);
    }
    return post(victim, message);
}

TSUCHI_Result send_knock(uint64_t victim, uint64_t attacker, const float velocity[3], float damage) {
    namespace m = t::knock_message;
    if (!t::bound(t::fn::knock_message) || !t::bound(t::fn::post_message)) return TSUCHI_NOT_READY;
    const uint64_t message = make_message(t::fn::knock_message, m::size, attacker);
    if (!message) return TSUCHI_NO_MEMORY;
    for (int i = 0; i < 3; ++i) write(message + m::velocity + 4 * i, velocity[i]);
    write(message + m::damage, damage);
    return post(victim, message);
}

} // namespace tsuchi::game
