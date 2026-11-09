#include "mod_choices.h"
// Checks of mounting mods' containers, against a made-up game: its files, and an engine that records
// what it is asked to mount. The containers are made here, in the layout the engine's tools write.
// Exits with the number of failures.
//
// With `--read <.utoc> [<.ucas>]` it prints what the framework reads of a real container instead.
// With `--admit <.pak> <.utoc> <.ucas> <package-root>` it checks a real trio against an empty
// stand-in game census. This validates admission only; the recorded mount is not a game result.
// With `--claims <.utoc> <.ucas> [header-sha256]` it reads a bounded startup TOC and only the
// UCAS spans needed for its header. It never loads the complete UCAS into memory.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "containers.h"
#include "files.h"
#include "mods.h"
#include "registry.h"
#include "sha1.h"
#include "sha256.h"

namespace store_fixture {
uint64_t row[2] = {}, entries[2][4] = {};
bool active = false;
unsigned mounts = 0;
bool contains(uint64_t at, uint64_t bytes, const void* base, uint64_t size) {
    const uint64_t first = reinterpret_cast<uint64_t>(base);
    return active && at >= first && at - first <= size && bytes <= size - (at - first);
}
}

// The stand-in exposes only the package-store row and entries used by this regression.
namespace tsuchi::game {
uint64_t main_base() { return 0; }
bool readable(uint64_t at, uint64_t bytes) {
    return store_fixture::contains(at, bytes, store_fixture::row, sizeof store_fixture::row) ||
        store_fixture::contains(at, bytes, store_fixture::entries, sizeof store_fixture::entries);
}
bool writable(uint64_t at, uint64_t bytes) {
    return store_fixture::contains(at, bytes, store_fixture::row, sizeof store_fixture::row);
}
bool executable(uint64_t) { return false; }
} // namespace tsuchi::game

namespace {

namespace t = tsuchi::game::table;

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

// ---- The made-up game's files ------------------------------------------------------------------

std::map<std::string, std::string> files;
std::vector<std::string> opened;
std::vector<std::vector<std::pair<std::string, bool>>> listings; // names, and whether each is a folder
std::vector<size_t> next_entry;
uint32_t refuse_open = 0;  // what opening a folder gives, when not 0
int refuse_read_after = -1; // a folder read fails after this many entries
uint64_t bytes_read = 0, read_calls = 0;

uint32_t open_file(void* out, void* path, int32_t) {
    const auto found = files.find(static_cast<const char*>(path));
    if (found == files.end()) return t::constant::fs_no_such_path;
    opened.push_back(found->first);
    *static_cast<uint64_t*>(out) = opened.size();
    return 0;
}
void close_file(void*) {}
uint32_t file_size(void* out, void* file) {
    *static_cast<int64_t*>(out) = int64_t(files[opened[reinterpret_cast<uint64_t>(file) - 1]].size());
    return 0;
}
uint32_t read_file(void* got, void* file, int64_t offset, void* buffer, uint64_t size) {
    const std::string& bytes = files[opened[reinterpret_cast<uint64_t>(file) - 1]];
    if (offset < 0 || uint64_t(offset) + size > bytes.size()) return 9;
    std::memcpy(buffer, bytes.data() + offset, size);
    bytes_read += size; ++read_calls;
    *static_cast<uint64_t*>(got) = size;
    return 0;
}
uint32_t open_folder(void* out, void* path, int32_t) {
    const std::string prefix = std::string(static_cast<const char*>(path)) + "/";
    std::vector<std::pair<std::string, bool>> entries;
    for (const auto& [where, bytes] : files) {
        if (where.compare(0, prefix.size(), prefix)) continue;
        const std::string rest = where.substr(prefix.size());
        const size_t slash = rest.find('/');
        const std::pair<std::string, bool> entry{rest.substr(0, slash), slash != std::string::npos};
        bool seen = false;
        for (const auto& had : entries) seen = seen || had == entry;
        if (!seen) entries.push_back(entry);
    }
    if (refuse_open) return refuse_open;
    if (entries.empty()) return t::constant::fs_no_such_path;
    listings.push_back(entries), next_entry.push_back(0);
    *static_cast<uint64_t*>(out) = listings.size();
    return 0;
}
uint32_t read_folder(void* got, void* entries, void* folder, int64_t) {
    const size_t which = reinterpret_cast<uint64_t>(folder) - 1;
    *static_cast<int64_t*>(got) = 0;
    if (refuse_read_after >= 0 && next_entry[which] >= size_t(refuse_read_after)) return 0x2EE202;
    if (next_entry[which] >= listings[which].size()) return 0;
    const auto& [name, folder_kind] = listings[which][next_entry[which]++];
    auto* entry = static_cast<uint8_t*>(entries);
    std::memset(entry, 0, t::fs_entry::size);
    CHECK(t::fs_entry::name + name.size() < t::fs_entry::bytes); // the name has to fit before the entry's next field
    if (t::fs_entry::name + name.size() >= t::fs_entry::bytes) return 9;
    std::memcpy(entry + t::fs_entry::name, name.c_str(), name.size() + 1);
    entry[t::fs_entry::kind] = uint8_t(folder_kind ? t::constant::fs_entry_folder : t::constant::fs_entry_folder + 1);
    *static_cast<int64_t*>(got) = 1;
    return 0;
}
void close_folder(void*) {}

// ---- The made-up engine's layer of containers ---------------------------------------------------

struct Mount {
    std::string path;
    uint32_t order;
};
std::vector<Mount> mounts;
int manager = 0, layer = 0;
bool refuse_mounts = false;
std::string reenter_bundle;
TSUCHI_Result reenter_result = TSUCHI_OK;
void* platform_files() { return &manager; }
void* find_platform_file(void* keeper, const char16_t* name) {
    const std::u16string wanted = u"Packs";
    return keeper == &manager && name == wanted ? &layer : nullptr;
}
bool pak_mount(void* on, const char16_t* path, uint32_t order, const char16_t* mount_point, bool read_index) {
    std::string narrow;
    for (const char16_t* at = path; *at; ++at) narrow += char(*at);
    if (on != &layer || mount_point || !read_index || refuse_mounts) return false;
    if (store_fixture::active) {
        const uint64_t entry = reinterpret_cast<uint64_t>(store_fixture::entries[store_fixture::mounts++]);
        tsuchi::refresh_mounted_package_entry(reinterpret_cast<uint64_t>(&store_fixture::row[1]), entry);
    }
    if (!reenter_bundle.empty()) {
        const std::string path = "sd:/tsuchi/bundles/" + reenter_bundle;
        reenter_bundle.clear();
        reenter_result = tsuchi::mount_asset_container("app.owner", (path + ".pak").c_str(),
            (path + ".utoc").c_str(), (path + ".ucas").c_str(), "/Game/MyMeshes");
    }
    mounts.push_back({narrow, order});
    return true;
}

// The made-up engine's libraries of shaders: what it was asked to open, each as `<folder>|<name>`, and when,
// as the number of containers mounted by then. The folder and name come as the engine's strings.
struct EngineString {
    const char16_t* data;
    int32_t count, room;
};
std::vector<std::pair<std::string, size_t>> libraries_opened;
bool refuse_libraries = false;
std::string narrow_string(const void* string) {
    const auto* engine = static_cast<const EngineString*>(string);
    std::string narrow;
    for (int32_t i = 0; i + 1 < engine->count; ++i) narrow += char(engine->data[i]);
    return engine->count >= 1 && engine->room >= engine->count && engine->data[engine->count - 1] == 0 ? narrow : "<not a string>";
}
bool shader_library_open(void* name, void* folder) {
    if (refuse_libraries) return false;
    libraries_opened.push_back({narrow_string(folder) + "|" + narrow_string(name), mounts.size()});
    return true;
}

// ---- The log --------------------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> lines;
void sink(const char* id, const char* text) { lines.emplace_back(id, text); }
bool told(const std::string& id, const std::string& part) {
    for (const auto& [who, text] : lines)
        if (who == id && text.find(part) != std::string::npos) return true;
    return false;
}
size_t lines_of(const std::string& id) {
    size_t count = 0;
    for (const auto& line : lines) count += line.first == id;
    return count;
}

// ---- Containers, as the engine's tools write them ------------------------------------------------

void put32(std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); }
void put64(std::string& out, uint64_t value) { out.append(reinterpret_cast<const char*>(&value), 8); }
void put_string(std::string& out, const std::string& text) {
    put32(out, uint32_t(text.size() + 1));
    out += text, out += '\0';
}

struct Package {
    uint64_t id;
    std::string file; // below the mount point, such as `Carrot4/Content/Mods/x/T_Leaf.uasset`
};
struct Made {
    std::string toc, cas;
};

// The container's header: its ID, the packages, and the redirects.
std::string make_header(uint64_t id, const std::vector<Package>& packages, const std::vector<std::pair<uint64_t, uint64_t>>& redirects,
                        bool culture = true) {
    std::string out;
    put64(out, id), put32(out, uint32_t(packages.size()));
    put32(out, 3), out += "abc"; // names
    put32(out, 0);               // their hashes
    put32(out, uint32_t(packages.size()));
    for (const Package& package : packages) put64(out, package.id);
    put32(out, uint32_t(32 * packages.size()));
    out.append(32 * packages.size(), '\0');
    put32(out, culture ? 1 : 0);
    if (culture) {
        put_string(out, "en");
        put32(out, 1), put64(out, 1), put64(out, 2);
    }
    put32(out, uint32_t(redirects.size()));
    for (const auto& [from, to] : redirects) put64(out, from), put64(out, to);
    return out;
}

// The index of files: folders by their names, each file under its folder, with its chunk's place.
std::string make_index(const std::string& mount, const std::vector<Package>& packages) {
    std::vector<std::string> strings;
    const auto string_number = [&](const std::string& text) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == text) return uint32_t(i);
        strings.push_back(text);
        return uint32_t(strings.size() - 1);
    };
    struct Folder { uint32_t name, child, sibling, file; };
    struct File { uint32_t name, next, entry; };
    std::vector<Folder> folders{{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}};
    std::vector<File> made;
    for (size_t p = 0; p < packages.size(); ++p) {
        uint32_t folder = 0;
        std::string rest = packages[p].file;
        for (size_t slash; (slash = rest.find('/')) != std::string::npos; rest = rest.substr(slash + 1)) {
            const uint32_t name = string_number(rest.substr(0, slash));
            uint32_t found = 0xFFFFFFFF;
            for (uint32_t c = folders[folder].child; c != 0xFFFFFFFF; c = folders[c].sibling)
                if (folders[c].name == name) found = c;
            if (found == 0xFFFFFFFF) {
                folders.push_back({name, 0xFFFFFFFF, folders[folder].child, 0xFFFFFFFF});
                found = uint32_t(folders.size() - 1);
                folders[folder].child = found;
            }
            folder = found;
        }
        made.push_back({string_number(rest), folders[folder].file, uint32_t(p + 1)});
        folders[folder].file = uint32_t(made.size() - 1);
    }
    std::string out;
    put_string(out, mount);
    put32(out, uint32_t(folders.size()));
    for (const Folder& f : folders) put32(out, f.name), put32(out, f.child), put32(out, f.sibling), put32(out, f.file);
    put32(out, uint32_t(made.size()));
    for (const File& f : made) put32(out, f.name), put32(out, f.next), put32(out, f.entry);
    put32(out, uint32_t(strings.size()));
    for (const std::string& text : strings) put_string(out, text);
    return out;
}

// A whole container: entry 0 is its header, and the packages follow, each in a block of its own.
Made make_container(uint64_t id, const std::vector<Package>& packages, const std::vector<std::pair<uint64_t, uint64_t>>& redirects = {},
                    bool compressed_header = false, uint8_t version = 2, bool culture = true,
                    bool pad_header = false, uint32_t extra_header_bytes = 0) {
    constexpr uint32_t block = 0x10000;
    std::vector<std::string> chunks{make_header(id, packages, redirects, culture)};
    if (pad_header) chunks[0].resize((chunks[0].size() + 15) & ~size_t(15), '\0');
    chunks[0].append(extra_header_bytes, '\0');
    for (const Package& package : packages) chunks.push_back("package " + std::to_string(package.id));
    const std::string index = make_index("../../../", packages);
    std::string toc(144, '\0');
    std::memcpy(&toc[0], "-==--==--==--==-", 16);
    toc[16] = char(version);
    const uint32_t numbers[] = {144, uint32_t(chunks.size()), uint32_t(chunks.size()), 12, compressed_header ? 1u : 0u, 32, block, uint32_t(index.size())};
    std::memcpy(&toc[20], numbers, sizeof numbers);
    std::memcpy(&toc[56], &id, 8);
    for (size_t i = 0; i < chunks.size(); ++i) { // the chunks' IDs
        const uint64_t owner = i ? packages[i - 1].id : id;
        put64(toc, owner), toc += '\0', toc += '\0', toc += '\0', toc += char(i ? 2 : 10);
    }
    std::string cas;
    std::vector<uint64_t> at;
    for (const std::string& chunk : chunks) {
        at.push_back(cas.size());
        cas += chunk;
        cas.resize((cas.size() + block - 1) / block * block, '\0');
    }
    for (size_t i = 0; i < chunks.size(); ++i) { // where each one is, in big-endian five-byte numbers
        const uint64_t offset = uint64_t(i) * block, length = chunks[i].size();
        for (int b = 4; b >= 0; --b) toc += char((offset >> (8 * b)) & 0xFF);
        for (int b = 4; b >= 0; --b) toc += char((length >> (8 * b)) & 0xFF);
    }
    for (size_t i = 0; i < chunks.size(); ++i) { // the blocks
        const uint64_t size = chunks[i].size();
        for (int b = 0; b < 5; ++b) toc += char((at[i] >> (8 * b)) & 0xFF);
        for (int b = 0; b < 3; ++b) toc += char((size >> (8 * b)) & 0xFF);
        for (int b = 0; b < 3; ++b) toc += char((size >> (8 * b)) & 0xFF);
        toc += char(i == 0 && compressed_header ? 1 : 0);
    }
    if (compressed_header) toc += std::string("Zlib") + std::string(28, '\0');
    toc += index;
    toc.append(33 * chunks.size(), '\0');
    return {toc, cas};
}

// RFC1950 with a stored RFC1951 block: no host compression library is needed by this fixture.
std::string stored_zlib(const std::string& plain) {
    std::string out("\x78\x01\x01", 3);
    const uint16_t size = uint16_t(plain.size()), inverse = uint16_t(~size);
    out.append(reinterpret_cast<const char*>(&size), 2);
    out.append(reinterpret_cast<const char*>(&inverse), 2);
    out += plain;
    uint32_t a = 1, b = 0;
    for (uint8_t byte : plain) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = b << 16 | a;
    for (int i = 3; i >= 0; --i) out += char(adler >> (8 * i));
    return out;
}

// The header starts seven bytes into a block and ends before a suffix. Block 1 is Zlib;
// other blocks are plain. The package is a separate final block outside the header span.
Made mixed_header(uint64_t id, uint64_t package, const std::string& path = "Carrot4/Content/MyMeshes/SM_Leaf.uasset",
                  bool block_pad = false, uint32_t extra_blocks = 0) {
    constexpr uint32_t block = 32;
    const std::vector<Package> packages{{package, path}};
    std::string header = make_header(id, packages, {}, false);
    if (block_pad) header.resize((header.size() + block - 1) / block * block, '\0');
    header.append(extra_blocks * block, '\0');
    const std::string logical = std::string(7, 'x') + header + std::string(13, 'y');
    std::vector<std::string> plain, stored;
    for (size_t at = 0; at < logical.size(); at += block) plain.push_back(logical.substr(at, block));
    const uint64_t package_at = plain.size() * block;
    plain.push_back("package");
    std::vector<uint64_t> offsets;
    std::string cas;
    for (size_t i = 0; i < plain.size(); ++i) {
        stored.push_back(i == 1 ? stored_zlib(plain[i]) : plain[i]);
        offsets.push_back(cas.size()); cas += stored.back();
    }
    const std::string index = make_index("../../../", packages);
    std::string toc(144, '\0');
    std::memcpy(&toc[0], "-==--==--==--==-", 16); toc[16] = 2;
    const uint32_t numbers[] = {144, 2, uint32_t(plain.size()), 12, 1, 32, block, uint32_t(index.size())};
    std::memcpy(&toc[20], numbers, sizeof numbers); std::memcpy(&toc[56], &id, 8);
    put64(toc, id), toc.append(3, '\0'), toc += char(10);
    put64(toc, package), toc.append(3, '\0'), toc += char(2);
    for (const auto& span : {std::pair<uint64_t,uint64_t>{7, header.size()}, {package_at, 7}}) {
        for (int i = 4; i >= 0; --i) toc += char(span.first >> (8 * i));
        for (int i = 4; i >= 0; --i) toc += char(span.second >> (8 * i));
    }
    for (size_t i = 0; i < plain.size(); ++i) {
        for (unsigned j = 0; j < 5; ++j) toc += char(offsets[i] >> (8 * j));
        for (unsigned j = 0; j < 3; ++j) toc += char(stored[i].size() >> (8 * j));
        for (unsigned j = 0; j < 3; ++j) toc += char(plain[i].size() >> (8 * j));
        toc += char(i == 1 ? 1 : 0);
    }
    toc += std::string("Zlib") + std::string(28, '\0');
    toc += index; toc.append(66, '\0');
    return {toc, cas};
}

void install(const std::string& id, const std::string& manifest, const std::string& container = "", const Made& made = {}) {
    files["rom:/tsuchi/mods/" + id + "/mod.json"] = manifest;
    if (container.empty()) return;
    files["rom:/tsuchi/mods/" + id + "/" + container + ".pak"] = "pak";
    files["rom:/tsuchi/mods/" + id + "/" + container + ".utoc"] = made.toc;
    files["rom:/tsuchi/mods/" + id + "/" + container + ".ucas"] = made.cas;
}
std::string manifest(const std::string& id, const std::string& more = "") {
    return "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"1.0.0\"" + more + "}";
}

// Finds the mods and mounts their containers, as the framework does on the engine's first frame.
void begin() {
    lines.clear(), mounts.clear(), libraries_opened.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::mount_mod_containers();
}
tsuchi::ModRecord* record(const char* id) { return tsuchi::find_mod(id); }
bool blocked_with(const char* id, const std::string& part) {
    const tsuchi::ModRecord* found = record(id);
    const bool right = found && found->state == tsuchi::ModState::blocked && std::string(found->problem.get()).find(part) != std::string::npos;
    if (!right) std::printf("  %s: %s\n", id, found ? found->problem.get() : "<not found>");
    return right;
}

constexpr uint64_t kGameTable = 0x1111222233334444, kGameTexture = 0x5555666677778888;

void replacement_store_follows_mount_order() {
    files.clear();
    install("app.first", manifest("app.first"), "First", make_container(0xD0,
        {{kGameTexture, "Carrot4/Content/T_Leaf.uasset"}}));
    install("app.last", manifest("app.last"), "Last", make_container(0xD1,
        {{kGameTexture, "Carrot4/Content/T_Leaf.uasset"}}));
    store_fixture::active = true;
    store_fixture::mounts = 0;
    store_fixture::row[0] = kGameTexture;
    store_fixture::row[1] = 123; // the stock metadata, already registered
    const uint64_t slot = reinterpret_cast<uint64_t>(&store_fixture::row[1]);
    const uint64_t first = reinterpret_cast<uint64_t>(store_fixture::entries[0]);
    const uint64_t last = reinterpret_cast<uint64_t>(store_fixture::entries[1]);
    tsuchi::refresh_mounted_package_entry(slot, first);
    CHECK(store_fixture::row[1] == 123); // a callback outside a mount leaves stock alone
    begin();
    CHECK(store_fixture::mounts == 2);
    CHECK(store_fixture::row[1] == last); // both the earlier mod and stock are superseded
    tsuchi::refresh_mounted_package_entry(slot, first);
    CHECK(store_fixture::row[1] == last); // the synchronous mount scope has ended
    store_fixture::row[0] = 0xDEADBEEF; store_fixture::row[1] = 123;
    store_fixture::mounts = 0;
    begin();
    CHECK(store_fixture::row[1] == 123); // another package's callback is never adopted
    store_fixture::active = false;
}

void mounts_in_order() {
    files.clear();
    install("app.base", manifest("app.base"), "Base", make_container(0xB0, {{0xA1, "Carrot4/Content/Mods/base/T_Leaf.uasset"}}));
    install("app.needs", manifest("app.needs", ", \"requires\": {\"zz.last\": \"*\"}"), "Needs_P",
            make_container(0xB1, {{0xA2, "Carrot4/Content/Mods/needs/T_Stem.uasset"}}));
    install("zz.last", manifest("zz.last"), "Last_2_P", make_container(0xB2, {{0xA3, "Carrot4/Content/Mods/last/T_Bud.uasset"}}));
    install("app.plain", manifest("app.plain"));
    files["rom:/StandIn/Content/Paks/StandIn-Main.pak"] = "pak";
    files["rom:/StandIn/Content/Paks/StandIn-Main.utoc"] = make_container(0xC0, {}).toc;
    begin();
    // Dependencies first, by ID otherwise; the order the engine ends with goes up with each, above the game's.
    CHECK(mounts.size() == 3);
    if (mounts.size() == 3) {
        CHECK(mounts[0].path == "../../../tsuchi/mods/app.base/Base.pak" && mounts[0].order == 10000);
        CHECK(mounts[1].path == "../../../tsuchi/mods/zz.last/Last_2_P.pak" && mounts[1].order + 300 == 10001);
        CHECK(mounts[2].path == "../../../tsuchi/mods/app.needs/Needs_P.pak" && mounts[2].order + 100 == 10002);
    }
    CHECK(tsuchi::containers_mounted() == 3);
    CHECK(record("app.plain")->state == tsuchi::ModState::ready && record("app.needs")->state == tsuchi::ModState::ready);
    CHECK(lines.size() == 3);
    CHECK(told("app.base", "Mounted container Base.pak from this mod folder."));
    CHECK(told("zz.last", "Mounted container Last_2_P.pak from this mod folder."));
    CHECK(told("app.needs", "Mounted container Needs_P.pak from this mod folder."));
    CHECK(lines_of("app.plain") == 0);
}

const char* player_activation = "";
const char* player_order = "";
TSUCHI_Result player_choices(tsuchi::ModChoices& choices) { return choices.load(player_activation, player_order); }
void player_controls_startup() {
    files.clear();
    install("choice.base", manifest("choice.base"), "Base", make_container(0xB0, {{0xA1, "Carrot4/Content/Mods/shared/T_Leaf.uasset"}}));
    install("choice.last", manifest("choice.last"), "Last", make_container(0xB2, {{0xA1, "Carrot4/Content/Mods/shared/T_Leaf.uasset"}}));
    install("choice.needs", manifest("choice.needs", ", \"requires\": {\"choice.last\": \"*\"}"), "Needs", make_container(0xB1, {}));
    tsuchi::set_mod_choices_loader(player_choices);
    player_activation = "p4a1:choice.last:"; player_order = "p4o1:choice.needs:choice.last:choice.base:";
    begin();
    CHECK(record("choice.last")->state == tsuchi::ModState::disabled);
    CHECK(record("choice.needs")->state == tsuchi::ModState::blocked);
    CHECK(mounts.size() == 1 && mounts[0].path == "../../../tsuchi/mods/choice.base/Base.pak");
    CHECK(!tsuchi::container_mounted_for("choice.last") && !tsuchi::container_mounted_for("choice.needs"));
    player_activation = "p4a1:";
    begin();
    CHECK(mounts.size() == 3);
    CHECK(record("choice.last")->order < record("choice.needs")->order);
    CHECK(record("choice.last")->order < record("choice.base")->order);
    CHECK(mounts[2].path == "../../../tsuchi/mods/choice.base/Base.pak");
    // The later mounted replacement has the higher native package priority.
    CHECK(mounts[0].order < mounts[2].order);
    player_order = "p4o1:choice.base:choice.last:choice.needs:";
    begin();
    CHECK(mounts.size() == 3 && mounts[0].path == "../../../tsuchi/mods/choice.base/Base.pak" && mounts[1].path == "../../../tsuchi/mods/choice.last/Last.pak");
    CHECK(record("choice.last")->order < record("choice.needs")->order);
    CHECK(mounts[0].order < mounts[1].order);
    player_activation = player_order = "";
    CHECK(tsuchi::load_mod_choices() == TSUCHI_OK);
    tsuchi::set_mod_choices_loader(nullptr);
}

void orders_names_raise() {
    CHECK(tsuchi::container_order_raise("Mod.pak") == 0 && tsuchi::container_order_raise("P.pak") == 0);
    CHECK(tsuchi::container_order_raise("Mod_P.pak") == 100 && tsuchi::container_order_raise("My_Mod_P.pak") == 100);
    CHECK(tsuchi::container_order_raise("Mod_2_P.pak") == 300 && tsuchi::container_order_raise("Mod_1_p.PAK") == 200);
    CHECK(tsuchi::container_order_raise("Mod_P.pakk") == 0);
    // The part between the last two underscores counts only when it is all digits and at least 1.
    CHECK(tsuchi::container_order_raise("name_P.pak") == 100 && tsuchi::container_order_raise("name_2_P.pak") == 300);
    CHECK(tsuchi::container_order_raise("kadan_slug_2681c9bb_P.pak") == 100 && tsuchi::container_order_raise("kadan_slug_00ab_P.pak") == 100);
    CHECK(tsuchi::container_order_raise("x_0_P.pak") == 100 && tsuchi::container_order_raise("x__P.pak") == 100 && tsuchi::container_order_raise("_P.pak") == 100);
    CHECK(tsuchi::container_order_raise("x_0000000000000000000001_P.pak") == 200);
    CHECK(tsuchi::container_order_raise("x_99999999999999999999999999999999999999_P.pak") == 1000000000); // held, not wrapped
}

// A folder that cannot be read is not an empty folder, and a listing cut short is not a whole one: discovery
// fails, and no mod list is made from what was read.
void unreadable_folders_fail() {
    files.clear();
    install("app.a", manifest("app.a"));
    install("app.b", manifest("app.b"));
    install("app.c", manifest("app.c"));
    tsuchi::Texts names, folders;
    CHECK(tsuchi::game::list_folder("rom:/tsuchi/mods", names, folders) == TSUCHI_OK && folders.first() && folders.after(folders.first()));
    names.clear(), folders.clear();
    CHECK(tsuchi::game::list_folder("rom:/tsuchi/none", names, folders) == TSUCHI_NOT_FOUND);
    refuse_read_after = 1;
    CHECK(tsuchi::game::list_folder("rom:/tsuchi/mods", names, folders) == TSUCHI_IO);
    names.clear(), folders.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_IO && tsuchi::mod_count() == 0);
    refuse_read_after = -1;
    refuse_open = 0x2EE202;
    CHECK(tsuchi::game::list_folder("rom:/tsuchi/mods", names, folders) == TSUCHI_IO);
    names.clear(), folders.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_IO && tsuchi::mod_count() == 0);
    refuse_open = 0;
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 3);
    tsuchi::forget_mods();
}

void single_partition_version_three() {
    auto made = make_container(0xBAC, {{0xAA, "Carrot4/Content/Mods/leaf/T_A.uasset"}});
    made.toc[16] = 3;
    uint32_t partitions = 1;
    uint64_t partition_size = UINT64_MAX;
    std::memcpy(&made.toc[52], &partitions, 4);
    std::memcpy(&made.toc[88], &partition_size, 8);
    tsuchi::ContainerToc toc{}; const char* wrong = nullptr;
    CHECK(tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    const uint64_t data_end = toc.data_bytes;
    tsuchi::forget_container_toc(toc);
    for (uint32_t count : {0u, 2u}) {
        std::memcpy(&made.toc[52], &count, 4);
        CHECK(!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    }
    std::memcpy(&made.toc[52], &partitions, 4);
    for (uint64_t size : {uint64_t(0), uint64_t(1), data_end - 1}) {
        std::memcpy(&made.toc[88], &size, 8);
        CHECK(!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    }
    std::memcpy(&made.toc[88], &data_end, 8);
    CHECK(tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    tsuchi::forget_container_toc(toc);
}

void source_header_id_in_startup_patch() {
    const auto made = make_container(0xBAC, {{0xAA, "Carrot4/Content/Mods/leaf/T_A.uasset"}});
    auto data = made.cas;
    const uint64_t source_id = 0x123456;
    std::memcpy(&data[0], &source_id, 8);
    files["sd:/header-id.ucas"] = data;
    tsuchi::ContainerToc toc{}; const char* wrong = nullptr;
    CHECK(tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    tsuchi::List<uint64_t> claims;
    CHECK(tsuchi::read_startup_container_claims(toc, "sd:/header-id.ucas", claims) == TSUCHI_OK && claims.count >= 1);
    claims.clear();
    // A source ID never excuses a header whose package set differs from its TOC.
    const uint32_t wrong_count = 2;
    std::memcpy(&data[8], &wrong_count, 4);
    files["sd:/header-id.ucas"] = data;
    CHECK(tsuchi::read_startup_container_claims(toc, "sd:/header-id.ucas", claims) == TSUCHI_INVALID);
    claims.clear(); tsuchi::forget_container_toc(toc);
    files.erase("sd:/header-id.ucas");
}

void blocked_mods_mount_nothing() {
    files.clear();
    install("app.lost", manifest("app.lost", ", \"requires\": {\"app.gone\": \"*\"}"), "Lost", make_container(0xB3, {{0xA4, "Carrot4/Content/Mods/lost/T_A.uasset"}}));
    install("app.broken", "{\"id\": \"app.broken\"", "Broken", make_container(0xB4, {{0xA5, "Carrot4/Content/Mods/broken/T_A.uasset"}}));
    install("app.half", manifest("app.half"), "Half", make_container(0xB5, {{0xA6, "Carrot4/Content/Mods/half/T_A.uasset"}}));
    files.erase("rom:/tsuchi/mods/app.half/Half.ucas");
    install("app.after", manifest("app.after", ", \"requires\": {\"app.half\": \"1.0.0\"}"), "After",
            make_container(0xB6, {{0xA7, "Carrot4/Content/Mods/after/T_A.uasset"}}));
    install("app.two", manifest("app.two"), "One", make_container(0xB7, {}));
    files["rom:/tsuchi/mods/app.two/Two.pak"] = "pak";
    install("app.loose", manifest("app.loose"));
    files["rom:/tsuchi/mods/app.loose/Loose.utoc"] = make_container(0xB8, {}).toc;
    install("app.odd", manifest("app.odd"), "Odd name", make_container(0xB9, {}));
    install("app.old", manifest("app.old"), "Old", make_container(0xBA, {}, {}, false, 4));
    begin();
    CHECK(mounts.empty());
    CHECK(blocked_with("app.lost", "app.gone") && blocked_with("app.broken", "mod.json"));
    CHECK(blocked_with("app.half", "its container Half.pak needs Half.ucas"));
    CHECK(blocked_with("app.after", "it needs app.half, which cannot run")); // what needs a blocked mod is blocked before its turn
    CHECK(blocked_with("app.two", "more than one container"));
    CHECK(blocked_with("app.loose", "Loose.utoc has no .pak"));
    CHECK(blocked_with("app.odd", "its container's name"));
    CHECK(blocked_with("app.old", "another version of the engine"));
    // Mods start in their places with the gaps the blocked ones left.
    files.clear();
    install("app.a", manifest("app.a"));
    install("app.b", manifest("app.b"), "B", make_container(0xBB, {}));
    install("app.c", manifest("app.c"));
    refuse_mounts = true;
    begin();
    refuse_mounts = false;
    CHECK(blocked_with("app.b", "the game could not mount its container B.pak"));
    CHECK(!told("app.b", "Mounted container"));
    tsuchi::start_mods();
    CHECK(record("app.a")->state == tsuchi::ModState::running && record("app.c")->state == tsuchi::ModState::running);
    tsuchi::forget_mods();
}

// A `.pak` as the engine's tools write one of version 10 or 11, with nothing in it but its index: the mount point,
// and the folders with the names of their files. `version` below 10 writes the old index with a count of files.
using PakFolders = std::vector<std::pair<std::string, std::vector<std::string>>>;
std::string pak_string_bytes(const std::string& text) {
    std::string out;
    const uint32_t length = uint32_t(text.size() + 1);
    out.append(reinterpret_cast<const char*>(&length), 4);
    out += text, out += '\0';
    return out;
}
template <typename T> void put(std::string& out, T value) { out.append(reinterpret_cast<const char*>(&value), sizeof value); }
std::string make_pak(const std::string& mount, const PakFolders& folders, uint32_t version = 11) {
    std::string directory, primary, pak;
    uint32_t files = 0;
    put(directory, uint32_t(folders.size()));
    for (const auto& [folder, names] : folders) {
        directory += pak_string_bytes(folder);
        put(directory, uint32_t(names.size()));
        for (const std::string& name : names) directory += pak_string_bytes(name), put(directory, int32_t(files++));
    }
    uint8_t hash[20];
    primary += pak_string_bytes(mount);
    put(primary, files);
    if (version >= 10) {
        put(primary, uint64_t(0)); // the path hash seed
        put(primary, uint32_t(0)); // no index of hashed paths
        put(primary, uint32_t(1)), put(primary, uint64_t(0)), put(primary, uint64_t(directory.size()));
        tsuchi::sha1(reinterpret_cast<const uint8_t*>(directory.data()), uint32_t(directory.size()), hash);
        primary.append(reinterpret_cast<const char*>(hash), 20);
        put(primary, uint32_t(0)), put(primary, uint32_t(0)); // no encoded or plain entries
        pak = directory;
    }
    const uint64_t primary_at = pak.size();
    pak += primary;
    std::string footer(17, '\0');
    put(footer, uint32_t(0x5A6F12E1)), put(footer, version), put(footer, primary_at), put(footer, uint64_t(primary.size()));
    tsuchi::sha1(reinterpret_cast<const uint8_t*>(primary.data()), uint32_t(primary.size()), hash);
    footer.append(reinterpret_cast<const char*>(hash), 20);
    footer.append(160, '\0');
    return pak + footer;
}

// A mod's shader library is opened from its container's `.pak` right after the container is mounted, by its name
// and the folder the pak puts it in.
void libraries_are_opened() {
    const std::string format = "-StandInShaders.ushaderbytecode";
    files.clear();
    install("lib.a", manifest("lib.a"), "A_P", make_container(0xD0, {}));
    files["rom:/tsuchi/mods/lib.a/A_P.pak"] = make_pak("../../../", {{"/", {}}, {"tsuchi/", {}}, {"tsuchi/mods/lib.a/",
        {"ShaderArchive-kadan_0123456789abcdef" + format, "ShaderArchive-Other-GLSL_ELSE.ushaderbytecode", "notes.txt"}}});
    install("lib.b", manifest("lib.b"), "B", make_container(0xD1, {}));
    files["rom:/tsuchi/mods/lib.b/B.pak"] = make_pak("../../../Mods/b", {{"/", {"ShaderArchive-Own_2" + format}}});
    install("lib.loose", manifest("lib.loose"), "Loose", make_container(0xD2, {})); // the engine reads no library loose
    files["rom:/tsuchi/mods/lib.loose/ShaderArchive-loose" + format] = "library";
    install("lib.old", manifest("lib.old"), "Old", make_container(0xD3, {}));
    files["rom:/tsuchi/mods/lib.old/Old.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-old" + format}}}, 8);
    install("lib.none", manifest("lib.none"), "None", make_container(0xD4, {}));
    files["rom:/tsuchi/mods/lib.none/None.pak"] = make_pak("../../../", {{"/", {}}});
    begin();
    CHECK(libraries_opened.size() == 2);
    if (libraries_opened.size() == 2) {
        CHECK(libraries_opened[0].first == "../../../tsuchi/mods/lib.a/|kadan_0123456789abcdef" && libraries_opened[0].second == 1);
        CHECK(libraries_opened[1].first == "../../../Mods/b/|Own_2" && libraries_opened[1].second == 2);
    }
    CHECK(told("lib.a", "Opened shader library ShaderArchive-kadan_0123456789abcdef" + format + " in A_P.pak"));
    CHECK(told("lib.b", "Opened shader library ShaderArchive-Own_2" + format + " in B.pak"));
    CHECK(told("lib.old", "from before the engine's tools listed its folders"));
    for (const char* id : {"lib.a", "lib.b", "lib.loose", "lib.old", "lib.none"}) CHECK(record(id)->state == tsuchi::ModState::ready);
    CHECK(!told("lib.loose", "shader library") && !told("lib.none", "shader library"));
    tsuchi::forget_mods();

    // Refused: two libraries, a name of other letters or too long, a name an earlier mod's library has, and one the
    // engine does not open. A refused library blocks its mod before its container is mounted.
    files.clear();
    install("lib.first", manifest("lib.first"), "First", make_container(0xD5, {}));
    files["rom:/tsuchi/mods/lib.first/First.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-same" + format}}});
    install("lib.same", manifest("lib.same"), "Same", make_container(0xD6, {}));
    files["rom:/tsuchi/mods/lib.same/Same.pak"] = make_pak("../../../", {{"x/", {"ShaderArchive-same" + format}}});
    install("lib.two", manifest("lib.two"), "Two", make_container(0xD7, {}));
    files["rom:/tsuchi/mods/lib.two/Two.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-one" + format}}, {"y/", {"ShaderArchive-two" + format}}});
    install("lib.odd", manifest("lib.odd"), "Odd", make_container(0xD8, {}));
    files["rom:/tsuchi/mods/lib.odd/Odd.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-odd-name" + format}}});
    install("lib.long", manifest("lib.long"), "Long", make_container(0xD9, {}));
    files["rom:/tsuchi/mods/lib.long/Long.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-" + std::string(64, 'x') + format}}});
    begin();
    CHECK(libraries_opened.size() == 1 && libraries_opened[0].first == "../../../|same");
    CHECK(blocked_with("lib.same", "its shader library has the name of lib.first's: ShaderArchive-same"));
    CHECK(blocked_with("lib.two", "more than one shader library: ShaderArchive-one" + format + " and ShaderArchive-two"));
    CHECK(blocked_with("lib.odd", "its shader library's name"));
    CHECK(blocked_with("lib.long", "its shader library's name"));
    CHECK(mounts.size() == 1);
    tsuchi::forget_mods();
    files.clear();
    install("lib.refused", manifest("lib.refused"), "Refused", make_container(0xDA, {}));
    files["rom:/tsuchi/mods/lib.refused/Refused.pak"] = make_pak("../../../", {{"/", {"ShaderArchive-refused" + format}}});
    refuse_libraries = true;
    begin();
    refuse_libraries = false;
    CHECK(mounts.size() == 1 && libraries_opened.empty());
    CHECK(blocked_with("lib.refused", "the game could not open its shader library ShaderArchive-refused" + format + " in Refused.pak"));
    tsuchi::forget_mods();
}

// A mod blocked as it starts keeps its container, and its log says so.
TSUCHI_Result refuse_to_start(TSUCHI_Mod*, tsuchi::Text&) { return TSUCHI_IO; }
void blocked_as_it_starts() {
    files.clear();
    CHECK(tsuchi::add_mod_runtime({"refuse.bin", refuse_to_start}));
    install("app.late", manifest("app.late"), "Late", make_container(0xBC, {}));
    files["rom:/tsuchi/mods/app.late/refuse.bin"] = "x";
    install("app.fine", manifest("app.fine"), "Fine", make_container(0xBD, {}));
    begin();
    CHECK(mounts.size() == 2);
    tsuchi::start_mods();
    tsuchi::tell_containers_left_mounted();
    CHECK(blocked_with("app.late", "refuse.bin") && told("app.late", "its container stays mounted until the game is closed"));
    CHECK(record("app.fine")->state == tsuchi::ModState::running && !told("app.fine", "stays mounted"));
    tsuchi::forget_mods();
}

void names_are_told_apart() {
    files.clear();
    files["rom:/StandIn/Content/Paks/StandIn-Main.pak"] = "pak";
    files["rom:/StandIn/Content/Paks/StandIn-Main.utoc"] = make_container(0xC0, {}).toc;
    files["rom:/StandIn/Content/Paks/Settings_P.pak"] = "pak";
    files["rom:/StandIn/Content/Paks/Settings_P.utoc"] = make_container(0xC1, {}).toc;
    files["rom:/StandIn/Content/Paks/Settings_P.ucas"] = "";
    install("app.same", manifest("app.same"), "Settings_P", make_container(0xC1, {}));
    install("app.first", manifest("app.first"), "Shared", make_container(0xD0, {}));
    install("app.second", manifest("app.second"), "Shared", make_container(0xD0, {}));
    install("app.game", manifest("app.game"), "StandIn-Main", make_container(0xC0, {}));
    begin();
    // A mod's container left in the game's folder is noted, and the game's own is not.
    CHECK(told("tsuchi", "StandIn/Content/Paks/Settings_P.pak belongs to a mod") && !told("tsuchi", "StandIn-Main"));
    CHECK(blocked_with("app.same", "StandIn/Content/Paks") && blocked_with("app.game", "StandIn/Content/Paks"));
    CHECK(record("app.first")->state == tsuchi::ModState::ready && blocked_with("app.second", "the name of app.first's"));
    CHECK(mounts.size() == 1);
}

void common_packages_are_told() {
    files.clear();
    const std::string table = "Carrot4/Content/Carrot4/Core/DT_Creatures.uasset";
    // Two mods put their own table in the game's place; a third moves its copy to an ID of its own and redirects;
    // a fourth has the same texture as the third; a fifth has nothing in common with any.
    install("app.one", manifest("app.one"), "One", make_container(0xE1, {{kGameTable, table}}));
    install("app.two", manifest("app.two"), "Two", make_container(0xE2, {{kGameTable, table}}));
    install("app.three", manifest("app.three"), "Three",
            make_container(0xE3, {{0x9999, table}, {kGameTexture, "Carrot4/Content/Carrot4/UI/T_Icon.uasset"}}, {{kGameTable, 0x9999}, {0x42, 0x42}}));
    install("app.zfour", manifest("app.zfour"), "Four", make_container(0xE4, {{kGameTexture, "Engine/Content/T_Icon.uasset"}}, {}, true));
    install("app.five", manifest("app.five"), "Five", make_container(0xE5, {{0x77, "Carrot4/Content/Mods/five/T_Leaf.uasset"}}));
    begin();
    CHECK(mounts.size() == 5); // by ID: five, one, three, two, zfour
    CHECK(told("app.one", "/Game/Carrot4/Core/DT_Creatures is in app.two's container too, and the game uses app.two's, as it starts later"));
    CHECK(told("app.two", "/Game/Carrot4/Core/DT_Creatures is in app.one's container too, and the game uses this mod's, as it starts later"));
    CHECK(told("app.three", "/Game/Carrot4/Core/DT_Creatures is in app.two's container too, and the game uses app.two's"));
    CHECK(told("app.two", "/Game/Carrot4/Core/DT_Creatures is in app.three's container too, and the game uses this mod's"));
    CHECK(told("app.three", "/Game/Carrot4/Core/DT_Creatures is in app.one's container too, and the game uses this mod's"));
    CHECK(told("app.zfour", "/Engine/T_Icon is in app.three's container too, and the game uses this mod's"));
    CHECK(told("app.three", "/Engine/T_Icon is in app.zfour's container too, and the game uses app.zfour's"));
    CHECK(lines_of("app.five") == 1 && lines_of("app.one") == 3 && lines_of("app.three") == 4 && lines_of("app.two") == 3 && lines_of("app.zfour") == 2);
    // Past ten packages in common, the rest are counted.
    files.clear();
    std::vector<Package> many;
    for (uint64_t i = 0; i < 13; ++i) many.push_back({0x500 + i, "Carrot4/Content/Mods/many/T_" + std::to_string(i) + ".uasset"});
    install("app.many", manifest("app.many"), "Many", make_container(0xF1, many));
    install("app.more", manifest("app.more"), "More", make_container(0xF2, many));
    begin();
    CHECK(lines_of("app.many") == 12 && told("app.many", "and 3 more packages are in both this mod's container and app.more"));
    CHECK(told("app.more", "/Game/Mods/many/T_12 is in app.many's container too") || told("app.more", "/Game/Mods/many/T_9 is in"));
}

void reads_a_table_of_contents() {
    const Made made = make_container(0xAB, {{0x10, "Carrot4/Content/Mods/a/T_A.uasset"}, {0x20, "Carrot4/Content/Mods/b/C/T_B.uasset"}},
                                     {{0x30, 0x20}});
    tsuchi::ContainerToc toc;
    const char* wrong = nullptr;
    CHECK(tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
    CHECK(toc.id == 0xAB && toc.packages.count == 2 && toc.header_at == 0 && toc.header_bytes > 0);
    tsuchi::Texts path;
    CHECK(tsuchi::container_package_path(toc, 0x20, path) && std::string(path.data) == "/Game/Mods/b/C/T_B");
    path.clear();
    CHECK(!tsuchi::container_package_path(toc, 0x30, path));
    tsuchi::List<tsuchi::ContainerRedirect> redirects;
    CHECK(tsuchi::read_container_redirects(reinterpret_cast<const uint8_t*>(made.cas.data()), size_t(toc.header_bytes), redirects) &&
          redirects.count == 1 && redirects[0].from == 0x30 && redirects[0].to == 0x20);
    redirects.clear();
    CHECK(!tsuchi::read_container_redirects(reinterpret_cast<const uint8_t*>(made.cas.data()), size_t(toc.header_bytes) - 3, redirects));
    redirects.clear();
    // Cut short, not one at all, or encrypted.
    CHECK(!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), 100, toc, wrong));
    std::string other = made.toc;
    other[0] = 'x';
    CHECK(!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(other.data()), other.size(), toc, wrong));
    other = made.toc, other[80] = 2;
    CHECK(!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(other.data()), other.size(), toc, wrong) && std::string(wrong).find("encrypted") != std::string::npos);
    tsuchi::forget_container_toc(toc);
}

void startup_compressed_claims() {
    const auto check = [&](const Made& made, TSUCHI_Result wanted, bool hash_match = false) {
        files["rom:/startup.ucas"] = made.cas;
        tsuchi::ContainerToc toc;
        const char* wrong = nullptr;
        CHECK(tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(made.toc.data()), made.toc.size(), toc, wrong));
        tsuchi::List<uint64_t> claims;
        uint8_t actual[32]{}, expected[32]{};
        bytes_read = read_calls = 0;
        const TSUCHI_Result result = tsuchi::read_startup_container_claims(toc, "rom:/startup.ucas", claims, actual);
        CHECK(result == wanted);
        if (result == TSUCHI_OK) CHECK(claims.count == 1 && claims[0] == 0xA4);
        else CHECK(!claims.count);
        if (hash_match) {
            const std::string header = make_header(0xB4, {{0xA4, "Carrot4/Content/MyMeshes/SM_Leaf.uasset"}}, {}, false);
            tsuchi::sha256(header.data(), header.size(), expected);
            CHECK(!std::memcmp(actual, expected, sizeof actual));
            CHECK(read_calls == 3 && bytes_read < made.cas.size());
        }
        claims.clear(); tsuchi::forget_container_toc(toc);
    };
    check(mixed_header(0xB4, 0xA4), TSUCHI_OK, true);
    check(mixed_header(0xB4, 0xA4, "Carrot4/Content/MyMeshes/SM_Leaf.uasset", true), TSUCHI_OK);
    Made bad = mixed_header(0xB4, 0xA4);
    bad.cas[32 + 43 - 1] ^= 1; // the Zlib block's Adler32
    check(bad, TSUCHI_INVALID);
    bad = mixed_header(0xB4, 0xA4);
    const size_t methods = 144 + 44 + 12 * 5;
    std::memcpy(&bad.toc[methods], "Oodle", 6);
    check(bad, TSUCHI_UNSUPPORTED);
    bad = mixed_header(0xB4, 0xA4, "Carrot4/Content/MyMeshes/SM_Leaf.uasset", true);
    const size_t serialized = make_header(0xB4, {{0xA4, "Carrot4/Content/MyMeshes/SM_Leaf.uasset"}}, {}, false).size();
    bad.cas[7 + serialized + 11] = 1; // header padding is in a plain block after the Zlib overhead
    check(bad, TSUCHI_INVALID);
    check(mixed_header(0xB4, 0xA4, "Carrot4/Content/MyMeshes/SM_Leaf.uasset", true, 1), TSUCHI_INVALID);

    // Contiguous physical bytes cannot fill a logical hole in a short plain block.
    // The first/last byte checks alone accept offset0,length96 over plain32+plain64.
    Made gap{std::string(144, '\0'), {}};
    std::memcpy(&gap.toc[0], "-==--==--==--==-", 16); gap.toc[16] = 2;
    const uint32_t numbers[] = {144, 1, 2, 12, 0, 32, 64, 0};
    const uint64_t owner = 0xB4;
    put64(gap.cas, owner); put32(gap.cas, 0); // a valid zero-package physical header
    put32(gap.cas, 60); gap.cas.append(60, 'n');
    for (unsigned i = 0; i < 5; ++i) put32(gap.cas, 0);
    CHECK(gap.cas.size() == 96);
    std::memcpy(&gap.toc[20], numbers, sizeof numbers); std::memcpy(&gap.toc[56], &owner, 8);
    put64(gap.toc, owner), gap.toc.append(3, '\0'), gap.toc += char(10);
    gap.toc.append(9, '\0'); gap.toc += char(96);
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t at = i ? 32 : 0, size = i ? 64 : 32;
        for (unsigned j = 0; j < 5; ++j) gap.toc += char(at >> (8 * j));
        for (unsigned twice = 0; twice < 2; ++twice)
            for (unsigned j = 0; j < 3; ++j) gap.toc += char(size >> (8 * j));
        gap.toc += '\0';
    }
    gap.toc.append(33, '\0');
    check(gap, TSUCHI_INVALID);
}

// An empty UE pak. Versions 10 and 11 have both secondary indexes, even with no entries.
std::string empty_pak(uint32_t version = 8, uint32_t entries = 0) {
    std::string index;
    put_string(index, "../../../");
    put32(index, entries);
    std::string out;
    if (version >= 10) {
        const std::string hashed(8, '\0'), directory(4, '\0');
        out = hashed + directory;
        put64(index, 0);
        uint8_t hash[20];
        put32(index, 1), put64(index, 0), put64(index, hashed.size());
        tsuchi::sha1(reinterpret_cast<const uint8_t*>(hashed.data()), uint32_t(hashed.size()), hash);
        index.append(reinterpret_cast<const char*>(hash), sizeof hash);
        put32(index, 1), put64(index, hashed.size()), put64(index, directory.size());
        tsuchi::sha1(reinterpret_cast<const uint8_t*>(directory.data()), uint32_t(directory.size()), hash);
        index.append(reinterpret_cast<const char*>(hash), sizeof hash);
        put32(index, 0), put32(index, 0);
    }
    const uint64_t index_at = out.size();
    out += index;
    std::string footer(17, '\0');
    put32(footer, 0x5A6F12E1), put32(footer, version), put64(footer, index_at), put64(footer, index.size());
    uint8_t hash[20];
    tsuchi::sha1(reinterpret_cast<const uint8_t*>(index.data()), uint32_t(index.size()), hash);
    footer.append(reinterpret_cast<const char*>(hash), sizeof hash);
    if (version == 9) footer += '\0';
    footer.append(160, '\0');
    return out + footer;
}

void stage_bundle(const std::string& name, const Made& made) {
    const std::string path = "sd:/tsuchi/bundles/" + name;
    files[path + ".pak"] = empty_pak();
    files[path + ".utoc"] = made.toc;
    files[path + ".ucas"] = made.cas;
}

TSUCHI_Result mount_bundle(const std::string& name, const char* root = "/Game/MyMeshes") {
    const std::string path = "sd:/tsuchi/bundles/" + name;
    return tsuchi::mount_asset_container("app.owner", (path + ".pak").c_str(),
        (path + ".utoc").c_str(), (path + ".ucas").c_str(), root);
}

// The identity the engine gives the package a container file path holds.
uint64_t id_of(const std::string& file) {
    std::string package = "/Game/" + file.substr(file.find("/Content/") + 9);
    package.resize(package.rfind('.'));
    return tsuchi::container_package_id(package.c_str());
}
const std::string kLeafPath = "Carrot4/Content/MyMeshes/SM_Leaf.uasset";
Made bundle(uint64_t id, uint64_t package, const std::string& path = "Carrot4/Content/MyMeshes/SM_Leaf.uasset") {
    return make_container(id, {{package, path}}, {}, false, 2, false);
}

Made bundle_with_bulk(uint64_t id, uint64_t package, uint64_t bulk_owner, const std::string& stem = "Carrot4/Content/MyMeshes/SM_Leaf") {
    const std::vector<Package> owned{{package, stem + ".uasset"}};
    Made made = make_container(id, {owned[0], {bulk_owner, stem + ".ubulk"}}, {}, false, 2, false);
    const std::string header = make_header(id, owned, {}, false);
    made.cas.replace(0, header.size(), header);
    made.toc[144 + 2 * 12 + 11] = 3;
    const uint64_t header_size = header.size();
    const size_t places_at = 144 + 3 * 12, blocks_at = places_at + 3 * 10;
    for (unsigned i = 0; i < 5; ++i) made.toc[places_at + 5 + i] = char(header_size >> (8 * (4 - i)));
    for (unsigned i = 0; i < 3; ++i)
        made.toc[blocks_at + 5 + i] = made.toc[blocks_at + 8 + i] = char(header_size >> (8 * i));
    return made;
}

void late_downloaded_assets() {
    files.clear();install("app.owner",manifest("app.owner"));
    const auto stock=make_container(0xC0,{});
    files["rom:/StandIn/Content/Paks/StandIn-Main.pak"]="pak";
    files["rom:/StandIn/Content/Paks/StandIn-Main.utoc"]=stock.toc;
    files["rom:/StandIn/Content/Paks/StandIn-Main.ucas"]=stock.cas;
    begin();
    const auto made=make_container(0xD123,{{0xA321,"Carrot4/Content/Maps/CaveTest.umap"}});
    const std::string folder="tsuchisd:/tsuchi/downloads/test/mods/external.test";
    files[folder+"/Map_P.pak"]="pak";files[folder+"/Map_P.utoc"]=made.toc;files[folder+"/Map_P.ucas"]=made.cas;
    tsuchi::Text problem;
    CHECK(tsuchi::validate_downloaded_assets("external.test",folder.c_str(),problem)==TSUCHI_OK);
    CHECK(tsuchi::mount_downloaded_assets("external.test",folder.c_str(),problem)==TSUCHI_OK);
    CHECK(mounts.size()==1&&mounts.back().path==folder+"/Map_P.pak");
    CHECK(tsuchi::container_mounted_for("external.test"));
    CHECK(tsuchi::mount_downloaded_assets("external.test",folder.c_str(),problem)==TSUCHI_OK&&mounts.size()==1);
    CHECK(tsuchi::mount_downloaded_assets("external.test","tsuchisd:/another/version",problem)==TSUCHI_BUSY);
    problem.clear();
}

void new_asset_bundles() {
    files.clear();
    const Made game = mixed_header(0xC0, kGameTexture, "Carrot4/Content/Carrot4/T_Icon.uasset");
    files["rom:/StandIn/Content/Paks/StandIn-Main.pak"] = "pak";
    files["rom:/StandIn/Content/Paks/StandIn-Main.utoc"] = game.toc;
    files["rom:/StandIn/Content/Paks/StandIn-Main.ucas"] = game.cas;
    Made global = make_container(0, {}, {}, false, 2, false);
    global.toc[144 + 11] = 6; // script metadata, without a package header
    files["rom:/StandIn/Content/Paks/global.utoc"] = global.toc;
    Made sentinel = make_container(UINT64_MAX, {}, {}, false, 2, false);
    sentinel.toc[144 + 11] = 7;
    files["rom:/StandIn/Content/Paks/metadata.utoc"] = sentinel.toc;
    Made bulk_patch = make_container(0xC8, {{0xA8, "Carrot4/Content/Mods/base/Leaf.ubulk"}}, {}, false, 2, false);
    bulk_patch.toc[144 + 11] = 6, bulk_patch.toc[144 + 12 + 11] = 3;
    files["rom:/StandIn/Content/Paks/bulkpatch.utoc"] = bulk_patch.toc;
    install("app.owner", manifest("app.owner"));
    install("app.base", manifest("app.base"), "Base", make_container(0xB0,
        {{0xA0, "Carrot4/Content/Mods/base/T_Leaf.uasset"}}, {{0xA1, 0xA0}}));
    begin();
    CHECK(mounts.size() == 1 && record("app.owner")->state == tsuchi::ModState::ready);

    // The startup TOCs were freed, but game, mod, and redirect identities still belong to them.
    const uint64_t leaf = id_of(kLeafPath);
    stage_bundle("new", bundle(0xB1, leaf));
    CHECK(mount_bundle("new") == TSUCHI_OK);
    CHECK(tsuchi::container_mounted_for("app.owner"));
    CHECK(mounts.size() == 2 && mounts.back().path == "sd:/tsuchi/bundles/new.pak" && mounts.back().order == 10001);
    stage_bundle("same-container", bundle(0xB1, 0xA3));
    CHECK(mount_bundle("same-container") == TSUCHI_BUSY);
    stage_bundle("game-container", bundle(0xC0, 0xA3));
    CHECK(mount_bundle("game-container") == TSUCHI_BUSY);
    stage_bundle("global-container", bundle(0, 0xA3));
    CHECK(mount_bundle("global-container") == TSUCHI_BUSY);
    stage_bundle("sentinel-container", bundle(UINT64_MAX, 0xA3));
    CHECK(mount_bundle("sentinel-container") == TSUCHI_INVALID);
    const uint64_t occupied[] = {kGameTexture, 0xA0, 0xA1, leaf, 0xA8, 1, 2};
    for (uint64_t id : occupied) {
        stage_bundle("occupied", bundle(0xB2, id));
        CHECK(mount_bundle("occupied") == TSUCHI_BUSY);
    }

    // Authored assets can keep their own folders, including a root declared as /Game.
    stage_bundle("own-folder", bundle(0xB3, id_of("Carrot4/Content/Leaf/SM_Leaf.uasset"), "Carrot4/Content/Leaf/SM_Leaf.uasset"));
    CHECK(mount_bundle("own-folder", "/Game") == TSUCHI_OK);
    // A package's identity is its path's. Another identity could name a future game package or another mod's.
    stage_bundle("unbound", bundle(0xB6, 0xA6, "Carrot4/Content/MyMeshes/SM_Unbound.uasset"));
    CHECK(mount_bundle("unbound") == TSUCHI_INVALID);
    stage_bundle("future-game", bundle(0xB6, id_of("Carrot4/Content/Carrot4/Future.uasset"), "Carrot4/Content/MyMeshes/SM_Unbound.uasset"));
    CHECK(mount_bundle("future-game") == TSUCHI_INVALID);
    stage_bundle("outside", bundle(0xB4, 0xA4, "Carrot4/Content/MyMeshesOther/SM_Leaf.uasset"));
    CHECK(mount_bundle("outside") == TSUCHI_INVALID);
    const char* bad_roots[] = {"/Engine", "/Game/Carrot4", "/Game/Carrot4/Props", "/Game/", "/Game/MyMeshes/", "/Game/../Leaf", "/Game//Leaf"};
    for (const char* root : bad_roots) CHECK(mount_bundle("outside", root) == TSUCHI_INVALID);
    const char* bad_paths[] = {"Carrot4/Content/Carrot4/New.uasset", "Carrot4/Content/carrot4/New.uasset",
        "Engine/Content/Leaf.uasset", "Carrot4/Content/MyMeshes/../Leaf.uasset", "Carrot4/Content/MyMeshes//Leaf.uasset"};
    for (const char* path : bad_paths) {
        stage_bundle("bad-path", bundle(0xB4, 0xA4, path));
        CHECK(mount_bundle("bad-path", "/Game") == TSUCHI_INVALID);
    }
    stage_bundle("hidden-path", bundle(0xB4, 0xA4, std::string("Carrot4/Content/MyMeshes/SM_Leaf\0Hidden.uasset", 42)));
    CHECK(mount_bundle("hidden-path") == TSUCHI_INVALID);

    // A redirect may repeat only an identity owned by this TOC; cultures remain forbidden.
    stage_bundle("redirect", make_container(0xB4, {{0xA4, "Carrot4/Content/MyMeshes/T_A.uasset"}}, {{0xA5, 0xA4}}, false, 2, false));
    CHECK(mount_bundle("redirect") == TSUCHI_INVALID);
    stage_bundle("foreign-identity", make_container(0xB4, {{0xA4, "Carrot4/Content/MyMeshes/T_A.uasset"}}, {{0xA5, 0xA5}}, false, 2, false));
    CHECK(mount_bundle("foreign-identity") == TSUCHI_INVALID);
    const std::vector<Package> padded_packages{{0xA4, "Carrot4/Content/MyMeshes/T_A.uasset"}};
    const std::vector<std::pair<uint64_t, uint64_t>> padded_redirects{{0xA4, 0xA4}};
    Made padded = make_container(0xB4, padded_packages, padded_redirects, false, 2, false, true);
    const size_t serialized_bytes = make_header(0xB4, padded_packages, padded_redirects, false).size();
    CHECK(serialized_bytes % 16 != 0);
    padded.cas[serialized_bytes] = 1;
    stage_bundle("nonzero-padding", padded);
    CHECK(mount_bundle("nonzero-padding") == TSUCHI_INVALID);
    stage_bundle("excess-padding", make_container(0xB4, padded_packages, padded_redirects, false, 2, false, true, 16));
    CHECK(mount_bundle("excess-padding") == TSUCHI_INVALID);
    stage_bundle("culture", make_container(0xB4, {{0xA4, "Carrot4/Content/MyMeshes/T_A.uasset"}}));
    CHECK(mount_bundle("culture") == TSUCHI_INVALID);
    stage_bundle("compressed", mixed_header(0xB4, 0xA4));
    CHECK(mount_bundle("compressed") == TSUCHI_UNSUPPORTED);
    const uint32_t block_tail = uint32_t((65536 - make_header(0xB4, padded_packages, {}, false).size()) % 65536);
    stage_bundle("startup-only-padding", make_container(0xB4, padded_packages, {}, false, 2, false, false, block_tail));
    CHECK(mount_bundle("startup-only-padding") == TSUCHI_INVALID);

    Made broken = bundle(0xB4, 0xA4);
    broken.toc.resize(broken.toc.size() - 1);
    stage_bundle("short-toc", broken);
    CHECK(mount_bundle("short-toc") == TSUCHI_INVALID);
    broken = bundle(0xB4, 0xA4), broken.cas.resize(20);
    stage_bundle("short-data", broken);
    CHECK(mount_bundle("short-data") == TSUCHI_INVALID);
    broken = bundle(0xB4, 0xA4), broken.cas[0] ^= 1;
    stage_bundle("wrong-header", broken);
    CHECK(mount_bundle("wrong-header") == TSUCHI_INVALID);
    broken = bundle(0xB4, 0xA4), broken.cas[31] ^= 1; // the header's package ID, after its name map
    stage_bundle("wrong-package", broken);
    CHECK(mount_bundle("wrong-package") == TSUCHI_INVALID);
    stage_bundle("foreign-bulk", bundle_with_bulk(0xB4, 0xA4, kGameTexture));
    CHECK(mount_bundle("foreign-bulk") == TSUCHI_INVALID);
    stage_bundle("unknown-bulk", bundle_with_bulk(0xB4, 0xA4, 0xA6));
    CHECK(mount_bundle("unknown-bulk") == TSUCHI_INVALID);
    broken = bundle(0xB4, 0xA4), broken.toc[144 + 11] = 6;
    stage_bundle("metadata", broken);
    CHECK(mount_bundle("metadata") == TSUCHI_UNSUPPORTED);
    stage_bundle("missing", bundle(0xB4, 0xA4));
    files.erase("sd:/tsuchi/bundles/missing.pak");
    CHECK(mount_bundle("missing") == TSUCHI_NOT_FOUND);
    files["sd:/tsuchi/bundles/missing.pak"] = "";
    CHECK(mount_bundle("missing") == TSUCHI_INVALID);
    stage_bundle("large", bundle(0xB4, 0xA4));
    files["sd:/tsuchi/bundles/large.utoc"].resize((16 << 20) + 1, '\0');
    CHECK(mount_bundle("large") == TSUCHI_CAPACITY);
    files.erase("sd:/tsuchi/bundles/large.utoc");

    // A pak must have no loose entries. Footer, primary, and secondary indexes are verified.
    stage_bundle("pak", bundle(0xB4, 0xA4));
    const char* pak = "sd:/tsuchi/bundles/pak.pak";
    files[pak] = empty_pak(8, 1);
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(11, 1);
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(), files[pak][0] ^= 1;
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(11), files[pak][0] ^= 1; // secondary index hash mismatch
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(), files[pak][files[pak].size() - 221 + 16] = 1;
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(9), files[pak][files[pak].size() - 222 + 61] = 1;
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);
    files[pak] = empty_pak(), files[pak].pop_back();
    CHECK(mount_bundle("pak") == TSUCHI_INVALID);

    for (uint32_t version : {9u, 10u, 11u}) {
        const std::string name = "pak-version-" + std::to_string(version);
        const std::string stem = "Carrot4/Content/MyMeshes/SM_Leaf" + std::to_string(version);
        const uint64_t package = id_of(stem + ".uasset");
        stage_bundle(name, version == 10 ? bundle_with_bulk(0x100 + version, package, package, stem) :
            make_container(0x100 + version, {{package, stem + ".uasset"}},
                {{package, package}}, false, 2, false, version == 11));
        files["sd:/tsuchi/bundles/" + name + ".pak"] = empty_pak(version);
        if (version == 11) {
            stage_bundle("reenter", bundle(0x100 + version, package, stem + ".uasset"));
            reenter_bundle = "reenter";
        }
        CHECK(mount_bundle(name) == TSUCHI_OK);
        if (version == 11) CHECK(reenter_result == TSUCHI_BUSY);
    }

    // A failed engine mount claims nothing, and does not block its mod.
    stage_bundle("retry_P", bundle(0xB4, id_of("Carrot4/Content/MyMeshes/SM_Retry.uasset"), "Carrot4/Content/MyMeshes/SM_Retry.uasset"));
    tsuchi::set_allocator({[](size_t) -> void* { return nullptr; }, std::free});
    CHECK(mount_bundle("retry_P") == TSUCHI_NO_MEMORY);
    tsuchi::set_allocator({std::malloc, std::free});
    refuse_mounts = true;
    CHECK(mount_bundle("retry_P") == TSUCHI_IO);
    refuse_mounts = false;
    CHECK(record("app.owner")->state == tsuchi::ModState::ready && mounts.size() == 6);
    CHECK(mount_bundle("retry_P") == TSUCHI_OK && mounts.size() == 7 && mounts.back().order + 100 == 10006);

    // An incomplete startup census disables dynamic admission, while startup behavior stays intact.
    files.erase("rom:/StandIn/Content/Paks/StandIn-Main.ucas");
    begin();
    stage_bundle("uncertain", bundle(0xB5, 0xA5));
    CHECK(mount_bundle("uncertain") == TSUCHI_UNSUPPORTED);
    CHECK(record("app.owner")->state == tsuchi::ModState::ready && mounts.size() == 1);
}

// Prints what the framework reads of a container on disk.
int read_real(const char* toc_path, const char* cas_path) {
    const auto whole = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string toc_bytes = whole(toc_path);
    tsuchi::ContainerToc toc;
    const char* wrong = nullptr;
    if (!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(toc_bytes.data()), toc_bytes.size(), toc, wrong)) {
        std::printf("refused: %s\n", wrong);
        return 1;
    }
    std::printf("container %016llx, header at %llu, %llu bytes, %u packages\n", (unsigned long long)toc.id, (unsigned long long)toc.header_at,
                (unsigned long long)toc.header_bytes, toc.packages.count);
    for (uint32_t i = 0; i < toc.packages.count; ++i) {
        tsuchi::Texts path;
        const bool named = tsuchi::container_package_path(toc, toc.packages[i], path);
        std::printf("  %016llx %s\n", (unsigned long long)toc.packages[i], named ? path.data : "<no path>");
        path.clear();
    }
    if (cas_path && toc.header_bytes) {
        const std::string cas = whole(cas_path);
        tsuchi::List<tsuchi::ContainerRedirect> redirects;
        const bool fine = toc.header_at + toc.header_bytes <= cas.size() &&
                          tsuchi::read_container_redirects(reinterpret_cast<const uint8_t*>(cas.data()) + toc.header_at, size_t(toc.header_bytes), redirects);
        std::printf("header %s, %u redirects\n", fine ? "read" : "refused", redirects.count);
        for (uint32_t i = 0; i < redirects.count; ++i) std::printf("  %016llx -> %016llx\n", (unsigned long long)redirects[i].from, (unsigned long long)redirects[i].to);
        redirects.clear();
    }
    tsuchi::forget_container_toc(toc);
    return 0;
}

int admit_real(char** argv) {
    const auto whole = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    files.clear();
    const Made game = make_container(0xFFFF1234, {}, {}, false, 2, false);
    files["rom:/StandIn/Content/Paks/StandIn-Main.utoc"] = game.toc;
    files["rom:/StandIn/Content/Paks/StandIn-Main.ucas"] = game.cas;
    install("app.owner", manifest("app.owner"));
    begin();
    files["sd:/tsuchi/bundles/real.pak"] = whole(argv[2]);
    files["sd:/tsuchi/bundles/real.utoc"] = whole(argv[3]);
    files["sd:/tsuchi/bundles/real.ucas"] = whole(argv[4]);
    const TSUCHI_Result result = mount_bundle("real", argv[5]);
    std::printf("admission result %u, %zu stand-in mounts\n", unsigned(result), mounts.size());
    tsuchi::forget_mods();
    return result == TSUCHI_OK ? 0 : 1;
}

// Direct bounded file adapter for --claims. Unlike the fixture map, this can address a stock
// multi-gigabyte UCAS without retaining any of its unrequested bytes in the host process.
struct DiskFile {
    std::ifstream stream;
    int64_t size;
    explicit DiskFile(const char* path) : stream(path, std::ios::binary | std::ios::ate), size(int64_t(stream.tellg())) {}
};
uint64_t disk_bytes_read = 0, disk_reads = 0;
uint32_t disk_open(void* out, void* path, int32_t) {
    auto* file = new DiskFile(static_cast<const char*>(path));
    if (!file->stream || file->size < 0) { delete file; return t::constant::fs_no_such_path; }
    *static_cast<void**>(out) = file;
    return 0;
}
void disk_close(void* file) { delete static_cast<DiskFile*>(file); }
uint32_t disk_size(void* out, void* file) {
    *static_cast<int64_t*>(out) = static_cast<DiskFile*>(file)->size;
    return 0;
}
uint32_t disk_read(void* got, void* file, int64_t offset, void* buffer, uint64_t size) {
    auto& stream = static_cast<DiskFile*>(file)->stream;
    stream.clear(); stream.seekg(offset);
    stream.read(static_cast<char*>(buffer), std::streamsize(size));
    *static_cast<uint64_t*>(got) = uint64_t(stream.gcount());
    disk_bytes_read += uint64_t(stream.gcount()); ++disk_reads;
    return uint64_t(stream.gcount()) == size ? 0 : 1;
}
int real_claims(int argc, char** argv) {
    std::ifstream in(argv[2], std::ios::binary | std::ios::ate);
    const int64_t length = int64_t(in.tellg());
    if (!in || length <= 0 || length > (64ll << 20)) return 1;
    std::string bytes(size_t(length), '\0');
    in.seekg(0); in.read(&bytes[0], length);
    if (in.gcount() != length) return 1;
    tsuchi::ContainerToc toc;
    const char* wrong = nullptr;
    if (!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), toc, wrong)) {
        std::printf("TOC rejected: %s\n", wrong ? wrong : "invalid");
        tsuchi::forget_container_toc(toc); return 1;
    }
    t::fn::fs_open_file = disk_open; t::fn::fs_close_file = disk_close;
    t::fn::fs_file_size = disk_size; t::fn::fs_read_file = disk_read;
    tsuchi::List<uint64_t> claims;
    uint8_t hash[32]{};
    const TSUCHI_Result result = tsuchi::read_startup_container_claims(toc, argv[3], claims, hash);
    std::set<uint64_t> unique;
    for (uint32_t i = 0; i < claims.count; ++i) unique.insert(claims[i]);
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) hex[2*i] = "0123456789abcdef"[hash[i] >> 4], hex[2*i+1] = "0123456789abcdef"[hash[i] & 15];
    hex[64] = 0;
    std::printf("claims result %u, container %016llx, %u packages, %u claims, %zu unique, %llu UCAS bytes in %llu reads, header sha256 %s\n",
        unsigned(result), (unsigned long long)toc.id, toc.packages.count, claims.count, unique.size(),
        (unsigned long long)disk_bytes_read, (unsigned long long)disk_reads, hex);
    const bool matched = argc < 5 || !std::strcmp(argv[4], hex);
    if (!matched) std::puts("header hash mismatch");
    claims.clear(); tsuchi::forget_container_toc(toc);
    return result == TSUCHI_OK && matched ? 0 : 1;
}

} // namespace

// Checks that every package of a real container has its path's identity.
int real_ids(const char* toc_path) {
    std::ifstream in(toc_path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    tsuchi::ContainerToc toc;
    const char* wrong = nullptr;
    if (!tsuchi::read_container_toc(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), toc, wrong)) {
        std::printf("refused: %s\n", wrong);
        return 1;
    }
    uint32_t named = 0, same = 0;
    // Each path lookup scans the index, so a large container is checked on its first 3000 packages.
    for (uint32_t i = 0; i < toc.packages.count && i < 3000; ++i) {
        tsuchi::Texts path;
        // Engine plugin packages are named by their plugin's mount, which the path reader does not give.
        if (!tsuchi::container_package_path(toc, toc.packages[i], path) || std::strncmp(path.data, "/Game/", 6)) { path.clear(); continue; }
        ++named;
        if (tsuchi::container_package_id(path.data) == toc.packages[i]) ++same;
        else if (named - same <= 5) std::printf("differs: %s %016llx\n", path.data, (unsigned long long)toc.packages[i]);
        path.clear();
    }
    std::printf("%u packages, %u with a path, %u with their path's identity\n", toc.packages.count, named, same);
    tsuchi::forget_container_toc(toc);
    return named && named == same ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--ids")) return real_ids(argv[2]);
    if (argc >= 3 && !std::strcmp(argv[1], "--read")) return read_real(argv[2], argc > 3 ? argv[3] : nullptr);
    if ((argc == 4 || argc == 5) && !std::strcmp(argv[1], "--claims")) return real_claims(argc, argv);
    t::fn::fs_open_file = open_file, t::fn::fs_close_file = close_file, t::fn::fs_file_size = file_size, t::fn::fs_read_file = read_file;
    t::fn::fs_open_folder = open_folder, t::fn::fs_read_folder = read_folder, t::fn::fs_close_folder = close_folder;
    t::fn::platform_files = platform_files, t::fn::find_platform_file = find_platform_file, t::fn::pak_mount = pak_mount;
    t::fn::shader_library_open = shader_library_open;
    tsuchi::set_log_sink(sink);
    if (argc == 6 && !std::strcmp(argv[1], "--admit")) return admit_real(argv);
    reads_a_table_of_contents();
    single_partition_version_three();
    source_header_id_in_startup_patch();
    startup_compressed_claims();
    orders_names_raise();
    mounts_in_order();
    unreadable_folders_fail();
    blocked_mods_mount_nothing();
    player_controls_startup();
    names_are_told_apart();
    common_packages_are_told();
    replacement_store_follows_mount_order();
    late_downloaded_assets();
    new_asset_bundles();
    libraries_are_opened();
    blocked_as_it_starts(); // last, as the kind of mod it adds stays
    tsuchi::forget_mods();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
