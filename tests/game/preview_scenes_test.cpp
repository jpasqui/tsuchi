// Owned preview controller with native and immutable bundle stand-ins. No game result.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include "preview_scenes.h"
#include "game_service.h"
#include "registry.h"

static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("line %d: %s\n", __LINE__, #x); } } while (0)
static std::map<uint64_t, uint32_t> claims;
static std::set<uint64_t> retained_claims;
static bool ready = true, inactive = true, pending = false, retain = true, stop_in_give = false, nested = false;
static bool stop_in_reserve, nested_reserve, stop_in_prepare, nested_prepare;
static TSUCHI_Result give_result = TSUCHI_OK;
static TSUCHI_Mod* preview_mod;
static TSUCHI_PreviewScene active;
static int given, prepared;
static int member_calls, stop_at_member;
static const char* world1 = "/Game/First/Area005_P";
static const char* scene1 = "/Game/First/SD_Area005.SD_Area005";
static const char* world2 = "/Game/Second/Area005_P";
static const char* scene2 = "/Game/Second/SD_Area005.SD_Area005";

namespace tsuchi {
TSUCHI_Result reserve_preview_area(uint64_t key, uint32_t* area) {
    if (!ready) return TSUCHI_NOT_READY;
    if (pending) return TSUCHI_BUSY;
    if (nested_reserve) {
        TSUCHI_PreviewScene ignored{};
        CHECK(game_service::table.reserve_preview(preview_mod, &ignored) == TSUCHI_BUSY && !ignored.id);
    }
    for (uint32_t n : {5u, 7u, 8u, 9u}) {
        bool used = false;
        for (const auto& p : claims) used = used || p.second == n;
        if (!used) {
            claims[key] = *area = n;
            if (stop_in_reserve) mod_stop(preview_mod);
            return TSUCHI_OK;
        }
    }
    return TSUCHI_CAPACITY;
}
void release_preview_area(uint64_t key) { if (!retained_claims.count(key)) claims.erase(key); }
TSUCHI_Result asset_bundle_package(TSUCHI_Mod* mod, TSUCHI_AssetBundle bundle, const char* path) {
    if (++member_calls == stop_at_member) { mod_stop(mod); return TSUCHI_NOT_FOUND; }
    if (mod != preview_mod || bundle.id == 90) return TSUCHI_DENIED;
    if (!bundle.id || bundle.id == 91) return TSUCHI_NOT_FOUND;
    if (bundle.id == 92) return TSUCHI_NOT_READY;
    if (bundle.id == 93 || !strncmp(path, "/Game/Missing/", 14)) return TSUCHI_INVALID;
    return TSUCHI_OK;
}
TSUCHI_Result give_preview_area(TSUCHI_Mod* mod, uint64_t key, TSUCHI_AssetBundle,
                               const char*, const char*, bool* kept) {
    CHECK(claims.count(key));
    if (!ready) return TSUCHI_NOT_READY;
    if (!inactive || pending) return TSUCHI_BUSY;
    if (nested) {
        TSUCHI_PreviewScene ignored{};
        CHECK(game_service::table.reserve_preview(mod, &ignored) == TSUCHI_BUSY && !ignored.id);
        CHECK(game_service::table.admit_preview(mod, active, {1}, world1, scene1) == TSUCHI_BUSY);
        CHECK(game_service::table.prepare_preview(mod, active) == TSUCHI_BUSY);
        CHECK(game_service::table.release_preview(mod, active) == TSUCHI_BUSY);
    }
    ++given;
    *kept = retain;
    if (retain) retained_claims.insert(key);
    if (stop_in_give) mod_stop(mod);
    return give_result;
}
TSUCHI_Result prepare_preview_area(uint64_t key) {
    CHECK(retained_claims.count(key));
    if (!ready) return TSUCHI_NOT_READY;
    if (pending) return TSUCHI_BUSY;
    if (nested_prepare) {
        TSUCHI_PreviewScene ignored{};
        CHECK(game_service::table.reserve_preview(preview_mod, &ignored) == TSUCHI_BUSY && !ignored.id);
        CHECK(game_service::table.admit_preview(preview_mod, active, {1}, world1, scene1) == TSUCHI_BUSY);
        CHECK(game_service::table.prepare_preview(preview_mod, active) == TSUCHI_BUSY);
        CHECK(game_service::table.release_preview(preview_mod, active) == TSUCHI_BUSY);
    }
    ++prepared;
    if (stop_in_prepare) mod_stop(preview_mod);
    return TSUCHI_OK;
}
}
static TSUCHI_PreviewScene reserve(TSUCHI_Mod* mod) {
    TSUCHI_PreviewScene p{};
    CHECK(tsuchi::game_service::table.reserve_preview(mod, &p) == TSUCHI_OK && p.id);
    return p;
}
int main(int argc, char** argv) {
    using namespace tsuchi;
    game_service::Backend backend{};
    backend.reserve_preview = reserve_game_preview; backend.preview_scene = game_preview_scene;
    backend.admit_preview = admit_game_preview; backend.prepare_preview = prepare_game_preview;
    backend.release_preview = release_game_preview;
    game_service::set_backend(&backend);
    preview_mod = mod_create("dev.server");
    TSUCHI_Mod* other = mod_create("other.tool");
    const std::string mode = argc > 1 ? argv[1] : "policy";
    const auto& api = game_service::table;
    if (mode == "policy") {
        TSUCHI_PreviewScene p{};
        ready = false;
        for (int i = 0; i < 1030; ++i)
            CHECK(api.reserve_preview(preview_mod, &p) == TSUCHI_NOT_READY && !p.id);
        ready = true; pending = true;
        CHECK(api.reserve_preview(preview_mod, &p) == TSUCHI_BUSY && !p.id);
        pending = false; p = reserve(preview_mod);
        TSUCHI_PreviewSceneInfo info{};
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_OK && info.area == 5 && !info.admitted && !info.generation);
        CHECK(!strcmp(info.world_name, "Area005_P") && !strcmp(info.scene_name, "SD_Area005") && !*info.world_package && !*info.scene_object);
        CHECK(api.preview_scene(other, p, &info) == TSUCHI_DENIED);
        CHECK(api.prepare_preview(preview_mod, p) == TSUCHI_NOT_READY);
        CHECK(api.admit_preview(preview_mod, p, {1}, "/Game/A/Area007_P", scene1) == TSUCHI_INVALID);
        CHECK(api.admit_preview(preview_mod, p, {1}, world1, "/Game/A/SD_Area005.Wrong") == TSUCHI_INVALID);
        CHECK(api.admit_preview(preview_mod, p, {90}, world1, scene1) == TSUCHI_DENIED);
        CHECK(api.admit_preview(preview_mod, p, {91}, world1, scene1) == TSUCHI_NOT_FOUND);
        CHECK(api.admit_preview(preview_mod, p, {92}, world1, scene1) == TSUCHI_NOT_READY);
        CHECK(api.admit_preview(preview_mod, p, {93}, world1, scene1) == TSUCHI_INVALID);
        CHECK(!given && !prepared);
        CHECK(api.admit_preview(preview_mod, p, {1}, world1, scene1) == TSUCHI_OK && given == 1 && !prepared);
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_OK && info.admitted && info.generation == 1 && !strcmp(info.world_package, world1));
        inactive = false; pending = true;
        CHECK(api.admit_preview(preview_mod, p, {1}, world1, scene1) == TSUCHI_OK && given == 1 && !prepared);
        CHECK(api.prepare_preview(preview_mod, p) == TSUCHI_BUSY);
        pending = false;
        CHECK(api.prepare_preview(preview_mod, p) == TSUCHI_OK && prepared == 1);
        CHECK(api.prepare_preview(preview_mod, p) == TSUCHI_OK && prepared == 2);
        CHECK(api.admit_preview(preview_mod, p, {2}, world2, scene2) == TSUCHI_BUSY);
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_OK && info.generation == 1 && !strcmp(info.world_package, world1));
        inactive = true;
        CHECK(api.admit_preview(preview_mod, p, {2}, world2, scene2) == TSUCHI_OK);
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_OK && info.generation == 2 && !strcmp(info.scene_object, scene2));
        CHECK(api.admit_preview(preview_mod, p, {1}, world1, scene1) == TSUCHI_OK);
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_OK && info.generation == 3 && !strcmp(info.world_package, world1));
        CHECK(api.release_preview(other, p) == TSUCHI_DENIED);
        CHECK(api.release_preview(preview_mod, p) == TSUCHI_OK && claims.size() == 1);
        CHECK(api.preview_scene(preview_mod, p, &info) == TSUCHI_NOT_FOUND);
        TSUCHI_PreviewScene q = reserve(preview_mod);
        CHECK(api.preview_scene(preview_mod, q, &info) == TSUCHI_OK && info.area == 7);
        CHECK(api.release_preview(preview_mod, q) == TSUCHI_OK && claims.size() == 1);
        q = reserve(preview_mod);
        mod_stop(preview_mod);
        CHECK(claims.size() == 1 && api.preview_scene(preview_mod, q, &info) == TSUCHI_INVALID);
    } else if (mode == "stopped_reserve" || mode == "nested_reserve") {
        stop_in_reserve = mode == "stopped_reserve";
        nested_reserve = !stop_in_reserve;
        TSUCHI_PreviewScene p{};
        CHECK(api.reserve_preview(preview_mod, &p) == (stop_in_reserve ? TSUCHI_DENIED : TSUCHI_OK));
        if (stop_in_reserve) CHECK(!p.id && claims.empty());
        else CHECK(api.release_preview(preview_mod, p) == TSUCHI_OK && claims.empty());
    } else if (mode == "stopped_prepare" || mode == "nested_prepare") {
        active = reserve(preview_mod);
        CHECK(api.admit_preview(preview_mod, active, {1}, world1, scene1) == TSUCHI_OK && !prepared);
        stop_in_prepare = mode == "stopped_prepare";
        nested_prepare = !stop_in_prepare;
        CHECK(api.prepare_preview(preview_mod, active) == (stop_in_prepare ? TSUCHI_DENIED : TSUCHI_OK));
        CHECK(prepared == 1 && claims.size() == 1);
    } else {
        active = reserve(preview_mod);
        if (mode == "nested") nested = true;
        if (mode == "stopped_retained" || mode == "stopped_clean") stop_in_give = true;
        if (mode == "stopped_first_member") stop_at_member = 1;
        if (mode == "stopped_second_member") stop_at_member = 2;
        if (mode == "stopped_clean" || mode == "failed_clean") retain = false;
        if (mode == "failed_retained" || mode == "failed_clean") give_result = TSUCHI_IO;
        const TSUCHI_Result result = api.admit_preview(preview_mod, active, {1}, world1, scene1);
        CHECK(result == (stop_in_give || stop_at_member ? TSUCHI_DENIED : give_result));
        CHECK(!prepared);
        if (stop_in_give || stop_at_member) {
            CHECK(claims.size() == (stop_at_member ? 0u : retain ? 1u : 0u));
        } else {
            CHECK(api.release_preview(preview_mod, active) == TSUCHI_OK);
            CHECK(claims.size() == (retain ? 1u : 0u));
        }
    }
    mod_stop(preview_mod); mod_stop(other);
    if (failures) return 1;
    std::puts("Preview scene controller checks passed");
}
