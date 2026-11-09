#include "containers.h"
#include "cityhash.h"

#include <string.h>

#include "files.h"
#include "game.h"
#include "inflate.h"
#include "mods.h"
#include "registry.h"
#include "sha1.h"
#include "sha256.h"
#include "text.h"

namespace tsuchi {

namespace t = game::table;

namespace {

// ---- Reading a container ----------------------------------------------------------------------

constexpr uint8_t kTocMagic[16] = {'-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-'};
constexpr uint32_t kTocHeaderBytes = 144;
constexpr uint8_t kTocVersion = 2;        // indexed TOCs
constexpr uint8_t kTocVersionPartitioned = 3;
constexpr uint8_t kEncrypted = 2;         // of the container's flags
constexpr uint8_t kSigned = 4;
constexpr uint8_t kExportBundleData = 2;  // the kinds of chunk the framework looks at
constexpr uint8_t kContainerHeader = 10;
constexpr uint32_t kNone = 0xFFFFFFFF;

bool contains_byte(const void* bytes, uint8_t wanted, size_t size) {
    const auto* at = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) if (at[i] == wanted) return true;
    return false;
}

uint32_t u32_at(const uint8_t* at) {
    uint32_t value;
    memcpy(&value, at, 4);
    return value;
}
uint64_t u64_at(const uint8_t* at) {
    uint64_t value;
    memcpy(&value, at, 8);
    return value;
}
uint64_t big40_at(const uint8_t* at) {
    uint64_t value = 0;
    for (int i = 0; i < 5; ++i) value = value << 8 | at[i];
    return value;
}
uint64_t little_at(const uint8_t* at, int bytes) {
    uint64_t value = 0;
    for (int i = bytes; i--;) value = value << 8 | at[i];
    return value;
}

// Reads what the engine serializes, one piece at a time. A piece past the end makes `ok` false, and
// every piece after it reads as nothing.
struct Reader {
    const uint8_t* bytes;
    size_t size, at = 0;
    bool ok = true;

    bool has(size_t count) {
        ok = ok && count <= size - at;
        return ok;
    }
    uint32_t u32() {
        if (!has(4)) return 0;
        at += 4;
        return u32_at(bytes + at - 4);
    }
    uint64_t u64() {
        if (!has(8)) return 0;
        at += 8;
        return u64_at(bytes + at - 8);
    }
    void skip(uint64_t count) {
        if (has(count)) at += count;
    }
    // An engine string: a length, negative for one in UTF-16, and the terminator in it.
    void skip_string() {
        const int32_t length = int32_t(u32());
        skip(length < 0 ? uint64_t(-int64_t(length)) * 2 : uint64_t(length));
    }
};

} // namespace

bool read_container_toc(const uint8_t* bytes, size_t size, ContainerToc& toc, const char*& wrong) {
    forget_container_toc(toc);
    wrong = "its table of contents is not one the game reads";
    if (size < kTocHeaderBytes || memcmp(bytes, kTocMagic, sizeof kTocMagic)) return false;
    const uint8_t version = bytes[16];
    const uint32_t header_bytes = u32_at(bytes + 20), entries = u32_at(bytes + 24), blocks = u32_at(bytes + 28),
                   block_bytes = u32_at(bytes + 32), methods = u32_at(bytes + 36), method_bytes = u32_at(bytes + 40),
                   block_size = u32_at(bytes + 44), index_bytes = u32_at(bytes + 48);
    const uint8_t flags = bytes[80]; // after the ID, at 56, and the key's GUID
    if (version != kTocVersion && version != kTocVersionPartitioned)
        return wrong = "its table of contents is of another version of the engine than the game's", false;
    // Version 3 keeps this table layout and adds partition metadata in the
    // header's reserved space. The game's reader accepts the layout, but our
    // file path addresses one UCAS only, so split containers stay unsupported.
    if (version == kTocVersionPartitioned && (u32_at(bytes + 52) != 1 || !u64_at(bytes + 88)))
        return wrong = "its container needs more than one data file", false;
    if (flags & kEncrypted) return wrong = "its container is encrypted", false;
    if (header_bytes != kTocHeaderBytes || block_bytes != 12 || !block_size || (methods && method_bytes != 32)) return false;
    // The table: the chunks' IDs, where each one is, the blocks of the container's data, the names of
    // how blocks are compressed, the signatures, the index of files, and what is known of each chunk.
    uint64_t at = kTocHeaderBytes;
    const uint64_t ids_at = at, places_at = ids_at + 12ull * entries, blocks_at = places_at + 10ull * entries,
                   methods_at = blocks_at + 12ull * blocks;
    at = methods_at + uint64_t(methods) * method_bytes;
    if (flags & kSigned) {
        if (at + 4 > size) return false;
        const uint32_t hash_bytes = u32_at(bytes + at);
        at += 4 + 2ull * hash_bytes + 20ull * blocks;
    }
    if (at > size || index_bytes > size - at || 33ull * entries > size - at - index_bytes) return false;
    toc.index_at = size_t(at), toc.index_bytes = index_bytes;
    toc.id = u64_at(bytes + 56);
    toc.bytes = bytes, toc.size = size;
    for (uint32_t method = 0; method < methods; ++method)
        if (!bytes[methods_at + 32ull * method] || !contains_byte(bytes + methods_at + 32ull * method, 0, 32)) return false;
    for (uint32_t block = 0; block < blocks; ++block) {
        const uint8_t* entry = bytes + blocks_at + 12ull * block;
        const uint64_t compressed = little_at(entry + 5, 3), plain = little_at(entry + 8, 3);
        if (!compressed || !plain || plain > block_size || entry[11] > methods || (!entry[11] && compressed != plain)) return false;
        const uint64_t end = little_at(entry, 5) + compressed;
        if (end > toc.data_bytes) toc.data_bytes = end;
    }
    if (version == kTocVersionPartitioned && toc.data_bytes > u64_at(bytes + 88))
        return wrong = "its container's blocks extend beyond its data file", false;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t* id = bytes + ids_at + 12ull * i;
        const uint8_t kind = id[11];
        const uint64_t offset = big40_at(bytes + places_at + 10ull * i), length = big40_at(bytes + places_at + 10ull * i + 5);
        const uint64_t first = offset / block_size, last = length ? (offset + length - 1) / block_size : first;
        if (!length || last >= blocks || offset % block_size >= little_at(bytes + blocks_at + 12ull * first + 8, 3) ||
            (offset + length - 1) % block_size >= little_at(bytes + blocks_at + 12ull * last + 8, 3)) return false;
        if (kind == kExportBundleData && !(toc.packages.add(u64_at(id)) && toc.entries.add(i))) {
            forget_container_toc(toc);
            return wrong = "there is no memory to read its container", false;
        }
        if (kind != kContainerHeader || toc.header_bytes) continue;
        // The header is read when its blocks are stored as they are, one after another.
        bool whole = length && last < blocks;
        uint64_t stored = 0;
        for (uint64_t block = first; whole && block <= last; ++block) {
            const uint8_t* entry = bytes + blocks_at + 12 * block;
            const uint64_t block_at = little_at(entry, 5), compressed = little_at(entry + 5, 3), plain = little_at(entry + 8, 3);
            const uint64_t logical = block * block_size;
            const uint64_t needed = offset + length < logical + block_size ? offset + length - logical : block_size;
            whole = entry[11] == 0 && compressed == plain && needed <= plain && (block == first ? true : block_at == stored);
            stored = block_at + compressed;
        }
        if (!whole) continue;
        const uint8_t* entry = bytes + blocks_at + 12 * first;
        toc.header_at = little_at(entry, 5) + offset % block_size;
        toc.header_bytes = length;
    }
    return true;
}

void forget_container_toc(ContainerToc& toc) {
    toc.packages.clear(), toc.entries.clear();
    toc = ContainerToc{};
}

bool read_container_redirects(const uint8_t* header, size_t size, List<ContainerRedirect>& redirects) {
    Reader read{header, size};
    read.u64(), read.u32();                                     // the container's ID, how many packages
    read.skip(read.u32()), read.skip(read.u32());               // the names, and their hashes
    read.skip(8ull * read.u32());                               // the packages' IDs
    read.skip(read.u32());                                      // what the engine keeps of each package
    for (uint32_t cultures = read.u32(); read.ok && cultures--;) {
        read.skip_string();
        read.skip(16ull * read.u32());
    }
    for (uint32_t count = read.u32(); read.ok && count--;) {
        const uint64_t from = read.u64(), to = read.u64();
        if (read.ok && from != to && !redirects.add(ContainerRedirect{from, to})) return false;
    }
    return read.ok;
}

namespace {

// True when `text` begins with `prefix`.
bool starts(const char* text, const char* prefix) {
    for (; *prefix; ++text, ++prefix)
        if (*text != *prefix) return false;
    return true;
}

// Finds in the index of files the file whose chunk is at `entry` of the table, and adds to `out` the name
// the engine gives its package: the mount point, the folders and the file's name without its extension.
bool index_path(const ContainerToc& toc, uint32_t entry, Texts& out) {
    if (!toc.index_bytes) return false;
    Reader read{toc.bytes + toc.index_at, toc.index_bytes};
    read.skip_string();
    const uint32_t folders = read.u32();
    const size_t folders_at = read.at;
    read.skip(16ull * folders);
    const uint32_t files = read.u32();
    const size_t files_at = read.at;
    read.skip(12ull * files);
    const uint32_t strings = read.u32();
    const size_t strings_at = read.at;
    if (!read.ok) return false;
    // The file, then the folder it is in: each folder lists its files and the folders in it, so the folder of
    // a file is the one whose list leads to it.
    const uint8_t* base = toc.bytes + toc.index_at;
    uint32_t file = kNone;
    for (uint32_t i = 0; i < files && file == kNone; ++i)
        if (u32_at(base + files_at + 12ull * i + 8) == entry) file = i;
    if (file == kNone) return false;
    uint32_t chain[24], depth = 0;
    uint32_t holder = kNone;
    for (uint32_t i = 0; i < folders && holder == kNone; ++i)
        for (uint32_t f = u32_at(base + folders_at + 16ull * i + 12), guard = 0; f != kNone && f < files && guard <= files; f = u32_at(base + files_at + 12ull * f + 4), ++guard)
            if (f == file) holder = i;
    // Up from the folder to the top, by which folder has it among its own.
    if (holder == kNone) return false;
    uint32_t folder = holder;
    for (; folder != kNone && folder != 0 && depth < 24;) {
        chain[depth++] = folder;
        uint32_t parent = kNone;
        for (uint32_t i = 0; i < folders && parent == kNone; ++i)
            for (uint32_t c = u32_at(base + folders_at + 16ull * i + 4), guard = 0; c != kNone && c < folders && guard <= folders; c = u32_at(base + folders_at + 16ull * c + 8), ++guard)
                if (c == folder) parent = i;
        folder = parent;
    }
    if (folder != 0) return false;
    // The strings, found by number.
    const auto string_at = [&](uint32_t number, const char*& text, uint32_t& length) {
        Reader strings_read{base, toc.index_bytes, strings_at};
        for (uint32_t i = 0; i < strings && strings_read.ok; ++i) {
            const int32_t size = int32_t(strings_read.u32());
            if (size <= 0 || !strings_read.has(size_t(size)) || base[strings_read.at + size - 1] != 0 ||
                contains_byte(base + strings_read.at, 0, size_t(size - 1))) return false; // plain text, with one terminator
            if (i == number) {
                text = reinterpret_cast<const char*>(base + strings_read.at), length = uint32_t(size - 1);
                return true;
            }
            strings_read.skip(uint32_t(size));
        }
        return false;
    };
    // The mount point without the steps up to the top, then the path below it, joined, then made the name the
    // engine gives the package.
    Texts joined;
    const char* text = nullptr;
    uint32_t length = 0;
    char piece[256];
    Reader mount{base, toc.index_bytes};
    const int32_t mount_length = int32_t(mount.u32());
    bool fine = mount_length > 0 && mount.has(size_t(mount_length)) && size_t(mount_length) < sizeof piece;
    if (fine) {
        if (base[mount.at + mount_length - 1] != 0 || contains_byte(base + mount.at, 0, size_t(mount_length - 1))) return false;
        memcpy(piece, base + mount.at, size_t(mount_length));
        piece[mount_length - 1] = 0;
        const char* below = piece;
        while (starts(below, "../")) below += 3;
        if (*below == '/') ++below;
        fine = joined.add(below);
    }
    for (uint32_t i = depth; fine && i--;) {
        fine = string_at(u32_at(base + folders_at + 16ull * chain[i]), text, length) && length < sizeof piece;
        if (!fine) break;
        memcpy(piece, text, length), piece[length] = 0;
        --joined.used;
        fine = joined.add(piece, "/");
    }
    fine = fine && string_at(u32_at(base + files_at + 12ull * file), text, length) && length < sizeof piece;
    if (fine) {
        memcpy(piece, text, length), piece[length] = 0;
        for (uint32_t at = length; at--;)
            if (piece[at] == '.') {
                piece[at] = 0;
                break;
            }
        --joined.used;
        fine = joined.add(piece);
    }
    // `Engine/Content/...` is `/Engine/...`, and `<project>/Content/...` is `/Game/...`.
    if (fine) {
        const char* path = joined.data;
        const char* slash = path;
        while (*slash && *slash != '/') ++slash;
        if (starts(slash, "/Content/")) fine = out.add(starts(path, "Engine/") ? "/Engine/" : "/Game/", slash + 9);
        else fine = out.add("/", path);
    }
    joined.clear();
    return fine;
}

} // namespace

bool container_package_path(const ContainerToc& toc, uint64_t package, Texts& out) {
    for (uint32_t i = 0; i < toc.packages.count; ++i)
        if (toc.packages[i] == package) return index_path(toc, toc.entries[i], out);
    return false;
}
uint64_t container_package_id(const char* path) {
    uint8_t wide[1024];
    size_t n = 0;
    for (const char* at = path; *at; ++at) {
        if (n + 2 > sizeof wide || uint8_t(*at) >= 0x80) return 0;
        wide[n++] = uint8_t(*at >= 'A' && *at <= 'Z' ? *at + 32 : *at);
        wide[n++] = 0;
    }
    return city_hash64(wide, n);
}


uint32_t container_order_raise(const char* pak_name) {
    // As the engine's Mount does: a name that ends in `_P.pak` is a patch, and `<name>_<N>_P.pak` patch N. The
    // part between the last two underscores counts only when it is all digits and at least 1, so a name that
    // ends in a hash, such as `kadan_bulbmin_2681c9bb_P.pak`, is patch 1 whatever digits the hash starts with.
    const size_t length = strlen(pak_name);
    if (length < 6) return 0;
    const char* end = pak_name + length - 6;
    const char tail[] = "_p.pak";
    for (int i = 0; i < 6; ++i)
        if ((end[i] >= 'A' && end[i] <= 'Z' ? end[i] + 32 : end[i]) != tail[i]) return 0;
    const char* number = end;
    while (number > pak_name && number[-1] != '_') --number;
    bool digits = number > pak_name && number < end;
    uint32_t value = 0;
    for (const char* digit = number; digits && digit < end; ++digit) {
        if (*digit < '0' || *digit > '9') digits = false;
        else if (value < 1000000) value = value * 10 + uint32_t(*digit - '0'); // past that, held, so the raise cannot wrap
    }
    return 100 * (digits && value >= 1 ? value + 1 : 1);
}

namespace {

// ---- Mounting ---------------------------------------------------------------------------------

constexpr uint32_t kFirstOrder = 10000; // above every container the game mounts as it starts
constexpr uint64_t kMostTocBytes = 64ull << 20;
constexpr uint64_t kMostHeaderBytes = 64ull << 20;
constexpr uint64_t kMostHeaderBlockBytes = 1ull << 20;
constexpr uint64_t kMostAssetTableBytes = 16ull << 20;
constexpr uint64_t kMostPakIndexBytes = 64ull << 10;
constexpr uint32_t kConflictsTold = 10; // packages named for one pair of mods, before the rest are counted

// A container the framework mounted, kept until what it has in common with the others is told.
struct Mounted {
    char id[kModIdBytes];
    char* toc_bytes;
    ContainerToc toc;
    List<ContainerRedirect> redirects;
};
List<Mounted> mounted;
List<uint64_t> taken; // the IDs of the containers the game mounted itself
List<uint64_t> packages; // sorted IDs, including both sides of startup redirects
TSUCHI_Result census = TSUCHI_UNSUPPORTED;
uint32_t next_mount_order = kFirstOrder;
uint32_t asset_mounts = 0;
bool asset_mounting = false;
// Mount waits for the engine's header callbacks. Publish this immutable list to those
// worker threads only for that synchronous call, and retire it before the TOC is released.
const List<uint64_t>* mounting_packages = nullptr;
struct Holder {
    char id[kModIdBytes];
    char folder[384]{};
};
List<Holder> holders; // every mod whose container is mounted, for as long as the game runs
List<Holder> asset_holders;

bool ends_with(const char* text, const char* end) {
    const size_t length = strlen(text), tail = strlen(end);
    return length > tail && !strcmp(text + length - tail, end);
}

// A container's name the engine is given as it is: letters, digits, `.`, `_` and `-`.
bool plain_name(const char* name) {
    for (const char* at = name; *at; ++at)
        if (!((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') || *at == '.' || *at == '_' || *at == '-')) return false;
    return *name != 0;
}

// Text of plain letters as the engine's, in `out`, which has room for `room`. False when it does not fit.
bool wide(const char* const* parts, size_t count, char16_t* out, size_t room) {
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
        for (const char* at = parts[i]; *at; ++at) {
            if (used + 1 >= room) return false;
            out[used++] = char16_t(uint8_t(*at));
        }
    out[used] = 0;
    return true;
}

bool engine_mount_path(const char* pak, uint32_t order, const List<uint64_t>* replacement_packages = nullptr) {
    if (!t::bound(t::fn::platform_files) || !t::bound(t::fn::find_platform_file) || !t::bound(t::fn::pak_mount)) return false;
    char16_t layer_name[32], path[1024];
    const char* const layer_parts[] = {t::name::container_layer};
    const char* const path_parts[] = {pak};
    if (!wide(layer_parts, 1, layer_name, 32) || !wide(path_parts, 1, path, 1024)) return false;
    void* layer = t::fn::find_platform_file(t::fn::platform_files(), layer_name);
    if (!layer) return false;
    // Mod containers are mounted serially in increasing priority. The original package
    // store otherwise retains the stock (or earlier mod's) export counts and imports.
    if (__atomic_load_n(&mounting_packages, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&mounting_packages, replacement_packages, __ATOMIC_RELEASE);
    const bool mounted_ok = t::fn::pak_mount(layer, path, order, nullptr, true);
    __atomic_store_n(&mounting_packages, static_cast<const List<uint64_t>*>(nullptr), __ATOMIC_RELEASE);
    return mounted_ok;
}

// The engine's layer of containers mounts one, with the .utoc and .ucas beside it. The game names its own by
// a path from the top of its files, as `../../../Carrot4/Content/Paks/`, and a mod's is named the same way.
bool container_path(Texts& out,const ModRecord& record,const char* within){
    if(record.folder[0])return out.add(record.folder,*within?"/":"",within);
    return mod_folder_path(out,record.id,within);
}
bool engine_mount(const ModRecord& record, const char* pak, uint32_t order, const List<uint64_t>& replacement_packages) {
    Texts path;
    bool fine = record.folder[0] ? container_path(path,record,pak) : path.add("../../../", kModsFolder + 5, "/", record.id);
    if (fine && !record.folder[0]) --path.used, fine = path.add("/", pak);
    fine = fine && engine_mount_path(path.data, order, &replacement_packages);
    path.clear();return fine;
}

// ---- Shader libraries -------------------------------------------------------------------------

constexpr size_t kLibraryNameBytes = 64; // 63 letters and the terminator
constexpr const char* kLibraryStart = "ShaderArchive-";
constexpr const char* kLibraryEnd = ".ushaderbytecode";

// A string as the engine's functions take one by reference: its letters with the 0 after them, how
// many that is, and how many there is room for.
struct EngineString {
    const char16_t* data;
    int32_t count, room;
};

// The engine opens a library of shaders by its name and the folder it is in, the way it opens the game's.
bool engine_open_library(const char* library, const char* in) {
    if (!t::bound(t::fn::shader_library_open)) return false;
    char16_t name[kLibraryNameBytes], folder[1024];
    const char* const name_parts[] = {library};
    const char* const folder_parts[] = {in};
    if (!wide(name_parts, 1, name, kLibraryNameBytes) || !wide(folder_parts, 1, folder, 1024)) return false;
    const auto length = [](const char16_t* text) {
        int32_t count = 0;
        while (text[count]) ++count;
        return count + 1;
    };
    EngineString name_string{name, length(name), length(name)}, folder_string{folder, length(folder), length(folder)};
    return t::fn::shader_library_open(&name_string, &folder_string);
}

struct Library {
    char id[kModIdBytes];
    char name[kLibraryNameBytes];
};
List<Library> libraries; // every library opened, for as long as the game runs

// The library's name in a file's name, `ShaderArchive-<name>-<format>.ushaderbytecode`, in `name`. False
// when the file is no library of this platform's format, or the name is not one the framework gives the engine.
bool library_name(const char* file, char* name, bool& library) {
    library = false;
    const size_t length = strlen(file), start = strlen(kLibraryStart);
    Texts end;
    if (!end.add("-", t::name::shader_format, kLibraryEnd)) return false;
    const size_t tail = strlen(end.data);
    library = length > start + tail && starts(file, kLibraryStart) && !strcmp(file + length - tail, end.data);
    end.clear();
    if (!library) return false;
    const size_t letters = length - start - tail;
    if (letters >= kLibraryNameBytes) return false;
    for (size_t i = 0; i < letters; ++i) {
        const char at = file[start + i];
        if (!((at >= 'a' && at <= 'z') || (at >= 'A' && at <= 'Z') || (at >= '0' && at <= '9') || at == '_')) return false;
    }
    memcpy(name, file + start, letters);
    name[letters] = 0;
    return true;
}

// These IDs outlive their TOCs. Sorting once after startup gives bounded lookups for new bundles.
void sort_ids(List<uint64_t>& ids) {
    const auto down = [&](uint32_t root, uint32_t end) {
        while (root < end / 2) {
            uint32_t child = root * 2 + 1;
            if (child + 1 < end && ids[child] < ids[child + 1]) ++child;
            if (ids[root] >= ids[child]) break;
            const uint64_t value = ids[root];
            ids[root] = ids[child], ids[child] = value;
            root = child;
        }
    };
    for (uint32_t i = ids.count / 2; i--;) down(i, ids.count);
    for (uint32_t end = ids.count; end > 1;) {
        const uint64_t value = ids[0];
        ids[0] = ids[--end], ids[end] = value;
        down(0, end);
    }
}

bool has_id(const List<uint64_t>& ids, uint64_t id) {
    uint32_t begin = 0, end = ids.count;
    while (begin < end) {
        const uint32_t middle = begin + (end - begin) / 2;
        if (ids[middle] < id) begin = middle + 1;
        else end = middle;
    }
    return begin < ids.count && ids[begin] == id;
}

TSUCHI_Result read_bounded(const char* path, uint64_t limit, char*& bytes, uint64_t& size) {
    return game::with_file(path, [&](void* file, uint64_t length) {
        if (!length) return TSUCHI_INVALID;
        if (length > limit) return TSUCHI_CAPACITY;
        auto* made = static_cast<char*>(allocate(size_t(length)));
        if (!made) return TSUCHI_NO_MEMORY;
        uint64_t got = 0;
        if (t::fn::fs_read_file(&got, file, 0, made, length) != 0 || got != length) {
            release(made);
            return TSUCHI_IO;
        }
        bytes = made, size = length;
        return TSUCHI_OK;
    });
}

bool pak_string(Reader& read) {
    const int32_t length = int32_t(read.u32());
    if (!length) return read.ok; // the engine also serializes an empty string with a zero length
    const uint64_t units = length < 0 ? uint64_t(-int64_t(length)) : uint64_t(length);
    const uint64_t width = length < 0 ? 2 : 1;
    if (units > 1024 || !read.has(units * width)) return false;
    for (uint64_t i = 0; i < units; ++i) {
        const uint8_t* unit = read.bytes + read.at + i * width;
        const bool zero = !unit[0] && (width == 1 || !unit[1]);
        if (zero != (i + 1 == units)) return false;
    }
    read.skip(units * width);
    return true;
}

bool empty_pak_directory(Reader& read) {
    const uint32_t folders = read.u32();
    if (folders > 1024) return false;
    for (uint32_t i = 0; i < folders; ++i)
        if (!pak_string(read) || read.u32() != 0) return false;
    return read.ok;
}

struct PakIndex {
    uint64_t at = 0, size = 0;
    const uint8_t* hash = nullptr;
    bool there = false;
};

bool pak_index_reference(Reader& read, PakIndex& index) {
    const uint32_t present = read.u32();
    if (!read.ok || present > 1) return false;
    if (!present) return true;
    index.there = true, index.at = read.u64(), index.size = read.u64();
    if (!read.has(20)) return false;
    index.hash = read.bytes + read.at;
    read.skip(20);
    return true;
}

TSUCHI_Result read_pak_index(void* file, uint64_t before, const PakIndex& index, uint8_t*& bytes) {
    if (!index.size || index.at > before || index.size > before - index.at) return TSUCHI_INVALID;
    if (index.size > kMostPakIndexBytes) return TSUCHI_CAPACITY;
    auto* made = static_cast<uint8_t*>(allocate(size_t(index.size)));
    if (!made) return TSUCHI_NO_MEMORY;
    uint64_t got = 0;
    TSUCHI_Result result = TSUCHI_OK;
    if (t::fn::fs_read_file(&got, file, int64_t(index.at), made, index.size) != 0 || got != index.size) result = TSUCHI_IO;
    uint8_t hash[20];
    if (result == TSUCHI_OK) {
        sha1(made, uint32_t(index.size), hash);
        if (memcmp(hash, index.hash, sizeof hash)) result = TSUCHI_INVALID;
    }
    if (result != TSUCHI_OK) release(made);
    else bytes = made;
    return result;
}

// The `.pak` is mounted alongside IoStore. Only an empty pak index is admitted: a loose file
// entry could otherwise replace game files without appearing in the IoStore package census.
// A `.pak`'s footer, as the engine's tools write one with nothing encrypted, and its first index, whose bytes go to
// `bytes` for the caller to release.
struct PakFooter {
    uint32_t version = 0;
    uint64_t footer_at = 0, primary_bytes = 0;
};
TSUCHI_Result read_pak_primary(void* file, uint64_t size, PakFooter& out, uint8_t*& bytes) {
    constexpr uint64_t usual = 221, frozen = 222;
    uint8_t footer[frozen];
    uint64_t footer_bytes = usual, got = 0;
    if (size < usual || t::fn::fs_read_file(&got, file, int64_t(size - usual), footer, usual) != 0 || got != usual) return TSUCHI_INVALID;
    uint32_t version = u32_at(footer + 21);
    if (u32_at(footer + 17) != 0x5A6F12E1 || (version != 8 && version != 10 && version != 11)) {
        footer_bytes = frozen;
        if (size < frozen || t::fn::fs_read_file(&got, file, int64_t(size - frozen), footer, frozen) != 0 || got != frozen ||
            u32_at(footer + 17) != 0x5A6F12E1 || u32_at(footer + 21) != 9) return TSUCHI_INVALID;
        version = 9;
        if (footer[61]) return TSUCHI_INVALID; // the game refuses frozen indexes
    }
    for (unsigned i = 0; i < 17; ++i) if (footer[i]) return TSUCHI_INVALID; // no key GUID or encryption
    const unsigned methods_at = version == 9 ? 62 : 61;
    for (unsigned method = 0; method < 5; ++method)
        if (!contains_byte(footer + methods_at + 32 * method, 0, 32)) return TSUCHI_INVALID;
    out.version = version, out.footer_at = size - footer_bytes;
    const PakIndex primary{u64_at(footer + 25), u64_at(footer + 33), footer + 41, true};
    out.primary_bytes = primary.size;
    return read_pak_index(file, out.footer_at, primary, bytes);
}

TSUCHI_Result empty_asset_pak(const char* path) {
    return game::with_file(path, [&](void* file, uint64_t size) {
        if (size > kMostAssetTableBytes) return TSUCHI_CAPACITY;
        PakFooter footer;
        uint8_t* bytes = nullptr;
        TSUCHI_Result result = read_pak_primary(file, size, footer, bytes);
        if (result != TSUCHI_OK) return result;
        const uint32_t version = footer.version;
        const uint64_t footer_at = footer.footer_at;
        const auto drop = [&](TSUCHI_Result answer) { release(bytes); return answer; };
        Reader read{bytes, size_t(footer.primary_bytes)};
        if (!pak_string(read) || read.u32() != 0) return drop(TSUCHI_INVALID);
        if (version < 10) return drop(read.ok && read.at == read.size ? TSUCHI_OK : TSUCHI_INVALID);
        read.u64(); // the path hash seed
        PakIndex hashed, directory;
        if (!pak_index_reference(read, hashed) || !pak_index_reference(read, directory) ||
            (!hashed.there && !directory.there) || read.u32() != 0 || read.u32() != 0 || !read.ok || read.at != read.size)
            return drop(TSUCHI_INVALID); // no encoded entries or serialized file entries
        const PakIndex* const indexes[] = {&hashed, &directory};
        for (const PakIndex* index : indexes) {
            if (!index->there) continue;
            uint8_t* secondary = nullptr;
            result = read_pak_index(file, footer_at, *index, secondary);
            if (result != TSUCHI_OK) return drop(result);
            Reader entries{secondary, size_t(index->size)};
            const bool fine = (index != &hashed || entries.u32() == 0) && empty_pak_directory(entries) && entries.at == entries.size;
            release(secondary);
            if (!fine) return drop(TSUCHI_INVALID);
        }
        return drop(TSUCHI_OK);
    });
}

// An engine string in a pak's index: its letters with the terminator, or null for one in UTF-16. False when it is
// not shaped as one.
bool pak_text(Reader& read, const char*& text) {
    const size_t at = read.at;
    if (!pak_string(read)) return false;
    const int32_t length = int32_t(u32_at(read.bytes + at));
    text = length > 0 ? reinterpret_cast<const char*>(read.bytes + at + 4) : length ? nullptr : "";
    return true;
}

// Finds a mod's shader library in its container's `.pak`: a file `ShaderArchive-<name>-<format>.ushaderbytecode`
// in the pak's index of folders, which the engine's tools write from version 10 on. The engine reads a library only
// from a container, not loose in the mod's folder. The library's name goes to `name`, empty when there is none, and
// the folder the engine reads it from to `folder`: the pak's mount point and the file's folder under it. False, with
// the mod blocked, when the pak has more than one library or one whose name the framework does not give the engine.
bool find_library(ModRecord& record, const char* pak, char* name, Texts& folder) {
    *name = 0;
    Texts path;
    if (!container_path(path, record, pak)) return path.clear(), true;
    char first[256] = {}, second[256] = {}; // the files found, for the log
    bool odd = false;
    uint32_t old_files = 0; // files in a pak of a version without an index of folders
    const auto keep = [](char* into, const char* file) {
        const size_t length = strlen(file) < 255 ? strlen(file) : 255;
        memcpy(into, file, length), into[length] = 0;
    };
    game::with_file(path.data, [&](void* file, uint64_t size) {
        PakFooter footer;
        uint8_t* bytes = nullptr;
        TSUCHI_Result result = read_pak_primary(file, size, footer, bytes);
        if (result != TSUCHI_OK) return result;
        Reader read{bytes, size_t(footer.primary_bytes)};
        const char* mount = nullptr;
        PakIndex hashed, directory;
        bool fine = pak_text(read, mount) && mount && *mount;
        const uint32_t files = read.u32();
        if (fine && footer.version < 10) old_files = files;
        fine = fine && footer.version >= 10;
        if (fine) read.u64(); // the path hash seed
        fine = fine && pak_index_reference(read, hashed) && pak_index_reference(read, directory) && read.ok && directory.there;
        uint8_t* folders = nullptr;
        if (fine) result = read_pak_index(file, footer.footer_at, directory, folders);
        if (fine && result == TSUCHI_OK) {
            Reader list{folders, size_t(directory.size)};
            const uint32_t count = list.u32();
            for (uint32_t i = 0; i < count && list.ok && !second[0] && !odd; ++i) {
                const char* in = nullptr;
                if (!pak_text(list, in)) break;
                const uint32_t listed = list.u32();
                for (uint32_t j = 0; j < listed && list.ok && !second[0] && !odd; ++j) {
                    const char* entry = nullptr;
                    if (!pak_text(list, entry)) break;
                    list.u32(); // where the file's entry is
                    bool library = false;
                    char named[kLibraryNameBytes];
                    const bool good = entry && in && library_name(entry, named, library);
                    if (!library) continue;
                    if (!good) odd = true, keep(first, entry);
                    else if (*name) keep(second, entry);
                    else {
                        keep(first, entry);
                        memcpy(name, named, strlen(named) + 1);
                        // The index names the top folder `/` and the others `a/b/`, under the mount point.
                        folder.clear();
                        if (!folder.add(mount, mount[strlen(mount) - 1] == '/' ? "" : "/", in[0] == '/' ? in + 1 : in)) *name = 0;
                    }
                }
            }
        }
        release(folders), release(bytes);
        return result;
    });
    path.clear();
    if (odd) return block_mod(record, "its shader library's name has other than a to z, digits and '_', or more than 63 of them: ", first), false;
    if (second[0]) return block_mod(record, "its container has more than one shader library: ", first, " and ", second), false;
    if (old_files) {
        Texts line;
        if (line.add("Its container ", pak, " is a .pak of a version from before the engine's tools listed its folders, so a "
                     "shader library in it is not opened. Write it with version 10 or 11.")) mod_note(record.id, line.data);
        line.clear();
    }
    if (!*name) return true;
    for (uint32_t i = 0; i < libraries.count; ++i)
        if (!strcmp(libraries[i].name, name)) return block_mod(record, "its shader library has the name of ", libraries[i].id, "'s: ", first), false;
    return true;
}

// Opens a mod's shader library, which stays open until the game is closed. False, with the mod blocked,
// when the engine does not open it.
bool open_library(ModRecord& record, const char* pak, const char* name, const char* folder) {
    Texts file, line;
    const bool named = file.add(kLibraryStart, name, "-", t::name::shader_format) && (--file.used, file.add(kLibraryEnd));
    if (!engine_open_library(name, folder)) {
        block_mod(record, "the game could not open its shader library ", named ? file.data : name, " in ", pak);
        file.clear();
        return false;
    }
    Library opened{};
    memcpy(opened.id, record.id, strlen(record.id) + 1);
    memcpy(opened.name, name, strlen(name) + 1);
    libraries.add(opened); // with no memory for it, only the check of a later mod's name against it is lost
    if (named && line.add("Opened shader library ", file.data, " in ", pak)) mod_note(record.id, line.data);
    file.clear(), line.clear();
    return true;
}

// Includes culture aliases, which the startup conflict log does not need. Dynamic bundles allow
// only a package's own identity mapping, as retoc writes, without changing another lookup.
TSUCHI_Result header_claims(const uint8_t* header, size_t size, const ContainerToc& toc,
                            List<uint64_t>& claims, bool no_redirects) {
    Reader read{header, size};
    if (read.u64() != toc.id || read.u32() != toc.packages.count) return TSUCHI_INVALID;
    read.skip(read.u32()), read.skip(read.u32());
    const uint32_t count = read.u32();
    if (count != toc.packages.count || !read.has(8ull * count)) return TSUCHI_INVALID;
    List<uint64_t> listed, expected;
    struct IdsGuard {
        List<uint64_t>& listed;
        List<uint64_t>& expected;
        ~IdsGuard() { listed.clear(), expected.clear(); }
    } ids_guard{listed, expected};
    if (!listed.reserve(count) || !expected.reserve(count)) {
        return TSUCHI_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) listed.add(read.u64()), expected.add(toc.packages[i]);
    sort_ids(listed), sort_ids(expected);
    bool fine = true;
    for (uint32_t i = 0; i < count && fine; ++i)
        fine = (!i || listed[i] != listed[i - 1]) && listed[i] == expected[i];
    expected.clear();
    if (!fine) return TSUCHI_INVALID;
    const uint32_t store_bytes = read.u32();
    if (!read.has(store_bytes) || store_bytes < 32ull * count) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry_at = 32ull * i;
        const uint8_t* entry = header + read.at + entry_at;
        const uint64_t imports = u32_at(entry + 24), imports_at = entry_at + 24 + u32_at(entry + 28);
        if (int32_t(u32_at(entry + 8)) < 0 || int32_t(u32_at(entry + 12)) < 0 ||
            (imports && (imports_at < 32ull * count || imports_at > store_bytes || 8 * imports > store_bytes - imports_at))) return TSUCHI_INVALID;
    }
    read.skip(store_bytes);
    const uint32_t cultures = read.u32();
    if (no_redirects && cultures) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < cultures && read.ok; ++i) {
        read.skip_string();
        const uint32_t aliases = read.u32();
        if (!read.has(16ull * aliases)) return TSUCHI_INVALID;
        for (uint32_t j = 0; j < aliases; ++j) {
            const uint64_t from = read.u64(), to = read.u64();
            if (!claims.add(from) || !claims.add(to)) return TSUCHI_NO_MEMORY;
        }
    }
    const uint32_t redirects = read.u32();
    if (!read.has(16ull * redirects)) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < redirects; ++i) {
        const uint64_t from = read.u64(), to = read.u64();
        if (no_redirects) {
            if (from != to || !has_id(listed, from)) return TSUCHI_INVALID;
            continue; // The owned package is counted once below, not as an alias.
        }
        if (!claims.add(from) || !claims.add(to)) return TSUCHI_NO_MEMORY;
    }
    if (!read.ok) return TSUCHI_INVALID;
    if (read.at == size) return TSUCHI_OK;
    // retoc pads to AES alignment. Stock startup headers can instead pad to the TOC block size.
    const uint64_t block = u32_at(toc.bytes + 44);
    const uint64_t block_end = read.at + (block - read.at % block) % block;
    if (size != ((read.at + 15) & ~size_t(15)) && (no_redirects || size != block_end)) return TSUCHI_INVALID;
    for (size_t i = read.at; i < size; ++i) if (header[i]) return TSUCHI_INVALID;
    return TSUCHI_OK;
}

struct HeaderSpan {
    uint64_t at = 0, bytes = 0;
    bool present = false;
};

bool header_span(const ContainerToc& toc, HeaderSpan& span) {
    const uint32_t entries = u32_at(toc.bytes + 24);
    const uint64_t places = kTocHeaderBytes + 12ull * entries;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t* chunk = toc.bytes + kTocHeaderBytes + 12ull * i;
        if (chunk[11] != kContainerHeader) continue;
        if (span.present || u64_at(chunk) != toc.id || chunk[8] || chunk[9] || chunk[10]) return false;
        span = {big40_at(toc.bytes + places + 10ull * i), big40_at(toc.bytes + places + 10ull * i + 5), true};
    }
    return true;
}

// Startup containers can mix plain and Zlib blocks. Reconstruct only the header's logical span;
// never read the rest of a game UCAS, and never use this path for a newly uploaded container.
TSUCHI_Result startup_header(const ContainerToc& toc, const HeaderSpan& span, const char* cas, uint8_t*& out) {
    const uint32_t entries = u32_at(toc.bytes + 24), block_size = u32_at(toc.bytes + 44);
    const uint64_t blocks_at = kTocHeaderBytes + 22ull * entries;
    const uint64_t methods_at = blocks_at + 12ull * u32_at(toc.bytes + 28);
    const uint64_t first = span.at / block_size, last = (span.at + span.bytes - 1) / block_size;
    uint64_t input_bytes = 0, output_bytes = 0;
    for (uint64_t i = first; i <= last; ++i) {
        const uint8_t* block = toc.bytes + blocks_at + 12 * i;
        const uint64_t stored = little_at(block + 5, 3), plain = little_at(block + 8, 3);
        if (stored > kMostHeaderBlockBytes || plain > kMostHeaderBlockBytes) return TSUCHI_CAPACITY;
        if (block[11] && strcmp(reinterpret_cast<const char*>(toc.bytes + methods_at + 32ull * (block[11] - 1)), "Zlib"))
            return TSUCHI_UNSUPPORTED;
        if (stored > input_bytes) input_bytes = stored;
        if (block[11] && plain > output_bytes) output_bytes = plain;
    }
    auto* input = static_cast<uint8_t*>(allocate(size_t(input_bytes)));
    auto* output = output_bytes ? static_cast<uint8_t*>(allocate(size_t(output_bytes))) : nullptr;
    auto* header = static_cast<uint8_t*>(allocate(size_t(span.bytes)));
    if (!input || (output_bytes && !output) || !header) {
        release(input), release(output), release(header);
        return TSUCHI_NO_MEMORY;
    }
    const TSUCHI_Result result = game::with_file(cas, [&](void* file, uint64_t size) {
        if (toc.data_bytes > size) return TSUCHI_INVALID;
        for (uint64_t i = first; i <= last; ++i) {
            const uint8_t* block = toc.bytes + blocks_at + 12 * i;
            const uint64_t at = little_at(block, 5), stored = little_at(block + 5, 3), plain = little_at(block + 8, 3);
            const uint64_t logical = i * block_size;
            const uint64_t begin = span.at > logical ? span.at - logical : 0;
            const uint64_t end = span.at + span.bytes < logical + block_size ? span.at + span.bytes - logical : block_size;
            if (begin > end || end > plain) return TSUCHI_INVALID;
            uint64_t got = 0;
            if (t::fn::fs_read_file(&got, file, int64_t(at), input, stored) != 0 || got != stored) return TSUCHI_IO;
            const uint8_t* decoded = input;
            if (block[11]) {
                if (!inflate_zlib(input, size_t(stored), output, size_t(plain))) return TSUCHI_INVALID;
                decoded = output;
            }
            memcpy(header + logical + begin - span.at, decoded + begin, size_t(end - begin));
        }
        return TSUCHI_OK;
    });
    release(input), release(output);
    if (result == TSUCHI_OK) out = header;
    else release(header);
    return result;
}

TSUCHI_Result remember_packages(const ContainerToc& toc, const char* cas, uint64_t limit,
                              List<uint64_t>& claims, bool no_redirects, uint8_t* header_hash = nullptr) {
    const auto add_packages = [&] {
        if (toc.packages.count > UINT32_MAX - claims.count || !claims.reserve(claims.count + toc.packages.count)) return TSUCHI_NO_MEMORY;
        for (uint32_t i = 0; i < toc.packages.count; ++i) claims.add(toc.packages[i]);
        if (!no_redirects) {
            // A startup patch can contain only a package's bulk data. Its identity is occupied too.
            const uint32_t entries = u32_at(toc.bytes + 24);
            for (uint32_t i = 0; i < entries; ++i) {
                const uint8_t* chunk = toc.bytes + kTocHeaderBytes + 12ull * i;
                if (chunk[11] >= 3 && chunk[11] <= 5 && !claims.add(u64_at(chunk))) return TSUCHI_NO_MEMORY;
            }
        }
        return TSUCHI_OK;
    };
    HeaderSpan span;
    if (!header_span(toc, span)) return TSUCHI_INVALID;
    // The global container can hold script metadata without an asset package or package header.
    if (!span.present) return toc.packages.count ? TSUCHI_UNSUPPORTED : add_packages();
    if (!toc.header_bytes && no_redirects) return TSUCHI_UNSUPPORTED;
    if (span.bytes > limit) return TSUCHI_CAPACITY;
    uint8_t* header = nullptr;
    TSUCHI_Result result = TSUCHI_OK;
    if (!toc.header_bytes) result = startup_header(toc, span, cas, header);
    else {
        uint64_t size = 0;
        result = game::file_size(cas, size);
        if (result != TSUCHI_OK) return result;
        if (toc.data_bytes > size || toc.header_at > size || span.bytes > size - toc.header_at) return TSUCHI_INVALID;
        header = static_cast<uint8_t*>(allocate(size_t(span.bytes)));
        if (!header) return TSUCHI_NO_MEMORY;
        uint64_t got = 0;
        result = game::read_file_part(cas, toc.header_at, header, span.bytes, got);
        if (result == TSUCHI_OK && got != span.bytes) result = TSUCHI_INVALID;
    }
    if (result == TSUCHI_OK) result = header_claims(header, size_t(span.bytes), toc, claims, no_redirects);
    if (result == TSUCHI_OK && header_hash) sha256(header, size_t(span.bytes), header_hash);
    release(header);
    if (result != TSUCHI_OK) return result;
    return add_packages();
}

// The ID in a .utoc's first bytes, or 0.
uint64_t toc_id(const char* path) {
    uint8_t head[kTocHeaderBytes];
    uint64_t got = 0;
    if (game::read_file_part(path, 0, head, sizeof head, got) != TSUCHI_OK || got != sizeof head || memcmp(head, kTocMagic, sizeof kTocMagic)) return 0;
    return u64_at(head + 56);
}

// Every `.pak` in the game's own folder of containers but the game's was put there by a mod, and the game mounts
// it as it starts, whatever becomes of the mod. The IDs of all of them are kept: the engine keeps one container of
// an ID.
void look_at_game_paks() {
    Texts folder, own, files, folders;
    if (!folder.add("rom:/", t::name::project, "/Content/Paks") || !own.add(t::name::game_container, ".pak") ||
        game::list_folder(folder.data, files, folders) != TSUCHI_OK) {
        census = TSUCHI_UNSUPPORTED;
        folder.clear(), own.clear(), files.clear(), folders.clear();
        return;
    }
    for (const char* file = files.first(); file; file = files.after(file)) {
        if (ends_with(file, ".utoc")) {
            Texts where;
            const uint64_t id = where.add(folder.data, "/", file) ? toc_id(where.data) : 0;
            if (id && !taken.add(id)) census = TSUCHI_UNSUPPORTED;
            char* bytes = nullptr;
            uint64_t size = 0;
            ContainerToc toc;
            const char* wrong = nullptr;
            const bool read = where.data && read_bounded(where.data, kMostTocBytes, bytes, size) == TSUCHI_OK &&
                read_container_toc(reinterpret_cast<const uint8_t*>(bytes), size_t(size), toc, wrong);
            if (read) {
                if (!id && !taken.add(toc.id)) census = TSUCHI_UNSUPPORTED;
                // Both suffixes have the same length. Only this temporary path changes.
                memcpy(where.data + strlen(where.data) - 5, ".ucas", 6);
                if (remember_packages(toc, where.data, kMostHeaderBytes, packages, false) != TSUCHI_OK) census = TSUCHI_UNSUPPORTED;
            } else census = TSUCHI_UNSUPPORTED;
            forget_container_toc(toc);
            release(bytes);
            where.clear();
        }
        if (!ends_with(file, ".pak") || !strcmp(file, own.data)) continue;
        Texts line;
        if (line.add(folder.data + 5, "/", file, " belongs to a mod, and the game mounts it as it starts, so it stays "
                     "in use when its mod is blocked. Put it in its mod's folder instead, beside mod.json."))
            mod_note(kFrameworkLogId, line.data);
        line.clear();
    }
    folder.clear(), own.clear(), files.clear(), folders.clear();
}

// Finds a mod's container among the files in its folder: the name of its `.pak`, null when it has none. False,
// with the mod blocked, when what it has is not one container.
bool find_container(ModRecord& record, Texts& files, const char*& pak) {
    pak = nullptr;
    for (const char* file = files.first(); file; file = files.after(file)) {
        if (!ends_with(file, ".pak")) continue;
        if (pak) return block_mod(record, "it has more than one container: ", pak, " and ", file), false;
        pak = file;
    }
    char stem[256];
    if (!pak) {
        for (const char* file = files.first(); file; file = files.after(file))
            if (ends_with(file, ".utoc") || ends_with(file, ".ucas")) return block_mod(record, file, " has no .pak of the same name beside it"), false;
        return true;
    }
    if (!plain_name(pak)) return block_mod(record, "its container's name has letters other than a to z, digits, '.', '_' and '-': ", pak), false;
    const size_t length = strlen(pak) - 4;
    if (length + 6 > sizeof stem) return block_mod(record, "its container's name is too long: ", pak), false;
    const char* const others[] = {".utoc", ".ucas"};
    for (const char* other : others) {
        memcpy(stem, pak, length), memcpy(stem + length, other, strlen(other) + 1);
        bool there = false;
        for (const char* file = files.first(); file && !there; file = files.after(file)) there = !strcmp(file, stem);
        if (!there) return block_mod(record, "its container ", pak, " needs ", stem), false;
    }
    return true;
}

// Reads, checks and mounts one mod's container. False, with the mod blocked, when it cannot be.
bool mount_one(ModRecord& record, const char* pak, uint32_t order) {
    Mounted entry{};
    memcpy(entry.id, record.id, strlen(record.id) + 1);
    const size_t stem = strlen(pak) - 4;
    char name[256];
    memcpy(name, pak, stem), memcpy(name + stem, ".utoc", 6);
    Texts where;
    uint64_t size = 0;
    TSUCHI_Result read = container_path(where, record, name) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
    if (read == TSUCHI_OK) read = game::file_size(where.data, size);
    if (read == TSUCHI_OK && size > kMostTocBytes) read = TSUCHI_CAPACITY;
    if (read == TSUCHI_OK) read = game::read_whole_file(where.data, entry.toc_bytes, size);
    where.clear();
    if (read != TSUCHI_OK) return block_mod(record, read == TSUCHI_NO_MEMORY ? "there is no memory to read " : "its container could not be read: ", name), false;
    const char* wrong = nullptr;
    if (!read_container_toc(reinterpret_cast<const uint8_t*>(entry.toc_bytes), size_t(size), entry.toc, wrong)) {
        release(entry.toc_bytes);
        return block_mod(record, name, ": ", wrong), false;
    }
    const auto drop = [&] {
        forget_container_toc(entry.toc);
        entry.redirects.clear();
        release(entry.toc_bytes);
    };
    // Two containers with one name have one ID, and the engine keeps one of them.
    for (uint32_t i = 0; i < mounted.count; ++i)
        if (mounted[i].toc.id == entry.toc.id) return drop(), block_mod(record, "its container has the name of ", mounted[i].id, "'s: ", pak), false;
    for (uint32_t i = 0; i < taken.count; ++i)
        if (taken[i] == entry.toc.id) return drop(), block_mod(record, "its container has the name of one in ", t::name::project, "/Content/Paks: ", pak), false;
    // The packages it moves in place of others are in its header.
    if (entry.toc.header_bytes && entry.toc.header_bytes <= kMostHeaderBytes) {
        memcpy(name + stem, ".ucas", 6);
        Texts data;
        auto* header = static_cast<uint8_t*>(allocate(size_t(entry.toc.header_bytes)));
        uint64_t got = 0;
        bool fine = header && container_path(data, record, name);
        fine = fine && game::read_file_part(data.data, entry.toc.header_at, header, entry.toc.header_bytes, got) == TSUCHI_OK && got == entry.toc.header_bytes;
        fine = fine && read_container_redirects(header, size_t(got), entry.redirects);
        data.clear();
        release(header);
        if (!fine) return drop(), block_mod(record, "its container's header could not be read: ", name), false;
    }
    const uint32_t raise = container_order_raise(pak);
    // Reserve the census before publishing a mount, since it cannot be undone after an allocation fails.
    List<uint64_t> claims;
    memcpy(name + stem, ".ucas", 6);
    Texts cas;
    bool known = container_path(cas, record, name);
    known = known && remember_packages(entry.toc, cas.data, kMostHeaderBytes, claims, false) == TSUCHI_OK;
    cas.clear();
    known = known && taken.count < UINT32_MAX && taken.reserve(taken.count + 1) &&
        claims.count <= UINT32_MAX - packages.count && packages.reserve(packages.count + claims.count);
    if (raise >= order || !engine_mount(record, pak, order - raise, entry.toc.packages)) {
        claims.clear();
        return drop(), block_mod(record, "the game could not mount its container ", pak), false;
    }
    if (known) {
        taken.add(entry.toc.id);
        for (uint32_t i = 0; i < claims.count; ++i) packages.add(claims[i]);
    } else census = TSUCHI_UNSUPPORTED;
    claims.clear();
    Holder holder{};
    memcpy(holder.id, record.id, strlen(record.id) + 1);
    memcpy(holder.folder,record.folder,sizeof holder.folder);
    holders.add(holder); // with no memory for it, only the line of a mod blocked as it starts is lost
    if (!mounted.add(entry)) drop(); // mounted all the same: only what it has in common with others goes untold
    Texts line;
    if (line.add("Mounted container ", pak, " from this mod folder.")) mod_note(record.id, line.data);
    line.clear();
    return true;
}

// The package a container's package stands for: the one it is put in place of, or itself.
uint64_t stands_for(const Mounted& mod, uint64_t package) {
    for (uint32_t i = 0; i < mod.redirects.count; ++i)
        if (mod.redirects[i].to == package) return mod.redirects[i].from;
    return package;
}

// Tells both mods of a pair what their containers have in common: the game uses the later one's.
void tell_common(const Mounted& earlier, const Mounted& later) {
    uint32_t common = 0;
    for (uint32_t i = 0; i < later.toc.packages.count; ++i) {
        const uint64_t package = stands_for(later, later.toc.packages[i]);
        bool shared = false;
        for (uint32_t j = 0; j < earlier.toc.packages.count && !shared; ++j) shared = stands_for(earlier, earlier.toc.packages[j]) == package;
        if (!shared) continue;
        if (++common > kConflictsTold) continue;
        Texts path, line;
        char hex[19] = "0x";
        for (int digit = 0; digit < 16; ++digit) hex[2 + digit] = "0123456789abcdef"[(package >> (60 - 4 * digit)) & 15];
        hex[18] = 0;
        const char* named = container_package_path(later.toc, later.toc.packages[i], path) ? path.data : hex;
        if (line.add(named, " is in ", later.id, "'s container too, and the game uses ")) {
            --line.used;
            if (line.add(later.id, "'s, as it starts later")) mod_note(earlier.id, line.data);
        }
        line.clear();
        if (line.add(named, " is in ", earlier.id, "'s container too, and the game uses this mod's, as it starts later")) mod_note(later.id, line.data);
        line.clear(), path.clear();
    }
    if (common <= kConflictsTold) return;
    char count[12], *digit = count + sizeof count;
    *--digit = 0;
    for (uint32_t left = common - kConflictsTold; left; left /= 10) *--digit = char('0' + left % 10);
    const Mounted* const pair[2][2] = {{&earlier, &later}, {&later, &earlier}}; // whose log, and the other mod
    for (const auto& told : pair) {
        Texts line;
        if (line.add("and ", digit, " more packages are in both this mod's container and ", told[1]->id)) {
            --line.used;
            if (line.add("'s")) mod_note(told[0]->id, line.data);
        }
        line.clear();
    }
}

} // namespace

void refresh_mounted_package_entry(uint64_t slot, uint64_t entry) {
    const List<uint64_t>* active = __atomic_load_n(&mounting_packages, __ATOMIC_ACQUIRE);
    if (!active || slot < 8 || !entry || (slot & 7) || (entry & 7)) return;
    uint64_t id = 0;
    if (!game::read(slot - 8, id) || !game::readable(entry, 32)) return;
    for (uint32_t i = 0; i < active->count; ++i) if ((*active)[i] == id) {
        game::write(slot, entry);
        return;
    }
}

TSUCHI_Result read_startup_container_claims(const ContainerToc& toc, const char* cas, List<uint64_t>& claims,
                                          uint8_t* header_hash) {
    List<uint64_t> read;
    uint8_t hash[32]{};
    TSUCHI_Result result = remember_packages(toc, cas, kMostHeaderBytes, read, false, header_hash ? hash : nullptr);
    if (result == TSUCHI_OK) {
        if (read.count > UINT32_MAX - claims.count || !claims.reserve(claims.count + read.count)) result = TSUCHI_NO_MEMORY;
        else for (uint32_t i = 0; i < read.count; ++i) claims.add(read[i]);
    }
    read.clear();
    if (result == TSUCHI_OK && header_hash) memcpy(header_hash, hash, sizeof hash);
    return result;
}

void mount_mod_containers() {
    // Called only once by the runtime. Keeping this reset here also permits isolated host fixtures.
    taken.clear(), packages.clear();
    asset_holders.clear();
    libraries.clear();
    census = TSUCHI_OK;
    asset_mounts = 0;
    look_at_game_paks();
    uint32_t places = 0;
    for (uint32_t i = 0; i < mod_count(); ++i)
        if (mod_at(i)->state == ModState::ready && mod_at(i)->order + 1 > places) places = mod_at(i)->order + 1;
    uint32_t next_order = kFirstOrder;
    for (uint32_t place = 0; place < places; ++place) {
        for (uint32_t i = 0; i < mod_count(); ++i) {
            ModRecord& record = *mod_at(i);
            if (record.state != ModState::ready || record.order != place) continue; // blocked, or blocked since: nothing of it is mounted
            Texts where, files, folders;
            TSUCHI_Result listed = mod_folder_path(where, record.id, "") ? game::list_folder(where.data, files, folders) : TSUCHI_NO_MEMORY;
            where.clear(), folders.clear();
            const char* pak = nullptr;
            if (listed != TSUCHI_OK) block_mod(record, "its folder could not be read");
            else if (find_container(record, files, pak) && pak) {
                // The library is opened after the container is mounted, before anything loads a package of the mod's.
                char library[kLibraryNameBytes];
                Texts folder;
                if (find_library(record, pak, library, folder) && mount_one(record, pak, next_order++) && *library)
                    open_library(record, pak, library, folder.data);
                folder.clear();
            }
            files.clear();
        }
    }
    for (uint32_t later = 1; later < mounted.count; ++later)
        for (uint32_t earlier = 0; earlier < later; ++earlier) tell_common(mounted[earlier], mounted[later]);
    for (uint32_t i = 0; i < mounted.count; ++i) {
        forget_container_toc(mounted[i].toc);
        mounted[i].redirects.clear();
        release(mounted[i].toc_bytes);
    }
    mounted.clear();
    sort_ids(taken), sort_ids(packages);
    next_mount_order = next_order;
}

TSUCHI_Result validate_downloaded_assets(const char* id,const char* folder,Text& problem){
    if(!id||!folder||!valid_mod_id(id)||strlen(folder)>=sizeof(ModRecord{}.folder))return TSUCHI_INVALID;
    ModRecord record{};memcpy(record.id,id,strlen(id)+1);memcpy(record.folder,folder,strlen(folder)+1);
    Texts files,dirs,where;auto result=game::list_folder(folder,files,dirs);dirs.clear();const char* pak=nullptr;
    if(result==TSUCHI_OK){for(const char* name=files.first();name;name=files.after(name))if(ends_with(name,".pak")){if(pak||!plain_name(name)||strlen(name)>250){result=TSUCHI_INVALID;break;}pak=name;}if(!pak)result=TSUCHI_INVALID;}
    char* data=nullptr;uint64_t size=0;ContainerToc toc{};
    if(result==TSUCHI_OK){char name[256];const size_t stem=strlen(pak)-4;memcpy(name,pak,stem);memcpy(name+stem,".utoc",6);
        if(!container_path(where,record,name))result=TSUCHI_NO_MEMORY;
        else result=read_bounded(where.data,kMostTocBytes,data,size);where.clear();
        const char* wrong=nullptr;
        if(result==TSUCHI_OK&&!read_container_toc(reinterpret_cast<const uint8_t*>(data),size_t(size),toc,wrong)){result=TSUCHI_INVALID;problem.set(wrong);}
        if(result==TSUCHI_OK){memcpy(name+stem,".ucas",6);List<uint64_t> claims;
            result=container_path(where,record,name)?read_startup_container_claims(toc,where.data,claims):TSUCHI_NO_MEMORY;
            claims.clear();where.clear();}
    }
    if(result!=TSUCHI_OK&&!problem.get()[0])problem.set(record.problem.get()[0]?record.problem.get():"The cooked container is incomplete or incompatible.");
    forget_container_toc(toc);release(data);record.problem.clear();files.clear();return result;
}

TSUCHI_Result mount_downloaded_assets(const char* id,const char* folder,Text& problem){
    if(!id||!folder||!valid_mod_id(id)||strlen(folder)>=sizeof(ModRecord{}.folder))return TSUCHI_INVALID;
    if(census!=TSUCHI_OK||asset_mounting)return TSUCHI_NOT_READY;
    for(uint32_t i=0;i<holders.count;++i)if(!strcmp(holders[i].id,id)&&!strcmp(holders[i].folder,folder))return TSUCHI_OK;
    if(container_mounted_for(id)){problem.set("An earlier container for this mod is still mounted. Restart to change versions.");return TSUCHI_BUSY;}
    ModRecord record{};memcpy(record.id,id,strlen(id)+1);memcpy(record.folder,folder,strlen(folder)+1);record.state=ModState::ready;
    Texts files,dirs;auto result=game::list_folder(folder,files,dirs);dirs.clear();
    const char* pak=nullptr;
    if(result==TSUCHI_OK&&(!find_container(record,files,pak)||!pak))result=TSUCHI_INVALID;
    if(result==TSUCHI_OK){
        char library[kLibraryNameBytes];Texts library_folder;
        if(!find_library(record,pak,library,library_folder))result=TSUCHI_INVALID;
        else if(!mount_one(record,pak,next_mount_order++))result=TSUCHI_INVALID;
        else if(*library)open_library(record,pak,library,library_folder.data);
        library_folder.clear();
    }
    if(result!=TSUCHI_OK)problem.set(record.problem.get());
    record.problem.clear();files.clear();
    for(uint32_t i=0;i<mounted.count;++i){forget_container_toc(mounted[i].toc);mounted[i].redirects.clear();release(mounted[i].toc_bytes);}mounted.clear();
    sort_ids(taken);sort_ids(packages);return result;
}

namespace {

bool canonical_package_path(const char* path) {
    if (!path) return false;
    if (!strcmp(path, "/Game")) return true;
    if (!starts(path, "/Game/") || !path[6]) return false;
    const char* const reserved = "Carrot4";
    bool game_folder = true;
    for (size_t i = 0; reserved[i] && game_folder; ++i) {
        const char character = path[6 + i];
        game_folder = (character >= 'A' && character <= 'Z' ? character + 32 : character) ==
            (reserved[i] >= 'A' && reserved[i] <= 'Z' ? reserved[i] + 32 : reserved[i]);
    }
    if (game_folder && (!path[13] || path[13] == '/')) return false;
    bool segment = false;
    for (const char* at = path + 6; *at; ++at) {
        if (*at == '/') {
            if (!segment) return false;
            segment = false;
        } else {
            if (!((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') ||
                (*at >= '0' && *at <= '9') || *at == '_' || *at == '-')) return false;
            segment = true;
        }
    }
    return segment;
}

// Index the directory once. Looking up each package through the diagnostic path reader would
// repeatedly scan all files and strings, so a large uploaded index could stall the game thread.
TSUCHI_Result package_paths_below(const ContainerToc& toc, const char* root) {
    if (!toc.index_bytes) return TSUCHI_INVALID;
    const uint8_t* bytes = toc.bytes + toc.index_at;
    Reader read{bytes, toc.index_bytes};
    const int32_t mount_bytes = int32_t(read.u32());
    if (mount_bytes <= 0 || !read.has(size_t(mount_bytes)) || bytes[read.at + mount_bytes - 1] != 0 ||
        contains_byte(bytes + read.at, 0, size_t(mount_bytes - 1))) return TSUCHI_INVALID;
    const char* mount = reinterpret_cast<const char*>(bytes + read.at);
    read.skip(uint32_t(mount_bytes));
    const uint32_t folders = read.u32();
    const size_t folders_at = read.at;
    read.skip(16ull * folders);
    const uint32_t files = read.u32();
    const size_t files_at = read.at;
    read.skip(12ull * files);
    const uint32_t strings = read.u32(), entries = u32_at(toc.bytes + 24);
    if (!read.ok || !folders || strings > (read.size - read.at) / 5) return TSUCHI_INVALID;
    List<const char*> names;
    List<uint32_t> parents, owners, by_entry;
    const auto drop = [&](TSUCHI_Result result) {
        names.clear(), parents.clear(), owners.clear(), by_entry.clear();
        return result;
    };
    if (!names.reserve(strings) || !parents.reserve(folders) || !owners.reserve(files) || !by_entry.reserve(entries)) return drop(TSUCHI_NO_MEMORY);
    for (uint32_t i = 0; i < strings; ++i) {
        const int32_t size = int32_t(read.u32());
        if (size <= 0 || !read.has(size_t(size)) || bytes[read.at + size - 1] != 0 ||
            contains_byte(bytes + read.at, 0, size_t(size - 1))) return drop(TSUCHI_INVALID);
        names.add(reinterpret_cast<const char*>(bytes + read.at));
        read.skip(uint32_t(size));
    }
    for (uint32_t i = 0; i < folders; ++i) parents.add(kNone);
    for (uint32_t i = 0; i < files; ++i) owners.add(kNone);
    for (uint32_t i = 0; i < entries; ++i) by_entry.add(kNone);
    for (uint32_t i = 0; i < folders; ++i) {
        const uint8_t* folder = bytes + folders_at + 16ull * i;
        if (i && u32_at(folder) >= names.count) return drop(TSUCHI_INVALID);
        for (uint32_t child = u32_at(folder + 4); child != kNone;) {
            if (!child || child >= folders || parents[child] != kNone) return drop(TSUCHI_INVALID);
            parents[child] = i;
            child = u32_at(bytes + folders_at + 16ull * child + 8);
        }
        for (uint32_t file = u32_at(folder + 12); file != kNone;) {
            if (file >= files || owners[file] != kNone) return drop(TSUCHI_INVALID);
            owners[file] = i;
            file = u32_at(bytes + files_at + 12ull * file + 4);
        }
    }
    for (uint32_t i = 0; i < files; ++i) {
        const uint32_t entry = u32_at(bytes + files_at + 12ull * i + 8);
        if (entry >= entries || owners[i] == kNone || by_entry[entry] != kNone ||
            u32_at(bytes + files_at + 12ull * i) >= names.count) return drop(TSUCHI_INVALID);
        by_entry[entry] = i;
    }
    while (starts(mount, "../")) mount += 3;
    if (*mount == '/') ++mount;
    const size_t root_bytes = strlen(root);
    for (uint32_t i = 0; i < toc.packages.count; ++i) {
        const uint32_t file = by_entry[toc.entries[i]];
        if (file == kNone) return drop(TSUCHI_INVALID);
        uint32_t chain[24], depth = 0, folder = owners[file];
        while (folder && folder != kNone && depth < 24) {
            chain[depth++] = folder;
            folder = parents[folder];
        }
        if (folder != 0) return drop(TSUCHI_INVALID);
        Texts joined, path;
        bool memory = joined.add(mount), fine = memory;
        for (uint32_t j = depth; fine && j--;) {
            const uint32_t name = u32_at(bytes + folders_at + 16ull * chain[j]);
            if (name >= names.count) { fine = false; break; }
            --joined.used;
            memory = fine = joined.add(names[name], "/");
        }
        const uint32_t name = u32_at(bytes + files_at + 12ull * file);
        char leaf[256];
        fine = fine && name < names.count && strlen(names[name]) < sizeof leaf;
        if (fine) {
            memcpy(leaf, names[name], strlen(names[name]) + 1);
            char* extension = nullptr;
            for (char* at = leaf; *at; ++at) if (*at == '.') extension = at;
            fine = extension && (!strcmp(extension, ".uasset") || !strcmp(extension, ".umap"));
            if (fine) {
                *extension = 0;
                --joined.used;
                memory = fine = joined.add(leaf);
            }
        }
        if (fine) {
            const char* slash = joined.data;
            while (*slash && *slash != '/') ++slash;
            fine = !starts(joined.data, "Engine/") && starts(slash, "/Content/");
            if (fine) memory = fine = path.add("/Game/", slash + 9);
        }
        fine = fine && canonical_package_path(path.data) && starts(path.data, root) && path.data[root_bytes] == '/';
        // The engine finds a package by its identity, so the identity must be its path's. Otherwise
        // a bundle could take the identity of a path below /Game/Carrot4 or of another mod's package.
        fine = fine && container_package_id(path.data) == toc.packages[i];
        joined.clear(), path.clear();
        if (!fine) return drop(memory ? TSUCHI_INVALID : TSUCHI_NO_MEMORY);
    }
    return drop(TSUCHI_OK);
}

} // namespace

TSUCHI_Result mount_asset_container(const char* id, const char* engine_pak_path,
                                   const char* native_toc_path, const char* native_ucas_path,
                                   const char* package_root) {
    if (!id || !valid_mod_id(id) ||
        !engine_pak_path || !ends_with(engine_pak_path, ".pak") ||
        !native_toc_path || !ends_with(native_toc_path, ".utoc") ||
        !native_ucas_path || !ends_with(native_ucas_path, ".ucas") ||
        !canonical_package_path(package_root)) return TSUCHI_INVALID;
    if (census != TSUCHI_OK || !game::files_known() || !t::bound(t::fn::platform_files) ||
        !t::bound(t::fn::find_platform_file) || !t::bound(t::fn::pak_mount)) return TSUCHI_UNSUPPORTED;
    if (asset_mounting) return TSUCHI_BUSY;
    asset_mounting = true;
    struct MountGuard { ~MountGuard() { asset_mounting = false; } } guard;
    // The native paths must name one sibling trio. No filesystem path comes from public arguments.
    const size_t stem = strlen(native_toc_path) - 5;
    if (strlen(native_ucas_path) != stem + 5 || memcmp(native_toc_path, native_ucas_path, stem)) return TSUCHI_INVALID;
    Texts pak;
    if (!pak.add(native_toc_path)) return TSUCHI_NO_MEMORY;
    memcpy(pak.data + stem, ".pak", 5);
    TSUCHI_Result result = empty_asset_pak(pak.data);
    pak.clear();
    if (result != TSUCHI_OK) return result;
    char* bytes = nullptr;
    uint64_t size = 0;
    result = read_bounded(native_toc_path, kMostAssetTableBytes, bytes, size);
    if (result != TSUCHI_OK) return result;
    ContainerToc toc;
    const char* wrong = nullptr;
    List<uint64_t> claims;
    const auto drop = [&] {
        forget_container_toc(toc);
        claims.clear();
        release(bytes);
    };
    if (!read_container_toc(reinterpret_cast<const uint8_t*>(bytes), size_t(size), toc, wrong)) {
        result = wrong && starts(wrong, "there is no memory") ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
        return drop(), result;
    }
    if (toc.id == UINT64_MAX || !toc.packages.count) return drop(), TSUCHI_INVALID;
    if (toc.bytes[80] & kSigned) return drop(), TSUCHI_UNSUPPORTED;
    if (has_id(taken, toc.id)) return drop(), TSUCHI_BUSY;
    result = remember_packages(toc, native_ucas_path, kMostAssetTableBytes, claims, true);
    if (result != TSUCHI_OK) return drop(), result;
    sort_ids(claims);
    for (uint32_t i = 0; i < claims.count; ++i) {
        if ((i && claims[i] == claims[i - 1]) || has_id(packages, claims[i])) return drop(), TSUCHI_BUSY;
    }
    uint32_t headers = 0;
    const uint32_t entries = u32_at(toc.bytes + 24);
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t* chunk = toc.bytes + kTocHeaderBytes + 12ull * i;
        if (chunk[10]) return drop(), TSUCHI_INVALID;
        if (chunk[11] == kContainerHeader) {
            if (++headers != 1 || u64_at(chunk) != toc.id || chunk[8] || chunk[9]) return drop(), TSUCHI_INVALID;
        } else if (chunk[11] < kExportBundleData || chunk[11] > 5 || !has_id(claims, u64_at(chunk))) return drop(), TSUCHI_INVALID;
    }
    if (headers != 1) return drop(), TSUCHI_INVALID;
    result = package_paths_below(toc, package_root);
    if (result != TSUCHI_OK) return drop(), result;
    if (taken.count == UINT32_MAX || claims.count > UINT32_MAX - packages.count ||
        !taken.reserve(taken.count + 1) || !packages.reserve(packages.count + claims.count)) return drop(), TSUCHI_NO_MEMORY;
    const bool holder_known = container_mounted_for(id);
    if (!holder_known && (asset_holders.count == UINT32_MAX || !asset_holders.reserve(asset_holders.count + 1))) return drop(), TSUCHI_NO_MEMORY;
    const uint32_t raise = container_order_raise(engine_pak_path);
    if (raise >= next_mount_order || !engine_mount_path(engine_pak_path, next_mount_order - raise)) return drop(), TSUCHI_IO;
    taken.add(toc.id);
    for (uint32_t i = 0; i < claims.count; ++i) packages.add(claims[i]);
    sort_ids(taken), sort_ids(packages);
    if (!holder_known) {
        Holder holder{};
        memcpy(holder.id, id, strlen(id) + 1);
        asset_holders.add(holder);
    }
    ++next_mount_order, ++asset_mounts;
    drop();
    return TSUCHI_OK;
}

uint32_t containers_mounted() { return holders.count + asset_mounts; }

bool container_mounted_for(const char* id) {
    for (uint32_t i = 0; i < holders.count; ++i)
        if (!strcmp(holders[i].id, id)) return true;
    for (uint32_t i = 0; i < asset_holders.count; ++i)
        if (!strcmp(asset_holders[i].id, id)) return true;
    return false;
}

void tell_containers_left_mounted() {
    for (uint32_t i = 0; i < mod_count(); ++i)
        if (mod_at(i)->state == ModState::blocked && container_mounted_for(mod_at(i)->id))
            mod_note(mod_at(i)->id, "it was blocked as it started, and its container stays mounted until the game is closed");
}

} // namespace tsuchi
