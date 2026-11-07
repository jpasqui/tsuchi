// New immutable cooked containers on the SD card. No RomFS rewrite or emulator restart.
#include "asset_bundles.h"
#include <string.h>
#include "containers.h"
#include "cost_clock.h"
#include "files.h"
#include "game.h"
#include "json.h"
#include "list.h"
#include "modules.h"
#include "registry.h"
#include "shader_materials.h"

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr Kind kBundle{"asset.bundle"};
constexpr uint32_t kChunk = 32768, kOwnedLimit = 8, kMountedLimit = 32, kRecordsLimit = 64;
constexpr uint64_t kIssuedLimit = 1024;
constexpr uint64_t kDiskLimit = 1ull << 30;
constexpr uint64_t kLimits[] = {16ull << 20, 16ull << 20, 256ull << 20, 64ull << 20};
constexpr const char* kExtensions[] = {".pak", ".utoc", ".ucas", ".shaders"};
using GetContext = const void* (*)();
using SetContext = void (*)(const void*);
using DeleteFolder = uint32_t (*)(const char*);
struct SdAccess { GetContext get; SetContext set; const void* returning; DeleteFolder remove_tree, remove_empty; bool mounted; } sd{};
constexpr char kSpool[] = "tsuchiasset:/tsuchi/preview";
struct Scope {
    const void* before;
    Scope() : before(sd.get()) { sd.set(sd.returning); }
    ~Scope() { sd.set(before); }
};
struct Bundle {
    TSUCHI_Mod* mod;
    uint64_t id, bytes[4], accounted[4], shaders;
    char name[65], root[256], folder[256];
    TSUCHI_Result failure;
    bool mounted, live, broken, pinned, mounting;
};
List<Bundle> bundles;
uint64_t next_id = 1, disk_bytes = 0, run_stamp = 0;
uint32_t mounted_count = 0;
bool bundle_mounting = false;
bool package_reading = false;

bool plain(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool root_valid(const char* root) {
    const size_t n = strlen(root);
    if (n < 5 || n >= 256 || memcmp(root, "/Game", 5) || (root[5] && root[5] != '/')) return false;
    const char* protected_name = "/game/carrot4";
    bool protected_root = n >= 13;
    for (size_t i = 0; i < 13 && protected_root; ++i) {
        const char c = root[i];
        protected_root = (c >= 'A' && c <= 'Z' ? c + 32 : c) == protected_name[i];
    }
    if (protected_root && (!root[13] || root[13] == '/')) return false;
    for (size_t i = 6; i < n; ++i)
        if (!plain(root[i]) && (root[i] != '/' || root[i - 1] == '/')) return false;
    return n == 5 || (root[5] == '/' && n > 6 && root[n - 1] != '/');
}
bool mount_sd() {
    if (sd.mounted) return true;
    if (!t::bound(t::fn::fs_entry_kind) || !t::bound(t::fn::fs_create_folder) || !t::bound(t::fn::fs_create_file) ||
        !t::bound(t::fn::fs_open_file) || !t::bound(t::fn::fs_close_file) || !t::bound(t::fn::fs_file_size) ||
        !t::bound(t::fn::fs_read_file) || !t::bound(t::fn::fs_write_file) || !t::bound(t::fn::fs_set_file_size) || !t::bound(t::fn::fs_delete_file)) return false;
    const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    sd.get = reinterpret_cast<GetContext>(game::module_symbol(library, "_ZN2nn2fs25GetCurrentThreadFsContextEv"));
    sd.set = reinterpret_cast<SetContext>(game::module_symbol(library, "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"));
    sd.returning = game::returning_fs_context(library);
    const auto mount = reinterpret_cast<uint32_t (*)(const char*)>(game::module_symbol(library, "_ZN2nn2fs11MountSdCardEPKc"));
    sd.remove_tree = reinterpret_cast<DeleteFolder>(game::module_symbol(library, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"));
    sd.remove_empty = reinterpret_cast<DeleteFolder>(game::module_symbol(library, "_ZN2nn2fs15DeleteDirectoryEPKc"));
    if (!sd.get || !sd.set || !sd.returning || !mount) return false;
    Scope scope;
    if (mount("tsuchiasset") != 0) return false;
    // Uploads of earlier runs. This process holds none of their files and has made none of its own yet,
    // so the whole spool goes; without the function the files stay, as they did before.
    char spool[sizeof kSpool]; memcpy(spool, kSpool, sizeof kSpool);
    int32_t kind = -1;
    if (sd.remove_tree && t::fn::fs_entry_kind(&kind, spool) == 0 && uint32_t(kind) == t::constant::fs_entry_folder)
        sd.remove_tree(spool);
    return sd.mounted = true;
}
bool append(char* out, size_t room, const char* text) {
    const size_t a = strlen(out), b = strlen(text);
    if (a + b >= room) return false;
    memcpy(out + a, text, b + 1); return true;
}
void hex(char* out, uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) out[i] = digits[(value >> (60 - i * 4)) & 15];
    out[16] = 0;
}
bool file_path(const Bundle& bundle, uint32_t part, char (&out)[384]) {
    out[0] = 0;
    return append(out, sizeof out, bundle.folder) && append(out, sizeof out, "/") &&
        append(out, sizeof out, bundle.name) && append(out, sizeof out, kExtensions[part]);
}
bool create_folder(char* path, bool fresh_leaf) {
    for (char* at = path + strlen("tsuchiasset:/");; ++at) {
        if (*at && *at != '/') continue;
        const char kept = *at; *at = 0;
        int32_t kind = -1;
        const uint32_t found = t::fn::fs_entry_kind(&kind, path);
        const bool good = (!fresh_leaf || kept || found == t::constant::fs_no_such_path) &&
            ((!found && uint32_t(kind) == t::constant::fs_entry_folder) ||
             (found == t::constant::fs_no_such_path && t::fn::fs_create_folder(path) == 0));
        *at = kept;
        if (!good) return false;
        if (!kept) return true;
    }
}
Bundle* lookup(TSUCHI_Mod* mod, uint64_t id, TSUCHI_Result& result) {
    for (uint32_t i = 0; i < bundles.count; ++i) {
        Bundle& b = bundles[i];
        if (b.id != id || !b.live) continue;
        if (b.mod != mod) { result = TSUCHI_DENIED; return nullptr; }
        result = TSUCHI_OK; return &b;
    }
    result = TSUCHI_NOT_FOUND; return nullptr;
}
Bundle* record(uint64_t id) {
    for (uint32_t i = 0; i < bundles.count; ++i) if (bundles[i].id == id) return &bundles[i];
    return nullptr;
}
struct MountGuard {
    uint64_t id;
    ~MountGuard() {
        if (Bundle* b = record(id)) b->mounting = false;
        bundle_mounting = false;
    }
};
TSUCHI_Result note(Bundle& b, TSUCHI_Result result) { b.failure = result; return result; }
const char* failure_name(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_OK: return "";
    case TSUCHI_INVALID: return "invalid";
    case TSUCHI_NOT_FOUND: return "not_found";
    case TSUCHI_BUSY: return "busy";
    case TSUCHI_NOT_READY: return "not_ready";
    case TSUCHI_CAPACITY: return "capacity";
    case TSUCHI_UNSUPPORTED: return "unsupported";
    case TSUCHI_DENIED: return "denied";
    case TSUCHI_NO_MEMORY: return "no_memory";
    case TSUCHI_IO: return "io";
    }
    return "invalid";
}
bool erase_files(Bundle& b) {
    bool good = true;
    Scope scope;
    for (uint32_t part = 0; part < 4; ++part) {
        if (!b.accounted[part]) continue;
        char path[384]; file_path(b, part, path);
        if (t::fn::fs_delete_file(path) == 0) {
            disk_bytes -= b.accounted[part]; b.bytes[part] = b.accounted[part] = 0;
        } else good = false;
    }
    // The bundle's own folder, now empty. Its run and mod folders go with the next run's sweep.
    if (good && sd.remove_empty) sd.remove_empty(b.folder);
    return good;
}
// A shader may target only an instance shipped by this bundle. Its donor game material
// is validated independently by the existing shader transaction.
TSUCHI_Result shader_targets(const Bundle& b, const char* toc_path, const char* metadata) {
    if (strlen(metadata) > (1u << 20)) return TSUCHI_CAPACITY;
    const Json* json = read_json(metadata, strlen(metadata));
    if (!json) return TSUCHI_INVALID;
    const Json* rows = json->find("materials");
    if (json->kind != Json::Kind::object || !rows || rows->kind != Json::Kind::list ||
        !rows->count || rows->count > 1024) { release_json(json); return TSUCHI_INVALID; }
    char* bytes = nullptr; uint64_t size = 0;
    TSUCHI_Result result = game::file_size(toc_path, size);
    if (result == TSUCHI_OK && size != b.bytes[1]) result = TSUCHI_IO;
    if (result == TSUCHI_OK) result = game::read_whole_file(toc_path, bytes, size);
    ContainerToc toc;
    const char* wrong = nullptr;
    if (result == TSUCHI_OK && !read_container_toc(reinterpret_cast<uint8_t*>(bytes), size_t(size), toc, wrong))
        result = TSUCHI_INVALID;
    for (const Json* row = rows->first; result == TSUCHI_OK && row; row = row->next) {
        const Json* material = row->find("material");
        if (!material || material->kind != Json::Kind::text || material->bytes > 511 ||
            !material->bytes || strlen(material->text) != material->bytes) { result = TSUCHI_INVALID; break; }
        const char* dot = material->text;
        while (*dot && *dot != '.') ++dot;
        if (!*dot) { result = TSUCHI_INVALID; break; }
        // Mounting binds every package identity to its path, so the identity alone names the package.
        // Scanning the index for each row's path would be quadratic on the game thread.
        const size_t package_size = size_t(dot - material->text);
        char package[512];
        if (package_size >= sizeof package) { result = TSUCHI_INVALID; break; }
        memcpy(package, material->text, package_size); package[package_size] = 0;
        const uint64_t id = container_package_id(package);
        bool found = false;
        for (uint32_t i = 0; !found && i < toc.packages.count; ++i) found = id && toc.packages[i] == id;
        if (!found) result = TSUCHI_INVALID;
    }
    forget_container_toc(toc); release(bytes); release_json(json);
    return result;
}
void end_bundle(void*, uint64_t id) {
    for (uint32_t i = 0; i < bundles.count; ++i) {
        Bundle& b = bundles[i];
        if (b.id != id || !b.live) continue;
        const uint64_t shaders = b.shaders;
        TSUCHI_Mod* mod = b.mod;
        b.live = false; b.shaders = 0;
        // Remove table references before native shader restoration can reenter mod cleanup.
        if (!b.pinned && erase_files(b)) bundles.remove(&b);
        if (shaders) release_asset_shaders(mod, shaders);
        return;
    }
}
}

TSUCHI_Result begin_asset_bundle(TSUCHI_Mod* mod, const char* name, const char* root, TSUCHI_AssetBundle* out) {
    *out = {};
    if (!on_game_thread()) return TSUCHI_INVALID;
    const size_t n = strlen(name);
    if (!n || n > 64 || !root_valid(root)) return TSUCHI_INVALID;
    for (size_t i = 0; i < n; ++i) if (!plain(name[i])) return TSUCHI_INVALID;
    uint32_t owned = 0;
    for (uint32_t i = 0; i < bundles.count; ++i)
        if (bundles[i].live && !bundles[i].mounted && bundles[i].mod == mod) ++owned;
    if (owned >= kOwnedLimit || bundles.count >= kRecordsLimit || mounted_count >= kMountedLimit ||
        next_id > kIssuedLimit || disk_bytes >= kDiskLimit)
        return TSUCHI_CAPACITY;
    if (!mount_sd()) return TSUCHI_UNSUPPORTED;
    if (!bundles.reserve(bundles.count + 1)) return TSUCHI_NO_MEMORY;
    Bundle b{}; b.mod = mod; b.live = true;
    memcpy(b.name, name, n + 1); memcpy(b.root, root, strlen(root) + 1);
    if (!run_stamp) run_stamp = cost_now() ^ reinterpret_cast<uint64_t>(&bundles);
    bool made = false;
    {
        Scope scope;
        for (unsigned tries = 0; tries < 64 && !made; ++tries) {
            b.id = next_id++;
            if (!b.id) return TSUCHI_CAPACITY;
            char stamp[17], id[17]; hex(stamp, run_stamp); hex(id, b.id);
            b.folder[0] = 0;
            if (!append(b.folder, sizeof b.folder, kSpool) || !append(b.folder, sizeof b.folder, "/") ||
                !append(b.folder, sizeof b.folder, stamp) || !append(b.folder, sizeof b.folder, "/") ||
                !append(b.folder, sizeof b.folder, mod_id(mod)) || !append(b.folder, sizeof b.folder, "/") ||
                !append(b.folder, sizeof b.folder, id)) return TSUCHI_INVALID;
            made = create_folder(b.folder, true);
        }
    }
    if (!made) return TSUCHI_IO;
    const TSUCHI_Result owned_result = own(mod, kBundle, b.id, end_bundle, nullptr);
    if (owned_result != TSUCHI_OK) return owned_result;
    bundles.add(b); out->id = b.id;
    return TSUCHI_OK;
}

TSUCHI_Result write_asset_bundle(TSUCHI_Mod* mod, TSUCHI_AssetBundle handle, TSUCHI_BundlePart part,
    uint64_t offset, const void* data, uint64_t size) {
    // The bundle table and the census are the game thread's; a compiled mod's worker must not race them.
    if (!on_game_thread()) return TSUCHI_INVALID;
    TSUCHI_Result result;
    Bundle* b = lookup(mod, handle.id, result);
    if (!b) return result;
    if (b->pinned || b->mounting) return note(*b, TSUCHI_BUSY);
    if (b->broken) return note(*b, TSUCHI_IO);
    if (part > 3 || !data || !size || size > kChunk || offset > b->bytes[part] ||
        (offset < b->bytes[part] && size > b->bytes[part] - offset)) return note(*b, TSUCHI_INVALID);
    if (offset == b->bytes[part] && (size > kLimits[part] - offset || size > kDiskLimit - disk_bytes))
        return note(*b, TSUCHI_CAPACITY);
    char path[384]; file_path(*b, part, path);
    Scope scope;
    if (offset < b->bytes[part]) {
        auto* compare = static_cast<uint8_t*>(allocate(size_t(size)));
        if (!compare) return note(*b, TSUCHI_NO_MEMORY);
        uint64_t got = 0;
        result = game::read_file_part(path, offset, compare, size, got);
        const bool same = result == TSUCHI_OK && got == size && !memcmp(compare, data, size_t(size));
        release(compare);
        return note(*b, result != TSUCHI_OK ? result : same ? TSUCHI_OK : TSUCHI_INVALID);
    }
    const bool fresh = offset == 0;
    if (fresh && t::fn::fs_create_file(path, int64_t(size)) != 0) return note(*b, TSUCHI_IO);
    void* file = nullptr;
    if (t::fn::fs_open_file(&file, path, int32_t(t::constant::fs_write)) != 0) {
        if (fresh && t::fn::fs_delete_file(path) != 0) {
            b->broken = true; b->accounted[part] = size; disk_bytes += size;
        }
        return note(*b, TSUCHI_IO);
    }
    int64_t before = -1;
    uint32_t how = uint32_t(t::constant::fs_write_through);
    const bool sized = t::fn::fs_file_size(&before, file) == 0 && before == int64_t(fresh ? size : offset);
    const bool grown = sized && (fresh || t::fn::fs_set_file_size(file, int64_t(offset + size)) == 0);
    const bool written = grown && t::fn::fs_write_file(file, int64_t(offset), const_cast<void*>(data), size, &how) == 0;
    if (!written && grown && !fresh && t::fn::fs_set_file_size(file, int64_t(offset)) != 0) {
        b->broken = true; b->accounted[part] += size; disk_bytes += size;
    }
    t::fn::fs_close_file(file);
    if (!written) {
        if (fresh && t::fn::fs_delete_file(path) != 0) {
            b->broken = true; b->accounted[part] = size; disk_bytes += size;
        }
        return note(*b, TSUCHI_IO);
    }
    b->bytes[part] += size; b->accounted[part] += size; disk_bytes += size;
    return note(*b, TSUCHI_OK);
}

TSUCHI_Result mount_asset_bundle(TSUCHI_Mod* mod, TSUCHI_AssetBundle handle, const char* metadata) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    TSUCHI_Result result;
    Bundle* b = lookup(mod, handle.id, result);
    if (!b) return result;
    if (b->mounted) return note(*b, TSUCHI_OK);
    if (b->mounting || bundle_mounting) return note(*b, TSUCHI_BUSY);
    if (b->broken) return note(*b, TSUCHI_IO);
    if (!b->bytes[0] || !b->bytes[1] || !b->bytes[2] || (!!b->bytes[3] != !!*metadata))
        return note(*b, TSUCHI_INVALID);
    if (mounted_count >= kMountedLimit) return note(*b, TSUCHI_CAPACITY);
    b->mounting = true;
    bundle_mounting = true;
    const MountGuard mounting{handle.id};
    char pak[384], toc[384], data[384], shaders[384];
    file_path(*b, 0, pak); file_path(*b, 1, toc); file_path(*b, 2, data); file_path(*b, 3, shaders);
    Scope scope;
    uint64_t set = 0;
    if (*metadata) {
        result = shader_targets(*b, toc, metadata);
        if (result != TSUCHI_OK) return note(*b, result);
        result = admit_asset_shaders(mod, shaders, metadata, set);
        TSUCHI_Result lookup_result;
        b = lookup(mod, handle.id, lookup_result);
        // Asset loading can stop a mod or grow the bundle table through native callbacks.
        if (!b || !mod_usable(mod)) {
            if (set) release_asset_shaders(mod, set);
            return TSUCHI_NOT_FOUND;
        }
        if (result != TSUCHI_OK) return note(*b, result);
    }
    char root[256]; memcpy(root, b->root, strlen(b->root) + 1);
    b->pinned = true; // Cleanup must not remove files while the engine can retain their handles.
    result = mount_asset_container(mod_id(mod), pak, toc, data, root);
    b = record(handle.id); // A native mount callback may already have ended the owner.
    if (result != TSUCHI_OK) {
        if (b && b->live && !b->mounted) b->pinned = false;
        if (set) release_asset_shaders(mod, set);
        b = record(handle.id);
        return b && b->live ? note(*b, result) : result;
    }
    ++mounted_count;
    if (b) { b->mounted = true; b->shaders = b->live ? set : 0; }
    if (!b || !b->live) {
        if (set) release_asset_shaders(mod, set);
        return TSUCHI_NOT_FOUND;
    }
    return note(*b, TSUCHI_OK);
}

TSUCHI_Result asset_bundle_status(TSUCHI_Mod* mod, TSUCHI_AssetBundle handle, TSUCHI_BundleInfo* out) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    TSUCHI_Result result;
    const Bundle* b = lookup(mod, handle.id, result);
    if (!b) return result;
    *out = {TSUCHI_BundleState(b->mounted ? TSUCHI_BUNDLE_STATE_MOUNTED :
        b->broken ? TSUCHI_BUNDLE_STATE_FAILED : TSUCHI_BUNDLE_STATE_UPLOADING),
        b->bytes[0], b->bytes[1], b->bytes[2], b->bytes[3], failure_name(b->failure)};
    return TSUCHI_OK;
}

TSUCHI_Result asset_bundle_package(TSUCHI_Mod* mod, TSUCHI_AssetBundle handle, const char* package) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    TSUCHI_Result result;
    const Bundle* b = lookup(mod, handle.id, result);
    if (!b) return result;
    if (b->mounting || bundle_mounting || package_reading) return TSUCHI_BUSY;
    if (!b->mounted) return TSUCHI_NOT_READY;
    if (!package || !root_valid(package)) return TSUCHI_INVALID;
    const size_t root_bytes = strlen(b->root);
    if (strlen(package) <= root_bytes || memcmp(package, b->root, root_bytes) || package[root_bytes] != '/') return TSUCHI_INVALID;
    if (!sd.mounted || !sd.get || !sd.set || !sd.returning || !t::bound(t::fn::fs_open_file) ||
        !t::bound(t::fn::fs_close_file) || !t::bound(t::fn::fs_file_size) || !t::bound(t::fn::fs_read_file)) return TSUCHI_IO;
    // Native file callbacks can end an owner or grow the bundle table. Keep no table pointer
    // across them, and verify the live handle again before accepting the package.
    const Bundle snapshot = *b;
    char path[384];
    if (!file_path(snapshot, 1, path)) return TSUCHI_INVALID;
    package_reading = true;
    struct ReadGuard { ~ReadGuard() { package_reading = false; } } guard;
    char* bytes = nullptr;
    uint64_t size = 0;
    {
        Scope scope;
        result = game::with_file(path, [&](void* file, uint64_t length) {
            if (!length || length != snapshot.bytes[1] || length > kLimits[1]) return TSUCHI_IO;
            auto* made = static_cast<char*>(allocate(size_t(length)));
            if (!made) return TSUCHI_NO_MEMORY;
            uint64_t got = 0;
            if (t::fn::fs_read_file(&got, file, 0, made, length) != 0 || got != length) {
                release(made);
                return TSUCHI_IO;
            }
            bytes = made; size = length;
            return TSUCHI_OK;
        });
    }
    if (result != TSUCHI_OK) return result == TSUCHI_NO_MEMORY ? result : TSUCHI_IO;
    ContainerToc toc;
    const char* wrong = nullptr;
    if (!read_container_toc(reinterpret_cast<const uint8_t*>(bytes), size_t(size), toc, wrong)) {
        forget_container_toc(toc); release(bytes);
        return wrong && strlen(wrong) >= 18 && !memcmp(wrong, "there is no memory", 18) ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
    }
    // The mounted bundle's identities were bound to their paths when it mounted.
    const uint64_t id = container_package_id(package);
    bool member = false;
    for (uint32_t i = 0; i < toc.packages.count && !member; ++i) member = id && toc.packages[i] == id;
    forget_container_toc(toc); release(bytes);
    b = lookup(mod, handle.id, result);
    if (!b) return result;
    if (!b->mounted) return TSUCHI_NOT_READY;
    return member ? TSUCHI_OK : TSUCHI_INVALID;
}

TSUCHI_Result release_asset_bundle(TSUCHI_Mod* mod, TSUCHI_AssetBundle handle) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    TSUCHI_Result result;
    Bundle* b = lookup(mod, handle.id, result);
    if (!b) return result;
    if (!b->pinned && !erase_files(*b)) return note(*b, TSUCHI_IO);
    if (disown(mod, kBundle, handle.id) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    end_bundle(nullptr, handle.id);
    return TSUCHI_OK;
}
}
