#include "motion.h"

#include "actors.h"
#include "holds.h"
#include "flow.h"
#include "tsuchi/game.h"
#include "objects.h"

namespace tsuchi {
namespace {

constexpr uint32_t kAskAgain = 8;    // frames a launch is asked for again while the actor is still on the ground
constexpr uint32_t kHoldFrames = 300; // the longest an actor is held awake
constexpr uint32_t kAirSteps = 3;     // physics steps off the ground that count as flying

// What is kept for an actor that is followed.
struct Motion {
    float velocity[3];
    bool launch, airborne, resume;
    uint32_t asks, age;
};

// Holds the actor awake when it is first followed, and starts following it afresh each time.
TSUCHI_Result begin(const Hold& hold, Motion& m, const float* const& velocity, bool fresh) {
    if (fresh) game::hold_awake(game::from_handle(hold.object), true);
    m.age = 0;
    m.airborne = false;
    m.resume = false;
    m.launch = velocity != nullptr;
    m.asks = velocity ? kAskAgain : 0;
    for (int i = 0; i < 3; ++i) m.velocity[i] = velocity ? velocity[i] : 0;
    return TSUCHI_OK;
}

// Once a frame. False once the actor has settled, or has been held as long as one is.
bool follow(const Hold& hold, Motion& m) {
    const uint64_t actor = game::from_handle(hold.object);
    uint32_t paused = 0;
    if (game::paused_kinds(paused) && (paused & TSUCHI_PAUSE_ACTORS)) {
        // Native actor updates cannot consume a queued launch while actors are paused.
        // Off-ground placement alone must not cancel it or spend its active-update budget.
        m.resume = m.launch && !m.airborne;
        return true;
    }
    ++m.age;
    if (m.resume) {
        m.resume = false;
        if (game::can_launch(actor)) {
            game::request_velocity(actor, m.velocity);
            return true; // one active actor update consumes it before flying can clear it
        }
    }
    const bool awake = game::body_awake(actor), flying = awake && game::air_steps(actor) >= kAirSteps;
    if (flying) m.airborne = true;
    if (m.launch && m.asks && !flying && !m.airborne && game::can_launch(actor)) {
        game::request_velocity(actor, m.velocity);
        --m.asks;
    } else if (m.launch && m.airborne) {
        game::drop_velocity_requests(actor); // in the air only gravity and what it hits move it
    }
    const bool settled = m.age >= 3 && awake && game::air_steps(actor) < kAirSteps && (m.airborne || !m.launch);
    return !settled && m.age < kHoldFrames;
}

// Lets go of the actor. One that is being destroyed takes its hold with it.
void let_go(const Hold& hold, Motion&, HoldEnd why) {
    if (why == HoldEnd::destroyed) return;
    if (const uint64_t actor = game::from_handle(hold.object)) game::hold_awake(actor, false);
}

// A hold of the framework's own: no mod holds an actor awake, and no mod's stopping ends it.
Holds<Motion, const float*, begin, follow, let_go> motions{"entity.motion", HoldOn::actor, true};

} // namespace

void motion_start() { motions.start(); }

void motion_begin(uint64_t actor, const float* velocity) {
    // With no memory to follow it by, the launch itself still happens; it just is not followed.
    if (const uint64_t handle = game::to_handle(actor)) motions.take(nullptr, handle, 0, velocity);
}

void motion_end(uint64_t actor) { motions.release(nullptr, game::to_handle(actor), 0); }

uint32_t motion_count() { return motions.count(); }
bool motion_following(uint64_t actor) { return motions.find(game::to_handle(actor), 0) != nullptr; }
bool motion_launch_pending(uint64_t actor) {
    const Motion* motion = motions.find(game::to_handle(actor), 0);
    return motion && motion->launch && !motion->airborne;
}

} // namespace tsuchi
