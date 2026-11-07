// A shader set: the shaders of a mod's own materials, compiled for the game, which the mod's manifest names
// under `shaders`.
//
//   "P4MSHDR3", u32 materials, then for each material:
//     u16 + text   the mod's material: the path of a material instance of the mod's own, /Game/Folder/MI_Lava.MI_Lava
//     u16 + text   the game material it stands on: the path of that material's package
//     20 bytes     the hash the game knows that game material's shader map by
//     u32          shaders in that shader map
//     u32          shaders of the mod's, then for each:
//       u32 its place in the shader map, u8 its kind (3: a pixel shader), 20 bytes its hash, u32 its size, and
//       the shader as the game's library holds one, not compressed
//     u32 0, or u32 1 and the edits that make the game's description of the shader map into the material's own:
//       u32 the size of the game's description, u32 the size of the new one, u32 operations, then for each:
//         u8 0, u32 offset, u32 length   that stretch of the game's description
//         u8 1, u32 length, bytes        new bytes
//       u32 names, then for each: u16 + text, u32 number, u32 places, and a u32 offset for each place: where in
//       the new description the name is written, in 12 bytes, once the engine has given it its number
//
// Whole numbers are stored low byte first, with no padding. The file holds nothing of the game's: it names a
// game material by its path, and says what the mod's shaders were compiled against.
//
// A file with another mark at its start, or with any field out of range, is refused whole. The hashes are not
// worked out here: the one of the shader map is for whoever applies the set to compare with the running game's,
// and a shader's own names it to the engine. No library is called, and the caller owns every buffer.
#pragma once
#include "content_path.h"
#include <stdint.h>

namespace tsuchi::shaders {

constexpr uint32_t kMaterialLimit = 1024;      // materials in one set
constexpr uint32_t kPathLimit = 511;           // a path
constexpr uint32_t kMapShaderLimit = 4096;     // shaders in one shader map
constexpr uint32_t kShaderLimit = 1u << 20;    // the bytes of one shader
constexpr uint32_t kContentLimit = 1u << 18;   // the bytes of a shader map's description
constexpr uint32_t kNameLimit = 127;           // a name in a description
constexpr uint32_t kHashBytes = 20;
constexpr uint32_t kNameBytes = 12;            // a name as a description holds one
constexpr uint8_t kPixel = 3;

// Places are counted from the start of the file. Text has no terminator there: its length says where it ends.
struct Shader {
    uint32_t place;         // in the shader map
    uint32_t hash;          // where its 20 bytes are
    uint32_t payload, size; // the shader itself
};
struct Material {
    uint32_t material, material_length; // the mod's material, by its path
    uint32_t game_material, game_material_length;
    uint32_t leaf, leaf_length;         // the last part of the game material's path: what the material in that package is called
    uint32_t map_hash, map_shaders;
    uint32_t first_shader, shader_count;
    // A description of its own. `old_size` is 0 for a material that keeps the game material's.
    uint32_t old_size, new_size, operations, operation_count, names, name_count;
};
struct File {
    const uint8_t* data;
    uint32_t size;
    Material* materials;
    uint32_t material_capacity, material_count;
    Shader* shaders;
    uint32_t shader_capacity, shader_count;
};
struct Counts {
    uint32_t materials, shaders;
};
enum class Read : uint32_t { ok, not_a_set, cut_short, bad_value, full };

namespace detail {
inline uint32_t u32_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint32_t u16_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline bool same(const uint8_t* a, const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
// A path of plain characters, from the top: every part between slashes has something in it. An object's path
// ends in one dot and the object's name; a package's has no dot. `leaf` gets where the last part begins.
inline bool path(const uint8_t* text, uint32_t length, bool object, uint32_t& leaf) {
    if (length < 2 || length > kPathLimit || text[0] != '/') return false;
    uint32_t dots = 0;
    leaf = 0;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t c = text[i];
        if (c == '/') {
            if (dots || i + 1 == length || text[i + 1] == '/') return false;
            leaf = i + 1;
        } else if (c == '.') {
            if (++dots > 1 || i == leaf || i + 1 == length) return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return (object ? dots == 1 : dots == 0) && !content_path::wrong(reinterpret_cast<const char*>(text),length,object ? content_path::Names::object : content_path::Names::package);
}

// Goes through the file once. With tables, fills them from `counts` on; without, only counts. Whatever it
// answers but `ok`, `counts` is as it was.
inline Read walk(const uint8_t* d, uint32_t size, Material* materials, Shader* shaders, Counts& counts) {
    static const char mark[] = "P4MSHDR3";
    if (size < 12) return Read::not_a_set;
    for (uint32_t i = 0; i < 8; ++i)
        if (d[i] != uint8_t(mark[i])) return Read::not_a_set;
    const uint32_t count = u32_at(d + 8);
    if (count > kMaterialLimit) return Read::bad_value;
    Counts now = counts;
    uint32_t at = 12;
    uint32_t named_at[kMaterialLimit]; // where each material so far is named
    uint16_t named_length[kMaterialLimit];
    for (uint32_t m = 0; m < count; ++m) {
        Material row{};
        for (uint32_t which = 0; which < 2; ++which) {
            if (size - at < 2) return Read::cut_short;
            const uint32_t length = u16_at(d + at);
            at += 2;
            if (size - at < length) return Read::cut_short;
            uint32_t leaf = 0;
            if (!path(d + at, length, which == 0, leaf)) return Read::bad_value;
            if (which == 0) row.material = at, row.material_length = length;
            else row.game_material = at, row.game_material_length = length, row.leaf = at + leaf, row.leaf_length = length - leaf;
            at += length;
        }
        // One entry for one material of the mod's.
        for (uint32_t earlier = 0; earlier < m; ++earlier)
            if (named_length[earlier] == row.material_length && same(d + named_at[earlier], d + row.material, row.material_length)) return Read::bad_value;
        named_at[m] = row.material, named_length[m] = uint16_t(row.material_length);
        if (size - at < kHashBytes + 8) return Read::cut_short;
        row.map_hash = at, row.map_shaders = u32_at(d + at + kHashBytes);
        const uint32_t own = u32_at(d + at + kHashBytes + 4);
        at += kHashBytes + 8;
        if (!row.map_shaders || row.map_shaders > kMapShaderLimit || !own || own > row.map_shaders) return Read::bad_value;
        row.first_shader = now.shaders, row.shader_count = own;
        uint8_t taken[kMapShaderLimit / 8] = {}; // the places this material's shaders have taken so far
        for (uint32_t s = 0; s < own; ++s) {
            if (size - at < 9 + kHashBytes) return Read::cut_short;
            Shader shader{};
            shader.place = u32_at(d + at), shader.hash = at + 5, shader.size = u32_at(d + at + 5 + kHashBytes), shader.payload = at + 9 + kHashBytes;
            if (shader.place >= row.map_shaders || d[at + 4] != kPixel || !shader.size || shader.size > kShaderLimit) return Read::bad_value;
            if (taken[shader.place >> 3] & (1u << (shader.place & 7))) return Read::bad_value;
            taken[shader.place >> 3] |= uint8_t(1u << (shader.place & 7));
            at += 9 + kHashBytes;
            if (size - at < shader.size) return Read::cut_short;
            at += shader.size;
            if (shaders) shaders[now.shaders] = shader;
            ++now.shaders;
        }
        if (size - at < 4) return Read::cut_short;
        const uint32_t edited = u32_at(d + at);
        at += 4;
        if (edited > 1) return Read::bad_value;
        if (edited) {
            if (size - at < 12) return Read::cut_short;
            row.old_size = u32_at(d + at), row.new_size = u32_at(d + at + 4), row.operation_count = u32_at(d + at + 8);
            at += 12, row.operations = at;
            if (!row.old_size || row.old_size > kContentLimit || !row.new_size || row.new_size > kContentLimit || row.operation_count > row.new_size) return Read::bad_value;
            uint32_t made = 0;
            for (uint32_t o = 0; o < row.operation_count; ++o) {
                if (size - at < 5) return Read::cut_short;
                if (d[at] == 0) {
                    if (size - at < 9) return Read::cut_short;
                    const uint32_t from = u32_at(d + at + 1), length = u32_at(d + at + 5);
                    if (from > row.old_size || length > row.old_size - from || length > row.new_size - made) return Read::bad_value;
                    made += length, at += 9;
                } else if (d[at] == 1) {
                    const uint32_t length = u32_at(d + at + 1);
                    at += 5;
                    if (length > row.new_size - made) return Read::bad_value;
                    if (size - at < length) return Read::cut_short;
                    made += length, at += length;
                } else {
                    return Read::bad_value;
                }
            }
            if (made != row.new_size) return Read::bad_value;
            if (size - at < 4) return Read::cut_short;
            row.name_count = u32_at(d + at);
            at += 4, row.names = at;
            if (row.name_count > row.new_size) return Read::bad_value;
            for (uint32_t n = 0; n < row.name_count; ++n) {
                if (size - at < 2) return Read::cut_short;
                const uint32_t length = u16_at(d + at);
                at += 2;
                if (!length || length > kNameLimit) return Read::bad_value;
                if (size - at < length || size - at - length < 8) return Read::cut_short;
                for (uint32_t i = 0; i < length; ++i)
                    if (d[at + i] < 0x20 || d[at + i] > 0x7E) return Read::bad_value;
                const uint32_t places = u32_at(d + at + length + 4);
                at += length + 8;
                if (places > row.new_size / kNameBytes) return Read::bad_value;
                if ((size - at) / 4 < places) return Read::cut_short;
                for (uint32_t k = 0; k < places; ++k) {
                    const uint32_t where = u32_at(d + at + 4 * k);
                    if (where > row.new_size || row.new_size - where < kNameBytes) return Read::bad_value;
                }
                at += 4 * places;
            }
        }
        if (materials) materials[now.materials] = row;
        ++now.materials;
    }
    if (at != size) return Read::bad_value;
    counts = now;
    return Read::ok;
}
} // namespace detail

// How many materials and shaders a file holds, added to `counts`. Anything but `ok` says why the file is no
// shader set, and `counts` is as it was.
inline Read measure(const uint8_t* data, uint32_t size, Counts& counts) { return detail::walk(data, size, nullptr, nullptr, counts); }

// Reads `file->data` into the two tables, after what they already hold. On anything but `ok` the counts are as
// they were.
inline Read read(File* file) {
    Counts more{};
    const Read measured = measure(file->data, file->size, more);
    if (measured != Read::ok) return measured;
    if (more.materials > file->material_capacity - file->material_count || more.shaders > file->shader_capacity - file->shader_count) return Read::full;
    Counts counts{file->material_count, file->shader_count};
    const Read result = detail::walk(file->data, file->size, file->materials, file->shaders, counts);
    if (result == Read::ok) file->material_count = counts.materials, file->shader_count = counts.shaders;
    return result;
}

// A material's own description from the game's: `old_size` bytes at `old`, `new_size` bytes at `out`. Where a
// name goes, the bytes are whatever was copied: write each with each_name afterward.
inline void build_content(const uint8_t* file, const Material& row, const uint8_t* old, uint8_t* out) {
    using namespace detail;
    uint32_t at = row.operations, made = 0;
    for (uint32_t o = 0; o < row.operation_count; ++o) {
        if (file[at] == 0) {
            const uint32_t from = u32_at(file + at + 1), length = u32_at(file + at + 5);
            for (uint32_t i = 0; i < length; ++i) out[made + i] = old[from + i];
            made += length, at += 9;
        } else {
            const uint32_t length = u32_at(file + at + 1);
            for (uint32_t i = 0; i < length; ++i) out[made + i] = file[at + 5 + i];
            made += length, at += 5 + length;
        }
    }
}

// Calls visit(text, length, number, place) once for every place of every name of a material that was read.
template <class Visit>
inline void each_name(const uint8_t* file, const Material& row, Visit&& visit) {
    using namespace detail;
    uint32_t at = row.names;
    for (uint32_t n = 0; n < row.name_count; ++n) {
        const uint32_t length = u16_at(file + at);
        const uint8_t* text = file + at + 2;
        const uint32_t number = u32_at(text + length), places = u32_at(text + length + 4);
        at += 2 + length + 8;
        for (uint32_t k = 0; k < places; ++k) visit(text, length, number, u32_at(file + at + 4 * k));
        at += 4 * places;
    }
}

} // namespace tsuchi::shaders
