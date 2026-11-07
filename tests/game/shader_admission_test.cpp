// Production archive admission, donor validation, and set ownership.
#include "shader_materials.cpp"
#include "reflect_world.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fixture {
int failures = 0, reads = 0, restores = 0, allocations_left = -1;
uint64_t donor = 0, map = 0, map_resource = 0, source = 0;
TSUCHI_Mod* stop_in_restore = nullptr;
TSUCHI_Result read_result = TSUCHI_OK, load_result = TSUCHI_OK;
bool oversized = false;
std::string last_path;
std::vector<uint8_t> bytes;
void* allocate(size_t size) {
    if (!allocations_left) return nullptr;
    if (allocations_left > 0) --allocations_left;
    return std::malloc(size);
}
void release(void* block) { std::free(block); }
}

using namespace tsuchi;
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n", __LINE__, #x); ++fixture::failures; } } while (0)

namespace fixture {
void word(uint32_t value) { for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (8*i))); }
std::string hex(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += "0123456789abcdef"[data[i] >> 4];
        out += "0123456789abcdef"[data[i] & 15];
    }
    return out;
}
std::string archive() {
    const std::string payload = "creator pixel program";
    uint8_t hash[20];
    shaders::archive::sha1(reinterpret_cast<const uint8_t*>(payload.data()), uint32_t(payload.size()), hash);
    bytes.clear(); word(2); word(1); bytes.insert(bytes.end(), 20, 0);
    word(1); bytes.insert(bytes.end(), hash, hash + 20);
    word(1); word(0); word(1); word(0); word(1);
    word(1); word(0); word(0); word(uint32_t(payload.size())); word(uint32_t(payload.size())); bytes.push_back(3);
    word(1); word(0); word(0); word(uint32_t(payload.size())); word(0);
    word(1); word(0); bytes.insert(bytes.end(), payload.begin(), payload.end());
    return "{\"library\":\"preview\",\"archive\":\"ShaderArchive-preview-StandInShaders.ushaderbytecode\","
        "\"materials\":[{\"material\":\"/Game/Mods/Preview/M.M\",\"game_material\":\"/Game/Carrot4/Game\","
        "\"map_id\":\"0000000000000000000000000000000000000000\","
        "\"archive_map\":\"0000000000000000000000000000000000000000\",\"map_shaders\":2,"
        "\"shaders\":[{\"place\":1,\"hash\":\"" + hex(hash, sizeof hash) + "\"}]}]}";
}
TSUCHI_Result load(TSUCHI_Mod*, const char* path, TSUCHI_Object* out) {
    CHECK(std::string(path) == "/Game/Carrot4/Game.Game");
    if (load_result == TSUCHI_OK) out->id = game::to_handle(donor);
    return load_result;
}
const TSUCHI_AssetService assets = [] {
    TSUCHI_AssetService value{}; value.table_size = sizeof value; value.load = load; return value;
}();
}

namespace tsuchi {
const TSUCHI_Host host = [] { TSUCHI_Host value{}; value.asset = &fixture::assets; return value; }();
bool add_manifest_section(const ManifestSection&) { std::abort(); }
bool each_frame(void (*)()) { std::abort(); }
bool on_actor_entered(void (*)(uint64_t, uint64_t)) { std::abort(); }
bool mod_file_path(Texts& path, const TSUCHI_Mod*, const char* file) { return path.add("romfs:/mod/", file); }
uint64_t shader_slot_source(uint64_t, uint32_t) { return fixture::source; }
TSUCHI_Result restore_material(TSUCHI_Mod*, uint64_t, uint32_t) {
    ++fixture::restores;
    if (fixture::stop_in_restore) {
        TSUCHI_Mod* stopped = fixture::stop_in_restore; fixture::stop_in_restore = nullptr;
        mod_stop(stopped);
    }
    return TSUCHI_OK;
}
TSUCHI_Result hold_material(TSUCHI_Mod*, uint64_t, uint32_t, uint64_t) { std::abort(); }
}
namespace tsuchi::game {
bool path_stays_within(const char*) { std::abort(); }
TSUCHI_Result file_size(const char* path, uint64_t& size) {
    fixture::last_path = path;
    size = fixture::oversized ? uint64_t(shaders::archive::limit) + 1 : fixture::bytes.size();
    return fixture::read_result;
}
TSUCHI_Result read_whole_file(const char* path, char*& out, uint64_t& size) {
    ++fixture::reads; fixture::last_path = path;
    if (fixture::read_result != TSUCHI_OK) return fixture::read_result;
    size = fixture::bytes.size();
    out = static_cast<char*>(allocate(size));
    if (!out) return TSUCHI_NO_MEMORY;
    memcpy(out, fixture::bytes.data(), size);
    return TSUCHI_OK;
}
uint64_t loaded_world() { std::abort(); }
uint64_t table_entry(uint64_t, uint64_t) { std::abort(); }
}

int main() {
    build_world(); set_allocator({fixture::allocate, fixture::release});
    const uint64_t type = fake::engine_object(u"MaterialInstanceConstant", w.root);
    fixture::donor = fake::engine_object(u"Game", type);
    const uint64_t resource = fake::memory(0x30), list = fake::memory(8);
    fixture::map = fake::memory(game::table::shader_map::size);
    fixture::map_resource = fake::memory(game::table::shader_map_resource::size);
    fake::put(list, resource);
    fake::put(fixture::donor + game::table::material_instance::resources + game::table::array::data, list);
    fake::put<int32_t>(fixture::donor + game::table::material_instance::resources + game::table::array::count, 1);
    fake::put(resource + game::table::material_resource::instance, fixture::donor);
    fake::put(resource + game::table::material_resource::game_map, fixture::map);
    fake::put(fixture::map + game::table::shader_map::resource, fixture::map_resource);
    fake::put(fixture::map_resource + game::table::shader_map_resource::shaders, fake::memory(16));
    fake::put<uint32_t>(fixture::map_resource + game::table::shader_map_resource::shader_count, 2);
    const std::string metadata = fixture::archive();
    TSUCHI_Mod* mod = mod_create("shaders.bundle"), *other = mod_create("shaders.other");
    CHECK(mod && other);
    constexpr const char* path = "sdmc:/tsuchi/assets/1/shaders.bin";
    uint64_t id = 99;
    for (const char* invalid : {"", "{", "null", "[]", "{}", "\"shaders.bin\"", "{\"archive\":\"bad\"}"}) {
        CHECK(admit_asset_shaders(mod, path, invalid, id) == TSUCHI_INVALID && id == 0);
    }
    CHECK(admit_asset_shaders(nullptr, path, metadata.c_str(), id) == TSUCHI_INVALID && id == 0);
    CHECK(admit_asset_shaders(mod, nullptr, metadata.c_str(), id) == TSUCHI_INVALID && id == 0);
    CHECK(admit_asset_shaders(mod, "", metadata.c_str(), id) == TSUCHI_INVALID && id == 0);
    CHECK(admit_asset_shaders(mod, path, nullptr, id) == TSUCHI_INVALID && id == 0);
    CHECK(fixture::reads == 0);
    fixture::oversized = true;
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_INVALID && id == 0 && fixture::reads == 0);
    fixture::oversized = false; fixture::read_result = TSUCHI_NOT_FOUND;
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_NOT_FOUND && id == 0);
    fixture::read_result = TSUCHI_OK; fixture::bytes.back() ^= 1;
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_INVALID && id == 0);
    fixture::bytes.back() ^= 1;
    fake::put<uint8_t>(fixture::map + game::table::shader_map::id_hash, 1);
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_INVALID && id == 0);
    fake::put<uint8_t>(fixture::map + game::table::shader_map::id_hash, 0);
    fixture::load_result = TSUCHI_NOT_READY;
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_NOT_READY && id == 0);
    fixture::load_result = TSUCHI_OK;
    CHECK(sets.count == 0 && counts.sets == 0 && owned_count(mod) == 0 && !mod_block_reason(mod));

    // Every allocation boundary leaves the mod running and publishes no partial set.
    bool admitted = false;
    for (int left = 0; left < 80; ++left) {
        fixture::allocations_left = left;
        const auto result = admit_asset_shaders(mod, path, metadata.c_str(), id);
        fixture::allocations_left = -1;
        if (result == TSUCHI_OK) { admitted = true; break; }
        CHECK(result == TSUCHI_NO_MEMORY && id == 0 && sets.count == 0 && owned_count(mod) == 0);
        CHECK(mod_usable(mod) && !mod_block_reason(mod));
    }
    CHECK(admitted && id && sets.count == 1 && counts.sets == 1 && counts.materials == 1);
    CHECK(fixture::last_path == path && owned_count(mod) == 1);
    release_asset_shaders(other, id);
    CHECK(sets.count == 1 && owned_count(mod) == 1);
    release_asset_shaders(mod, id);
    CHECK(sets.count == 0 && counts.sets == 0 && counts.materials == 0 && owned_count(mod) == 0);
    release_asset_shaders(mod, id);

    // Startup still obtains its archive through the mod's RomFS path.
    const Json* named = read_json(metadata.data(), metadata.size()); Text problem;
    CHECK(named && shaders_begin(mod, *named, problem) == TSUCHI_OK);
    CHECK(fixture::last_path == "romfs:/mod/ShaderArchive-preview-StandInShaders.ushaderbytecode");
    CHECK(sets.count == 1 && owned_count(mod) == 1); release_json(named); problem.clear();
    id = sets[0].id;
    // A native restoration can stop the owner while explicit release is active.
    fixture::source = game::to_handle(fixture::donor);
    CHECK(sets[0].materials[0].placed.add(Placed{game::to_handle(w.red), 0, fixture::source}));
    fixture::stop_in_restore = mod;
    release_asset_shaders(mod, id);
    CHECK(!mod_usable(mod) && sets.count == 0 && counts.sets == 0 && fixture::restores == 1);
    release_asset_shaders(mod, id);
    CHECK(admit_asset_shaders(mod, path, metadata.c_str(), id) == TSUCHI_INVALID && id == 0);

    CHECK(admit_asset_shaders(other, path, metadata.c_str(), id) == TSUCHI_OK && id);
    mod_stop(other); release_asset_shaders(other, id);
    CHECK(sets.count == 0 && counts.sets == 0 && counts.materials == 0);
    if (!fixture::failures) std::puts("Shader admission transaction checks passed");
    return fixture::failures;
}
