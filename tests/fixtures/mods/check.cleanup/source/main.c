/* Observes the ordinary loader stopping a producer whose startup refuses. */
#include "../../check.cleanup-producer/common.h"
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static CleanupConfig config;
static CleanupSnapshot before;
static TSUCHI_Actor survivor, owned;
static TSUCHI_Object actor_class, replacement;
static uint64_t raw, brain, mover;
static uint8_t requested[3];
static TSUCHI_Subscription stepping, spawned, destroyed;
static unsigned steps, births, removals, birth_count;
static uint64_t birth_handles[CLEANUP_PARTS];
static bool birth_overflow;
static bool ready, survivor_removed;
static const char *const checks[] = {"setup", "prepared", "ticks", "body", "visibility", "material", "components", "owned", "callbacks", "reused"};
static void on_spawned(void *user, const TSUCHI_EntitySpawned *event) {
    (void)user;
    /* Startup cleanup may already have invalidated this handle. Preserve identity without reading its class. */
    if (!ready || event->actor.id == survivor.id) return;
    if (birth_count < CLEANUP_PARTS) birth_handles[birth_count++] = event->actor.id;
    else birth_overflow = true;
    cleanup_log_id(host, self, "observed_birth", event->actor.id);
}
static void on_destroyed(void *user, const TSUCHI_EntityDestroyed *event) {
    (void)user;
    if (event->actor.id == survivor.id) { survivor_removed = true; return; }
    /* The removal event retains the producer's unique startup position, even though its actor is gone. */
    if (ready && event->location.x == cleanup_place.x + 1000 && event->location.y == cleanup_place.y) {
        if (!owned.id) owned = event->actor;
        if (event->actor.id == owned.id) { ++removals; cleanup_log_id(host, self, "producer_removal", owned.id); }
    }
}
static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user; (void)event;
    if (++steps < 3) return;
    bool alive = true;
    if (ready && !owned.id && steps < 60) return;
    if (ready && owned.id && host->actor->alive(self, owned, &alive) == TSUCHI_OK && alive && steps < 60) return;
    bool donor_alive = false;
    if (ready && (host->actor->alive(self, survivor, &donor_alive) != TSUCHI_OK || !donor_alive)) ready = false;
    if (!ready) {
        for (unsigned i = 1; i < sizeof checks / sizeof *checks; ++i) {
            tsuchi_check_begin(checks[i]); tsuchi_expect(false, "a full creature was not available during startup"); tsuchi_check_end();
        }
        host->cancel(self, stepping); return;
    }
    births = 0;
    for (unsigned i = 0; i < birth_count; ++i) if (birth_handles[i] == owned.id) ++births;
    tsuchi_check_begin("prepared");
    tsuchi_expect(!birth_overflow, "all startup birth handles fit the bounded observer buffer");
    TSUCHI_Pose at = {0};
    tsuchi_expect_result(host->entity->pose(self, survivor, &at), TSUCHI_OK, "the surviving creature's place");
    /* Written only after the producer independently verified every changed resource. */
    tsuchi_expect_near(at.location.x, cleanup_place.x + 500, 100, "the producer's completed setup marker");
    tsuchi_expect_number(births, 1, "exactly one producer-owned creature");
    tsuchi_check_end();
    tsuchi_check_begin("ticks");
    const bool same_parts = cleanup_pointer(raw + config.ai) == brain && cleanup_pointer(raw + config.mover) == mover;
    tsuchi_expect(same_parts, "original AI and movement component identity");
    tsuchi_expect_number(cleanup_byte(raw + config.actor_requested), requested[0], "original requested actor update");
    if (same_parts) {
        tsuchi_expect_number(cleanup_byte(brain + config.part_requested), requested[1], "original requested AI update");
        tsuchi_expect_number(cleanup_byte(mover + config.part_requested), requested[2], "original requested movement update");
    }
    TSUCHI_EntityParts held = CLEANUP_ALL;
    tsuchi_expect_result(host->entity->suspended(self, survivor, &held), TSUCHI_OK, "holds after producer refusal");
    tsuchi_expect_number(held, 0, "no suspension survives failed startup"); tsuchi_check_end();
    tsuchi_check_begin("body");
    for (uint64_t i = 0; i < before.count; ++i) {
        const CleanupPart *part = &before.parts[i]; TSUCHI_Value value = {0};
        if (part->has_simulation) {
            tsuchi_expect_result(cleanup_call(host, self, tsuchi_component_as_object(part->handle), "IsSimulatingPhysics", 0, 0, &value), TSUCHI_OK, "engine simulation after cleanup");
            tsuchi_expect(value.kind == TSUCHI_VALUE_BOOLEAN && value.as.boolean == part->simulating, "the original simulation state");
        }
        if (part->collision >= 0) {
            tsuchi_expect_result(cleanup_call(host, self, tsuchi_component_as_object(part->handle), "GetCollisionEnabled", 0, 0, &value), TSUCHI_OK, "engine collision after cleanup");
            tsuchi_expect(value.kind == TSUCHI_VALUE_INTEGER && value.as.integer == part->collision, "every original collision mode");
        }
    }
    tsuchi_check_end();
    tsuchi_check_begin("visibility");
    for (uint64_t i = 0; i < before.count; ++i) if (before.parts[i].has_visibility) {
        const CleanupPart *part = &before.parts[i]; TSUCHI_Value value = {0}; bool hidden = true;
        tsuchi_expect_result(host->reflection->get(self, tsuchi_component_as_object(part->handle), "bVisible", &value), TSUCHI_OK, "engine visibility after cleanup");
        tsuchi_expect(value.kind == TSUCHI_VALUE_BOOLEAN && value.as.boolean == part->visible, "the original component visibility");
        tsuchi_expect_result(host->component->hidden(self, part->handle, &hidden), TSUCHI_OK, "remaining visibility hold");
        tsuchi_expect(!hidden, "no visibility hold survives");
    }
    tsuchi_check_end();
    tsuchi_check_begin("material");
    tsuchi_expect(replacement.id != before.material.id && cleanup_material_is(host, self, &before, before.material), "the distinct override ended and the engine shows the original material");
    tsuchi_check_end();
    tsuchi_check_begin("components");
    TSUCHI_Component parts[CLEANUP_PARTS]; uint64_t count = 0;
    tsuchi_expect_result(host->component->list(self, survivor, parts, CLEANUP_PARTS, &count), TSUCHI_OK, "components after cleanup");
    tsuchi_expect_number((int64_t)count, (int64_t)before.count, "both producer additions are gone");
    for (uint64_t i = 0; i < before.count; ++i) {
        bool found = false;
        for (uint64_t j = 0; j < count; ++j) if (parts[j].id == before.parts[i].handle.id) found = true;
        tsuchi_expect(found, "every original component keeps its public identity");
    }
    tsuchi_check_end();
    tsuchi_check_begin("owned");
    tsuchi_expect(owned.id && !alive && removals == 1, "the exact owned actor left once without an explicit destroy");
    bool survivor_alive = false;
    tsuchi_expect_result(host->actor->alive(self, survivor, &survivor_alive), TSUCHI_OK, "the game-owned actor's life");
    tsuchi_expect(survivor_alive && !survivor_removed, "the keep=true creature survives"); tsuchi_check_end();
    tsuchi_check_begin("callbacks");
    tsuchi_expect_number(cleanup_byte(raw + config.actor_requested), requested[0], "the refused producer callback never changed its donor, even with logging rejected");
    char version[32]; uint64_t bytes = 0;
    tsuchi_expect_result(host->mod->version_of(self, "check.cleanup-producer", version, sizeof version, &bytes), TSUCHI_NOT_FOUND, "the failed producer is not running");
    tsuchi_check_end();
    tsuchi_check_begin("reused");
    tsuchi_expect_result(host->entity->hold_suspended(self, survivor, TSUCHI_ENTITY_PARTS_LOGIC), TSUCHI_OK, "another mod can suspend it");
    tsuchi_expect_result(host->entity->release_suspended(self, survivor, TSUCHI_ENTITY_PARTS_LOGIC), TSUCHI_OK, "ending the observer's new hold");
    tsuchi_expect_result(host->component->hold_material(self, before.mesh, before.slot, replacement), TSUCHI_OK, "another mod can hold the formerly exclusive material slot");
    tsuchi_expect_result(host->component->release_material(self, before.mesh, before.slot), TSUCHI_OK, "ending the observer's new material hold");
    tsuchi_check_end();
    host->cancel(self, spawned); host->cancel(self, destroyed); host->cancel(self, stepping);
    /* The observer now removes its game-owned donor only after all restoration observations. */
    host->entity->destroy(self, survivor);
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given; self = mod; ready = survivor_removed = birth_overflow = false; steps = births = removals = birth_count = 0;
    memset(&before, 0, sizeof before); survivor.id = owned.id = actor_class.id = replacement.id = raw = brain = mover = 0;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof checks / sizeof *checks; ++i) tsuchi_check_plan(checks[i]);
    tsuchi_check_begin("setup");
    bool ok = tsuchi_expect(cleanup_config(&config), "addresses and paths from the game table");
    if (ok) ok = tsuchi_expect_result(host->asset->load(self, config.actor_path, &actor_class), TSUCHI_OK, "loading the full creature class");
    if (ok) ok = tsuchi_expect_result(host->asset->load(self, config.material_path, &replacement), TSUCHI_OK, "loading a distinct replacement material");
    if (ok) ok = tsuchi_expect_result(host->entity->spawn(self, actor_class, &cleanup_place, &cleanup_facing, -1, true, &survivor), TSUCHI_OK, "giving a full creature to the game");
    if (ok) ok = tsuchi_expect(cleanup_locate(host, self, &config, survivor, &raw), "locating its native requested states through the public hook");
    if (ok) {
        brain = cleanup_pointer(raw + config.ai); mover = cleanup_pointer(raw + config.mover);
        ok = tsuchi_expect(brain && mover, "the creature already has native AI and movement");
    }
    if (ok) {
        requested[0] = cleanup_byte(raw + config.actor_requested); requested[1] = cleanup_byte(brain + config.part_requested); requested[2] = cleanup_byte(mover + config.part_requested);
        const bool complete = cleanup_snapshot(host, self, survivor, &before);
        cleanup_log_id(host, self, "original_actor_requested", requested[0]);
        cleanup_log_id(host, self, "original_ai_requested", requested[1]);
        cleanup_log_id(host, self, "original_mover_requested", requested[2]);
        cleanup_log_id(host, self, "original_parts", before.count);
        cleanup_log_id(host, self, "original_simulating_parts", before.simulations);
        cleanup_log_id(host, self, "original_blocking_parts", before.blocking);
        cleanup_log_id(host, self, "original_visible_parts", before.visible);
        ok = tsuchi_expect(complete, "the creature already has visible, simulating, blocking and material components");
        ok = tsuchi_expect(replacement.id != before.material.id, "the replacement differs from its original material") && ok;
    }
    if (ok) {
        cleanup_log_id(host, self, "survivor", survivor.id);
        ok = tsuchi_expect_result(host->entity->on_spawned(self, on_spawned, 0, &spawned), TSUCHI_OK, "observing the producer's birth");
        ok = tsuchi_expect_result(host->entity->on_destroyed(self, on_destroyed, 0, &destroyed), TSUCHI_OK, "observing its automatic removal") && ok;
    }
    ready = ok; tsuchi_check_end();
    if (!ready && survivor.id) host->entity->destroy(self, survivor);
    if (host->time->on_step(self, on_step, 0, &stepping) != TSUCHI_OK) return TSUCHI_NOT_READY;
    return TSUCHI_OK;
}
