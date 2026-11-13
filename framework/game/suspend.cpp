#include "suspend.h"
#include "ground_contact.h"

#include <string.h>

#include "actors.h"
#include "game.h"
#include "holds.h"
#include "objects.h"

namespace tsuchi {
namespace {

// The game applies a request for velocity during the actor's own update. So the
// body is brought to rest over these frames while that update still runs, and
// only then is the update switched off, when the actor's decisions are suspended too.
constexpr uint32_t kHaltFrames = 3;

// A solid part as it was. A handle, since a part can be destroyed and its place taken.
struct Solid {
    uint64_t part;
    uint8_t mode;
};

// What is kept for an actor a mod has parts of suspended.
struct Held {
    uint32_t parts;
    uint32_t halts;  // frames left in which its body is still being brought to rest
    bool update_due; // its temporary update is to be settled once the body has stopped
    bool actor_saved, actor_on;
    uint64_t brain, mover; // handles of the components whose states were saved
    bool brain_on, mover_on;
    // Its body as it was: whether physics was moving it, and each solid part with how it collided,
    // as many as it has. The list is kept for as long as its solidity is suspended.
    bool was_simulating;
    uint32_t solid_count;
    Solid* solid;
};
SuspendCounts counts{};
void (*message_listener)(const TakenMessage&) = nullptr;

// Stops parts that are running. The actor's decisions are its own update and
// the component that decides for it; its movement is the component that moves it.
void stop(uint64_t actor, Held& entry, uint32_t parts) {
    if (parts & kSuspendMovement) {
        if (const uint64_t mover = game::movement_component(actor)) game::set_component_ticking(mover, false);
        if (game::can_launch(actor)) { // it has a body of its own that would coast on
            game::halt(actor);
            entry.halts = kHaltFrames;
            if ((entry.parts & kSuspendLogic) || !entry.actor_on) { // temporarily run the update that stops the body
                game::set_actor_ticking(actor, true);
                entry.update_due = true;
            }
        }
    }
    if (parts & kSuspendLogic) {
        if (entry.halts) entry.update_due = true;
        else game::set_actor_ticking(actor, false);
        if (const uint64_t brain = game::ai_component(actor)) game::set_component_ticking(brain, false);
    }
}

// Starts parts that were stopped.
void start(uint64_t actor, Held& entry, uint32_t parts) {
    if (parts & kSuspendLogic) {
        if (!entry.update_due) game::set_actor_ticking(actor, entry.actor_on);
        entry.update_due = entry.halts && !entry.actor_on;
        const uint64_t brain = game::from_handle(entry.brain);
        if (brain && brain == game::ai_component(actor)) game::set_component_ticking(brain, entry.brain_on);
        entry.brain = 0;
    }
    if (parts & kSuspendMovement) {
        const uint64_t mover = game::from_handle(entry.mover);
        if (mover && mover == game::movement_component(actor)) game::set_component_ticking(mover, entry.mover_on);
        entry.mover = 0;
        entry.halts = 0;
        if (entry.update_due) { // its decisions stay suspended, and there is nothing left to wait for
            game::set_actor_ticking(actor, (entry.parts & kSuspendLogic) && !(parts & kSuspendLogic) ? false : entry.actor_on);
            entry.update_due = false;
        }
    }
}

// A remembered solid part, if it is still that part and still the actor's.
uint64_t solid_part(uint64_t actor, const Held& entry, uint32_t index) {
    const uint64_t component = game::from_handle(entry.solid[index].part);
    return component && game::read_or<uint64_t>(component + game::table::scene_component::owner) == actor ? component : 0;
}

void drop_solid(Held& entry) {
    release(entry.solid);
    entry.solid = nullptr;
    entry.solid_count = 0;
}

// Puts an actor's body back as it was, for the parts given.
void start_body(uint64_t actor, Held& entry, uint32_t parts) {
    bool changed = false;
    if (parts & kSuspendCollision) {
        for (uint32_t i = 0; i < entry.solid_count; ++i) {
            const uint64_t component = solid_part(actor, entry, i);
            if (!component) continue;
            changed = changed || !game::blocks(game::collision_of(component));
            game::set_collision(component, entry.solid[i].mode);
        }
        drop_solid(entry);
    }
    if (parts & kSuspendPhysics) {
        if (entry.was_simulating) {
            game::set_simulating(actor, true);
            changed = true;
        }
        entry.was_simulating = false;
    }
    if (changed) game::rebuild_collision_tree(actor);
}

// An actor's parts that block, counted and then listed, before any is changed.
struct Gather {
    Solid* into;
    uint32_t room, found;
};
void gather(void* context, uint64_t component) {
    Gather& g = *static_cast<Gather*>(context);
    const uint8_t mode = game::collision_of(component);
    if (!game::blocks(mode)) return;
    if (g.found < g.room) g.into[g.found] = Solid{component, mode}; // its address, until it is changed
    ++g.found;
}

// Stops an actor's physics, makes its solid parts something to pass through, or
// both. Either change makes the physics engine rebuild the body's shapes, so the
// game's record of them is rebuilt too. When that cannot be done safely
// everything is put back and nothing is suspended.
TSUCHI_Result stop_body(uint64_t actor, Held& entry, uint32_t parts) {
    if (parts & kSuspendCollision) {
        Gather counted{}, listed{};
        // Parts that cannot all be found could not all be put back.
        if (!game::each_primitive(actor, gather, &counted)) return TSUCHI_UNSUPPORTED;
        if (counted.found) {
            listed.room = counted.found;
            listed.into = static_cast<Solid*>(allocate(sizeof(Solid) * counted.found));
            if (!listed.into) return TSUCHI_NO_MEMORY;
            if (!game::each_primitive(actor, gather, &listed) || listed.found != counted.found) {
                release(listed.into);
                return TSUCHI_UNSUPPORTED;
            }
        }
        entry.solid = listed.into;
        entry.solid_count = listed.found;
    }
    if (parts & kSuspendPhysics) {
        entry.was_simulating = game::is_simulating(actor);
        if (entry.was_simulating) game::set_simulating(actor, false);
    }
    if (parts & kSuspendCollision) {
        for (uint32_t i = 0; i < entry.solid_count; ++i) {
            const uint64_t component = entry.solid[i].part;
            entry.solid[i].part = game::to_handle(component);
            if (entry.solid[i].part) game::set_collision(component, game::without_blocking(entry.solid[i].mode));
        }
    }
    // Also for a body that was still already: its parts may have shapes of their own that moved.
    if (game::rebuild_collision_tree(actor)) return TSUCHI_OK;
    start_body(actor, entry, parts);
    return TSUCHI_UNSUPPORTED;
}

// Once a frame: the game's own code may have switched the body back.
void keep_body(uint64_t actor, Held& entry) {
    bool changed = false;
    if ((entry.parts & kSuspendPhysics) && game::is_simulating(actor)) {
        game::set_simulating(actor, false);
        changed = true;
    }
    if (entry.parts & kSuspendCollision) {
        for (uint32_t i = 0; i < entry.solid_count; ++i) {
            const uint64_t component = solid_part(actor, entry, i);
            if (!component || !game::blocks(game::collision_of(component))) continue;
            game::set_collision(component, game::without_blocking(entry.solid[i].mode));
            changed = true;
        }
    }
    if (changed) game::rebuild_collision_tree(actor);
}

void take(void* context, const game::Message& message) {
    ++counts.messages;
    if (!message_listener) return;
    TakenMessage taken{};
    taken.actor = *static_cast<const uint64_t*>(context);
    taken.sender = message.sender;
    taken.kind = MessageKind(message.kind);
    taken.amount = message.amount;
    taken.attack = message.attack;
    memcpy(taken.vector, message.vector, sizeof taken.vector);
    taken.press = message.press;
    memcpy(taken.point, message.point, sizeof taken.point);
    message_listener(taken);
}

// Suspends the parts asked for that are not suspended yet. When the body cannot be stopped
// safely nothing of what was asked is suspended, and what was suspended before stays.
TSUCHI_Result apply(const Hold& hold, Held& entry, const uint32_t& parts, bool) {
    const uint64_t actor = game::from_handle(hold.object);
    const uint32_t newly = parts & ~entry.parts;
    // Read every new state before changing physics or ticking. Retain component identities.
    bool actor_on = entry.actor_on, brain_on = false, mover_on = false;
    const uint64_t brain = newly & kSuspendLogic ? game::ai_component(actor) : 0;
    const uint64_t mover = newly & kSuspendMovement ? game::movement_component(actor) : 0;
    if ((newly & (kSuspendLogic | kSuspendMovement)) && !entry.actor_saved && !game::actor_ticking(actor, actor_on)) return TSUCHI_UNSUPPORTED;
    if ((brain && !game::component_ticking(brain, brain_on)) || (mover && !game::component_ticking(mover, mover_on))) return TSUCHI_UNSUPPORTED;
    const uint64_t brain_handle = brain ? game::to_handle(brain) : 0, mover_handle = mover ? game::to_handle(mover) : 0;
    if ((brain && !brain_handle) || (mover && !mover_handle)) return TSUCHI_NOT_FOUND;
    if (newly & (kSuspendPhysics | kSuspendCollision)) {
        const TSUCHI_Result body = stop_body(actor, entry, newly);
        if (body != TSUCHI_OK) return body;
    }
    if (newly & (kSuspendLogic | kSuspendMovement)) entry.actor_saved = true, entry.actor_on = actor_on;
    if (newly & kSuspendLogic) entry.brain = brain_handle, entry.brain_on = brain_on;
    if (newly & kSuspendMovement) entry.mover = mover_handle, entry.mover_on = mover_on;
    stop(actor, entry, newly);
    entry.parts |= parts;
    return TSUCHI_OK;
}

// Once a frame: a body still coming to rest, a body the game has switched back, and what the
// game sent to an actor whose decisions are suspended. The messages last: whoever is told of
// one may suspend and give back.
bool keep(const Hold& hold, Held& entry) {
    uint64_t actor = game::from_handle(hold.object);
    if (entry.halts) {
        game::halt(actor);
        if (!--entry.halts && entry.update_due) {
            game::set_actor_ticking(actor, entry.parts & kSuspendLogic ? false : entry.actor_on);
            entry.update_due = false;
        }
    }
    if (entry.parts & (kSuspendPhysics | kSuspendCollision)) keep_body(actor, entry);
    if (entry.parts & kSuspendLogic) game::take_messages(actor, take, &actor);
    return true;
}

// The actor gets back whatever is still suspended. One that is being destroyed is left as it is.
void restore(const Hold& hold, Held& entry, HoldEnd why) {
    const uint64_t actor = why == HoldEnd::destroyed ? 0 : game::from_handle(hold.object);
    if (actor) {
        start(actor, entry, entry.parts);
        start_body(actor, entry, entry.parts);
    }
    drop_solid(entry); // still there when the actor was gone before it could be put back
}

Holds<Held, uint32_t, apply, keep, restore> held{"entity.suspended", HoldOn::actor, true};

} // namespace

void suspend_start() { held.start(); }

TSUCHI_Result suspend_parts(TSUCHI_Mod* mod, uint64_t actor, uint32_t parts) {
    if (!parts || (parts & ~kSuspendAll)) return TSUCHI_INVALID;
    const uint64_t handle = game::to_handle(actor);
    if (!handle) return TSUCHI_NOT_FOUND;
    if (parts & (kSuspendPhysics | kSuspendCollision)) ground_contact_forget(actor);
    return held.take(mod, handle, 0, parts);
}

TSUCHI_Result resume_parts(TSUCHI_Mod* mod, uint64_t actor, uint32_t parts) {
    if (!parts || (parts & ~kSuspendAll)) return TSUCHI_INVALID;
    const uint64_t handle = game::to_handle(actor);
    Held* entry = handle ? held.find(mod, handle, 0) : nullptr;
    if (!entry) return TSUCHI_NOT_FOUND;
    if (parts & (kSuspendPhysics | kSuspendCollision)) ground_contact_forget(actor);
    start(actor, *entry, parts & entry->parts);
    start_body(actor, *entry, parts & entry->parts);
    entry->parts &= ~parts;
    if (!entry->parts) held.release(mod, handle, 0);
    return TSUCHI_OK;
}

uint32_t suspended_parts(uint64_t actor) {
    const Held* entry = held.find(game::to_handle(actor), 0);
    return entry ? entry->parts : 0;
}

uint32_t suspended_body(uint64_t actor) {
    const Held* entry = held.find(game::to_handle(actor), 0);
    if (!entry) return 0;
    return (entry->was_simulating ? kSuspendPhysics : 0) | (entry->solid_count ? kSuspendCollision : 0);
}

void on_message_taken(void (*listener)(const TakenMessage&)) { message_listener = listener; }
const SuspendCounts& suspend_counts() {
    counts.actors = held.count();
    return counts;
}

} // namespace tsuchi
