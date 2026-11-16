// The packages service and its SD card store, with the SDK's file calls on a memory disk and the HTTPS
// worker played by the test: install, update, remove, cancel, failures, start-up cleaning, and the
// manifest capability, through the service's own table.
#include "../../framework/game/packages.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
std::map<std::string, std::string> disk;
std::set<std::string> dirs;
struct Handle { std::string name; };
int depth = 0;
bool fail_receipt = false, refuse_queue = false;
TSUCHI_Result install_result = TSUCHI_OK; // what the manifest check says
std::vector<std::string> checked_incoming, checked_existing, checked_replaced;
std::vector<std::string> mounted, mounted_folders;
tsuchi::https::HttpsJob* queued = nullptr;
TSUCHI_Result (*listing)(tsuchi::Texts&) = nullptr;
std::vector<void (*)()> steps;
std::vector<tsuchi::ModRecord> records;
std::string folder_of_zips;
const uint32_t missing = tsuchi::game::table::constant::fs_no_such_path;

uint32_t kind(void* out, void* p) { std::string path = static_cast<char*>(p); if (!dirs.count(path) && !disk.count(path)) return missing; *static_cast<int32_t*>(out) = dirs.count(path) ? 0 : 1; return 0; }
uint32_t make_dir(void* p) { assert(depth); return dirs.insert(static_cast<char*>(p)).second ? 0 : 1; }
uint32_t make_file(void* p, int64_t n) { assert(depth); std::string path = static_cast<char*>(p); if (disk.count(path)) return 1; disk[path] = std::string(size_t(n), 0); return 0; }
uint32_t open_file(void* out, void* p, int32_t) { assert(depth); std::string path = static_cast<char*>(p); if (!disk.count(path)) return missing; *static_cast<void**>(out) = new Handle{path}; return 0; }
void close_file(void* p) { delete static_cast<Handle*>(p); }
uint32_t size_file(void* out, void* f) { *static_cast<int64_t*>(out) = int64_t(disk[static_cast<Handle*>(f)->name].size()); return 0; }
uint32_t resize_file(void* f, int64_t n) { disk[static_cast<Handle*>(f)->name].resize(size_t(n)); return 0; }
uint32_t write_file_fn(void* f, int64_t offset, void* bytes, uint64_t n, void*) {
    assert(depth);
    auto& h = *static_cast<Handle*>(f);
    if (fail_receipt && h.name.find("/receipt") != std::string::npos) return 1;
    auto& s = disk[h.name];
    if (uint64_t(offset) > s.size() || n > s.size() - uint64_t(offset)) return 1;
    memcpy(s.data() + offset, bytes, size_t(n));
    return 0;
}
uint32_t read_file_fn(void* out, void* f, int64_t offset, void* bytes, uint64_t n) {
    auto& s = disk[static_cast<Handle*>(f)->name];
    if (uint64_t(offset) > s.size()) return 1;
    const size_t got = std::min(size_t(n), s.size() - size_t(offset));
    memcpy(bytes, s.data() + offset, got);
    *static_cast<uint64_t*>(out) = got;
    return 0;
}
uint32_t delete_file(void* p) { assert(depth); return disk.erase(static_cast<char*>(p)) ? 0 : missing; }
uint32_t remove_tree(const char* p) {
    assert(depth);
    const std::string prefix = std::string(p) + "/";
    // Only ever one package folder.
    assert(prefix.rfind("tsuchisd:/tsuchi/downloads/", 0) == 0 && prefix.size() == strlen("tsuchisd:/tsuchi/downloads") + 34);
    for (auto i = disk.begin(); i != disk.end();) if (i->first.rfind(prefix, 0) == 0) i = disk.erase(i); else ++i;
    for (auto i = dirs.begin(); i != dirs.end();) if (*i == p || i->rfind(prefix, 0) == 0) i = dirs.erase(i); else ++i;
    return 0;
}
int64_t fixed_clock() { return 1700000000; }
} // namespace

namespace tsuchi {
bool container_mounted_for(const char*) { return false; }
bool container_mounted_from(const char* prefix) {
    for (const auto& folder : mounted_folders) if (folder.rfind(prefix, 0) == 0) return true;
    return false;
}
TSUCHI_Result validate_downloaded_assets(const char*, const char*, Text&) { return TSUCHI_OK; }
TSUCHI_Result mount_downloaded_assets(const char* id, const char* folder, Text&) { mounted.push_back(id); mounted_folders.push_back(folder); return TSUCHI_OK; }
const ModChoices& mod_choices() { static ModChoices c; return c; }
bool ModChoices::enabled(const char*) const { return true; }
bool each_engine_frame(void (*fn)()) { steps.push_back(fn); return true; }
bool each_frame(void (*)()) { assert(false); return false; }
const void* sd_enter() { ++depth; return nullptr; }
void sd_leave(const void*) { assert(depth > 0); --depth; }
bool sd_card_mounted() { return true; }
void set_installed_folders(TSUCHI_Result (*fn)(Texts&)) { listing = fn; }
void mod_note(const char*, const char*) {}
uint32_t mod_count() { return uint32_t(records.size()); }
ModRecord* mod_at(uint32_t i) { return i < records.size() ? &records[i] : nullptr; }
ModRecord* find_mod(const char* id) { for (auto& r : records) if (!strcmp(r.id, id)) return &r; return nullptr; }
TSUCHI_Result validate_mod_installation(const Texts& incoming, const Texts& existing, Text& problem, const Texts* replaced) {
    checked_incoming.clear(); checked_existing.clear(); checked_replaced.clear();
    for (const char* p = incoming.first(); p; p = incoming.after(p)) checked_incoming.push_back(p);
    for (const char* p = existing.first(); p; p = existing.after(p)) checked_existing.push_back(p);
    for (const char* p = replaced ? replaced->first() : nullptr; p; p = replaced->after(p)) checked_replaced.push_back(p);
    if (install_result == TSUCHI_NOT_FOUND) problem.set("author.lib");
    else if (install_result != TSUCHI_OK) problem.set("Incompatible test manifest.");
    return install_result;
}
namespace https { bool https_queue_file(HttpsJob* j) { assert(!queued); if (refuse_queue) return false; queued = j; j->state.store(HTTPS_RUNNING); return true; } }
namespace game {
uint64_t main_base() { return 0; }
bool readable(uint64_t, uint64_t) { return false; }
uint64_t module_containing(uint64_t) { return 0; }
uint64_t module_symbol(uint64_t, const char*) { return 0; }
bool files_known() { return true; }
TSUCHI_Result file_size(const char* p, uint64_t& n) { auto it = disk.find(p); if (it == disk.end()) return TSUCHI_NOT_FOUND; n = it->second.size(); return TSUCHI_OK; }
TSUCHI_Result read_file_part(const char* p, uint64_t off, void* out, uint64_t cap, uint64_t& n) {
    uint64_t size = 0;
    if (file_size(p, size) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    n = off >= size ? 0 : std::min(cap, size - off);
    memcpy(out, disk[p].data() + off, size_t(n));
    return TSUCHI_OK;
}
TSUCHI_Result list_folder(const char* p, Texts& files, Texts& folders) {
    assert(depth);
    if (!dirs.count(p)) return TSUCHI_NOT_FOUND;
    const std::string prefix = std::string(p) + "/";
    for (const auto& d : dirs) if (d.rfind(prefix, 0) == 0 && d.find('/', prefix.size()) == std::string::npos) folders.add(d.c_str() + prefix.size());
    for (const auto& f : disk) if (f.first.rfind(prefix, 0) == 0 && f.first.find('/', prefix.size()) == std::string::npos) files.add(f.first.c_str() + prefix.size());
    return TSUCHI_OK;
}
} // namespace game
} // namespace tsuchi

namespace {
using namespace tsuchi;
const TSUCHI_PackagesService& service = packages::table;
TSUCHI_Mod* downloader = nullptr;
TSUCHI_Mod* plain_mod = nullptr;
std::vector<TSUCHI_PackagesProgress> progressed;
std::vector<std::string> finished; // "<id> <phase> <failure>"

std::string zip(const char* name) {
    std::ifstream input(folder_of_zips + "/" + name + ".zip", std::ios::binary);
    assert(input);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}
void frame_steps() { for (auto fn : steps) fn(); assert(depth == 0); }
std::string root_of(const char* folder) { return std::string(kPackagesFolder) + "/" + folder; }
bool package_files(const char* folder) {
    const std::string prefix = root_of(folder) + "/";
    for (const auto& f : disk) if (f.first.rfind(prefix, 0) == 0) return true;
    return false;
}
size_t package_folders() {
    size_t n = 0;
    const std::string prefix = std::string(kPackagesFolder) + "/";
    for (const auto& d : dirs) if (d.rfind(prefix, 0) == 0 && d.find('/', prefix.size()) == std::string::npos) ++n;
    return n;
}

struct Options { const char* source = "github"; const char* key = "author/mod"; const char* sha = ""; const char* replacing = ""; uint64_t bytes = 0; bool cancel = false; size_t chunk = 777; };
// One install, played through: queued, streamed, committed or aborted, and published at the next frame.
TSUCHI_Result run(const std::string& data, const Options& o = {}, TSUCHI_PackageInstall* out = nullptr) {
    TSUCHI_PackageInstall handle{};
    const TSUCHI_Result started = service.install(downloader, "https://example.com/releases/v1/mod.zip?x=1", o.bytes ? o.bytes : data.size(),
                                                  o.source, o.key, "Example \"mod\"", "v1", o.sha, o.replacing, &handle);
    if (out) *out = handle;
    if (started != TSUCHI_OK) return started;
    assert(queued);
    void* stream = nullptr;
    bool fine = queued->sink->open(*queued, &stream);
    for (size_t off = 0; fine && off < data.size(); off += o.chunk) {
        const size_t size = std::min(o.chunk, data.size() - off);
        fine = queued->sink->write(stream, off, data.data() + off, size);
        if (fine) queued->received.store(off + size);
        if (off == 0) frame_steps(); // a frame while it downloads
        if (o.cancel && off == 0) service.cancel(downloader, handle);
    }
    const bool success = fine && !queued->cancelled.load() && queued->sink->commit(stream);
    if (!success && stream) queued->sink->abort(stream);
    queued->state.store(queued->cancelled.load() ? https::HTTPS_CANCELLED : success ? https::HTTPS_SUCCEEDED : https::HTTPS_FAILED);
    queued = nullptr;
    frame_steps();
    return TSUCHI_OK;
}
std::string last_finished() { return finished.empty() ? "" : finished.back(); }
TSUCHI_Package package(uint32_t i) { TSUCHI_Package p{}; assert(service.at(downloader, i, &p) == TSUCHI_OK); return p; }
uint32_t count() { uint32_t n = 0; assert(service.count(downloader, &n) == TSUCHI_OK); return n; }

void reset() {
    disk.clear(); dirs.clear(); records.clear(); mounted.clear(); mounted_folders.clear(); progressed.clear(); finished.clear();
    fail_receipt = refuse_queue = false; install_result = TSUCHI_OK;
    listed.clear(); cleaned = false; install_state = {};
}
ModRecord& loaded(const char* id, const char* folder = "") {
    records.emplace_back();
    ModRecord& r = records.back();
    memcpy(r.id, id, strlen(id) + 1);
    if (*folder) memcpy(r.folder, folder, strlen(folder) + 1);
    return r;
}
void add_capable(const char* id) { loaded(id).capabilities = kModCapabilityPackages; }
// A startup: the loader lists the folders, as find_mods does.
std::vector<std::string> start_listing() {
    Texts out;
    const TSUCHI_Result result = listing(out);
    assert(result == TSUCHI_OK);
    std::vector<std::string> folders;
    for (const char* p = out.first(); p; p = out.after(p)) folders.push_back(p);
    out.clear();
    return folders;
}

void installs_and_lists() {
    reset(); add_capable("the.downloader");
    TSUCHI_PackageInstall handle{};
    assert(run(zip("mods"), {}, &handle) == TSUCHI_OK);
    assert(last_finished() == std::to_string(handle.id) + " 3 ");
    assert(!progressed.empty() && progressed.front().phase == TSUCHI_INSTALL_PHASE_DOWNLOADING && progressed.front().install.id == handle.id);
    TSUCHI_InstallStatus s{};
    assert(service.status(downloader, handle, &s) == TSUCHI_OK && s.phase == TSUCHI_INSTALL_PHASE_INSTALLED && !*s.failure && strstr(s.message, "Restart"));
    assert(count() == 1);
    const auto p = package(0);
    assert(!strcmp(p.source, "github") && !strcmp(p.key, "author/mod") && !strcmp(p.title, "Example \"mod\"") && !strcmp(p.release, "v1"));
    assert(!strcmp(p.filename, "mod.zip") && p.updated == 1700000000 && p.kind == TSUCHI_PACKAGE_KIND_MODS && !strcmp(p.mods, "author.mod"));
    assert(installed_package(0));
    const package_store::Receipt r = *installed_package(0);
    assert(r.revision == 0 && package_files(r.folder));
    assert(disk.count(root_of(r.folder) + "/mods/author.mod/main.lua") && !disk.count(root_of(r.folder) + "/package.zip"));
    // The loader finds it at the next start.
    const auto folders = start_listing();
    assert(folders.size() == 1 && folders[0] == root_of(r.folder) + "/mods/author.mod");
    assert(package_holding("author.mod") == installed_package(0) && !package_holding("missing"));
    // The handle is the mod's own.
    assert(service.status(plain_mod, handle, &s) == TSUCHI_DENIED);
    assert(service.status(downloader, {handle.id + 1}, &s) == TSUCHI_NOT_FOUND);
}

void capability_and_arguments() {
    reset(); add_capable("the.downloader"); loaded("plain.mod");
    TSUCHI_PackageInstall handle{};
    uint32_t n = 0;
    TSUCHI_Package p{};
    // Without "packages" in its manifest, every call is refused before anything happens.
    assert(service.install(plain_mod, "https://example.com/a.zip", 10, "s", "k", "t", "r", "", "", &handle) == TSUCHI_DENIED && !queued);
    assert(service.count(plain_mod, &n) == TSUCHI_DENIED && service.at(plain_mod, 0, &p) == TSUCHI_DENIED && service.remove(plain_mod, 0) == TSUCHI_DENIED);
    assert(service.cancel(plain_mod, handle) == TSUCHI_DENIED || service.cancel(plain_mod, handle) == TSUCHI_INVALID);
    const auto refused = [&](const char* url, uint64_t bytes, const char* source, const char* key, const char* title, const char* release, const char* sha, const char* replacing) {
        return service.install(downloader, url, bytes, source, key, title, release, sha, replacing, &handle) == TSUCHI_INVALID && !queued;
    };
    assert(refused("http://example.com/a.zip", 10, "s", "k", "t", "", "", ""));
    assert(refused("https://user@example.com/a.zip", 10, "s", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip#part", 10, "s", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 0, "s", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", (512ull << 20) + 1, "s", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "two words", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "", "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, std::string(64, 's').c_str(), "k", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "line\nbreak", "t", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "\xff", "", "", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "t", "", std::string(64, 'A').c_str(), ""));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "t", "", "abc", ""));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "t", "", "", "Not An ID"));
    assert(refused("https://example.com/a.zip", 10, "s", "k", "t", "", "", "tsuchi.self"));
    // One install at a time; the service is refused while the queue refuses too.
    assert(service.install(downloader, "https://example.com/a.zip", 10, "s", "k", "t", "", "", "", &handle) == TSUCHI_OK && queued);
    TSUCHI_PackageInstall second{};
    assert(service.install(downloader, "https://example.com/b.zip", 10, "s", "k2", "t", "", "", "", &second) == TSUCHI_BUSY);
    assert(service.remove(downloader, 0) == TSUCHI_BUSY || service.remove(downloader, 0) == TSUCHI_NOT_FOUND);
    queued->state.store(https::HTTPS_FAILED); queued->error.store(https::HTTPS_ERROR_HTTP); queued = nullptr;
    frame_steps();
    assert(last_finished() == std::to_string(handle.id) + " 4 http" && package_folders() == 0);
    refuse_queue = true;
    assert(service.install(downloader, "https://example.com/a.zip", 10, "s", "k", "t", "", "", "", &handle) == TSUCHI_UNSUPPORTED && package_folders() == 0 && !package_install_running());
}

void failures_leave_nothing() {
    reset(); add_capable("the.downloader");
    const std::string data = zip("mods");
    assert(run(data, {.sha = "0000000000000000000000000000000000000000000000000000000000000000"}) == TSUCHI_OK);
    assert(last_finished().find(" 4 digest") != std::string::npos && package_folders() == 0 && count() == 0);
    // The right digest passes.
    uint8_t sum[32]; char digest[65];
    sha256(data.data(), data.size(), sum); hex(sum, 32, digest);
    assert(run(data, {.sha = digest}) == TSUCHI_OK && last_finished().find(" 3 ") != std::string::npos && count() == 1);
    reset(); add_capable("the.downloader");
    assert(run(data, {.bytes = data.size() + 5}) == TSUCHI_OK && last_finished().find(" 4 size") != std::string::npos && package_folders() == 0);
    assert(run(data, {.bytes = data.size() - 5}) == TSUCHI_OK && last_finished().find(" 4 size") != std::string::npos && package_folders() == 0);
    assert(run(data, {.cancel = true}) == TSUCHI_OK && last_finished().find(" 5 ") != std::string::npos && package_folders() == 0);
    install_result = TSUCHI_INVALID;
    assert(run(data) == TSUCHI_OK && last_finished().find(" 4 manifest") != std::string::npos && package_folders() == 0);
    TSUCHI_PackageInstall handle{};
    install_result = TSUCHI_NOT_FOUND;
    assert(run(data, {}, &handle) == TSUCHI_OK && last_finished().find(" 4 required") != std::string::npos && package_folders() == 0);
    TSUCHI_InstallStatus s{};
    assert(service.status(downloader, handle, &s) == TSUCHI_OK && strstr(s.message, "author.lib"));
    install_result = TSUCHI_OK; fail_receipt = true;
    assert(run(data) == TSUCHI_OK && last_finished().find(" 4 receipt") != std::string::npos && package_folders() == 0);
    fail_receipt = false;
    assert(run(zip("unsupported")) == TSUCHI_OK && last_finished().find(" 4 unsupported") != std::string::npos && package_folders() == 0);
    std::string broken = data; broken[broken.size() / 2] ^= 0x55;
    assert(run(broken) == TSUCHI_OK && last_finished().find(" 4 ") != std::string::npos && package_folders() == 0 && count() == 0);
}

void updates_replace() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK && count() == 1);
    const package_store::Receipt first = *installed_package(0);
    // Same source and key, same mod: an update. Nothing loaded uses the first, so it goes at once.
    assert(run(zip("mods2")) == TSUCHI_OK && last_finished().find(" 3 ") != std::string::npos);
    assert(count() == 1 && installed_package(0)->revision == 1 && strcmp(installed_package(0)->folder, first.folder) && !package_files(first.folder));
    assert(checked_replaced.size() == 1 && checked_replaced[0] == root_of(first.folder) + "/mods/author.mod");
    assert(disk.at(root_of(installed_package(0)->folder) + "/mods/author.mod/main.lua") == "print('two')");
    // Another key is another package, whose mods are checked against it.
    assert(run(zip("other"), {.key = "author/other"}) == TSUCHI_OK && count() == 2);
    assert(checked_existing.size() == 1 && checked_replaced.empty());
    // The same file under another source is not an update.
    assert(run(zip("other"), {.source = "gamebanana", .key = "author/other"}) == TSUCHI_OK && count() == 3);
}

void updates_in_use_wait_for_start() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK);
    const package_store::Receipt first = *installed_package(0);
    loaded("author.mod", (root_of(first.folder) + "/mods/author.mod").c_str()); // the loader started it from there
    assert(run(zip("mods2")) == TSUCHI_OK && count() == 1 && installed_package(0)->revision == 1);
    assert(package_files(first.folder) && disk.count(root_of(first.folder) + "/removed"));
    // The next start deletes the replaced one before listing; only the new one loads.
    records.pop_back(); cleaned = false;
    const auto folders = start_listing();
    assert(!package_files(first.folder) && folders.size() == 1 && folders[0].find(installed_package(0)->folder) != std::string::npos);
}

void replacing_must_match() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK);
    const std::string kept = installed_package(0)->folder;
    // An update chosen for author.mod that holds other mods is refused, and the installed one kept.
    assert(run(zip("other"), {.replacing = "author.mod"}) == TSUCHI_OK && last_finished().find(" 4 different") != std::string::npos);
    assert(count() == 1 && installed_package(0)->folder == kept && package_folders() == 1);
    // So is one from another source.
    assert(run(zip("mods2"), {.source = "elsewhere", .replacing = "author.mod"}) == TSUCHI_OK && last_finished().find(" 4 different") != std::string::npos);
    assert(run(zip("mods2"), {.replacing = "author.mod"}) == TSUCHI_OK && last_finished().find(" 3 ") != std::string::npos && count() == 1 &&
           installed_package(0)->folder != kept);
}

void removes() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK && run(zip("other"), {.key = "author/other"}) == TSUCHI_OK && count() == 2);
    const std::string a = installed_package(0)->folder, b = installed_package(1)->folder;
    // Not loaded: its files go at once.
    assert(service.remove(downloader, 0) == TSUCHI_OK && count() == 1 && !package_files(a.c_str()));
    assert(service.remove(downloader, 5) == TSUCHI_NOT_FOUND);
    // Loaded: it is marked, and its files stay until the next start.
    loaded("other.mod", (root_of(b.c_str()) + "/mods/other.mod").c_str());
    assert(service.remove(downloader, 0) == TSUCHI_OK && count() == 0 && package_files(b.c_str()) && disk.count(root_of(b.c_str()) + "/removed"));
    records.pop_back(); cleaned = false;
    assert(start_listing().empty() && !package_files(b.c_str()) && package_folders() == 0);
    // A reinstall after a removal is a fresh package.
    assert(run(zip("mods")) == TSUCHI_OK && count() == 1 && installed_package(0)->revision == 0);
}

void start_cleans() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK);
    const std::string kept = installed_package(0)->folder;
    // An install that never finished: a folder with a partial download and no receipt.
    const std::string interrupted = root_of("0123456789abcdef0123456789abcdef");
    dirs.insert(interrupted); dirs.insert(interrupted + "/mods");
    disk[interrupted + "/package.zip"] = "partial";
    // A receipt of a later framework: left alone, and not loaded.
    const std::string later = root_of("fedcba9876543210fedcba9876543210");
    dirs.insert(later); disk[later + "/receipt"] = "TSPKG99 something else";
    // A folder that is not a package's.
    dirs.insert(root_of("notes")); disk[root_of("notes") + "/readme.txt"] = "mine";
    // A stray download beside a published package.
    disk[root_of(kept.c_str()) + "/package.zip"] = "stray";
    cleaned = false;
    const auto folders = start_listing();
    assert(folders.size() == 1 && folders[0].find(kept) != std::string::npos);
    assert(!dirs.count(interrupted) && !disk.count(interrupted + "/package.zip"));
    assert(disk.count(later + "/receipt") && disk.count(root_of("notes") + "/readme.txt"));
    assert(!disk.count(root_of(kept.c_str()) + "/package.zip"));
}

void too_many_lists_none() {
    reset(); add_capable("the.downloader");
    assert(run(zip("mods")) == TSUCHI_OK);
    const package_store::Receipt model = *installed_package(0);
    for (uint32_t i = 0; i < kMostPackages; ++i) {
        package_store::Receipt r = model;
        char name[33];
        snprintf(name, sizeof name, "%032x", i + 1);
        memcpy(r.folder, name, sizeof name);
        snprintf(r.key, sizeof r.key, "key/%u", i);
        package_store::seal(r);
        dirs.insert(root_of(name));
        disk[root_of(name) + "/receipt"] = std::string(reinterpret_cast<const char*>(&r), sizeof r);
    }
    Texts out;
    cleaned = false;
    assert(listing(out) == TSUCHI_CAPACITY && !out.first()); // find_mods then loads the game's own mods alone
    out.clear();
    assert(refresh_installed_packages() == TSUCHI_CAPACITY);
}

void cooked_ids_come_from_source_and_key() {
    reset(); add_capable("the.downloader");
    char expected[package_store::kModIdBytes];
    package_store::derived_id("gamebanana", "12345", expected);
    assert(!strncmp(expected, "external.", 9) && strlen(expected) == 33);
    assert(run(zip("cooked"), {.source = "gamebanana", .key = "12345"}) == TSUCHI_OK && last_finished().find(" 3 ") != std::string::npos);
    assert(count() == 1 && package(0).kind == TSUCHI_PACKAGE_KIND_ASSETS && !strcmp(package(0).mods, expected));
    const std::string first = installed_package(0)->folder;
    assert(disk.count(root_of(first.c_str()) + "/mods/" + expected + "/Map_P.pak") && disk.count(root_of(first.c_str()) + "/mods/" + expected + "/mod.json"));
    assert(disk.at(root_of(first.c_str()) + "/mods/" + expected + "/mod.json").find("Example \\\"mod\\\"") != std::string::npos);
    assert(mounted.size() == 1 && mounted[0] == expected); // enabled: mounted for new loads at once
    // A renamed container in a new release is the same mod: it replaces the first. The engine still reads the
    // container mounted from the first, so its files stay, marked, until the next start.
    assert(run(zip("cooked_renamed"), {.source = "gamebanana", .key = "12345"}) == TSUCHI_OK && count() == 1 && !strcmp(package(0).mods, expected) &&
           installed_package(0)->revision == 1 && package_files(first.c_str()) && disk.count(root_of(first.c_str()) + "/removed"));
    assert(disk.count(root_of(installed_package(0)->folder) + "/mods/" + expected + "/Map_Renamed_P.pak"));
    // Another key is another mod.
    char other[package_store::kModIdBytes];
    package_store::derived_id("gamebanana", "777", other);
    assert(strcmp(other, expected) && run(zip("cooked"), {.source = "gamebanana", .key = "777"}) == TSUCHI_OK && count() == 2);
    // Two containers cannot share the one ID.
    assert(run(zip("cooked_two"), {.source = "gamebanana", .key = "888"}) == TSUCHI_OK && last_finished().find(" 4 unsupported") != std::string::npos && count() == 2);
    // Removing a package whose container was mounted this session marks it and keeps its files.
    const uint32_t at = strcmp(package(0).mods, other) ? 1 : 0;
    const std::string mounted_here = installed_package(at)->folder;
    assert(service.remove(downloader, at) == TSUCHI_OK && count() == 1 && package_files(mounted_here.c_str()) && disk.count(root_of(mounted_here.c_str()) + "/removed"));
    // One whose container was not mounted goes at once.
    mounted_folders.clear();
    const std::string unmounted = installed_package(0)->folder;
    assert(service.remove(downloader, 0) == TSUCHI_OK && count() == 0 && !package_files(unmounted.c_str()));
}

void cheats_install() {
    reset(); add_capable("the.downloader");
    assert(run(zip("cheats"), {.source = "gamebanana", .key = "55"}) == TSUCHI_OK && last_finished().find(" 3 ") != std::string::npos);
    char id[package_store::kModIdBytes];
    package_store::derived_id("gamebanana", "55", id);
    assert(package(0).kind == TSUCHI_PACKAGE_KIND_CHEATS && !strcmp(package(0).mods, id));
    const std::string base = root_of(installed_package(0)->folder) + "/mods/" + id;
    assert(disk.count(base + "/cheats/060F827BD2B38703.txt") && disk.count(base + "/mod.json"));
}

void owner_stopping_cancels() {
    reset(); add_capable("the.downloader");
    TSUCHI_Mod* brief = mod_create("brief.downloader");
    loaded("brief.downloader").capabilities = kModCapabilityPackages;
    TSUCHI_PackageInstall handle{};
    assert(service.install(brief, "https://example.com/a.zip", 10, "s", "k", "t", "", "", "", &handle) == TSUCHI_OK && queued);
    mod_stop(brief);
    assert(queued->cancelled.load());
    queued->state.store(https::HTTPS_CANCELLED); queued = nullptr;
    frame_steps();
    assert(package_folders() == 0 && !package_install_running());
}

void on_progress(void*, const TSUCHI_PackagesProgress* e) { progressed.push_back(*e); }
void on_finished(void*, const TSUCHI_PackagesFinished* e) {
    finished.push_back(std::to_string(e->install.id) + " " + std::to_string(uint32_t(e->phase)) + " " + e->failure);
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    folder_of_zips = argv[1];
    set_allocator({std::malloc, std::free});
    note_game_thread();
    delete_tree = remove_tree;
    t::fn::fs_open_file = open_file; t::fn::fs_close_file = close_file; t::fn::fs_create_file = make_file; t::fn::fs_create_folder = make_dir;
    t::fn::fs_entry_kind = kind; t::fn::fs_file_size = size_file; t::fn::fs_set_file_size = resize_file; t::fn::fs_write_file = write_file_fn;
    t::fn::fs_read_file = read_file_fn; t::fn::fs_delete_file = delete_file;
    packages_set_clock(fixed_clock);
    packages_start();
    downloader = mod_create("the.downloader");
    plain_mod = mod_create("plain.mod");
    TSUCHI_Subscription one{}, two{};
    assert(service.on_progress(downloader, on_progress, nullptr, &one) == TSUCHI_OK && service.on_finished(downloader, on_finished, nullptr, &two) == TSUCHI_OK);
    std::printf("installs_and_lists\n"); std::fflush(stdout); installs_and_lists();
    std::printf("capability_and_arguments\n"); std::fflush(stdout); capability_and_arguments();
    std::printf("failures_leave_nothing\n"); std::fflush(stdout); failures_leave_nothing();
    std::printf("updates_replace\n"); std::fflush(stdout); updates_replace();
    std::printf("updates_in_use_wait_for_start\n"); std::fflush(stdout); updates_in_use_wait_for_start();
    std::printf("replacing_must_match\n"); std::fflush(stdout); replacing_must_match();
    std::printf("removes\n"); std::fflush(stdout); removes();
    std::printf("start_cleans\n"); std::fflush(stdout); start_cleans();
    std::printf("too_many_lists_none\n"); std::fflush(stdout); too_many_lists_none();
    std::printf("cooked_ids_come_from_source_and_key\n"); std::fflush(stdout); cooked_ids_come_from_source_and_key();
    std::printf("cheats_install\n"); std::fflush(stdout); cheats_install();
    std::printf("owner_stopping_cancels\n"); std::fflush(stdout); owner_stopping_cancels();
    records.clear(); listed.clear();
    std::puts("packages ok");
    return 0;
}
