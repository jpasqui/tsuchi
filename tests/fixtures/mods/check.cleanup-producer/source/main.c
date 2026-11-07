/* No cleanup calls here: refusal makes the ordinary loader stop this mod. */
#include "../common.h"
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static uint64_t callback_requested;
static uint8_t forbidden_requested;
static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user; (void)event;
    /* A stopped mod cannot log. This one-shot write still exposes a leaked callback to the observer. */
    if (callback_requested) {
        *(volatile uint8_t *)(uintptr_t)callback_requested = forbidden_requested;
        callback_requested = 0;
    }
    host->mod->log(self, "cleanup forbidden callback after startup refusal");
}
static bool require_ok(TSUCHI_Result result, const char *what) {
    if (result == TSUCHI_OK) return true;
    char line[256] = "cleanup setup refused: "; tsuchi_check_add(line, what); tsuchi_check_add(line, " ");
    tsuchi_check_add(line, tsuchi_check_result_name(result)); host->mod->log(self, line); return false;
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given; self = mod; callback_requested = 0; tsuchi_check_start(host, self);
    char version[32]; uint64_t bytes = 0;
    if (host->mod->version_of(self, "check.cleanup", version, sizeof version, &bytes) != TSUCHI_OK) return TSUCHI_OK;
    CleanupConfig config; CleanupSnapshot before; TSUCHI_Object actor_class = {0}, replacement = {0};
    TSUCHI_Actor candidates[64], survivor = {0}, owned = {0}; uint64_t count = 0, raw = 0;
    if (!cleanup_config(&config) || !require_ok(host->asset->load(self, config.actor_path, &actor_class), "creature class") ||
        !require_ok(host->asset->load(self, config.material_path, &replacement), "replacement material") ||
        !require_ok(host->entity->list(self, actor_class, candidates, 64, &count), "donor list")) return TSUCHI_INVALID;
    for (uint64_t i = 0; i < count; ++i) {
        TSUCHI_Pose at = {0};
        if (host->entity->pose(self, candidates[i], &at) == TSUCHI_OK && at.location.x == cleanup_place.x && at.location.y == cleanup_place.y) survivor = candidates[i];
    }
    if (!survivor.id || !cleanup_snapshot(host, self, survivor, &before) || replacement.id == before.material.id ||
        !cleanup_locate(host, self, &config, survivor, &raw)) {
        host->mod->log(self, "cleanup setup refused: no fully initialized observer donor"); return TSUCHI_NOT_READY;
    }
    const uint64_t brain = cleanup_pointer(raw + config.ai), mover = cleanup_pointer(raw + config.mover);
    if (!brain || !mover) return TSUCHI_NOT_READY;
    forbidden_requested = cleanup_byte(raw + config.actor_requested) ? 0 : 1;
    TSUCHI_Vec3 place = cleanup_place; place.x += 1000;
    if (!require_ok(host->entity->spawn(self, actor_class, &place, &cleanup_facing, -1, false, &owned), "owned actor")) return TSUCHI_INVALID;
    cleanup_log_id(host, self, "owned", owned.id);
    if (!require_ok(host->entity->hold_suspended(self, survivor, CLEANUP_ALL), "full suspension")) return TSUCHI_INVALID;
    TSUCHI_EntityParts held = 0;
    if (!require_ok(host->entity->suspended(self, survivor, &held), "suspension flags") || held != CLEANUP_ALL ||
        cleanup_byte(brain + config.part_requested) || cleanup_byte(mover + config.part_requested)) return TSUCHI_INVALID;
    for (uint64_t i = 0; i < before.count; ++i) {
        const CleanupPart *part = &before.parts[i]; TSUCHI_Value value = {0};
        if (part->has_simulation && (!require_ok(cleanup_call(host, self, tsuchi_component_as_object(part->handle), "IsSimulatingPhysics", 0, 0, &value), "stopped simulation") || value.as.boolean)) return TSUCHI_INVALID;
        if (part->collision >= 0) {
            const int wanted = part->collision == 3 ? 1 : part->collision == 2 ? 0 : part->collision;
            if (!require_ok(cleanup_call(host, self, tsuchi_component_as_object(part->handle), "GetCollisionEnabled", 0, 0, &value), "stopped collision") || value.as.integer != wanted) return TSUCHI_INVALID;
        }
        if (part->has_visibility) {
            if (!require_ok(host->component->hold_hidden(self, part->handle, true), "subtree visibility")) return TSUCHI_INVALID;
            if (!require_ok(host->reflection->get(self, tsuchi_component_as_object(part->handle), "bVisible", &value), "hidden visibility") || value.as.boolean) return TSUCHI_INVALID;
        }
    }
    if (!require_ok(host->component->hold_material(self, before.mesh, before.slot, replacement), "distinct material") ||
        !cleanup_material_is(host, self, &before, replacement)) return TSUCHI_INVALID;
    TSUCHI_Object scene_class = {0}; TSUCHI_Component root_part = {0}, mesh_part = {0};
    if (!require_ok(host->reflection->find_class(self, "SceneComponent", &scene_class), "added component class") ||
        !require_ok(host->component->add(self, survivor, scene_class, (TSUCHI_Component){0}, "", &root_part), "root addition") ||
        !require_ok(host->component->add(self, survivor, scene_class, before.mesh, "", &mesh_part), "mesh addition")) return TSUCHI_INVALID;
    TSUCHI_Component parts[CLEANUP_PARTS]; uint64_t after = 0;
    if (!require_ok(host->component->list(self, survivor, parts, CLEANUP_PARTS, &after), "added component list") || after != before.count + 2) return TSUCHI_INVALID;
    callback_requested = raw + config.actor_requested;
    TSUCHI_Subscription subscription = {0};
    if (!require_ok(host->time->on_step(self, on_step, 0, &subscription), "producer callback")) return TSUCHI_INVALID;
    TSUCHI_Pose marker = {0};
    if (!require_ok(host->entity->pose(self, survivor, &marker), "setup marker place")) return TSUCHI_INVALID;
    marker.location.x += 500;
    if (!require_ok(host->entity->set_pose(self, survivor, &marker), "completed setup marker")) return TSUCHI_INVALID;
    cleanup_log_id(host, self, "added_root", root_part.id); cleanup_log_id(host, self, "added_mesh", mesh_part.id);
    host->mod->log(self, "cleanup prepared: AI movement physics collision subtree visibility distinct material two additions owned actor callback");
    return TSUCHI_NOT_READY;
}
