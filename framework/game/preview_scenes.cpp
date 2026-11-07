// The small owned controller around native temporary-area admission.
#include "preview_scenes.h"
#include <string.h>
#include "asset_bundles.h"
#include "content_path.h"
#include "new_maps.h"
#include "registry.h"

namespace tsuchi {
namespace {
constexpr Kind kPreview{"game.preview"};
constexpr uint32_t kMostPreviews = 4;
constexpr uint64_t kIssuedLimit = 1024;
struct Preview {
    TSUCHI_Mod* mod = nullptr;
    uint64_t id = 0, bundle = 0;
    uint32_t area = 0, generation = 0;
    char world_name[10]{}, scene_name[11]{}, world[512]{}, scene[512]{};
    bool live = false, admitted = false, retained = false, admitting = false;
};
Preview previews[kMostPreviews];
uint64_t next_id = 1;
bool admitting = false;

void names(Preview& p) {
    memcpy(p.world_name, "Area000_P", sizeof p.world_name);
    memcpy(p.scene_name, "SD_Area000", sizeof p.scene_name);
    for (uint32_t i = 0, divisor = 100; i < 3; ++i, divisor /= 10) {
        p.world_name[4 + i] = p.scene_name[7 + i] = char('0' + (p.area / divisor) % 10);
    }
}
Preview* lookup(TSUCHI_Mod* mod, uint64_t id, TSUCHI_Result& result) {
    for (Preview& p : previews) {
        if (!p.live || p.id != id) continue;
        if (p.mod != mod) { result = TSUCHI_DENIED; return nullptr; }
        result = TSUCHI_OK; return &p;
    }
    result = TSUCHI_NOT_FOUND; return nullptr;
}
void finish(Preview& p) {
    if (p.admitting || p.live || p.retained) return;
    release_preview_area(p.id);
    p = {};
}
void end_preview(void*, uint64_t id) {
    for (Preview& p : previews) {
        if (p.id != id || !p.live) continue;
        p.live = false;
        finish(p);
        return;
    }
}
struct Guard {
    Preview& preview;
    explicit Guard(Preview& p) : preview(p) { admitting = p.admitting = true; }
    ~Guard() {
        preview.admitting = false;
        admitting = false;
        finish(preview);
    }
};
struct ReservationGuard {
    ReservationGuard() { admitting = true; }
    ~ReservationGuard() { admitting = false; }
};
bool right_paths(const Preview& p, const char* world, const char* scene) {
    const size_t wn = strlen(world), sn = strlen(scene);
    if (wn >= sizeof p.world || sn >= sizeof p.scene ||
        content_path::wrong(world, wn, content_path::Names::package) ||
        content_path::wrong(scene, sn, content_path::Names::object)) return false;
    const char* wl = world;
    const char* sl = scene;
    for (const char* at = world; *at; ++at) if (*at == '/') wl = at + 1;
    for (const char* at = scene; *at; ++at) if (*at == '/') sl = at + 1;
    const size_t leaf = strlen(p.scene_name);
    return !strcmp(wl, p.world_name) && strlen(sl) == leaf * 2 + 1 &&
        !memcmp(sl, p.scene_name, leaf) && sl[leaf] == '.' && !strcmp(sl + leaf + 1, p.scene_name);
}
}

TSUCHI_Result reserve_game_preview(TSUCHI_Mod* mod, TSUCHI_PreviewScene* out) {
    *out = {};
    if (admitting) return TSUCHI_BUSY;
    if (next_id > kIssuedLimit) return TSUCHI_CAPACITY;
    Preview* available = nullptr;
    for (Preview& p : previews) if (!p.id && !available) available = &p;
    if (!available) return TSUCHI_CAPACITY;
    ReservationGuard guard;
    const uint64_t id = next_id;
    uint32_t area = 0;
    const TSUCHI_Result reserved = reserve_preview_area(id, &area);
    if (reserved != TSUCHI_OK) return reserved;
    if (!mod_usable(mod)) { release_preview_area(id); return TSUCHI_DENIED; }
    Preview& p = *available;
    p.mod = mod; p.id = id; p.area = area; p.live = true; names(p);
    const TSUCHI_Result owned = own(mod, kPreview, id, end_preview, nullptr);
    if (owned != TSUCHI_OK) {
        p.live = false; finish(p); return owned;
    }
    ++next_id;
    out->id = id;
    return TSUCHI_OK;
}

TSUCHI_Result game_preview_scene(TSUCHI_Mod* mod, TSUCHI_PreviewScene handle, TSUCHI_PreviewSceneInfo* out) {
    TSUCHI_Result result;
    Preview* p = lookup(mod, handle.id, result);
    if (!p) return result;
    *out = {p->area, p->world_name, p->scene_name, p->admitted, p->generation, p->world, p->scene};
    return TSUCHI_OK;
}

TSUCHI_Result admit_game_preview(TSUCHI_Mod* mod, TSUCHI_PreviewScene handle, TSUCHI_AssetBundle bundle,
                               const char* world, const char* scene) {
    TSUCHI_Result result;
    Preview* p = lookup(mod, handle.id, result);
    if (!p) return result;
    if (admitting) return TSUCHI_BUSY;
    if (!right_paths(*p, world, scene)) return TSUCHI_INVALID;
    Guard guard(*p);
    result = asset_bundle_package(mod, bundle, world);
    if (!p->live || !mod_usable(mod)) return TSUCHI_DENIED;
    if (result != TSUCHI_OK) return result;
    content_path::Parts scene_parts;
    content_path::split(scene, scene_parts);
    char package[512];
    memcpy(package, scene, scene_parts.package); package[scene_parts.package] = 0;
    result = asset_bundle_package(mod, bundle, package);
    if (!p->live || !mod_usable(mod)) return TSUCHI_DENIED;
    if (result != TSUCHI_OK) return result;
    if (p->admitted && p->bundle == bundle.id && !strcmp(p->world, world) && !strcmp(p->scene, scene)) return TSUCHI_OK;
    if (p->generation == UINT32_MAX) return TSUCHI_CAPACITY;
    bool retained = false;
    result = give_preview_area(mod, p->id, bundle, world, scene, &retained);
    p->retained = p->retained || retained;
    if (!p->live || !mod_usable(mod)) return TSUCHI_DENIED;
    if (result != TSUCHI_OK) return result;
    p->retained = p->admitted = true;
    p->bundle = bundle.id;
    memcpy(p->world, world, strlen(world) + 1); memcpy(p->scene, scene, strlen(scene) + 1);
    ++p->generation;
    return TSUCHI_OK;
}

TSUCHI_Result prepare_game_preview(TSUCHI_Mod* mod, TSUCHI_PreviewScene handle) {
    TSUCHI_Result result;
    Preview* p = lookup(mod, handle.id, result);
    if (!p) return result;
    if (admitting) return TSUCHI_BUSY;
    if (!p->admitted) return TSUCHI_NOT_READY;
    Guard guard(*p);
    result = prepare_preview_area(p->id);
    return p->live && mod_usable(mod) ? result : TSUCHI_DENIED;
}

TSUCHI_Result release_game_preview(TSUCHI_Mod* mod, TSUCHI_PreviewScene handle) {
    TSUCHI_Result result;
    Preview* p = lookup(mod, handle.id, result);
    if (!p) return result;
    if (admitting) return TSUCHI_BUSY;
    result = disown(mod, kPreview, p->id);
    if (result == TSUCHI_OK) end_preview(nullptr, p->id);
    return result;
}
}
