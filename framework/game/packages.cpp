// Packages on the SD card, and the packages service.
//
// An install streams the file to `<package folder>/package.zip` on an HTTPS worker, hashing it as it
// goes, and extracts it into `<package folder>/mods/` there. Nothing is published by then: the game
// thread checks the manifests and requirements after the worker ends, and writes the receipt last. A
// package folder without a receipt is never loaded, and the next start deletes it.
#include "packages.h"
#include "packages_service.h"
#include "mod_zip.h"
#include "cheat_vm.h"
#include "containers.h"
#include "cost_clock.h"
#include "files.h"
#include "frame.h"
#include "https_jobs.h"
#include "mod_choices.h"
#include "mods.h"
#include "modules.h"
#include "sd_card.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
namespace ps = package_store;

constexpr uint32_t kMostPackages = 256;
constexpr uint64_t kMostBytes = mod_zip::max_archive;
const Kind kInstall{"packages.install"};

using DeleteTree = uint32_t (*)(const char*);
DeleteTree delete_tree = nullptr;
int64_t (*clock_seconds)() = nullptr;
bool ready = false, cleaned = false, started = false;
List<ps::Receipt> listed; // the installed ones, as last read

// Why an install failed, as its status names it. Set by the worker before the job ends, or by the game thread.
enum class Failure : uint32_t { none, network, http, size, digest, storage, archive, unsupported, manifest, required, conflict, different, receipt };
const char* failure_name(Failure f) {
    static const char* const names[] = {"", "network", "http", "size", "digest", "storage", "archive", "unsupported",
                                        "manifest", "required", "conflict", "different", "receipt"};
    return uint32_t(f) < sizeof names / sizeof *names ? names[uint32_t(f)] : "storage";
}

// The one install that can run, and the last one's result until the next starts.
struct Install {
    uint64_t id;
    TSUCHI_Mod* owner;
    bool active;        // the worker or the game thread still has work for it
    bool told;          // `finished` was emitted
    TSUCHI_InstallPhase phase;
    uint64_t done, total, bytes;
    Failure failure;
    char message[200];
    char expected[65];  // the digest the mod gave, or empty
    char replacing[ps::kModIdBytes];
    TSUCHI_InstallPhase shown_phase; uint64_t shown_done; bool shown; // the last `progress` emitted
};
Install install_state{};
uint64_t next_install = 1;
https::HttpsJob job;
https::HttpsAtomic<uint32_t> phase{}, failure{}, archive_error{};
https::HttpsAtomic<uint64_t> expanded{}, expanded_total{};
ps::Receipt receipt{};
char directory[96]{}, archive[128]{};
void* spool = nullptr;
void* workspace = nullptr;
Sha256 hash;
bool fresh = false; // the new package folder is ours to delete if the install does not finish
mod_zip::Status extraction{};

bool append(char* out, size_t room, const char* part) {
    const size_t a = strlen(out), b = strlen(part);
    if (a + b >= room) return false;
    memcpy(out + a, part, b + 1);
    return true;
}
bool path(char* out, size_t room, const char* folder, const char* tail) {
    out[0] = 0;
    return append(out, room, kPackagesFolder) && append(out, room, "/") && append(out, room, folder) && append(out, room, tail);
}
void hex(const uint8_t* bytes, size_t size, char* out) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
    out[2 * size] = 0;
}
void set_message(const char* text) {
    size_t n = strlen(text);
    if (n >= sizeof install_state.message) n = sizeof install_state.message - 1;
    memcpy(install_state.message, text, n);
    install_state.message[n] = 0;
}
bool ours(const char* p) {
    const size_t n = sizeof kPackagesFolder - 1;
    return strlen(p) > n && !memcmp(p, kPackagesFolder, n) && p[n] == '/';
}
const void* enter(const char* p) { return ready && ours(p) ? sd_enter() : nullptr; }
void leave(const char* p, const void* previous) { if (ready && ours(p)) sd_leave(previous); }

bool initialize() {
    if (ready) return true;
    if (!game::files_known() || !t::bound(t::fn::fs_entry_kind) || !t::bound(t::fn::fs_create_folder) || !t::bound(t::fn::fs_create_file) ||
        !t::bound(t::fn::fs_write_file) || !t::bound(t::fn::fs_set_file_size) || !t::bound(t::fn::fs_delete_file) || !sd_card_mounted()) return false;
    if (!delete_tree) {
        const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
        delete_tree = reinterpret_cast<DeleteTree>(game::module_symbol(library, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"));
    }
    game::file_context_enter = enter;
    game::file_context_leave = leave;
    ready = true;
    return true;
}

// ---- Files ---------------------------------------------------------------------------------------

bool folders(char* file) {
    for (char* p = file + strlen("tsuchisd:/"); *p; ++p)
        if (*p == '/') {
            *p = 0;
            int32_t kind = -1;
            const uint32_t found = t::fn::fs_entry_kind(&kind, file);
            const bool ok = (!found && uint32_t(kind) == t::constant::fs_entry_folder) || (found == t::constant::fs_no_such_path && !t::fn::fs_create_folder(file));
            *p = '/';
            if (!ok) return false;
        }
    return true;
}
bool create(char* file, uint64_t size, void** out) {
    return folders(file) && !t::fn::fs_create_file(file, int64_t(size)) && !t::fn::fs_open_file(out, file, int32_t(t::constant::fs_write));
}
bool write(void* f, uint64_t offset, const void* bytes, uint64_t size) {
    uint32_t how = uint32_t(t::constant::fs_write_through);
    return !t::fn::fs_write_file(f, int64_t(offset), const_cast<void*>(bytes), size, &how);
}
bool write_file(char* file, const void* bytes, uint64_t size) {
    void* handle = nullptr;
    if (!create(file, size, &handle)) return false;
    const bool saved = write(handle, 0, bytes, size);
    t::fn::fs_close_file(handle);
    return saved;
}
// Deletes a package folder, never anything else.
bool delete_package(const char* folder) {
    char where[96];
    if (!ps::folder_valid(folder) || !path(where, sizeof where, folder, "") || !delete_tree) return false;
    SdScope scope;
    return !delete_tree(where);
}
bool marked(const char* folder) {
    char marker[128];
    uint64_t n = 0;
    return path(marker, sizeof marker, folder, "/removed") && game::file_size(marker, n) == TSUCHI_OK;
}
bool mark_removed(const char* folder) {
    if (marked(folder)) return true;
    char marker[128];
    return path(marker, sizeof marker, folder, "/removed") && write_file(marker, "", 0);
}
// A mod the loader found in this folder is loaded, or a container installed from it this session is
// mounted: its files and container can be in use until the game stops.
bool in_use(const char* folder) {
    char prefix[96];
    if (!path(prefix, sizeof prefix, folder, "/")) return true;
    if (container_mounted_from(prefix)) return true;
    const size_t n = strlen(prefix);
    for (uint32_t i = 0; i < mod_count(); ++i)
        if (const ModRecord* record = mod_at(i); record && strlen(record->folder) >= n && !memcmp(record->folder, prefix, n)) return true;
    return false;
}

// ---- Reading receipts ----------------------------------------------------------------------------

enum class Read { valid, none, unreadable };
Read read_receipt(const char* folder, ps::Receipt& out) {
    char file[128];
    uint64_t bytes = 0, got = 0;
    if (!path(file, sizeof file, folder, "/receipt")) return Read::unreadable;
    const auto measured = game::file_size(file, bytes);
    if (measured == TSUCHI_NOT_FOUND) return Read::none;
    if (measured != TSUCHI_OK || bytes != sizeof out) return Read::unreadable;
    alignas(8) uint8_t raw[sizeof(ps::Receipt)]{};
    if (game::read_file_part(file, 0, raw, bytes, got) != TSUCHI_OK || got != bytes || !ps::read_receipt(raw, size_t(bytes), out) || strcmp(out.folder, folder))
        return Read::unreadable;
    return Read::valid;
}

// Every package with a valid receipt, removed ones included. More than kMostPackages is a capacity failure:
// the caller then loads none of them, and the game's own mods still load.
TSUCHI_Result scan(List<ps::Receipt>& found) {
    if (!initialize()) return TSUCHI_UNSUPPORTED;
    SdScope scope;
    Texts files, dirs;
    auto result = game::list_folder(kPackagesFolder, files, dirs);
    files.clear();
    if (result == TSUCHI_NOT_FOUND) result = TSUCHI_OK;
    for (const char* name = dirs.first(); result == TSUCHI_OK && name; name = dirs.after(name)) {
        if (!ps::folder_valid(name)) continue;
        ps::Receipt r{};
        if (read_receipt(name, r) != Read::valid) continue;
        if (found.count >= kMostPackages) { result = TSUCHI_CAPACITY; break; }
        if (!found.add(r)) result = TSUCHI_NO_MEMORY;
    }
    dirs.clear();
    return result;
}
// Whether a newer revision of the same package is among them: the older one is not used.
bool superseded(const List<ps::Receipt>& all, uint32_t i) {
    for (uint32_t j = 0; j < all.count; ++j)
        if (j != i && ps::same_package(all[i], all[j]) &&
            (all[j].revision > all[i].revision || (all[j].revision == all[i].revision && strcmp(all[j].folder, all[i].folder) > 0))) return true;
    return false;
}
// The packages in use: neither removed nor replaced. A removed newest revision never brings back an older one.
TSUCHI_Result current(List<ps::Receipt>& out) {
    List<ps::Receipt> all;
    auto result = scan(all);
    for (uint32_t i = 0; result == TSUCHI_OK && i < all.count; ++i)
        if (!superseded(all, i) && !marked(all[i].folder) && !out.add(all[i])) result = TSUCHI_NO_MEMORY;
    all.clear();
    return result;
}

// Once, before the first listing: what never finished, and what was removed or replaced, goes.
void clean() {
    if (cleaned || !initialize()) return;
    cleaned = true;
    List<ps::Receipt> all;
    {
        SdScope scope;
        Texts files, dirs;
        if (game::list_folder(kPackagesFolder, files, dirs) == TSUCHI_OK)
            for (const char* name = dirs.first(); name; name = dirs.after(name)) {
                if (!ps::folder_valid(name)) continue;
                ps::Receipt r{};
                const Read read = read_receipt(name, r);
                // A receipt this framework cannot read may be a later framework's: its folder is left alone.
                if (read == Read::none || (read == Read::valid && marked(name))) delete_package(name);
                else if (read == Read::valid && all.reserve(all.count + 1)) all.add(r);
            }
        files.clear(); dirs.clear();
    }
    for (uint32_t i = 0; i < all.count; ++i)
        if (superseded(all, i)) delete_package(all[i].folder);
    for (uint32_t i = 0; i < all.count; ++i) {
        char stray[128];
        uint64_t n = 0;
        SdScope scope;
        if (path(stray, sizeof stray, all[i].folder, "/package.zip") && game::file_size(stray, n) == TSUCHI_OK) t::fn::fs_delete_file(stray);
    }
    all.clear();
}

bool add_folders(const ps::Receipt& r, Texts& out) {
    char base[128];
    if (!path(base, sizeof base, r.folder, "/mods/")) return false;
    for (uint32_t i = 0; i < r.count; ++i)
        if (!out.add(base, r.mods[i])) return false;
    return true;
}
// The loader's list. On any failure it lists none: the game's own mods load all the same.
TSUCHI_Result installed(Texts& out) {
    clean();
    List<ps::Receipt> found;
    auto result = current(found);
    if (result == TSUCHI_UNSUPPORTED) { found.clear(); return TSUCHI_OK; } // no SD card: no packages
    for (uint32_t i = 0; result == TSUCHI_OK && i < found.count; ++i)
        if (!add_folders(found[i], out)) result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) { listed.clear(); listed = found; }
    else found.clear();
    return result;
}

// ---- The download, on the worker -----------------------------------------------------------------

void fail_on_worker(Failure f) { failure.exchange(uint32_t(Failure::none), uint32_t(f)); }
void cleanup() {
    if (spool) { t::fn::fs_close_file(spool); spool = nullptr; }
    if (fresh && ours(directory)) delete_package(directory + sizeof kPackagesFolder);
    fresh = false;
}
bool stream_open(const https::HttpsJob&, void** out) {
    SdScope scope;
    hash = Sha256{};
    if (!create(archive, 0, &spool)) { fail_on_worker(Failure::storage); return false; }
    *out = &job;
    return true;
}
bool stream_write(void*, uint64_t offset, const void* bytes, uint64_t size) {
    SdScope scope;
    if (offset > install_state.bytes || size > install_state.bytes - offset) { fail_on_worker(Failure::size); return false; }
    if (t::fn::fs_set_file_size(spool, int64_t(offset + size)) || !write(spool, offset, bytes, size)) { fail_on_worker(Failure::storage); return false; }
    hash.add(bytes, size_t(size));
    return true;
}
void* zip_allocate(uint64_t bytes) { return bytes <= mod_zip::workspace_bytes() ? workspace : nullptr; }
void zip_free(void*) {}
uint32_t input_open(void*, const char*, void** out, uint64_t* size) {
    const uint32_t r = t::fn::fs_open_file(out, archive, int32_t(t::constant::fs_read));
    int64_t n = 0;
    if (!r && t::fn::fs_file_size(&n, *out)) { t::fn::fs_close_file(*out); *out = nullptr; return 1; }
    *size = uint64_t(n);
    return r;
}
uint32_t input_read(void*, void* f, uint64_t off, void* bytes, uint32_t size, uint32_t* got) {
    uint64_t n = 0;
    const auto r = t::fn::fs_read_file(&n, f, int64_t(off), bytes, size);
    *got = uint32_t(n);
    return r;
}
void input_close(void*, void* f) { t::fn::fs_close_file(f); }
uint32_t stage_begin(void*, const char*, void** out) { *out = &job; return 0; }
uint32_t output_open(void*, void*, const char* name, uint64_t size, void** out) {
    char file[768]{};
    if (!append(file, sizeof file, directory) || !append(file, sizeof file, "/mods/") || !append(file, sizeof file, name)) return 1;
    return create(file, size, out) ? 0 : 1;
}
uint32_t output_write(void*, void* f, uint64_t off, const void* bytes, uint32_t size) { return write(f, off, bytes, size) ? 0 : 1; }
uint32_t output_finish(void*, void* f) { t::fn::fs_close_file(f); return 0; }
uint32_t extracted(void*, void*) { return 0; }
bool cancelled(void*) { return job.cancelled.load() != 0; }
void progress(void*) { expanded.store(extraction.expanded_done); expanded_total.store(extraction.expanded_total); }
bool stream_commit(void*) {
    SdScope scope;
    t::fn::fs_close_file(spool);
    spool = nullptr;
    if (job.received.load() != install_state.bytes) { fail_on_worker(Failure::size); return false; }
    uint8_t digest[32];
    hash.finish(digest);
    hex(digest, 32, receipt.digest);
    if (install_state.expected[0] && strcmp(install_state.expected, receipt.digest)) { fail_on_worker(Failure::digest); return false; }
    phase.store(TSUCHI_INSTALL_PHASE_EXTRACTING);
    extraction = {};
    const mod_zip::Platform io{nullptr, zip_allocate, zip_free, input_open, input_read, input_close, stage_begin, output_open, output_write, output_finish, extracted, extracted};
    mod_zip::Request request{};
    request.program_id = t::constant::program_id;
    char id[ps::kModIdBytes];
    ps::derived_id(receipt.source, receipt.key, id);
    memcpy(request.derived_id, id, sizeof id);
    const auto result = mod_zip::extract_zip(io, request, {cancelled, nullptr}, extraction, progress, nullptr);
    if (!result.committed) {
        archive_error.store(uint32_t(result.error));
        fail_on_worker(result.error == mod_zip::Error::Unsupported || result.error == mod_zip::Error::Path ? Failure::unsupported :
                       result.error == mod_zip::Error::Storage ? Failure::storage : Failure::archive);
        return false;
    }
    t::fn::fs_delete_file(archive);
    return true;
}
void stream_abort(void*) { SdScope scope; if (spool) { t::fn::fs_close_file(spool); spool = nullptr; } }
const https::FileSink sink{stream_open, stream_write, stream_commit, stream_abort};

// ---- Checking and publishing, on the game thread -------------------------------------------------

TSUCHI_Result validate_cheat_folder(const char* folder, Text& why) {
    Texts files, dirs;
    auto result = game::list_folder(folder, files, dirs);
    if (result == TSUCHI_OK && (!files.first() || dirs.first())) result = TSUCHI_INVALID;
    auto* parsed = static_cast<cheat::File*>(allocate(sizeof(cheat::File)));
    if (!parsed) result = TSUCHI_NO_MEMORY;
    for (const char* name = files.first(); result == TSUCHI_OK && name; name = files.after(name)) {
        char file[512]{};
        append(file, sizeof file, folder); append(file, sizeof file, "/"); append(file, sizeof file, name);
        uint64_t bytes = 0, got = 0;
        result = game::file_size(file, bytes);
        if (result == TSUCHI_OK && (!bytes || bytes >= cheat::kFileBytes)) result = TSUCHI_INVALID;
        char* text = result == TSUCHI_OK ? static_cast<char*>(allocate(size_t(bytes))) : nullptr;
        if (result == TSUCHI_OK && !text) result = TSUCHI_NO_MEMORY;
        if (result == TSUCHI_OK) result = game::read_file_part(file, 0, text, bytes, got);
        if (result == TSUCHI_OK && got != bytes) result = TSUCHI_IO;
        char problem[160]{};
        if (result == TSUCHI_OK) result = cheat::parse(text, bytes, *parsed, problem, sizeof problem);
        if (result != TSUCHI_OK) why.set(problem[0] ? problem : "The cheat file could not be read or is too large.");
        release(text);
    }
    release(parsed);
    files.clear(); dirs.clear();
    return result;
}

// A manifest for a cooked container or cheat files, which come without one.
bool write_manifest(const char* folder, const char* id) {
    char manifest[512] = "{\"id\":\"";
    append(manifest, sizeof manifest, id);
    append(manifest, sizeof manifest, "\",\"name\":\"");
    // A title's quotes and backslashes are escaped, never read as JSON.
    for (const char* p = receipt.title; *p; ++p) {
        const char ch[3] = {*p == '"' || *p == '\\' ? '\\' : *p, *p == '"' || *p == '\\' ? *p : '\0', 0};
        if (!append(manifest, sizeof manifest, ch)) return false;
    }
    if (!append(manifest, sizeof manifest, "\",\"version\":\"1.0.0\"}")) return false;
    char file[384]{};
    append(file, sizeof file, folder); append(file, sizeof file, "/"); append(file, sizeof file, id); append(file, sizeof file, "/mod.json");
    return write_file(file, manifest, strlen(manifest));
}

bool mentions(const char* text, const char* word) {
    const size_t n = strlen(word), m = strlen(text);
    for (size_t i = 0; i + n <= m; ++i) if (!memcmp(text + i, word, n)) return true;
    return false;
}

void finish(TSUCHI_InstallPhase result, Failure why, const char* message) {
    install_state.phase = result;
    install_state.failure = result == TSUCHI_INSTALL_PHASE_FAILED ? why : Failure::none;
    set_message(message);
    install_state.active = false;
}

void publish() {
    install_state.phase = TSUCHI_INSTALL_PHASE_VALIDATING;
    receipt.kind = uint32_t(extraction.assets ? ps::Kind::assets : extraction.cheats ? ps::Kind::cheats : ps::Kind::mods);
    Texts files, mods, incoming, existing, replaced;
    Text why;
    Failure failed = Failure::manifest;
    SdScope scope;
    char folder[128]{};
    append(folder, sizeof folder, directory);
    append(folder, sizeof folder, "/mods");
    auto result = game::list_folder(folder, files, mods);
    files.clear();
    for (const char* id = mods.first(); result == TSUCHI_OK && id; id = mods.after(id)) {
        if (!ps::mod_id_valid(id) || receipt.count == ps::kMods) { why.set("The package holds more than 16 mods or a folder that is no mod ID."); result = TSUCHI_CAPACITY; break; }
        if (receipt.kind != uint32_t(ps::Kind::mods) && !write_manifest(folder, id)) { failed = Failure::storage; result = TSUCHI_IO; break; }
        memcpy(receipt.mods[receipt.count++], id, strlen(id) + 1);
        char mod_folder[384]{};
        append(mod_folder, sizeof mod_folder, folder); append(mod_folder, sizeof mod_folder, "/"); append(mod_folder, sizeof mod_folder, id);
        if (!incoming.add(mod_folder)) result = TSUCHI_NO_MEMORY;
        if (result == TSUCHI_OK && receipt.kind == uint32_t(ps::Kind::assets)) result = validate_downloaded_assets(id, mod_folder, why);
        if (result == TSUCHI_OK && receipt.kind == uint32_t(ps::Kind::cheats)) {
            append(mod_folder, sizeof mod_folder, "/cheats");
            result = validate_cheat_folder(mod_folder, why);
        }
    }
    // What it replaces: every package of the same source and key that has one of its mods, removed or not.
    List<ps::Receipt> all, replacing;
    if (result == TSUCHI_OK) result = scan(all);
    if (result == TSUCHI_CAPACITY) why.set("256 packages are installed. Remove one first.");
    bool target_found = !install_state.replacing[0];
    for (uint32_t i = 0; result == TSUCHI_OK && i < all.count; ++i) {
        const auto& old = all[i];
        if (ps::same_package(receipt, old)) {
            if (old.revision == UINT64_MAX) { result = TSUCHI_CAPACITY; break; }
            if (old.revision + 1 > receipt.revision) receipt.revision = old.revision + 1;
            if (!add_folders(old, replaced) || !replacing.add(old)) result = TSUCHI_NO_MEMORY;
            if (install_state.replacing[0] && ps::has_mod(old, install_state.replacing) && !marked(old.folder)) target_found = true;
        } else if (!marked(old.folder) && !superseded(all, i) && !add_folders(old, existing)) result = TSUCHI_NO_MEMORY;
    }
    all.clear();
    if (result == TSUCHI_OK && (!target_found || (install_state.replacing[0] && !ps::has_mod(receipt, install_state.replacing)))) {
        why.set("This file holds different mods or comes from a different source. The installed package was kept.");
        failed = Failure::different;
        result = TSUCHI_INVALID;
    }
    if (result == TSUCHI_OK) {
        result = validate_mod_installation(incoming, existing, why, &replaced);
        if (result == TSUCHI_NOT_FOUND) failed = Failure::required;
        else if (result != TSUCHI_OK && mentions(why.get(), "conflict")) failed = Failure::conflict;
    }
    files.clear(); mods.clear(); incoming.clear(); existing.clear(); replaced.clear();
    if (result == TSUCHI_NOT_FOUND) {
        char message[200] = "Install the required mod first: ";
        append(message, sizeof message, why.get());
        finish(TSUCHI_INSTALL_PHASE_FAILED, failed, message);
    } else if (result != TSUCHI_OK) {
        finish(TSUCHI_INSTALL_PHASE_FAILED, failed, why.get()[0] ? why.get() : "The package could not be installed.");
    } else {
        const int64_t now = clock_seconds ? clock_seconds() : -1;
        receipt.updated = now > 0 ? uint64_t(now) : 0;
        ps::seal(receipt);
        char file[128];
        path(file, sizeof file, receipt.folder, "/receipt");
        bool saved = write_file(file, &receipt, sizeof receipt);
        alignas(8) ps::Receipt verify{};
        uint64_t got = 0;
        saved = saved && game::read_file_part(file, 0, &verify, sizeof verify, got) == TSUCHI_OK && got == sizeof verify &&
                !memcmp(&verify, &receipt, sizeof verify) && ps::receipt_valid(verify);
        if (!saved) {
            t::fn::fs_delete_file(file);
            finish(TSUCHI_INSTALL_PHASE_FAILED, Failure::receipt, "The installation receipt could not be saved.");
        } else {
            fresh = false; // published: the folder is the package's now
            // The newer receipt already hides what it replaces; the marks let the next start delete them.
            for (uint32_t i = 0; i < replacing.count; ++i)
                if (mark_removed(replacing[i].folder) && !in_use(replacing[i].folder)) delete_package(replacing[i].folder);
            const char* message = "Installed. Restart Pikmin 4 to load it.";
            if (receipt.kind == uint32_t(ps::Kind::cheats)) message = "Installed. Restart Pikmin 4 to load its cheats.";
            if (receipt.kind == uint32_t(ps::Kind::assets) && mod_choices().enabled(receipt.mods[0])) {
                char where[128];
                Text problem;
                path(where, sizeof where, receipt.folder, "/mods/");
                append(where, sizeof where, receipt.mods[0]);
                message = mount_downloaded_assets(receipt.mods[0], where, problem) == TSUCHI_OK ?
                    "Installed. New loads use it; reload affected maps or objects." : "Installed. Restart Pikmin 4 to load this version.";
                problem.clear();
            }
            finish(TSUCHI_INSTALL_PHASE_INSTALLED, Failure::none, message);
        }
    }
    replacing.clear();
    why.clear();
    if (fresh) cleanup();
    refresh_installed_packages();
}

void report_progress() {
    Install& s = install_state;
    if (!s.active || !s.owner || !mod_usable(s.owner) || (s.shown && s.shown_phase == s.phase && s.shown_done == s.done)) return;
    s.shown = true; s.shown_phase = s.phase; s.shown_done = s.done;
    packages::emit_progress_to(s.owner, {{s.id}, s.phase, s.done, s.total});
}
void report_finished() {
    Install& s = install_state;
    if (s.active || s.told || !s.owner || !mod_usable(s.owner)) return;
    s.told = true;
    disown(s.owner, kInstall, s.id);
    packages::emit_finished_to(s.owner, {{s.id}, s.phase, failure_name(s.failure)});
}

void tick() {
    Install& s = install_state;
    if (s.active) {
        const uint32_t state = job.state.load();
        s.phase = TSUCHI_InstallPhase(phase.load());
        const bool unpacking = s.phase == TSUCHI_INSTALL_PHASE_EXTRACTING;
        s.done = unpacking ? expanded.load() : job.received.load();
        s.total = unpacking ? expanded_total.load() : s.bytes;
        report_progress();
        if (state >= https::HTTPS_SUCCEEDED && state <= https::HTTPS_CANCELLED) {
            release(workspace);
            workspace = nullptr;
            if (state == https::HTTPS_SUCCEEDED && !job.cancelled.load()) publish();
            else {
                { SdScope scope; cleanup(); }
                const Failure why = Failure(failure.load());
                if (state == https::HTTPS_CANCELLED || job.cancelled.load()) finish(TSUCHI_INSTALL_PHASE_CANCELLED, Failure::none, "The download was cancelled.");
                else if (why == Failure::unsupported) finish(TSUCHI_INSTALL_PHASE_FAILED, why, "This ZIP file holds files that are not a Tsuchi package, one cooked container, or cheat files.");
                else if (why == Failure::digest) finish(TSUCHI_INSTALL_PHASE_FAILED, why, "The file's SHA-256 digest is not the one expected.");
                else if (why == Failure::size) finish(TSUCHI_INSTALL_PHASE_FAILED, why, "The file's size is not the one expected.");
                else if (why == Failure::storage) finish(TSUCHI_INSTALL_PHASE_FAILED, why, "The SD card refused the package's files.");
                else if (why == Failure::archive) finish(TSUCHI_INSTALL_PHASE_FAILED, why, "The ZIP file is damaged or uses a format Tsuchi does not read.");
                else if (job.error.load() == https::HTTPS_ERROR_HTTP) finish(TSUCHI_INSTALL_PHASE_FAILED, Failure::http, "The server refused the download. Try again later.");
                else finish(TSUCHI_INSTALL_PHASE_FAILED, Failure::network, "The download failed. Check the connection and try again.");
            }
        }
    }
    report_finished();
}

// The install's owner stopped: the install stops with it.
void orphaned(void*, uint64_t id) {
    if (install_state.id != id) return;
    if (install_state.active) job.cancelled.store(1);
    install_state.owner = nullptr;
}

// ---- The service ---------------------------------------------------------------------------------

bool allowed(TSUCHI_Mod* mod) {
    const ModRecord* record = find_mod(mod_id(mod));
    return record && (record->capabilities & kModCapabilityPackages);
}
bool url_valid(const char* url) {
    const size_t n = strlen(url);
    if (n < 9 || n > 1023 || memcmp(url, "https://", 8)) return false;
    for (const char* p = url; *p; ++p)
        if (uint8_t(*p) <= 32 || uint8_t(*p) >= 127 || *p == '\\' || *p == '#') return false;
    // No credentials: an @ before the end of the authority.
    for (const char* p = url + 8; *p && *p != '/' && *p != '?'; ++p) if (*p == '@') return false;
    return true;
}
// The last part of the URL's path, as plain text.
void filename_of(const char* url, char* out, size_t room) {
    const char* end = url;
    while (*end && *end != '?') ++end;
    const char* start = end;
    while (start > url + 8 && start[-1] != '/') --start;
    size_t n = 0;
    for (const char* p = start; p < end && n + 1 < room; ++p) out[n++] = *p;
    out[n] = 0;
}

TSUCHI_Result install(TSUCHI_Mod* mod, const char* url, uint64_t bytes, const char* source, const char* key, const char* title, const char* release_text,
                      const char* sha256_text, const char* replacing, TSUCHI_PackageInstall* out) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    if (!url_valid(url) || !bytes || bytes > kMostBytes || !ps::source_valid(source) || !ps::text_valid(key, ps::kKeyBytes, false) ||
        !ps::text_valid(title, ps::kTitleBytes, false) || !ps::text_valid(release_text, ps::kReleaseBytes, true) ||
        (*sha256_text && !ps::digest_valid(sha256_text)) || (*replacing && !ps::mod_id_valid(replacing))) return TSUCHI_INVALID;
    if (install_state.active) return TSUCHI_BUSY;
    if (!initialize()) return TSUCHI_UNSUPPORTED;
    if (listed.count >= kMostPackages) return TSUCHI_CAPACITY;
    if (install_state.owner && !install_state.told && mod_usable(install_state.owner)) disown(install_state.owner, kInstall, install_state.id);
    const uint64_t id = next_install++;
    if (own(mod, kInstall, id, orphaned, nullptr) != TSUCHI_OK) return TSUCHI_NO_MEMORY;
    workspace = allocate(size_t(mod_zip::workspace_bytes()));
    if (!workspace) { disown(mod, kInstall, id); return TSUCHI_NO_MEMORY; }
    install_state = {};
    install_state.id = id;
    install_state.owner = mod;
    install_state.bytes = bytes;
    memcpy(install_state.expected, sha256_text, strlen(sha256_text) + 1);
    memcpy(install_state.replacing, replacing, strlen(replacing) + 1);
    receipt = {};
    memcpy(receipt.source, source, strlen(source) + 1);
    memcpy(receipt.key, key, strlen(key) + 1);
    memcpy(receipt.title, title, strlen(title) + 1);
    memcpy(receipt.release, release_text, strlen(release_text) + 1);
    char name[ps::kFilenameBytes];
    filename_of(url, name, sizeof name);
    if (ps::text_valid(name, sizeof name, true)) memcpy(receipt.filename, name, strlen(name) + 1);
    // A folder name of its own: never one that is there, even after a clock collision or an interrupted run.
    const uint64_t stamp[3] = {cost_now(), id, reinterpret_cast<uint64_t>(&job)};
    uint8_t sum[32];
    sha256(stamp, sizeof stamp, sum);
    hex(sum, 16, receipt.folder);
    path(directory, sizeof directory, receipt.folder, "");
    path(archive, sizeof archive, receipt.folder, "/package.zip");
    {
        SdScope scope;
        char parents[128];
        path(parents, sizeof parents, receipt.folder, "/x");
        int32_t kind = 0;
        if (t::fn::fs_entry_kind(&kind, directory) != t::constant::fs_no_such_path || !folders(parents)) {
            release(workspace); workspace = nullptr; disown(mod, kInstall, id);
            install_state.owner = nullptr;
            return TSUCHI_IO;
        }
    }
    fresh = true;
    phase.store(TSUCHI_INSTALL_PHASE_DOWNLOADING);
    failure.store(uint32_t(Failure::none));
    archive_error.store(0); expanded.store(0); expanded_total.store(0);
    install_state.phase = TSUCHI_INSTALL_PHASE_DOWNLOADING;
    install_state.total = bytes;
    install_state.active = true;
    memcpy(job.url, url, strlen(url) + 1);
    job.kind = https::HTTPS_FILE; job.max_bytes = bytes; job.timeout_ms = 1800000; job.connect_timeout_ms = 60000; job.sink = &sink;
    job.cancelled.store(0); job.error.store(0); job.http_status.store(0); job.transport_error.store(0); job.received.store(0); job.total.store(0);
    if (!https::https_queue_file(&job)) {
        { SdScope scope; cleanup(); }
        release(workspace); workspace = nullptr;
        disown(mod, kInstall, id);
        install_state = {};
        return TSUCHI_UNSUPPORTED;
    }
    *out = {id};
    return TSUCHI_OK;
}
Install* mine(TSUCHI_Mod* mod, TSUCHI_PackageInstall handle) {
    return install_state.id == handle.id && install_state.owner == mod ? &install_state : nullptr;
}
TSUCHI_Result status(TSUCHI_Mod* mod, TSUCHI_PackageInstall handle, TSUCHI_InstallStatus* out) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    const Install* s = mine(mod, handle);
    if (!s) return TSUCHI_NOT_FOUND;
    *out = {s->phase, s->done, s->total, failure_name(s->failure), s->active ? "" : s->message};
    return TSUCHI_OK;
}
TSUCHI_Result cancel(TSUCHI_Mod* mod, TSUCHI_PackageInstall handle) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    const Install* s = mine(mod, handle);
    if (!s) return TSUCHI_NOT_FOUND;
    if (s->active) job.cancelled.store(1);
    return TSUCHI_OK;
}
TSUCHI_Result count(TSUCHI_Mod* mod, uint32_t* out) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    if (!initialize()) return TSUCHI_UNSUPPORTED;
    *out = listed.count;
    return TSUCHI_OK;
}
char listed_mods[ps::kMods * ps::kModIdBytes];
TSUCHI_Result at(TSUCHI_Mod* mod, uint32_t index, TSUCHI_Package* out) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    if (index >= listed.count) return TSUCHI_NOT_FOUND;
    const auto& r = listed[index];
    listed_mods[0] = 0;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (i) append(listed_mods, sizeof listed_mods, " ");
        append(listed_mods, sizeof listed_mods, r.mods[i]);
    }
    *out = {r.source, r.key, r.title, r.release, r.filename, r.updated, TSUCHI_PackageKind(r.kind), listed_mods};
    return TSUCHI_OK;
}
TSUCHI_Result remove(TSUCHI_Mod* mod, uint32_t index) {
    if (!allowed(mod)) return TSUCHI_DENIED;
    return remove_installed_package(index);
}
const packages::Backend backend{install, status, cancel, count, at, remove};

} // namespace

void packages_set_clock(int64_t (*seconds)()) { clock_seconds = seconds; }
void packages_start() {
    if (started) return;
    started = true;
    set_installed_folders(installed);
    if (!each_engine_frame(tick)) each_frame(tick);
    packages::set_backend(&backend);
}
TSUCHI_Result refresh_installed_packages() {
    List<ps::Receipt> found;
    const auto result = current(found);
    if (result == TSUCHI_OK) { listed.clear(); listed = found; }
    else found.clear();
    return result;
}
uint32_t installed_package_count() { return listed.count; }
const ps::Receipt* installed_package(uint32_t index) { return index < listed.count ? &listed[index] : nullptr; }
const ps::Receipt* package_holding(const char* id, uint32_t* index) {
    for (uint32_t i = 0; i < listed.count; ++i)
        if (ps::has_mod(listed[i], id)) { if (index) *index = i; return &listed[i]; }
    return nullptr;
}
bool package_install_running() { return install_state.active; }
TSUCHI_Result remove_installed_package(uint32_t index) {
    if (install_state.active) return TSUCHI_BUSY;
    const ps::Receipt* r = installed_package(index);
    if (!r || !initialize()) return TSUCHI_NOT_FOUND;
    char folder[33];
    memcpy(folder, r->folder, sizeof folder);
    {
        SdScope scope;
        if (!mark_removed(folder)) return TSUCHI_IO;
    }
    if (!in_use(folder)) delete_package(folder);
    return refresh_installed_packages();
}

} // namespace tsuchi
