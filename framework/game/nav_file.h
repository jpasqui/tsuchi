// A navigation file: a mod's own navigation for the game's maps, which a `maps` entry of its manifest names.
//
//   "P4MNAVM1", u32 maps, then for each map:
//     u16 + text   the game's map, by the path of its package
//     u32          navmeshes, then for each:
//       u16 + text the navmesh actor's name in that map
//       u32 size, then what the engine serializes for such a navmesh: int32 0, int32 version 13, uint32 size,
//                  int32 tiles, the navmesh's parameters, then every tile as the engine writes one, each with
//                  an empty cache
//
// The game keeps a map's navigation in the map's own package, which a mod cannot ship. The mod's own goes here,
// and is put in the game's place when the map loads. This reads the file and builds the block of memory the
// navmesh takes for a tile from the tile as it is stored, as the engine's own loader does. The layouts are those
// of the game's navmesh: 64-bit polygon references, with cluster and segment links.
//
// A file with another mark at its start, or with any field out of range, is refused whole. No library is
// called, and the caller owns every buffer.
#pragma once
#include <stdint.h>

namespace tsuchi::nav {

constexpr uint32_t kPathLimit = 255;
constexpr uint32_t kNameLimit = 127;
constexpr uint32_t kTileLimit = 4096;    // tiles in one navmesh
constexpr int32_t kCountLimit = 1000000; // records of one kind in one tile
constexpr uint32_t kVersion = 13;        // of the engine's stored navmesh
constexpr uint32_t kTileMagic = 0x444E4156, kTileVersion = 7;

// The sizes of a tile's records in memory.
constexpr uint32_t kHeader = 116, kPoly = 32, kLink = 16, kDetailMesh = 12, kBvNode = 16, kOffMesh = 40, kSegment = 64, kCluster = 20;
// The stored sizes of the records whose stored form is another than the one in memory.
constexpr uint32_t kDetailMeshStored = 10, kOffMeshStored = 36, kSegmentStored = 60, kClusterStored = 12;
// A stored tile with nothing in it: its ten counts, its header, and the size of its empty cache.
constexpr uint32_t kEmptyTile = 40 + kHeader + 4;

// Places are counted from the start of the file.
struct Navmesh {
    uint32_t name, name_length, payload, payload_size, tile_count, max_tiles;
};
struct Map {
    uint32_t path, path_length, first_navmesh, navmesh_count;
};
struct File {
    const uint8_t* data;
    uint32_t size;
    Map* maps;
    uint32_t map_capacity, map_count;
    Navmesh* navmeshes;
    uint32_t navmesh_capacity, navmesh_count;
};
enum class Read : uint32_t { ok, not_navigation, cut_short, bad_value, full };

// One stored tile: where its parts are, and how large its block is in memory.
struct Tile {
    const uint8_t* at; // the ten counts
    int32_t verts, polys, links, detail_meshes, detail_verts, detail_tris, bv_nodes, off_mesh, segments, clusters;
    int32_t x, y, layer;
    uint32_t stored, memory; // its bytes in the file from `at`, the size of its cache included; the bytes of its block
};

namespace detail {
inline uint32_t u32_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline int32_t i32_at(const uint8_t* p) { return int32_t(u32_at(p)); }
inline uint32_t u16_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t align4(uint32_t n) { return (n + 3) & ~3u; }
inline void copy(uint8_t* to, const uint8_t* from, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) to[i] = from[i];
}
inline bool plain(const uint8_t* text, uint32_t length, uint32_t limit, bool path) {
    if (!length || length > limit || (path && text[0] != '/')) return false;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (path && c == '/'))) return false;
    }
    return true;
}
} // namespace detail

// Reads the tile at `at`: its ten counts, after the tile's reference and declared size. False when it does not
// fit in `available` bytes or a value is out of range.
inline bool read_tile(const uint8_t* at, uint32_t available, Tile* out) {
    using namespace detail;
    if (available < 40 + kHeader) return false;
    Tile t{};
    t.at = at;
    int32_t* const counts[10] = {&t.verts, &t.polys, &t.links, &t.detail_meshes, &t.detail_verts, &t.detail_tris, &t.bv_nodes, &t.off_mesh, &t.segments, &t.clusters};
    for (uint32_t i = 0; i < 10; ++i) {
        *counts[i] = i32_at(at + 4 * i);
        if (*counts[i] < 0 || *counts[i] > kCountLimit) return false;
    }
    const uint8_t* h = at + 40;
    if (u32_at(h) != kTileMagic || u32_at(h + 4) != kTileVersion) return false;
    t.x = i32_at(h + 8), t.y = i32_at(h + 12), t.layer = i32_at(h + 16);
    // The header says the counts again: polys, verts, links, detail meshes, detail verts, detail tris, bv nodes, off-mesh.
    if (i32_at(h + 24) != t.polys || i32_at(h + 28) != t.verts || i32_at(h + 32) != t.links || i32_at(h + 36) != t.detail_meshes ||
        i32_at(h + 40) != t.detail_verts || i32_at(h + 44) != t.detail_tris || i32_at(h + 48) != t.bv_nodes || i32_at(h + 52) != t.off_mesh ||
        i32_at(h + 100) != t.clusters || i32_at(h + 104) != t.segments)
        return false;
    const uint64_t stored = 40ull + kHeader + 12ull * t.verts + uint64_t(kPoly) * t.polys + uint64_t(kDetailMeshStored) * t.detail_meshes + 12ull * t.detail_verts +
                            4ull * t.detail_tris + uint64_t(kBvNode) * t.bv_nodes + uint64_t(kOffMeshStored) * t.off_mesh + 4ull * t.off_mesh +
                            uint64_t(kSegmentStored) * t.segments + uint64_t(kClusterStored) * t.clusters + 2ull * t.detail_meshes + 4;
    const uint64_t memory = uint64_t(align4(kHeader)) + align4(12u * t.verts) + align4(kPoly * t.polys) + align4(kLink * t.links) + align4(kDetailMesh * t.detail_meshes) +
                            align4(12u * t.detail_verts) + align4(4u * t.detail_tris) + align4(kBvNode * t.bv_nodes) + align4(kOffMesh * t.off_mesh) +
                            align4(kSegment * t.segments) + align4(kCluster * t.clusters) + align4(2u * t.detail_meshes);
    if (stored > available || memory > (64u << 20)) return false;
    if (u32_at(at + stored - 4) != 0) return false; // a compressed cache: the game's maps have none
    t.stored = uint32_t(stored), t.memory = uint32_t(memory);
    *out = t;
    return true;
}

// The tile's block as the navmesh takes one: `t.memory` bytes at `out`.
inline void build_tile(const Tile& t, uint8_t* out) {
    using namespace detail;
    for (uint32_t i = 0; i < t.memory; ++i) out[i] = 0;
    const uint8_t* s = t.at + 40;
    uint8_t* d = out;
    // The header, stored: 15 whole numbers, 10 numbers, how many clusters, how many segments, where the segments'
    // polygons begin, where their vertices begin. In memory the three words of the segments and the count of
    // clusters come before the numbers.
    copy(d, s, 60), copy(d + 60, s + 104, 12), copy(d + 72, s + 100, 4), copy(d + 76, s + 60, 40);
    s += kHeader, d += align4(kHeader);
    copy(d, s, 12u * t.verts);
    s += 12u * t.verts, d += align4(12u * t.verts);
    copy(d, s, kPoly * t.polys);
    s += kPoly * t.polys, d += align4(kPoly * t.polys);
    d += align4(kLink * t.links); // the links are made when the tile is added
    for (int32_t i = 0; i < t.detail_meshes; ++i) copy(d + kDetailMesh * i, s + kDetailMeshStored * i, kDetailMeshStored);
    s += kDetailMeshStored * t.detail_meshes, d += align4(kDetailMesh * t.detail_meshes);
    copy(d, s, 12u * t.detail_verts);
    s += 12u * t.detail_verts, d += align4(12u * t.detail_verts);
    copy(d, s, 4u * t.detail_tris);
    s += 4u * t.detail_tris, d += align4(4u * t.detail_tris);
    copy(d, s, kBvNode * t.bv_nodes);
    s += kBvNode * t.bv_nodes, d += align4(kBvNode * t.bv_nodes);
    // A connection off the mesh, stored: six numbers of place, the radius, the polygon in 16 bits, flags and side in
    // 8 each, the user's number; and the heights after all of them. In memory: place, radius, height, the user's
    // number, polygon, side, flags.
    const uint8_t* heights = s + kOffMeshStored * t.off_mesh;
    for (int32_t i = 0; i < t.off_mesh; ++i) {
        const uint8_t* from = s + kOffMeshStored * i;
        uint8_t* to = d + kOffMesh * i;
        copy(to, from, 28), copy(to + 28, heights + 4 * i, 4), copy(to + 32, from + 32, 4), copy(to + 36, from + 28, 2);
        to[38] = from[31], to[39] = from[30];
    }
    s = heights + 4u * t.off_mesh, d += align4(kOffMesh * t.off_mesh);
    // A segment, stored: start A, start B, end A, end B, radius, first polygon in 16 bits, polygons and flags in 8
    // each, the user's number. In memory: start A, end A, start B, end B, radius, height, the user's number, first
    // polygon, polygons, flags.
    for (int32_t i = 0; i < t.segments; ++i) {
        const uint8_t* from = s + kSegmentStored * i;
        uint8_t* to = d + kSegment * i;
        copy(to, from, 12), copy(to + 12, from + 24, 12), copy(to + 24, from + 12, 12), copy(to + 36, from + 36, 12), copy(to + 48, from + 48, 4);
        copy(to + 56, from + 56, 4), copy(to + 60, from + 52, 4);
    }
    s += kSegmentStored * t.segments, d += align4(kSegment * t.segments);
    for (int32_t i = 0; i < t.clusters; ++i) copy(d + kCluster * i, s + kClusterStored * i, kClusterStored); // its links are made when the tile is added
    s += kClusterStored * t.clusters, d += align4(kCluster * t.clusters);
    copy(d, s, 2u * t.detail_meshes);
}

// Calls visit(const Tile&) for every tile of a navmesh the file was read for. Stops when it returns false.
template <class Visit>
inline void each_tile(const uint8_t* file, const Navmesh& mesh, Visit&& visit) {
    const uint8_t* payload = file + mesh.payload;
    uint32_t at = 16 + 28; // what the engine writes first, then the navmesh's parameters
    for (uint32_t i = 0; i < mesh.tile_count; ++i) {
        Tile t{};
        if (!read_tile(payload + at + 12, mesh.payload_size - at - 12, &t)) return;
        if (!visit(t)) return;
        at += 12 + t.stored;
    }
}

// Reads `file->data` into the two tables, after what they already hold. On anything but `ok` the counts are as
// they were.
inline Read read(File* file) {
    using namespace detail;
    const uint8_t* d = file->data;
    const uint32_t size = file->size;
    static const char mark[] = "P4MNAVM1";
    if (size < 12) return Read::not_navigation;
    for (uint32_t i = 0; i < 8; ++i)
        if (d[i] != uint8_t(mark[i])) return Read::not_navigation;
    const uint32_t maps = u32_at(d + 8);
    const uint32_t first_map = file->map_count, first_mesh = file->navmesh_count;
    const auto fail = [&](Read why) {
        file->map_count = first_map, file->navmesh_count = first_mesh;
        return why;
    };
    uint32_t at = 12;
    for (uint32_t m = 0; m < maps; ++m) {
        if (file->map_count == file->map_capacity) return fail(Read::full);
        if (size - at < 2) return fail(Read::cut_short);
        Map map{};
        map.path_length = u16_at(d + at), map.path = at + 2;
        at += 2;
        if (size - at < map.path_length || size - at - map.path_length < 4) return fail(Read::cut_short);
        if (!plain(d + at, map.path_length, kPathLimit, true)) return fail(Read::bad_value);
        at += map.path_length;
        const uint32_t meshes = u32_at(d + at);
        at += 4;
        map.first_navmesh = file->navmesh_count;
        for (uint32_t n = 0; n < meshes; ++n) {
            if (file->navmesh_count == file->navmesh_capacity) return fail(Read::full);
            if (size - at < 2) return fail(Read::cut_short);
            Navmesh mesh{};
            mesh.name_length = u16_at(d + at), mesh.name = at + 2;
            at += 2;
            if (size - at < mesh.name_length || size - at - mesh.name_length < 4) return fail(Read::cut_short);
            if (!plain(d + at, mesh.name_length, kNameLimit, false)) return fail(Read::bad_value);
            at += mesh.name_length;
            mesh.payload_size = u32_at(d + at), mesh.payload = at + 4;
            at += 4;
            if (size - at < mesh.payload_size) return fail(Read::cut_short);
            const uint8_t* p = d + at;
            if (mesh.payload_size < 44 || u32_at(p) != 0 || u32_at(p + 4) != kVersion || u32_at(p + 8) != mesh.payload_size - 8) return fail(Read::bad_value);
            mesh.tile_count = u32_at(p + 12), mesh.max_tiles = u32_at(p + 36);
            if (!mesh.tile_count || mesh.tile_count > kTileLimit || mesh.max_tiles < mesh.tile_count) return fail(Read::bad_value);
            uint32_t tile_at = 44;
            for (uint32_t i = 0; i < mesh.tile_count; ++i) {
                Tile t{};
                if (mesh.payload_size - tile_at < 12 || !read_tile(p + tile_at + 12, mesh.payload_size - tile_at - 12, &t)) return fail(Read::bad_value);
                tile_at += 12 + t.stored;
            }
            if (tile_at != mesh.payload_size) return fail(Read::bad_value);
            at += mesh.payload_size;
            file->navmeshes[file->navmesh_count++] = mesh;
        }
        map.navmesh_count = meshes;
        file->maps[file->map_count++] = map;
    }
    if (at != size) return fail(Read::bad_value);
    return Read::ok;
}

} // namespace tsuchi::nav
