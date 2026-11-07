/* The game facts arrive in targets.txt, generated from game/table.yaml. */
#ifndef CLEANUP_COMMON_H
#define CLEANUP_COMMON_H
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"
#define CLEANUP_PARTS 128
#define CLEANUP_ALL (TSUCHI_ENTITY_PARTS_LOGIC | TSUCHI_ENTITY_PARTS_MOVEMENT | TSUCHI_ENTITY_PARTS_PHYSICS | TSUCHI_ENTITY_PARTS_COLLISION)
static const TSUCHI_Vec3 cleanup_place = {12000, 13000, 10000};
static const TSUCHI_Rotation cleanup_facing = {0, 0, 0};
typedef struct CleanupConfig {
    uint64_t table, setter, stock, ai, mover, actor_requested, part_requested;
    char actor_path[192], material_path[192];
} CleanupConfig;
typedef struct CleanupPart {
    TSUCHI_Component handle;
    bool visible, has_visibility, simulating, has_simulation;
    int collision;
} CleanupPart;
typedef struct CleanupSnapshot {
    CleanupPart parts[CLEANUP_PARTS];
    uint64_t count;
    TSUCHI_Component mesh;
    TSUCHI_Object material;
    uint32_t slot;
    unsigned simulations, blocking, visible;
} CleanupSnapshot;
static inline bool cleanup_config(CleanupConfig *out) {
    char text[1024]; const char *lines[9];
    if (tsuchi_check_lines("targets.txt", text, sizeof text, lines, 9) != 9) return false;
    uint64_t values[7] = {0};
    for (unsigned i = 0; i < 7; ++i) {
        const char *at = lines[i]; if (at[0] == '0' && at[1] == 'x') at += 2;
        unsigned digits = 0;
        while (*at) {
            unsigned digit;
            if (*at >= '0' && *at <= '9') digit = (unsigned)(*at - '0');
            else if (*at >= 'a' && *at <= 'f') digit = (unsigned)(*at - 'a' + 10);
            else if (*at >= 'A' && *at <= 'F') digit = (unsigned)(*at - 'A' + 10);
            else return false;
            if (++digits > 16) return false;
            values[i] = values[i] * 16 + digit; ++at;
        }
        if (!digits) return false;
    }
    out->table = values[0]; out->setter = values[1]; out->stock = values[2];
    out->ai = values[3]; out->mover = values[4]; out->actor_requested = values[5]; out->part_requested = values[6];
    if (strlen(lines[7]) >= sizeof out->actor_path || strlen(lines[8]) >= sizeof out->material_path) return false;
    memcpy(out->actor_path, lines[7], strlen(lines[7]) + 1); memcpy(out->material_path, lines[8], strlen(lines[8]) + 1);
    return out->table && out->setter && out->ai && out->mover && out->actor_requested && out->part_requested;
}
static inline uint64_t cleanup_pointer(uint64_t at) { return *(const uint64_t *)(uintptr_t)at; }
static inline uint8_t cleanup_byte(uint64_t at) { return *(const uint8_t *)(uintptr_t)at; }
static inline TSUCHI_Result cleanup_call(const TSUCHI_Host *host, TSUCHI_Mod *mod, TSUCHI_Object object,
                                       const char *name, const char *argument, int64_t integer, TSUCHI_Value *out) {
    TSUCHI_PreparedCall call = {0};
    TSUCHI_Result result = host->reflection->begin_call(mod, object, name, &call);
    if (result != TSUCHI_OK) return result;
    if (argument) {
        TSUCHI_Value value = {0}; value.kind = TSUCHI_VALUE_INTEGER; value.as.integer = integer;
        result = host->reflection->call_set(mod, call, argument, &value);
    }
    if (result == TSUCHI_OK) result = host->reflection->invoke(mod, call);
    if (result == TSUCHI_OK) result = host->reflection->call_get(mod, call, "ReturnValue", out);
    const TSUCHI_Result ended = host->reflection->end_call(mod, call);
    return result == TSUCHI_OK ? ended : result;
}
static inline bool cleanup_snapshot(const TSUCHI_Host *host, TSUCHI_Mod *mod, TSUCHI_Actor actor, CleanupSnapshot *out) {
    memset(out, 0, sizeof *out);
    TSUCHI_Component parts[CLEANUP_PARTS];
    if (host->component->list(mod, actor, parts, CLEANUP_PARTS, &out->count) != TSUCHI_OK || !out->count) return false;
    for (uint64_t i = 0; i < out->count; ++i) {
        CleanupPart *part = &out->parts[i]; part->handle = parts[i]; part->collision = -1;
        TSUCHI_Actor owner = {0}; TSUCHI_Value value = {0};
        if (host->component->owner(mod, parts[i], &owner) != TSUCHI_OK || owner.id != actor.id) return false;
        const TSUCHI_Object object = tsuchi_component_as_object(parts[i]);
        if (host->reflection->get(mod, object, "bVisible", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_BOOLEAN) {
            part->has_visibility = true; part->visible = value.as.boolean; out->visible += part->visible;
        }
        if (cleanup_call(host, mod, object, "IsSimulatingPhysics", 0, 0, &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_BOOLEAN) {
            part->has_simulation = true; part->simulating = value.as.boolean; out->simulations += part->simulating;
        }
        if (cleanup_call(host, mod, object, "GetCollisionEnabled", 0, 0, &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_INTEGER) {
            part->collision = (int)value.as.integer; out->blocking += part->collision == 2 || part->collision == 3;
        }
        if (!out->mesh.id && cleanup_call(host, mod, object, "GetNumMaterials", 0, 0, &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_INTEGER) {
            for (uint32_t slot = 0; slot < (uint32_t)value.as.integer && slot < 64; ++slot)
                if (host->component->material(mod, parts[i], slot, &out->material) == TSUCHI_OK) { out->mesh = parts[i]; out->slot = slot; break; }
        }
    }
    return out->visible && out->simulations && out->blocking && out->mesh.id && out->material.id;
}
static inline bool cleanup_material_is(const TSUCHI_Host *host, TSUCHI_Mod *mod, const CleanupSnapshot *state, TSUCHI_Object wanted) {
    TSUCHI_Value value = {0};
    return cleanup_call(host, mod, tsuchi_component_as_object(state->mesh), "GetMaterial", "ElementIndex", state->slot, &value) == TSUCHI_OK &&
           value.kind == TSUCHI_VALUE_OBJECT && value.as.object.id == wanted.id;
}
static inline void cleanup_log_id(const TSUCHI_Host *host, TSUCHI_Mod *mod, const char *name, uint64_t id) {
    char line[128] = "cleanup "; tsuchi_check_add(line, name); tsuchi_check_add(line, "="); tsuchi_check_add_number(line, (int64_t)id); host->mod->log(mod, line);
}

typedef void (*CleanupTick)(void *, bool);
static TSUCHI_Hook cleanup_locator;
static uint64_t cleanup_located;
static void cleanup_capture(void *actor, bool on) {
    cleanup_located = (uint64_t)(uintptr_t)actor;
    ((CleanupTick)cleanup_locator.original)(actor, on);
}
static inline bool cleanup_locate(const TSUCHI_Host *host, TSUCHI_Mod *mod, const CleanupConfig *config, TSUCHI_Actor actor, uint64_t *raw) {
    cleanup_located = 0; memset(&cleanup_locator, 0, sizeof cleanup_locator);
    if (host->hook->wrap_address(mod, config->table + config->setter, config->stock, (void *)cleanup_capture, &cleanup_locator) != TSUCHI_OK) return false;
    const TSUCHI_Result held = host->entity->hold_suspended(mod, actor, TSUCHI_ENTITY_PARTS_LOGIC);
    const uint64_t first = cleanup_located;
    const TSUCHI_Result released = held == TSUCHI_OK ? host->entity->release_suspended(mod, actor, TSUCHI_ENTITY_PARTS_LOGIC) : held;
    const bool same = first && first == cleanup_located;
    const TSUCHI_Result removed = host->hook->remove(mod, cleanup_locator.id);
    /* The continuation is usable only while the locator is installed. */
    memset(&cleanup_locator, 0, sizeof cleanup_locator);
    *raw = same ? first : 0;
    return held == TSUCHI_OK && released == TSUCHI_OK && removed == TSUCHI_OK && same;
}
#endif
