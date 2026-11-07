// Production native admission, with actual reflected guards and controlled
// asset/table/story callback boundaries. This is host evidence, not a game run.
#include "new_maps.cpp"
#include "reflect_world.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

using namespace tsuchi;
namespace fixture {
int failures = 0, writes = 0, links = 0;
uint64_t framework = 0, instance = 0, context = 0, current_world = 0, keeper = 0;
uint64_t story_type = 0, world_type = 0, scene_type = 0, tally_type = 0, parent = 0, table = 0;
TSUCHI_Mod* owner = nullptr;
bool fail_base = false, stop_load = false, replace_load = false, stop_grow = false;
bool replace_add = false, stop_add = false, trigger_self = false;
bool replace_read = false;
std::map<std::string, uint64_t> assets;
std::map<TSUCHI_Mod*, std::set<uint64_t>> pins;
std::set<std::string> members;
std::map<std::string, std::pair<std::string, int>> bases;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> records;
struct Candidate { std::string world, scene, tally; uint64_t world_at, scene_at, tally_at; };
}
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n", __LINE__, #x); ++fixture::failures; } } while (0)

namespace fixture {
std::u16string wide(const std::string& text) { return {text.begin(), text.end()}; }
uint64_t asset(const std::string& package, const std::string& leaf, uint64_t type) {
    const uint64_t outer = fake::object(wide(package), w.root);
    const uint64_t made = fake::object(wide(leaf), type, 0x100);
    fake::put(made + game::table::object::outer, outer);
    assets[package + "." + leaf] = made;
    return made;
}
uint64_t story() {
    const uint64_t made = fake::object(u"Story", story_type, 0x100);
    const uint64_t array = made + game::table::story_save::area_bases;
    fake::put(array + game::table::array::data, fake::memory(10 * game::table::area_bases::size));
    fake::put<int32_t>(array + game::table::array::count, 10);
    fake::put(instance + game::table::game_instance::story_save, made);
    return made;
}
uint8_t state(uint64_t value, uint32_t area) {
    const uint64_t data = fake::get<uint64_t>(value + game::table::story_save::area_bases + game::table::array::data);
    return fake::get<uint8_t>(data + area * game::table::area_bases::size + game::table::area_bases::first_state);
}
Candidate candidate(char generation, uint32_t area = 5) {
    char level[16], scene[16], tally[16];
    std::snprintf(level, sizeof level, "Area%03u_P", area);
    std::snprintf(scene, sizeof scene, "SD_Area%03u", area);
    std::snprintf(tally, sizeof tally, "APN_Area%03u", area);
    const std::string root = std::string("/Game/") + generation + "/";
    Candidate out{root + level, root + scene + "." + scene, root + tally + "." + tally, 0, 0, 0};
    out.world_at = asset(out.world, level, world_type);
    out.scene_at = asset(root + scene, scene, scene_type);
    out.tally_at = asset(root + tally, tally, tally_type);
    fake::put(out.scene_at + game::table::scene_data::world, fake::name(wide(out.world)));
    fake::put(out.scene_at + game::table::scene_data::parent, parent);
    fake::put(out.scene_at + game::table::scene_data::counts, out.tally_at);
    members.insert(out.world); members.insert(root + scene); members.insert(root + tally);
    return out;
}
TSUCHI_Result load(TSUCHI_Mod* holder, const char* path, TSUCHI_Object* out) {
    const auto found = assets.find(path);
    if (found == assets.end()) return TSUCHI_NOT_FOUND;
    if ((holder == self) == trigger_self) {
        if (stop_load) { stop_load = false; mod_stop(owner); }
        if (replace_load) { replace_load = false; story(); }
    }
    if (!mod_usable(holder)) return TSUCHI_INVALID;
    out->id = game::to_handle(found->second);
    pins[holder].insert(out->id);
    return TSUCHI_OK;
}
TSUCHI_Result get(TSUCHI_Mod*, TSUCHI_Object, const char* row, const char*, TSUCHI_Value* out) {
    if (fail_base) return TSUCHI_IO;
    const auto found = bases.find(row);
    if (found == bases.end()) return TSUCHI_NOT_FOUND;
    out->kind = TSUCHI_VALUE_INTEGER; out->as.integer = found->second.second;
    return TSUCHI_OK;
}
TSUCHI_Result get_text(TSUCHI_Mod*, TSUCHI_Object, const char* row, const char*, char* out, uint64_t room, uint64_t* needed) {
    const auto found = bases.find(row);
    if (found == bases.end()) return TSUCHI_NOT_FOUND;
    *needed = found->second.first.size() + 1;
    if (room < *needed) return TSUCHI_CAPACITY;
    memcpy(out, found->second.first.c_str(), size_t(*needed)); return TSUCHI_OK;
}
TSUCHI_Result add_row(TSUCHI_Mod*, TSUCHI_Object, const char* row, const char* from) {
    if (!bases.count(from)) return TSUCHI_NOT_FOUND;
    bases[row] = bases[from]; return TSUCHI_OK;
}
TSUCHI_Result set_text(TSUCHI_Mod*, TSUCHI_Object, const char* row, const char*, const char* text) {
    bases[row].first = text; return TSUCHI_OK;
}
TSUCHI_Result set(TSUCHI_Mod*, TSUCHI_Object, const char* row, const char*, const TSUCHI_Value* value) {
    bases[row].second = int(value->as.integer); return TSUCHI_OK;
}
const TSUCHI_AssetService asset_service = [] {
    TSUCHI_AssetService value{}; value.load = load; return value;
}();
const TSUCHI_DatatableService table_service = [] {
    TSUCHI_DatatableService value{}; value.get = get; value.get_text = get_text;
    value.add_row = add_row; value.override_text = set_text; value.override = set; return value;
}();
void grow(void* raw) {
    const uint64_t at = reinterpret_cast<uint64_t>(raw);
    const int32_t count = fake::get<int32_t>(at + game::table::array::count);
    const uint64_t old = fake::get<uint64_t>(at + game::table::array::data), next = fake::memory(8 * size_t(count + 4));
    if (count > 1) memcpy(reinterpret_cast<void*>(next), reinterpret_cast<void*>(old), 8 * size_t(count - 1));
    fake::put(at + game::table::array::data, next); fake::put<int32_t>(at + game::table::array::max, count + 4);
    if (stop_grow) { stop_grow = false; mod_stop(owner); }
}
void* find_area(void* raw, void* name) {
    const auto key = std::make_pair(reinterpret_cast<uint64_t>(raw), *static_cast<uint64_t*>(name));
    const auto found = records.find(key);
    return found == records.end() ? nullptr : reinterpret_cast<void*>(found->second);
}
void add_area(void* raw, void* name, int32_t) {
    records[{reinterpret_cast<uint64_t>(raw), *static_cast<uint64_t*>(name)}] = fake::memory(8);
    if (replace_add) { replace_add = false; story(); }
    if (stop_add) { stop_add = false; mod_stop(owner); }
}
bool link_area(void*, void*, uint64_t zero) { CHECK(zero == 0); ++links; return true; }
uint64_t published() {
    const uint64_t at = keeper + game::table::scene_manager::scenes;
    return fake::get<int32_t>(at + game::table::array::count) ? fake::get<uint64_t>(fake::get<uint64_t>(at + game::table::array::data)) : 0;
}
TSUCHI_Result give(const Candidate& value, bool& retained, uint64_t key = 1) {
    return give_preview_area(owner, key, TSUCHI_AssetBundle{1}, value.world.c_str(), value.scene.c_str(), &retained);
}
void setup() {
    build_world();
    self = mod_create("tsuchi.preview-test"); owner = mod_create("preview.owner");
    world_type = fake::object(u"World", w.root); scene_type = fake::object(u"GameSceneData", w.root);
    tally_type = fake::object(u"ActorPlacedNumAsset", w.root); story_type = w.component;
    const uint64_t manager_type = fake::object(u"GameSceneMgr", w.root);
    fake::field(manager_type, u"NextGameSceneReq", u"ObjectProperty", 0x60, 8, scene_type);
    fake::field(manager_type, u"NewGameSceneList", u"ArrayProperty", 0x68, 0x10, fake::loose_field(u"ObjectProperty", 8, scene_type));
    keeper = fake::object(u"Scenes", manager_type, 0x100);
    fake::put<uint8_t>(keeper + game::table::scene_manager::ready, uint8_t(game::table::constant::scenes_ready));
    instance = fake::object(u"Instance", w.component, 0x100); context = fake::object(u"Context", w.component, 0x100);
    framework = fake::object(u"Framework", w.component, 0x100); current_world = fake::object(u"Current", world_type, 0x100);
    fake::put<uint8_t>(framework + game::table::game_framework::world_begun, 1);
    fake::put<uint8_t>(context + game::table::game_context::rule, uint8_t(game::table::constant::rule_story));
    fake::put(current_world + game::table::world::level_name, fake::name(u"Area001_P"));
    fake::put(fake::main_image() + game::table::data::world, current_world);
    fake::put(fake::main_image() + game::table::data::game_instance, instance);
    fake::put(fake::main_image() + game::table::data::game_context, context);
    fake::put(instance + game::table::game_instance::scene_manager, keeper); story();
    parent = asset("/Game/Carrot4/GameScene/Game/SD_Area", "SD_Area", scene_type);
    table = fake::object(u"DT_AreaBaseCampTransition", w.root);
    assets[game::table::asset::area_bases_table] = table;
    bases["Area001_00"] = {"Area001_P", 1};
    game::table::fn::scene_array_grow = grow;
    game::table::fn::story_find_area = find_area; game::table::fn::story_add_area = add_area;
    game::table::fn::story_link_area = link_area;
}
}

namespace tsuchi {
ModRecord* find_mod(const char*) { return nullptr; }
bool add_manifest_section(const ManifestSection&) { std::abort(); }
bool each_frame(void (*)()) { std::abort(); }
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t, uint64_t, void*, uint32_t, uint64_t*, void**) { std::abort(); }
TSUCHI_Result table_resize(TSUCHI_Mod*, TSUCHI_Object, const char*, const char*, uint32_t) { std::abort(); }
namespace pikmin { Vocabulary PikminType_vocabulary{}; }
const TSUCHI_Host host = [] {
    TSUCHI_Host value{}; value.asset = &fixture::asset_service; value.datatable = &fixture::table_service; return value;
}();
TSUCHI_Result asset_bundle_package(TSUCHI_Mod* owner, TSUCHI_AssetBundle bundle, const char* path) {
    if (owner != fixture::owner) return TSUCHI_DENIED;
    if (bundle.id != 1) return TSUCHI_NOT_FOUND;
    return fixture::members.count(path) ? TSUCHI_OK : TSUCHI_INVALID;
}
TSUCHI_Result read_save_file(const char*, char*&, uint64_t&) {
    if (fixture::replace_read) { fixture::replace_read = false; fixture::story(); }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result write_save_file(const char*, const void*, uint64_t) { ++fixture::writes; return TSUCHI_IO; }
}
namespace tsuchi::game {
uint64_t game_framework() { return fixture::framework; }
void drop_shared(uint64_t) { std::abort(); }
}

int main(int argc, char** argv) {
    fixture::setup();
    const std::string mode = argc > 1 ? argv[1] : "admission";
    uint32_t area = 0; bool retained = false;
    if (mode == "reservation") {
        fake::put<uint8_t>(fixture::framework + game::table::game_framework::world_begun, 0);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_NOT_READY && area == 0);
        fake::put<uint8_t>(fixture::framework + game::table::game_framework::world_begun, 1);
        fake::put<uint8_t>(fake::main_image() + game::table::data::save_mounted, 1);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_BUSY);
        fake::put<uint8_t>(fake::main_image() + game::table::data::save_mounted, 0);
        fake::put(fixture::keeper + 0x60, fixture::parent);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_BUSY);
        fake::put<uint64_t>(fixture::keeper + 0x60, 0);
        fake::put<int32_t>(fixture::keeper + 0x68 + game::table::array::count, 1);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_BUSY);
        fake::put<int32_t>(fixture::keeper + 0x68 + game::table::array::count, 0);
        fixture::replace_read = true;
        CHECK(reserve_preview_area(1, &area) == TSUCHI_NOT_READY && area == 0 && !preview_claim(1));
        for (uint64_t key = 1; key <= 4; ++key) CHECK(reserve_preview_area(key, &area) == TSUCHI_OK);
        CHECK(reserve_preview_area(5, &area) == TSUCHI_CAPACITY && area == 0);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_OK && area == 5);
        for (uint64_t key = 1; key <= 4; ++key) release_preview_area(key);
        memcpy(kept[0].id, "old:map", 8);
        Map running{}; running.slot = 1; maps_running.add(running);
        const uint64_t at = fixture::keeper + game::table::scene_manager::scenes, data = fake::memory(8);
        fake::put(data, fake::object(u"SD_Area008", fixture::scene_type));
        fake::put(at + game::table::array::data, data); fake::put<int32_t>(at + game::table::array::count, 1);
        fake::put<int32_t>(at + game::table::array::max, 1);
        CHECK(reserve_preview_area(9, &area) == TSUCHI_OK && area == 9); release_preview_area(9);
        maps_running.count = 0; kept[0] = {}; fake::put<int32_t>(at + game::table::array::count, 0);
        CHECK(reserve_preview_area(1, &area) == TSUCHI_OK && area == 5);
        const char* text = "{\"builds\":[{\"world\":\"/Game/A/Area005_P\",\"scene\":\"/Game/A/SD_Area005\"},{\"world\":\"/Game/A/Area007_P\",\"scene\":\"/Game/A/SD_Area007\"}]}";
        const Json* map = read_json(text, strlen(text)); Kept plan[kAreas]{}; Line why;
        CHECK(map && pick(plan, "ordinary:map", *map, why) == 1); release_json(map); release_preview_area(1);
    } else {
        CHECK(reserve_preview_area(1, &area) == TSUCHI_OK && area == 5);
        const auto first = fixture::candidate('P');
        if (mode == "validation") {
            CHECK(give_preview_area(fixture::owner, 1, TSUCHI_AssetBundle{99}, first.world.c_str(), first.scene.c_str(), &retained) == TSUCHI_NOT_FOUND && !retained);
            fake::put(first.scene_at + game::table::scene_data::parent, fake::object(u"WrongParent", fixture::scene_type));
            CHECK(fixture::give(first, retained) == TSUCHI_INVALID && !retained);
            fake::put(first.scene_at + game::table::scene_data::parent, fixture::parent);
            const std::string world_object = first.world + ".Area005_P";
            fixture::assets[world_object] = fake::object(u"WrongWorld", w.component);
            CHECK(fixture::give(first, retained) == TSUCHI_INVALID && !retained);
            fixture::assets[world_object] = first.world_at;
            fake::put(first.scene_at + game::table::scene_data::counts, fake::object(u"WrongCounts", w.component));
            CHECK(fixture::give(first, retained) == TSUCHI_INVALID && !retained);
            fake::put(first.scene_at + game::table::scene_data::counts, first.tally_at);
            fixture::members.erase("/Game/P/APN_Area005");
            CHECK(fixture::give(first, retained) == TSUCHI_INVALID && !retained);
            fixture::members.insert("/Game/P/APN_Area005");
            fake::put(first.scene_at + game::table::scene_data::world, fake::name(u"/Game/Q/Area005_P"));
            CHECK(fixture::give(first, retained) == TSUCHI_INVALID && !retained);
            CHECK(fixture::pins[self].empty() && fixture::published() == 0 && fixture::records.empty());
            fake::put(first.scene_at + game::table::scene_data::world, fake::name(fixture::wide(first.world)));
            const uint64_t foreign = fake::object(u"SD_Area005", fixture::scene_type);
            const uint64_t at = fixture::keeper + game::table::scene_manager::scenes, data = fake::memory(8);
            fake::put(data, foreign); fake::put(at + game::table::array::data, data);
            fake::put<int32_t>(at + game::table::array::count, 1); fake::put<int32_t>(at + game::table::array::max, 1);
            CHECK(fixture::give(first, retained) == TSUCHI_BUSY && retained && fixture::published() == foreign);
        } else if (mode == "callbacks") {
            fixture::stop_load = true;
            CHECK(fixture::give(first, retained) == TSUCHI_NOT_FOUND && !retained && fixture::published() == 0);
            release_preview_area(1); CHECK(!preview_claim(1));
            fixture::owner = mod_create("preview.second"); CHECK(reserve_preview_area(2, &area) == TSUCHI_OK && area == 5);
            fixture::replace_load = true;
            CHECK(fixture::give(first, retained, 2) == TSUCHI_NOT_READY && !retained);
            release_preview_area(2);
            CHECK(reserve_preview_area(3, &area) == TSUCHI_OK && area == 5);
            fixture::trigger_self = true; fixture::stop_load = true;
            CHECK(fixture::give(first, retained, 3) == TSUCHI_NOT_FOUND && retained);
            release_preview_area(3); CHECK(preview_claim(3) && !preview_claim(3)->given && fixture::published() == 0);
            fixture::owner = mod_create("preview.third");
            CHECK(reserve_preview_area(4, &area) == TSUCHI_OK && area == 7);
            const auto later = fixture::candidate('R', 7); fixture::stop_grow = true;
            CHECK(fixture::give(later, retained, 4) == TSUCHI_NOT_FOUND && retained && fixture::published() == 0);
            CHECK(preview_claim(4) && !preview_claim(4)->given);
        } else {
            CHECK(fixture::give(first, retained) == TSUCHI_OK && retained && fixture::published() == first.scene_at);
            const uint64_t original_story = fake::get<uint64_t>(fixture::instance + game::table::game_instance::story_save);
            CHECK(fixture::records.empty() && fixture::state(original_story, 5) == 0 && fixture::links == 0);
            CHECK(fixture::bases["Area005_00"] == std::make_pair(std::string("Area005_P"), 1));
            if (mode == "generations") {
                fake::destroy(first.world_at); // world travel can retire the previous UWorld life
                const auto second = fixture::candidate('Q'); fixture::fail_base = true;
                CHECK(fixture::give(second, retained) == TSUCHI_IO && retained && fixture::published() == first.scene_at);
                CHECK(preview_claim(1)->scene == game::to_handle(first.scene_at)); fixture::fail_base = false;
                CHECK(prepare_preview_area(1) == TSUCHI_OK);
                CHECK(fixture::give(second, retained) == TSUCHI_OK && fixture::published() == second.scene_at);
                CHECK(preview_claim(1)->scene == game::to_handle(second.scene_at));
                CHECK(fixture::pins[self].count(game::to_handle(first.scene_at)) && fixture::pins[self].count(game::to_handle(second.scene_at)));
                fake::put(fixture::current_world + game::table::world::level_name, fake::name(u"Area005_P"));
                CHECK(fixture::give(first, retained) == TSUCHI_BUSY && fixture::published() == second.scene_at);
                CHECK(prepare_preview_area(1) == TSUCHI_OK);
            } else {
                const uint64_t pause = fake::memory(0x30);
                fake::put(fixture::instance + game::table::game_instance::pause_manager, pause);
                fake::put<uint32_t>(pause + game::table::pause_manager::active, 0xffffffffu);
                CHECK(prepare_preview_area(1) == TSUCHI_OK && fixture::links == 1 && fixture::state(original_story, 5) == game::table::constant::base_open);
                const uint64_t original_bases = fake::get<uint64_t>(original_story + game::table::story_save::area_bases + game::table::array::data);
                fake::put<uint8_t>(original_bases + 5 * game::table::area_bases::size + game::table::area_bases::first_state, 2);
                CHECK(prepare_preview_area(1) == TSUCHI_OK && fixture::state(original_story, 5) == 2 && fixture::links == 1);
                const uint64_t baseline = fixture::story(); fixture::replace_add = true;
                CHECK(prepare_preview_area(1) == TSUCHI_NOT_READY && fixture::state(baseline, 5) == 0);
                const uint64_t restored = fake::get<uint64_t>(fixture::instance + game::table::game_instance::story_save);
                CHECK(prepare_preview_area(1) == TSUCHI_OK && fixture::state(restored, 5) == game::table::constant::base_open);
                fixture::stop_add = true; fixture::story();
                CHECK(prepare_preview_area(1) == TSUCHI_NOT_FOUND);
            }
            release_preview_area(1); CHECK(preview_claim(1) && preview_claim(1)->retained);
        }
    }
    CHECK(fixture::writes == 0);
    if (!fixture::failures) std::puts("Preview area checks passed");
    return fixture::failures;
}
