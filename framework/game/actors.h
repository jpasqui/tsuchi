// The game's actors: the things in play. The game keeps them in a list of its
// own, apart from the engine's objects, and gives each a kind, a team and,
// when a level placed it, a number that stays the same across visits.
#pragma once
#include <stdint.h>

#include "names.h"
#include "tsuchi/core.h"

namespace tsuchi::game {

// The object that runs the game, or 0 while no world is loaded.
uint64_t game_framework();
// True for a live object of the game's own actor class.
bool is_game_actor(uint64_t object);
// The game's own actor class, or 0 while it is not loaded.
uint64_t game_actor_class();
// Calls `visit` for every actor in play, in the game's own order, until it returns false.
void each_actor(bool (*visit)(void* context, uint64_t actor), void* context);
// The same for another of the game's lists of actors, by where its first link is in the object that runs the game.
void each_listed(uint64_t list_offset, bool (*visit)(void* context, uint64_t actor), void* context);

Name actor_kind(uint64_t actor);        // the game's name for what kind of thing it is
int32_t actor_team(uint64_t actor);
uint64_t actor_placement(uint64_t actor); // 0 for an actor no level placed

// Where an actor is and which way it faces. Angles are pitch, yaw and roll in
// degrees, with the actor's forward along +X. A character keeps its root still
// and turns its body; the angles are the body's.
struct Transform {
    float location[3], angles[3];
};
bool get_transform(uint64_t actor, Transform& out);
// Moves the actor there at once, through nothing, as a teleport. TSUCHI_NOT_READY
// when the actor has nothing to move or the game's own functions are missing.
TSUCHI_Result set_transform(uint64_t actor, const Transform& to);

// How fast the actor is moving, in centimeters per second: its physics body's
// velocity while that is awake, else its root's own.
bool get_velocity(uint64_t actor, float out[3]);
// True when a velocity request can move the actor: it has a physics body of
// its own that is simulating, is not attached to anything, and is not on its way out.
bool can_launch(uint64_t actor);
// Asks the game to give the actor's body this velocity at the next world
// update, whole: not held under the usual speed limit, with its vertical part.
void request_velocity(uint64_t actor, const float velocity[3]);
// Brings the actor's body to rest across the ground at the next world update,
// leaving its fall to gravity. Whatever its own movement asked for this frame is dropped.
void halt(uint64_t actor);
// Drops the velocity requests the actor's own movement has made this frame.
void drop_velocity_requests(uint64_t actor);
bool body_awake(uint64_t actor);
uint32_t air_steps(uint64_t actor); // physics steps since any contact, including walls and ceilings
// Keeps the actor's physics running, or lets go of one such hold. The game
// counts holds, and gives an actor a frame's grace after the last one goes.
void hold_awake(uint64_t actor, bool hold);
// Moves the actor the way the game does when it warps one: set down on the
// ground there, with its body and its movement brought along. TSUCHI_NOT_READY
// with no world, TSUCHI_NOT_FOUND for an actor on its way out, TSUCHI_UNSUPPORTED
// for one with no physics body of its own or attached to another actor.
TSUCHI_Result warp_actor(uint64_t actor, const Transform& to);

// ---- An actor's body ------------------------------------------------------------
//
// How a part collides, as the engine numbers it.
enum : uint8_t { kCollisionNone = 0, kCollisionQueryOnly = 1, kCollisionPhysicsOnly = 2, kCollisionBoth = 3 };
inline bool blocks(uint8_t mode) { return mode == kCollisionPhysicsOnly || mode == kCollisionBoth; }
// The same, without the blocking: it can still be found, and nothing bumps into it.
inline uint8_t without_blocking(uint8_t mode) { return mode == kCollisionBoth ? kCollisionQueryOnly : mode == kCollisionPhysicsOnly ? kCollisionNone : mode; }
// The component that is the actor's physics body, or 0.
uint64_t physics_component(uint64_t actor);
// Whether the game's physics is moving the actor's body, and switching that.
bool is_simulating(uint64_t actor);
void set_simulating(uint64_t actor, bool on);
uint8_t collision_of(uint64_t component);
void set_collision(uint64_t component, uint8_t mode);
// Calls `visit` for a part and for each of the same actor's parts attached below
// it. False when they could not all be walked.
bool each_attached(uint64_t component, void (*visit)(void* context, uint64_t component), void* context);
// Calls `visit` for each of the actor's own parts that can collide: its root and
// what is attached below it, however many there are. False when they could not
// all be walked, because the parts lead back into themselves or cannot be read.
bool each_primitive(uint64_t actor, void (*visit)(void* context, uint64_t component), void* context);
// How many of them there are; writes those that fit.
uint32_t actor_primitives(uint64_t actor, uint64_t* out, uint32_t capacity);
// The game keeps shapes borrowed from the physics engine for each of an actor's
// collision parts. Switching a body's simulation or a part's collision makes the
// engine build new shapes, so the borrowed ones have to be dropped and found
// again before the game next uses them. False when the actor's records are not
// as expected; the borrowed shapes are dropped all the same wherever that is safe.
bool rebuild_collision_tree(uint64_t actor);

// The game's record of one of an actor's collision parts, or 0 when the game
// keeps none for that part, keeps more than one, or its records are not as expected.
uint64_t collision_record_of(uint64_t actor, uint64_t component);

// An entry of an object's own function table, checked to be code; 0 otherwise.
uint64_t table_entry(uint64_t object, uint64_t offset);

// The component that decides for the actor, and the one that moves it; 0 for an actor without.
uint64_t ai_component(uint64_t actor);
uint64_t movement_component(uint64_t actor);
// Switches an actor's own update, or a component's, on or off, the game's own way.
bool actor_ticking(uint64_t actor, bool& on);
bool component_ticking(uint64_t component, bool& on);
void set_actor_ticking(uint64_t actor, bool on);
void set_component_ticking(uint64_t component, bool on);

// What the game has sent an actor. `kind` is 0 for damage, 1 for a knock, 2 for a whistle, 3 for anything else.
struct Message {
    uint32_t kind;
    uint64_t sender; // 0 when none is named or it is gone
    float amount;    // damage: how much. knock: the damage that comes with it
    uint32_t attack; // damage: the game's number for the kind of hit
    float vector[3]; // damage: where it came from. knock: the velocity to be thrown with
};
// Takes every message waiting for the actor, as its own decisions would, and
// lets each go the game's way afterward. Returns how many there were.
uint32_t take_messages(uint64_t actor, void (*visit)(void* context, const Message& message), void* context);
// Makes a message as the game's constructors do where a build inlines them: `size` bytes from `sender`, with `table`
// (an offset in the game's main program) as its function table. Every word the base constructor sets is written; the
// rest is zero. Whether a build can make one of `size` bytes is can_build_message(size).
void build_message(uint8_t* message, uint64_t size, uint64_t table, uint64_t sender);
bool can_build_message(uint64_t size);

// Makes an actor of a class through the game's own spawner, which gives it its
// team, registers it and starts its parts. A team below 0 is the class's own.
// 0 when there is no world or the game makes nothing.
uint64_t spawn_actor(uint64_t type, const Transform& at, int32_t team);
// Starts an actor of a class that the game made where it stands and has not started: its parts are registered
// and begin, as the game's spawner does for an actor it made itself. False for an actor with no place, or
// while the game's own functions are missing.
bool start_made(uint64_t actor, uint64_t type);
// Asks the game to take an actor out of play quietly: no death, nothing dropped,
// no score. It happens at the start of the next world update. An actor from a
// pool goes back to it. TSUCHI_BUSY for one already on its way out.
TSUCHI_Result remove_actor(uint64_t actor, bool quiet = true, uint8_t cause = 0, uint64_t killer = 0);

} // namespace tsuchi::game
