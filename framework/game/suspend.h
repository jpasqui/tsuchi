// Suspending parts of an actor: its own decisions, its own movement, its
// physics, and its solidity. A suspended actor keeps its place in the world, is
// still drawn, and can still be found and hit; it only stops doing things of
// its own accord, so a mod can drive it.
//
// An actor that was walking keeps the speed its body had and would coast on
// for meters. So when its movement is suspended its body is also brought to
// rest, over the first few frames; after that only what pushes it moves it.
//
// The game sends an actor messages: it was hit, it was knocked, a whistle
// reached it. An actor's own decisions are what answer them. While those are
// suspended nothing does, so the messages are taken here every frame and
// handed to whoever asked to hear them.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

constexpr uint32_t kSuspendLogic = 1, kSuspendMovement = 2, kSuspendPhysics = 4, kSuspendCollision = 8,
                   kSuspendAll = kSuspendLogic | kSuspendMovement | kSuspendPhysics | kSuspendCollision;

// Starts keeping the holds. Called once when the framework starts.
void suspend_start();

// Suspends parts of an actor for a mod. TSUCHI_BUSY when another mod has parts of
// it suspended; TSUCHI_INVALID for no parts or an unknown one; TSUCHI_UNSUPPORTED when
// its body cannot be stopped safely, in which case nothing is changed.
TSUCHI_Result suspend_parts(TSUCHI_Mod* mod, uint64_t actor, uint32_t parts);
// Gives parts back. When the last is given back the actor is the game's again.
// TSUCHI_NOT_FOUND when the mod has nothing of this actor suspended.
TSUCHI_Result resume_parts(TSUCHI_Mod* mod, uint64_t actor, uint32_t parts);
// Which parts of the actor are suspended, by anyone.
uint32_t suspended_parts(uint64_t actor);

// A message taken from a suspended actor.
enum class MessageKind : uint32_t { damage, knock, whistle, other };
struct TakenMessage {
    uint64_t actor, sender; // sender is 0 when the game names none, or it is gone
    MessageKind kind;
    float amount;    // damage: how much. knock: the damage that comes with it
    uint32_t attack; // damage: the game's number for the kind of hit
    float vector[3]; // damage: where it came from. knock: the velocity to be thrown with
};
// One listener, told of every message as it is taken.
void on_message_taken(void (*listener)(const TakenMessage& message));

struct SuspendCounts {
    uint32_t actors;   // with something suspended now
    uint32_t messages; // taken since the start
};
const SuspendCounts& suspend_counts();

} // namespace tsuchi
