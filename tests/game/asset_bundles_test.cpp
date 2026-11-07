// The production SD bundle backend with file and engine stand-ins. No game result.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "asset_bundles.cpp"
#include "asset_service.h"

namespace t = tsuchi::game::table;
static int failures;
#define CHECK(value) do { if (!(value)) { ++failures; std::printf("line %d: %s\n", __LINE__, #value); } } while (0)
static std::string mode;
static int original_context, returning_context;
static const void* context = &original_context;
static std::map<std::string, std::string> files;
static std::set<std::string> folders;
struct Open { std::string name; };
static uint32_t engine_mounts, shader_admissions, shader_releases;
static TSUCHI_Mod* ending;
static bool fail_write, fail_delete, fail_resize, fail_mount, stop_on_mount, stop_on_shader;
static bool fail_shrink;
static bool fail_read, short_read, recurse_on_read, stop_on_read;
static uint32_t reads;
static TSUCHI_AssetBundle recursive_bundle;
static bool recurse_on_mount, recurse_on_shader;
static void recursive_call() {
    CHECK(tsuchi::asset::table.mount_bundle(ending, recursive_bundle, "") == TSUCHI_BUSY);
    CHECK(tsuchi::asset::table.write_bundle(ending, recursive_bundle, 0, 4, "x", 1) == TSUCHI_BUSY);
    CHECK(tsuchi::asset_bundle_package(ending, recursive_bundle, "/Game/MyMeshes/MI_New") == TSUCHI_BUSY);
}

static const void* get_context() { return context; }
static void set_context(const void* next) { context = next; }
static uint32_t mount_sd(const char* name) {
    CHECK(context == &returning_context && !strcmp(name, "tsuchiasset"));
    return mode == "sd_denied" ? 1 : 0;
}
static uint32_t entry_kind(void* out, void* path) {
    CHECK(context == &returning_context);
    const char* name = static_cast<const char*>(path);
    if (folders.count(name)) { *static_cast<int32_t*>(out) = int32_t(t::constant::fs_entry_folder); return 0; }
    if (files.count(name)) { *static_cast<int32_t*>(out) = 1; return 0; }
    return uint32_t(t::constant::fs_no_such_path);
}
static uint32_t create_folder(void* path) {
    CHECK(context == &returning_context);
    return folders.insert(static_cast<const char*>(path)).second ? 0 : 1;
}
static uint32_t create_file(void* path, int64_t size) {
    CHECK(context == &returning_context && size >= 0);
    const std::string name = static_cast<const char*>(path);
    if (files.count(name)) return 1;
    files[name].resize(size_t(size)); return 0;
}
static uint32_t open_file(void* out, void* path, int32_t) {
    CHECK(context == &returning_context);
    const std::string name = static_cast<const char*>(path);
    if (!files.count(name)) return uint32_t(t::constant::fs_no_such_path);
    *static_cast<void**>(out) = new Open{name}; return 0;
}
static void close_file(void* file) { CHECK(context == &returning_context); delete static_cast<Open*>(file); }
static uint32_t file_size(void* out, void* file) {
    CHECK(context == &returning_context);
    *static_cast<int64_t*>(out) = int64_t(files[static_cast<Open*>(file)->name].size()); return 0;
}
static uint32_t resize_file(void* file, int64_t size) {
    CHECK(context == &returning_context && size >= 0);
    if (fail_resize) return 1;
    if (fail_shrink && size_t(size) < files[static_cast<Open*>(file)->name].size()) return 1;
    files[static_cast<Open*>(file)->name].resize(size_t(size)); return 0;
}
static uint32_t write_file(void* file, int64_t offset, void* bytes, uint64_t size, void* how) {
    CHECK(context == &returning_context && *static_cast<uint32_t*>(how) == t::constant::fs_write_through);
    if (fail_write) return 1;
    auto& data = files[static_cast<Open*>(file)->name];
    CHECK(offset >= 0 && uint64_t(offset) + size <= data.size());
    memcpy(&data[size_t(offset)], bytes, size_t(size)); return 0;
}
static uint32_t read_file(void* out, void* file, int64_t offset, void* bytes, uint64_t size) {
    CHECK(context == &returning_context && offset >= 0);
    ++reads;
    if (fail_read) return 1;
    const auto& data = files[static_cast<Open*>(file)->name];
    CHECK(uint64_t(offset) + size <= data.size());
    const uint64_t got = short_read && size ? size - 1 : size;
    memcpy(bytes, data.data() + offset, size_t(got)); *static_cast<uint64_t*>(out) = got;
    if (recurse_on_read) {
        recurse_on_read = false;
        CHECK(tsuchi::asset_bundle_package(ending, recursive_bundle, "/Game/MyMeshes/MI_New") == TSUCHI_BUSY);
    }
    if (stop_on_read) { stop_on_read = false; tsuchi::mod_stop(ending); }
    return 0;
}
static uint32_t delete_file(void* path) {
    CHECK(context == &returning_context);
    if (fail_delete) return 1;
    return files.erase(static_cast<const char*>(path)) ? 0 : 1;
}
static uint32_t tree_removals;
static bool below(const std::string& name, const std::string& folder) {
    return name == folder || (name.size() > folder.size() && !name.compare(0, folder.size(), folder) && name[folder.size()] == '/');
}
static uint32_t remove_tree(const char* path) {
    CHECK(context == &returning_context);
    ++tree_removals;
    for (auto it = files.begin(); it != files.end();) it = below(it->first, path) ? files.erase(it) : std::next(it);
    for (auto it = folders.begin(); it != folders.end();) it = below(*it, path) ? folders.erase(it) : std::next(it);
    return 0;
}
static uint32_t remove_empty(const char* path) {
    CHECK(context == &returning_context);
    const std::string folder = path;
    for (const auto& file : files) if (below(file.first, folder)) return 1;
    for (const auto& inner : folders) if (inner != folder && below(inner, folder)) return 1;
    return folders.erase(folder) ? 0 : 1;
}
namespace tsuchi::game {
uint64_t main_base() { return 0; }
bool readable(uint64_t, uint64_t) { return false; }
uint64_t module_containing(uint64_t) { return 1; }
uint64_t module_symbol(uint64_t, const char* name) {
    if (!strcmp(name, "_ZN2nn2fs25GetCurrentThreadFsContextEv")) return reinterpret_cast<uint64_t>(&get_context);
    if (!strcmp(name, "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE")) return reinterpret_cast<uint64_t>(&set_context);
    if (!strcmp(name, "_ZN2nn2fs18g_AllReturnContextE")) return reinterpret_cast<uint64_t>(&returning_context);
    if (!strcmp(name, "_ZN2nn2fs11MountSdCardEPKc")) return reinterpret_cast<uint64_t>(&::mount_sd);
    if (!strcmp(name, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc")) return reinterpret_cast<uint64_t>(&::remove_tree);
    if (!strcmp(name, "_ZN2nn2fs15DeleteDirectoryEPKc")) return reinterpret_cast<uint64_t>(&::remove_empty);
    return 0;
}
}
namespace tsuchi {
TSUCHI_Result mount_asset_container(const char*, const char* pak, const char* toc, const char* data, const char* root) {
    CHECK(context == &returning_context && !strncmp(pak, "tsuchiasset:/tsuchi/preview/", 26));
    CHECK(files.count(pak) && files.count(toc) && files.count(data) && !strcmp(root, "/Game"));
    if (recurse_on_mount) recursive_call();
    if (stop_on_mount) mod_stop(ending);
    if (fail_mount) return TSUCHI_IO;
    ++engine_mounts; return TSUCHI_OK;
}
TSUCHI_Result admit_asset_shaders(TSUCHI_Mod*, const char* path, const char*, uint64_t& id) {
    CHECK(context == &returning_context && files.count(path));
    ++shader_admissions; id = 91;
    if (recurse_on_shader) recursive_call();
    if (stop_on_shader) mod_stop(ending);
    return TSUCHI_OK;
}
void release_asset_shaders(TSUCHI_Mod*, uint64_t) { ++shader_releases; }
bool read_container_toc(const uint8_t* bytes, size_t size, ContainerToc& toc, const char*&) {
    if (!size || bytes[0] != 't') return false;
    toc.packages.add(3); return true;
}
void forget_container_toc(ContainerToc& toc) { toc.packages.clear(); }
uint64_t container_package_id(const char* path) { return !strcmp(path, "/Game/MyMeshes/MI_New") ? 3 : 0x77; }
bool container_package_path(const ContainerToc&, uint64_t, Texts& path) {
    return path.add("/Game/MyMeshes/MI_New");
}
}
static void setup() {
    static const tsuchi::asset::Backend backend{nullptr, nullptr, tsuchi::begin_asset_bundle,
        tsuchi::write_asset_bundle, tsuchi::mount_asset_bundle, tsuchi::asset_bundle_status,
        tsuchi::release_asset_bundle};
    tsuchi::asset::set_backend(&backend);
    t::fn::fs_entry_kind = &entry_kind; t::fn::fs_create_folder = &create_folder;
    t::fn::fs_create_file = &create_file; t::fn::fs_open_file = &open_file;
    t::fn::fs_close_file = &close_file; t::fn::fs_file_size = &file_size;
    t::fn::fs_set_file_size = &resize_file; t::fn::fs_write_file = &write_file;
    t::fn::fs_read_file = &read_file; t::fn::fs_delete_file = &delete_file;
    tsuchi::cost_clock = []() -> uint64_t { return 0x1234; };
}
static TSUCHI_AssetBundle begin(TSUCHI_Mod* mod, const char* name = "preview") {
    TSUCHI_AssetBundle bundle{};
    CHECK(tsuchi::asset::table.begin_bundle(mod, name, "/Game", &bundle) == TSUCHI_OK && bundle.id);
    CHECK(context == &original_context); return bundle;
}
static void trio(TSUCHI_Mod* mod, TSUCHI_AssetBundle b) {
    for (uint32_t part = 0; part < 3; ++part)
        CHECK(tsuchi::write_asset_bundle(mod, b, part, 0, part == 1 ? "toc" : "data", part == 1 ? 3 : 4) == TSUCHI_OK);
}
static void membership_checks(TSUCHI_Mod* mod, TSUCHI_Mod* other, TSUCHI_AssetBundle b) {
    constexpr const char* member = "/Game/MyMeshes/MI_New";
    const uint32_t before_reads = reads;
    CHECK(tsuchi::asset_bundle_package(other, b, member) == TSUCHI_DENIED);
    CHECK(tsuchi::asset_bundle_package(mod, {b.id + 1000}, member) == TSUCHI_NOT_FOUND);
    for (const char* path : {"/Engine/New", "/Game/Carrot4/New", "/Game/MyMeshes/../New", "/Game//New", "/Game/MyMeshes/MI_New.MI_New"})
        CHECK(tsuchi::asset_bundle_package(mod, b, path) == TSUCHI_INVALID);
    CHECK(tsuchi::asset_bundle_package(mod, b, nullptr) == TSUCHI_INVALID);
    CHECK(reads == before_reads);
    TSUCHI_BundleInfo before{}, after{};
    CHECK(tsuchi::asset_bundle_status(mod, b, &before) == TSUCHI_OK);
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_OK);
    CHECK(tsuchi::asset_bundle_package(mod, b, "/Game/MyMeshes/MI_Other") == TSUCHI_INVALID);
    CHECK(tsuchi::asset_bundle_package(mod, b, "/Game/MyMeshes/mi_new") == TSUCHI_INVALID);
    CHECK(tsuchi::asset_bundle_status(mod, b, &after) == TSUCHI_OK &&
        before.state == after.state && before.utoc_bytes == after.utoc_bytes && !strcmp(before.failure, after.failure));
    CHECK(engine_mounts == 1 && !shader_admissions && context == &original_context);
    char path[384];
    CHECK(tsuchi::file_path(*tsuchi::record(b.id), 1, path));
    const std::string original = files[path];
    files[path] += 'x';
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    files[path] = "";
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    files.erase(path);
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    files[path] = "bad";
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_INVALID);
    files[path] = original;
    fail_read = true;
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    fail_read = false; short_read = true;
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    short_read = false;
    tsuchi::record(b.id)->bytes[1] = (16ull << 20) + 1;
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_IO);
    tsuchi::record(b.id)->bytes[1] = original.size();
    recursive_bundle = b; recurse_on_read = true;
    CHECK(tsuchi::asset_bundle_package(mod, b, member) == TSUCHI_OK && !recurse_on_read);
    CHECK(tsuchi::asset_bundle_status(mod, b, &after) == TSUCHI_OK && !strcmp(before.failure, after.failure));
    CHECK(context == &original_context);
}
int main(int argc, char** argv) {
    mode = argc > 1 ? argv[1] : "policy";
    setup();
    TSUCHI_Mod* mod = tsuchi::mod_create("dev.server");
    TSUCHI_Mod* other = tsuchi::mod_create("other.tool");
    ending = mod;
    if (mode == "spool_sweep") {
        // An earlier run left an upload behind; this run's first mount removes that run's whole spool.
        for (const char* folder : {"tsuchiasset:/tsuchi", "tsuchiasset:/tsuchi/preview", "tsuchiasset:/tsuchi/preview/aaaaaaaaaaaaaaaa",
                                   "tsuchiasset:/tsuchi/preview/aaaaaaaaaaaaaaaa/dev.server"})
            folders.insert(folder);
        files["tsuchiasset:/tsuchi/preview/aaaaaaaaaaaaaaaa/dev.server/old.ucas"] = "old";
        files["tsuchiasset:/tsuchi/kept.txt"] = "kept";
        const auto first = begin(mod);
        CHECK(tree_removals == 1 && files.size() == 1 && files.count("tsuchiasset:/tsuchi/kept.txt"));
        for (const auto& folder : folders) CHECK(folder.find("aaaaaaaaaaaaaaaa") == std::string::npos);
        trio(mod, first);
        CHECK(tsuchi::mount_asset_bundle(mod, first, "") == TSUCHI_OK);
        const auto second = begin(mod); trio(mod, second);
        CHECK(tree_removals == 1 && files.size() == 7);
        // Releasing the unmounted upload removes its files and its now empty folder; the mounted one stays.
        std::string folder;
        for (uint32_t i = 0; i < tsuchi::bundles.count; ++i) if (tsuchi::bundles[i].id == second.id) folder = tsuchi::bundles[i].folder;
        CHECK(!folder.empty() && folders.count(folder));
        CHECK(tsuchi::release_asset_bundle(mod, second) == TSUCHI_OK && files.size() == 4 && !folders.count(folder));
    } else if (mode == "sd_denied") {
        TSUCHI_AssetBundle b{};
        CHECK(tsuchi::begin_asset_bundle(mod, "preview", "/Game", &b) == TSUCHI_UNSUPPORTED && !b.id);
        CHECK(files.empty() && context == &original_context);
    } else if (mode == "policy") {
        TSUCHI_AssetBundle bad{};
        for (const char* name : {"", "../evil", "evil.pak", "a/b", "a:b"})
            CHECK(tsuchi::begin_asset_bundle(mod, name, "/Game", &bad) == TSUCHI_INVALID);
        for (const char* root : {"/Engine", "/Game/Carrot4", "/Game/carrot4/Maps", "/Game/../Maps", "/Game//Maps", "/Game/"})
            CHECK(tsuchi::begin_asset_bundle(mod, "preview", root, &bad) == TSUCHI_INVALID);
        const auto b = begin(mod);
        CHECK(tsuchi::asset_bundle_package(mod, b, "/Game/MyMeshes/MI_New") == TSUCHI_NOT_READY);
        CHECK(tsuchi::asset_bundle_package(other, b, "/Game/MyMeshes/MI_New") == TSUCHI_DENIED);
        CHECK(tsuchi::write_asset_bundle(other, b, 0, 0, "a", 1) == TSUCHI_DENIED);
        CHECK(tsuchi::write_asset_bundle(mod, b, 4, 0, "a", 1) == TSUCHI_INVALID);
        CHECK(tsuchi::asset::table.write_bundle(mod, b, 99, 0, "a", 1) == TSUCHI_INVALID);
        CHECK(tsuchi::asset::table.bundle_status(mod, b, nullptr) == TSUCHI_INVALID);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 1, "a", 1) == TSUCHI_INVALID);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "a", 32769) == TSUCHI_INVALID);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "ab", 2) == TSUCHI_OK);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "ab", 2) == TSUCHI_OK);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "ax", 2) == TSUCHI_INVALID);
        TSUCHI_BundleInfo info{};
        CHECK(tsuchi::asset_bundle_status(mod, b, &info) == TSUCHI_OK && info.pak_bytes == 2 && !strcmp(info.failure, "invalid"));
        CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_INVALID);
        CHECK(tsuchi::release_asset_bundle(other, b) == TSUCHI_DENIED);
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK && files.empty());
        CHECK(tsuchi::asset_bundle_status(mod, b, &info) == TSUCHI_NOT_FOUND);
        CHECK(tsuchi::asset_bundle_package(mod, b, "/Game/MyMeshes/MI_New") == TSUCHI_NOT_FOUND);
        const auto loaded = begin(mod); trio(mod, loaded);
        CHECK(tsuchi::mount_asset_bundle(mod, loaded, "") == TSUCHI_OK && engine_mounts == 1);
        CHECK(tsuchi::mount_asset_bundle(mod, loaded, "") == TSUCHI_OK && engine_mounts == 1);
        CHECK(tsuchi::write_asset_bundle(mod, loaded, 0, 0, "new", 3) == TSUCHI_BUSY);
        membership_checks(mod, other, loaded);
        CHECK(tsuchi::release_asset_bundle(mod, loaded) == TSUCHI_OK && files.size() == 3);
        CHECK(tsuchi::asset_bundle_package(mod, loaded, "/Game/MyMeshes/MI_New") == TSUCHI_NOT_FOUND);
        const auto temporary = begin(mod); trio(mod, temporary);
        tsuchi::mod_stop(mod);
        CHECK(files.size() == 3 && !tsuchi::mod_usable(mod));
        const auto foreign = begin(other); trio(other, foreign);
        CHECK(tsuchi::release_asset_bundle(other, foreign) == TSUCHI_OK && files.size() == 3);
        const auto ended = begin(other); trio(other, ended);
        CHECK(tsuchi::mount_asset_bundle(other, ended, "") == TSUCHI_OK);
        ending = other; stop_on_read = true;
        CHECK(tsuchi::asset_bundle_package(other, ended, "/Game/MyMeshes/MI_New") == TSUCHI_NOT_FOUND);
        CHECK(files.size() == 6 && !tsuchi::mod_usable(other) && context == &original_context);
    } else if (mode == "write_denied") {
        const auto b = begin(mod); fail_write = true;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "a", 1) == TSUCHI_IO && files.empty());
        fail_write = false;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "a", 1) == TSUCHI_OK);
        fail_write = true;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 1, "b", 1) == TSUCHI_IO && files.begin()->second == "a");
        fail_write = false;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 1, "b", 1) == TSUCHI_OK);
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK && files.empty());
    } else if (mode == "mount_failure") {
        const auto b = begin(mod); trio(mod, b); fail_mount = true;
        CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_IO && tsuchi::mod_usable(mod));
        fail_mount = false;
        CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_OK);
        tsuchi::mod_stop(mod); CHECK(files.size() == 3);
    } else if (mode == "reentrant_mount") {
        const auto b = begin(mod); trio(mod, b); stop_on_mount = true;
        CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_NOT_FOUND);
        CHECK(files.size() == 3 && engine_mounts == 1);
    } else if (mode == "reentrant_shader") {
        const auto b = begin(mod); trio(mod, b);
        CHECK(tsuchi::write_asset_bundle(mod, b, 3, 0, "archive", 7) == TSUCHI_OK);
        CHECK(tsuchi::mount_asset_bundle(mod, b, "{\"materials\":[{\"material\":\"/Game/Carrot4/MI_Stock.MI_Stock\"}]}") == TSUCHI_INVALID);
        CHECK(shader_admissions == 0);
        stop_on_shader = true;
        CHECK(tsuchi::mount_asset_bundle(mod, b, "{\"materials\":[{\"material\":\"/Game/MyMeshes/MI_New.MI_New\"}]}") == TSUCHI_NOT_FOUND);
        CHECK(files.empty() && shader_admissions == 1 && shader_releases == 1 && !engine_mounts);
    } else if (mode == "recursive_shader" || mode == "recursive_mount") {
        const auto b = begin(mod); trio(mod, b); recursive_bundle = b;
        recurse_on_mount = mode == "recursive_mount";
        recurse_on_shader = mode == "recursive_shader";
        const char* metadata = "";
        if (recurse_on_shader) {
            CHECK(tsuchi::write_asset_bundle(mod, b, 3, 0, "archive", 7) == TSUCHI_OK);
            metadata = "{\"materials\":[{\"material\":\"/Game/MyMeshes/MI_New.MI_New\"}]}";
        }
        CHECK(tsuchi::asset::table.mount_bundle(mod, b, metadata) == TSUCHI_OK && engine_mounts == 1);
        CHECK(tsuchi::asset::table.write_bundle(mod, b, 0, 4, "x", 1) == TSUCHI_BUSY);
        const size_t mounted_files = files.size();
        CHECK(tsuchi::asset::table.release_bundle(mod, b) == TSUCHI_OK && files.size() == mounted_files);
    } else if (mode == "retained_limit") {
        for (unsigned i = 0; i < 32; ++i) {
            const auto b = begin(mod); trio(mod, b);
            CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_OK);
            CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK);
        }
        TSUCHI_AssetBundle b{};
        CHECK(tsuchi::begin_asset_bundle(mod, "full", "/Game", &b) == TSUCHI_CAPACITY && files.size() == 96);
    } else if (mode == "disk_limit") {
        const auto b = begin(mod);
        tsuchi::disk_bytes = (1ull << 30) - 1;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "ab", 2) == TSUCHI_CAPACITY && files.empty());
        tsuchi::disk_bytes = 0;
        tsuchi::record(b.id)->bytes[0] = 16ull << 20;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 16ull << 20, "a", 1) == TSUCHI_CAPACITY);
        tsuchi::record(b.id)->bytes[0] = 0;
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK);
    } else if (mode == "cleanup_failure") {
        const auto b = begin(mod); trio(mod, b); fail_delete = true;
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_IO && files.size() == 3);
        fail_delete = false;
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK && files.empty());
    } else if (mode == "rollback_failed") {
        const auto b = begin(mod);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "a", 1) == TSUCHI_OK);
        fail_write = true; fail_shrink = true;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 1, "b", 1) == TSUCHI_IO);
        TSUCHI_BundleInfo info{};
        CHECK(tsuchi::asset_bundle_status(mod, b, &info) == TSUCHI_OK && info.pak_bytes == 1 &&
            info.state == TSUCHI_BUNDLE_STATE_FAILED && !strcmp(info.failure, "io"));
        CHECK(tsuchi::disk_bytes == 2 && files.begin()->second.size() == 2);
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 1, "b", 1) == TSUCHI_IO);
        CHECK(tsuchi::mount_asset_bundle(mod, b, "") == TSUCHI_IO);
        fail_write = fail_shrink = false;
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK && files.empty() && !tsuchi::disk_bytes);
    } else if (mode == "fresh_cleanup_failed") {
        const auto b = begin(mod); fail_write = fail_delete = true;
        CHECK(tsuchi::write_asset_bundle(mod, b, 0, 0, "a", 1) == TSUCHI_IO);
        TSUCHI_BundleInfo info{};
        CHECK(tsuchi::asset_bundle_status(mod, b, &info) == TSUCHI_OK && !info.pak_bytes &&
            info.state == TSUCHI_BUNDLE_STATE_FAILED && tsuchi::disk_bytes == 1);
        fail_write = fail_delete = false;
        CHECK(tsuchi::release_asset_bundle(mod, b) == TSUCHI_OK && files.empty() && !tsuchi::disk_bytes);
    }
    tsuchi::mod_stop(mod); tsuchi::mod_stop(other);
    CHECK(context == &original_context);
    if (!failures) std::puts("Asset bundle checks passed");
    return failures;
}
