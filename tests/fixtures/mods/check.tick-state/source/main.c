/* Public SDK hold/release, with independent native requested-state observations.
 * All offsets come from targets.txt generated from game/table.yaml. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

typedef void (*TickSetter)(void *, bool);
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Actor owned;
static TSUCHI_Subscription listener;
static TSUCHI_Hook locator;
static uint64_t target[9], raw_actor, raw_brain, raw_mover, setter_entry, count;
static TSUCHI_Component parts[128];
static TSUCHI_Object classes[128];
static unsigned calls, steps, phase, age, case_index;
static bool original[3];
static char donor[256];

static const char *const names[] = {"locator", "components", "all_disabled", "ai_disabled", "mover_disabled", "movement_disabled", "early_release", "halt_release", "removed"};
struct Case { bool actor, brain, mover; TSUCHI_EntityParts held; unsigned hold, after; };
static const struct Case cases[] = {
    {false, false, false, 3, 45, 45}, {true, false, true, 3, 45, 45},
    {true, true, false, 3, 45, 45}, {false, true, true, 2, 45, 45},
    {false, false, false, 3, 0, 15}, {false, false, false, 3, 4, 60}
};
static uint64_t word(uint64_t at) { return *(volatile const uint64_t *)(uintptr_t)at; }
static bool requested(uint64_t at) { return *(volatile const unsigned char *)(uintptr_t)at != 0; }
static bool alive(void) {
    bool present = false;
    return host->actor->alive(self, owned, &present) == TSUCHI_OK && present;
}
static void locate(void *actor, bool enabled) {
    raw_actor = (uint64_t)(uintptr_t)actor;
    setter_entry = word(word(raw_actor) + target[3]);
    ++calls;
    ((TickSetter)locator.original)(actor, enabled);
}
static void set_actor(bool on) { ((TickSetter)(uintptr_t)word(word(raw_actor) + target[3]))((void *)(uintptr_t)raw_actor, on); }
static void set_part(uint64_t part, bool on) { ((TickSetter)(uintptr_t)word(word(part) + target[8]))((void *)(uintptr_t)part, on); }
static void state(bool actor, bool brain, bool mover) {
    tsuchi_expect_number(requested(raw_actor + target[4]), actor, "the actor's native requested byte");
    tsuchi_expect_number(requested(raw_brain + target[7]), brain, "the AI's native requested byte");
    tsuchi_expect_number(requested(raw_mover + target[7]), mover, "the mover's native requested byte");
}
static void identity(void) {
    tsuchi_expect_number(word(raw_actor + target[5]), raw_brain, "the original native AI identity");
    tsuchi_expect_number(word(raw_actor + target[6]), raw_mover, "the original native mover identity");
    TSUCHI_Component now[128]; uint64_t got = 0;
    if (!tsuchi_expect_result(host->component->list(self, owned, now, 128, &got), TSUCHI_OK, "the current public component list")) return;
    for (uint64_t i = 0; i < count; ++i) {
        bool found = false;
        for (uint64_t j = 0; j < got; ++j) if (now[j].id == parts[i].id) found = true;
        TSUCHI_Actor owner = {0}; TSUCHI_Object type = {0}, object = {parts[i].id};
        tsuchi_expect(found, "an original public component remains listed");
        tsuchi_expect_result(host->component->owner(self, parts[i], &owner), TSUCHI_OK, "an original component's owner");
        tsuchi_expect_number(owner.id, owned.id, "the original owning actor");
        tsuchi_expect_result(host->reflection->class_of(self, object, &type), TSUCHI_OK, "an original component's class");
        tsuchi_expect_number(type.id, classes[i].id, "the original component class");
    }
}
static void flags(TSUCHI_EntityParts expected) {
    TSUCHI_EntityParts got = 0;
    tsuchi_expect_result(host->entity->suspended(self, owned, &got), TSUCHI_OK, "the public suspension flags");
    tsuchi_expect_number(got, expected, "the exact held parts");
}
static void stop(void) {
    if (owned.id && alive()) {
        host->entity->release_suspended(self, owned, 3);
        host->entity->destroy(self, owned);
    }
    host->cancel(self, listener);
    phase = 9;
}
static void dependencies(unsigned first, const char *why) {
    for (unsigned i = first; i < sizeof names / sizeof *names; ++i) {
        tsuchi_check_begin(names[i]); tsuchi_expect(false, why); tsuchi_check_end();
    }
    stop();
}
static bool read_targets(void) {
    char text[768]; const char *lines[10];
    if (tsuchi_check_lines("targets.txt", text, sizeof text, lines, 10) != 10 || strlen(lines[0]) >= sizeof donor) return false;
    memcpy(donor, lines[0], strlen(lines[0]) + 1);
    for (unsigned i = 0; i < 9; ++i) {
        const char *at = lines[i + 1]; if (at[0] == '0' && at[1] == 'x') at += 2;
        unsigned digits = 0; uint64_t value = 0;
        for (; *at; ++at) {
            unsigned n;
            if (*at >= '0' && *at <= '9') n = (unsigned)(*at - '0');
            else if (*at >= 'a' && *at <= 'f') n = (unsigned)(*at - 'a' + 10);
            else if (*at >= 'A' && *at <= 'F') n = (unsigned)(*at - 'A' + 10);
            else return false;
            if (++digits > 16) return false;
            value = (value << 4) | n;
        }
        if (!digits) return false;
        target[i] = value;
    }
    return target[0] && target[2] && target[3] && target[4] && target[5] && target[6] && target[7] && target[8];
}
static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user; (void)event; ++steps;
    if (!phase) {
        bool control = false;
        if (host->game->has_control(self, &control) != TSUCHI_OK || !control) { tsuchi_check_waiting("locator", "ordinary gameplay control"); return; }
        TSUCHI_Actor sources[512]; uint64_t found = 0; TSUCHI_Pose position;
        TSUCHI_Actor captain = {0};
        bool located = host->player->captain(self, 0, &captain) == TSUCHI_OK &&
                       host->entity->pose(self, captain, &position) == TSUCHI_OK;
        if (!located && host->entity->list(self, (TSUCHI_Object){0}, sources, 512, &found) == TSUCHI_OK) {
            for (uint64_t i = 0; i < found; ++i) {
                if (host->entity->pose(self, sources[i], &position) == TSUCHI_OK) { located = true; break; }
            }
        }
        if (!located) { tsuchi_check_waiting("locator", "an actor's position for the owned creature"); return; }
        TSUCHI_Object type = {0};
        tsuchi_check_begin("locator");
        bool loaded = tsuchi_expect_result(host->asset->load(self, donor, &type), TSUCHI_OK, "the native creature class from the game table");
        position.location.x += 250; position.location.z += 20;
        if (loaded) loaded = tsuchi_expect_result(host->entity->spawn(self, type, &position.location, &position.rotation, -1, false, &owned), TSUCHI_OK, "an owned native creature");
        if (!loaded) { tsuchi_check_end(); dependencies(1, "the native creature could not be created"); return; }
        phase = 1; age = steps; return;
    }
    if (!alive() && phase != 5) {
        if (phase == 2) tsuchi_check_begin(names[2 + case_index]);
        tsuchi_expect(false, "the owned actor left play during the matrix"); tsuchi_check_end();
        dependencies(phase == 1 ? 1 : 3 + case_index, "the owned creature is absent"); return;
    }
    if (phase == 1) {
        if (steps - age < 30) return;
        bool ready = tsuchi_expect_result(host->hook->wrap_address(self, target[0], target[1], (void *)locate, &locator), TSUCHI_OK, "one temporary native tick locator");
        if (ready) {
            tsuchi_expect_result(host->entity->hold_suspended(self, owned, TSUCHI_ENTITY_PARTS_LOGIC), TSUCHI_OK, "locating only the owned actor");
            tsuchi_expect_number(calls, 1, "one synchronous requested-tick call");
            tsuchi_expect_result(host->hook->remove(self, locator.id), TSUCHI_OK, "removing the native locator before the matrix");
            tsuchi_expect_result(host->entity->release_suspended(self, owned, TSUCHI_ENTITY_PARTS_LOGIC), TSUCHI_OK, "ending the locator's temporary hold");
        }
        ready = ready && raw_actor && calls == 1;
        if (ready) {
            // Removing a wrapper leaves Tsuchi's stable entry dispatch installed.
            // The returned original is another trampoline, not the entry pointer.
            tsuchi_expect_number(word(word(raw_actor) + target[3]), setter_entry, "the stable native requested-tick dispatch after wrapper removal");
        }
        bool passed = tsuchi_check_end_with(target[1] ? "native requested-tick locator observed, wrapper removed" : "host model only, no native stock-address claim");
        if (!ready || !passed) { dependencies(1, "the native locator was not qualified"); return; }
        tsuchi_check_begin("components");
        raw_brain = word(raw_actor + target[5]); raw_mover = word(raw_actor + target[6]);
        ready = tsuchi_expect(raw_brain && raw_mover && raw_brain != raw_mover, "distinct initialized AI and mover components");
        if (ready) ready = tsuchi_expect(word(word(raw_brain) + target[8]) && word(word(raw_mover) + target[8]), "both native component setters");
        if (ready) {
            original[0] = requested(raw_actor + target[4]); original[1] = requested(raw_brain + target[7]); original[2] = requested(raw_mover + target[7]);
            ready = tsuchi_expect_result(host->component->list(self, owned, parts, 128, &count), TSUCHI_OK, "the original public component list");
            for (uint64_t i = 0; ready && i < count; ++i) {
                TSUCHI_Object object = {parts[i].id};
                ready = tsuchi_expect_result(host->reflection->class_of(self, object, &classes[i]), TSUCHI_OK, "an original component class");
            }
            if (ready) identity();
        }
        passed = tsuchi_check_end();
        if (!ready || !passed) { dependencies(2, "the full native tick components were unavailable"); return; }
        phase = 2;
    }
    if (phase == 2) {
        const struct Case *which = cases + case_index;
        tsuchi_check_begin(names[2 + case_index]); identity(); flags(0);
        set_actor(which->actor); set_part(raw_brain, which->brain); set_part(raw_mover, which->mover);
        state(which->actor, which->brain, which->mover);
        tsuchi_expect_result(host->entity->hold_suspended(self, owned, which->held), TSUCHI_OK, "holding the specified parts");
        flags(which->held); age = steps; phase = 3;
    }
    if (phase == 3) {
        const struct Case *which = cases + case_index;
        identity(); flags(which->held);
        if (steps - age >= 4) state(which->held & 1 ? false : which->actor, which->held & 1 ? false : which->brain, which->held & 2 ? false : which->mover);
        if (steps - age < which->hold) return;
        tsuchi_expect_result(host->entity->release_suspended(self, owned, which->held), TSUCHI_OK, "releasing at the chosen halt boundary");
        flags(0); state(which->actor, which->brain, which->mover); identity(); age = steps; phase = 4;
    }
    if (phase == 4) {
        const struct Case *which = cases + case_index;
        identity(); flags(0); state(which->actor, which->brain, which->mover);
        if (steps - age < which->after) return;
        tsuchi_check_end_with("requested actor/AI/mover states and original components stayed restored");
        if (++case_index < sizeof cases / sizeof *cases) { phase = 2; return; }
        set_actor(original[0]); set_part(raw_brain, original[1]); set_part(raw_mover, original[2]);
        tsuchi_check_begin("removed"); state(original[0], original[1], original[2]);
        tsuchi_expect_result(host->entity->destroy(self, owned), TSUCHI_OK, "removing the matrix's owned actor");
        phase = 5; age = steps; return;
    }
    if (phase == 5) {
        if (alive() && steps - age < 60) return;
        tsuchi_expect(!alive(), "the owned actor left play after the matrix");
        tsuchi_check_end(); host->cancel(self, listener); phase = 9;
    }
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given; self = mod; owned.id = listener.id = 0;
    memset(&locator, 0, sizeof locator); memset(target, 0, sizeof target);
    raw_actor = raw_brain = raw_mover = setter_entry = count = 0; calls = steps = phase = age = case_index = 0;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) tsuchi_check_plan(names[i]);
    if (!read_targets()) { dependencies(0, "the game table's complete tick targets are required"); return TSUCHI_OK; }
    if (host->time->on_step(self, on_step, 0, &listener) != TSUCHI_OK) { dependencies(0, "a step subscription is required"); return TSUCHI_OK; }
    tsuchi_check_waiting("locator", "ordinary gameplay control"); return TSUCHI_OK;
}
