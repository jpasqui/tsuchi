// The manifest's "shaders": a mod's own materials drawn with the shaders the mod brings.
//
//   "shaders": "shaders.bin"
//
// names a shader set in the mod's folder (shader_file.h). The game finds a material's shaders in its own
// library, by a hash the material's package carries, so a package of a mod's cannot bring shaders. A mod's
// material is put together here instead, from an instance of a game material that the mod ships and the pixel
// shaders its set holds for that game material's shader map:
//
//   1. The engine loads the game material's package a second time under another name, through the function a
//      Blueprint loads a level with: the package holds no level, so none comes, and the package's objects stay.
//      The copy is a material of its own, and is kept for as long as the game runs.
//   2. The copy gets a shader map of its own: the game's shader map and its list of shaders copied into the
//      framework's memory, the list empty but for the mod's shaders, each made by the game's renderer from
//      the file. The engine fills a place that is empty from the game's library when it first draws with it.
//      A material with textures of its own also gets a description of its own, built from the game's by the
//      set's edits. The game's own material and its shader map are not changed.
//   3. In each world a stand-in is made: a dynamic instance of the copy with the values of the mod's
//      instance. It is put in the mesh, particle, decal or Image slot that uses the mod's instance.
//
// The copies in the framework's memory are never freed by the engine: their counts of holders start high. A
// mod that stops takes its stand-ins out of the slots; its copies and their shaders stay, since the renderer
// may be drawing with them.
#include "shader_materials.h"

#include <string.h>

#include "actor_events.h"
#include "actors.h"
#include "engine_call.h"
#include "files.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "materials.h"
#include "content_path.h"
#include "mods.h"
#include "names.h"
#include "objects.h"
#include "object_retention.h"
#include "parts.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "shader_file.h"
#include "shader_archive_manifest.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using game::read_or;
using maps::join;
using maps::tell;

const Kind kSet{"shaders.set"};
game::ClassRef instance_class{"MaterialInstanceConstant"};

constexpr uint32_t kCopyWait = 900;       // steps a copy is waited for
constexpr uint32_t kMostSlots = 64;       // slots of one mesh that are looked at
constexpr uint32_t kPartsAStep = 128;     // meshes asked for their materials in one step
constexpr uint64_t kAskMicroseconds = 2000; // and the most time a step spends asking them, where there is a clock
constexpr uint64_t kWalkMicroseconds = 2000; // the most time a step spends walking the engine's objects for meshes
constexpr uint32_t kRequeueEvery = 4;       // users other than meshes are queued again by one walk in this many
constexpr int32_t kHeldForGood = 1 << 29; // a count of holders the engine never brings down to nothing

enum class Stage : uint8_t {
    waiting, // its copy has not been asked for, or did not come in an earlier world
    asked,   // the engine was asked for its copy
    came,    // the copy is there; it gets its shaders at the next look, once the engine has finished with it
    ready,   // the copy has the mod's shaders
    failed,  // it keeps the game material's shaders for as long as its mod runs
};
// A slot a stand-in was put in, and what had been put there over the mesh's own material before: nothing, or
// the mod's instance. Handles.
struct Placed {
    uint64_t part;
    uint32_t slot;
    uint64_t source; // the source handle still owned by this material
};
// One material of a set, as it stands in the running game. `values` is the signature of the source's values that
// were last copied into the stand-in, or 0 when they have to be copied again.
struct Derived { uint64_t source, stand_in; uint32_t updated_at; bool copied; uint64_t values; };
struct Material {
    Stage stage;
    uint32_t asked_at;              // the world's step at which its copy was asked for
    uint32_t stand_in_tried;        // the world's step at which a stand-in was last tried for it, or 0
    uint64_t gave_up_in;            // the world in which its copy did not come: it is asked for again in the next
    game::Name name, game_name;     // what the mod's instance is called, and what the game material is
    uint64_t copy, plain, stand_in; // handles: the copy of the game material, the mod's instance once it is loaded, the stand-in in this world
    Text path, package;             // the mod's instance by its path, and the game material's package
    List<Placed> placed;            // in this world
    List<Derived> derived;          // one stand-in for each descendant instance
};
// One running mod's shader set.
struct Set {
    uint64_t id;
    TSUCHI_Mod *mod;
    uint8_t *bytes;
    shaders::Material *rows;
    shaders::Shader *programs;
    Material *materials;
    uint32_t count;
};
List<Set> sets;
uint64_t next_set = 1;
Set* set_by_id(uint64_t id) {
    for (uint32_t i = 0; i < sets.count; ++i) if (sets[i].id == id) return &sets[i];
    return nullptr;
}
uint64_t next_live_set(uint64_t after, uint64_t limit) {
    uint64_t found = limit;
    for (uint32_t i = 0; i < sets.count; ++i)
        if (sets[i].id > after && sets[i].id < found) found = sets[i].id;
    return found == limit ? 0 : found;
}
List<uint64_t> taken; // every copy that was taken for a material since the game started, by its handle
ShaderCounts counts{};

uint64_t world_now = 0; // the loaded world's handle
uint32_t world_steps = 0;

// A material that can be put in slots now: the mod's instance, and the stand-in for it. Addresses, good for
// the step they were gathered in.
struct Pair {
    uint64_t plain, stand_in;
    Material *material;
};
List<Pair> pairs;
List<uint64_t> entered; // actors of the game's that entered play since the last step
List<uint64_t> unasked; // meshes that have not been asked for their materials yet

// The meshes that were asked for their materials in this world, by their handles.
struct Seen {
    uint64_t *slots = nullptr;
    uint32_t capacity = 0, count = 0;

    static uint32_t first(uint64_t handle, uint32_t capacity) { return uint32_t((handle * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1); }
    bool has(uint64_t handle) const {
        for (uint32_t i = capacity ? first(handle, capacity) : 0; capacity && slots[i]; i = (i + 1) & (capacity - 1))
            if (slots[i] == handle) return true;
        return false;
    }
    // False when there is no memory to remember it by.
    bool add(uint64_t handle) {
        if (has(handle)) return true;
        if (count * 2 >= capacity) {
            const uint32_t next = capacity ? capacity * 2 : 1024;
            auto *grown = static_cast<uint64_t *>(allocate(sizeof(uint64_t) * next));
            if (!grown) return false;
            memset(grown, 0, sizeof(uint64_t) * next);
            for (uint32_t i = 0; i < capacity; ++i) {
                if (!slots[i]) continue;
                uint32_t at = first(slots[i], next);
                while (grown[at]) at = (at + 1) & (next - 1);
                grown[at] = slots[i];
            }
            release(slots);
            slots = grown, capacity = next;
        }
        uint32_t at = first(handle, capacity);
        while (slots[at]) at = (at + 1) & (capacity - 1);
        slots[at] = handle, ++count;
        return true;
    }
    void forget() {
        if (slots) memset(slots, 0, sizeof(uint64_t) * capacity);
        count = 0;
    }
};
Seen seen;
Seen queued; // the users other than meshes that wait in `unasked`, so a look that comes before they are asked adds none twice

// ---- Small tools ----------------------------------------------------------------------------------

bool is_template(uint64_t object) { return (read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags) != 0; }
bool same(game::Name a, game::Name b) { return a.index == b.index && a.number == b.number; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
// The engine's paths take no account of the case of letters.
bool same_path(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
bool same_path(const char *a, const char *b) {
    const size_t length = strlen(a);
    return length == strlen(b) && same_path(a, b, length);
}

// The memory two threads read is as it was written before the next write.
void settle() {
#if defined(__aarch64__)
    asm volatile("dmb ish" ::: "memory");
#endif
}

// Every stand-in that was made is put where its material is; so the meshes are looked at afresh.
void forget_walk();
// Counts every change to what a material may be drawn as: a stand-in made or let go, a copy ready, a copy given up.
// A held slot whose material and count are as they were last step is not resolved again.
uint32_t generation = 1;
void look_afresh() {
    ++generation;
    seen.forget();
    queued.forget();
    unasked.count = 0;
    forget_walk(); // a walk under way begins again, and queues every user again
}

// Where a material instance keeps its Parent, by its class: found once by reflection, and read directly for every
// object of that class after that. A class without the field is asked again each time, since a class can be given
// one later only in the made-up game the framework is checked against, and asking costs no more than before.
struct ParentField { uint64_t type, property; uint64_t offset; };
ParentField parent_fields[16];
uint32_t parent_field_count = 0;
TSUCHI_Result find_parent(uint64_t type, uint64_t object, game::Place &out) {
    for (uint32_t i = 0; i < parent_field_count; ++i)
        if (parent_fields[i].type == type) {
            out = game::Place{object + parent_fields[i].offset, parent_fields[i].property, false};
            return game::readable(out.address, 8) ? TSUCHI_OK : TSUCHI_NOT_FOUND;
        }
    const TSUCHI_Result found = game::find_place(type, object, "Parent", out);
    if (found == TSUCHI_OK && parent_field_count < 16 && out.address >= object)
        parent_fields[parent_field_count++] = ParentField{type, out.property, out.address - object};
    return found;
}

// ---- A material of the game's, and whether a set fits it ---------------------------------------------

// Where a material keeps its compiled shaders.
struct Shape {
    uint64_t resource;     // what the material is rendered through
    uint64_t map;          // its shader map
    uint64_t map_resource; // the shader map's list of shaders, as the renderer made them
};
enum class Fit : uint8_t {
    fits,
    not_yet,       // the engine has not finished with the material
    other_shape,   // it is no material with one shader map of its own
    other_shaders, // its shader map is not the one the set was compiled for
};

// Reads through a material instance to its shader map, and compares that with what the set says of it: its
// hash, how many shaders it has, and for a material with a description of its own, the size of the game's.
// With `settled`, the renderer's side of the material has to have the shader map as well.
Fit fit_of(uint64_t material, const uint8_t *file, const shaders::Material &row, Shape &shape, bool settled) {
    namespace m = t::shader_map;
    namespace r = t::shader_map_resource;
    const uint64_t list = material + t::material_instance::resources;
    const uint64_t data = read_or<uint64_t>(list + t::array::data);
    const int32_t count = read_or<int32_t>(list + t::array::count);
    if (!data || count <= 0) return Fit::not_yet;
    if (count != 1) return Fit::other_shape;
    shape.resource = read_or<uint64_t>(data);
    if (!shape.resource || read_or<uint64_t>(shape.resource + t::material_resource::instance) != material) return Fit::other_shape;
    shape.map = read_or<uint64_t>(shape.resource + t::material_resource::game_map);
    if (!shape.map || (settled && read_or<uint64_t>(shape.resource + t::material_resource::render_map) != shape.map)) return Fit::not_yet;
    if (!game::readable(shape.map, m::size)) return Fit::other_shape;
    shape.map_resource = read_or<uint64_t>(shape.map + m::resource);
    if (!shape.map_resource || !game::readable(shape.map_resource, r::size) || !read_or<uint64_t>(shape.map_resource + r::shaders)) return Fit::other_shape;
    if (read_or<uint32_t>(shape.map_resource + r::shader_count) != row.map_shaders) return Fit::other_shaders;
    if (memcmp(reinterpret_cast<const void *>(shape.map + m::id_hash), file + row.map_hash, shaders::kHashBytes)) return Fit::other_shaders;
    if (row.old_size) {
        const uint64_t content = read_or<uint64_t>(shape.map + m::content);
        if (read_or<uint32_t>(shape.map + m::content_bytes) != row.old_size || !game::readable(content, row.old_size)) return Fit::other_shaders;
    }
    return Fit::fits;
}

// ---- A mod's set, for as long as the mod runs --------------------------------------------------------

void forget(Set &set) {
    for (uint32_t i = 0; set.materials && i < set.count; ++i) set.materials[i].path.clear(), set.materials[i].package.clear(), set.materials[i].placed.clear();
    release(set.materials), release(set.rows), release(set.programs), release(set.bytes);
    set = Set{};
}

// Each slot a material's stand-in was put in gets back what it had, where the stand-in is still there.
void put_back(Material &material) {
    ++generation;
    // Transfer ownership before restoration can synchronously stop this provider.
    // No reference to its row or arrays survives a native call.
    List<Placed> placed = material.placed;
    List<Derived> derived = material.derived;
    const uint64_t stand_in = material.stand_in;
    material.placed = {}, material.derived = {}, material.stand_in = 0;
    for (uint32_t i = 0; i < placed.count; ++i) {
        const Placed saved = placed[i];
        if (const uint64_t part = game::from_handle(saved.part)) {
            if (shader_slot_source(part, saved.slot) == saved.source &&
                restore_material(nullptr, part, saved.slot) == TSUCHI_OK) ++counts.slots_put_back;
        }
    }
    placed.clear();
    release_game_object(stand_in);
    for (uint32_t i = 0; i < derived.count; ++i) {
        release_game_object(derived[i].source);
        release_game_object(derived[i].stand_in);
    }
    derived.clear();
}

// Remove the provider and its bookkeeping before its restoration calls can reenter.
void end_set(void *, uint64_t id) {
    Set ended{};
    for (uint32_t i = 0; i < sets.count; ++i) {
        if (sets[i].id != id) continue;
        ended = sets[i];
        counts.materials -= ended.count, --counts.sets;
        sets.remove(&sets[i]);
        pairs.count = 0;
        look_afresh();
        break;
    }
    if (!ended.id) return;
    for (uint32_t m = 0; m < ended.count; ++m) put_back(ended.materials[m]);
    forget(ended);
}

TSUCHI_Result refuse(Text &problem, TSUCHI_Result result, const char *a, const char *b = "", const char *c = "", const char *d = "", const char *e = "",
                     const char *f = "") {
    join(problem, a, b, c, d, e, f);
    return result;
}

bool shaders_well_formed(const Json &part, Text &wrong) {
    if (part.kind == Json::Kind::object && shaders::archive::shape(part)) return true;
    if (part.kind == Json::Kind::text && part.bytes && strlen(part.text) == part.bytes && game::path_stays_within(part.text)) return true;
    return wrong.set("\"shaders\" is the path of a file in the mod's folder: its shader set"), false;
}

// A part of the file's text as a text of its own.
bool keep_text(Text &out, const uint8_t *file, uint32_t at, uint32_t length, const char *more = "", uint32_t more_at = 0, uint32_t more_length = 0) {
    if (!out.reserve(size_t(length) + strlen(more) + more_length + 1)) return false;
    char *to = out.data;
    memcpy(to, file + at, length), to += length;
    memcpy(to, more, strlen(more)), to += strlen(more);
    memcpy(to, file + more_at, more_length), to += more_length;
    *to = 0;
    return true;
}

// Read a manifest or bundle file, then load and compare each donor material.
// Nothing of the game's is changed here. The caller owns the native file path.
TSUCHI_Result admit_file(TSUCHI_Mod *mod, const Json &named, const char* native_path,
                         Text &problem, uint64_t& set_id) {
    set_id = 0;
    const bool archived = named.kind == Json::Kind::object;
    const char* file_name = archived ? named.find("archive")->text : named.text;
    char *bytes = nullptr;
    uint64_t size = 0;
    TSUCHI_Result read = TSUCHI_OK;
    if (archived) {
        read = game::file_size(native_path, size);
        if (read == TSUCHI_OK && size > shaders::archive::limit) read = TSUCHI_INVALID;
    }
    if (read == TSUCHI_OK) read = game::read_whole_file(native_path, bytes, size);
    if (read != TSUCHI_OK)
        return refuse(problem, read, "\"shaders\": ", file_name,
                      read == TSUCHI_INVALID     ? ": the archive exceeds the supported size"
                      : read == TSUCHI_NOT_FOUND ? ": the mod has no such file"
                      : read == TSUCHI_NO_MEMORY ? ": there is no memory to read it"
                                                 : ": it could not be read");
    if (archived) {
        uint8_t* decoded = nullptr; uint32_t decoded_size = 0;
        const auto result = size <= shaders::archive::limit
            ? shaders::archive::read(named, reinterpret_cast<const uint8_t*>(bytes), uint32_t(size), decoded, decoded_size)
            : shaders::Read::bad_value;
        release(bytes);
        if (result != shaders::Read::ok) {
            ++counts.files_refused;
            return refuse(problem, result == shaders::Read::full ? TSUCHI_NO_MEMORY : TSUCHI_INVALID,
                "\"shaders\": ", file_name, " has invalid creator archive data or material metadata");
        }
        bytes = reinterpret_cast<char*>(decoded); size = decoded_size;
    }
    Set set{};
    set.mod = mod, set.bytes = reinterpret_cast<uint8_t *>(bytes);
    shaders::Counts holds{};
    const shaders::Read measured = size < (256ull << 20) ? shaders::measure(set.bytes, uint32_t(size), holds) : shaders::Read::bad_value;
    if (measured != shaders::Read::ok) {
        forget(set);
        ++counts.files_refused;
        return refuse(problem, TSUCHI_INVALID, "\"shaders\": ", file_name,
                      measured == shaders::Read::not_a_set   ? " is no shader set, or one of a kind this framework does not read"
                      : measured == shaders::Read::cut_short ? " is cut short"
                                                             : " is damaged: something in it is out of range");
    }
    if (!holds.materials) {
        forget(set);
        return TSUCHI_OK; // a set with nothing in it
    }
    set.count = holds.materials;
    set.rows = static_cast<shaders::Material *>(allocate(sizeof(shaders::Material) * holds.materials));
    set.programs = static_cast<shaders::Shader *>(allocate(sizeof(shaders::Shader) * (holds.shaders ? holds.shaders : 1)));
    set.materials = static_cast<Material *>(allocate(sizeof(Material) * holds.materials));
    for (uint32_t i = 0; set.materials && i < set.count; ++i) set.materials[i] = Material{};
    shaders::File file{set.bytes, uint32_t(size), set.rows, holds.materials, 0, set.programs, holds.shaders, 0};
    if (!set.rows || !set.programs || !set.materials || shaders::read(&file) != shaders::Read::ok) {
        forget(set);
        return TSUCHI_NO_MEMORY;
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        const shaders::Material &row = set.rows[i];
        Material &material = set.materials[i];
        Text asset; // the game material by the path of the object: its package, a dot, and its name
        const bool kept = keep_text(material.path, set.bytes, row.material, row.material_length) &&
                          keep_text(material.package, set.bytes, row.game_material, row.game_material_length) &&
                          keep_text(asset, set.bytes, row.game_material, row.game_material_length, ".", row.leaf, row.leaf_length);
        const char *name = kept ? material.path.get() : ""; // what the instance is called: what follows the one dot of its path
        for (const char *at = name; *at; ++at)
            if (*at == '.') name = at + 1;
        char leaf[shaders::kPathLimit + 1];
        if (kept) memcpy(leaf, set.bytes + row.leaf, row.leaf_length), leaf[row.leaf_length] = 0;
        // The game's names for the two materials: an object's name is compared as a name, not letter by letter.
        TSUCHI_Result result = kept ? game::name_from_utf8(name, material.name) : TSUCHI_NO_MEMORY;
        if (result == TSUCHI_OK) result = game::name_from_utf8(leaf, material.game_name);
        TSUCHI_Object loaded{};
        if (result == TSUCHI_OK) result = host.asset->load(mod, asset.get(), &loaded); // kept loaded for as long as the mod runs
        asset.clear();
        const uint64_t game_material = result == TSUCHI_OK ? game::from_handle(loaded.id) : 0;
        Shape shape{};
        const Fit fit = game_material && game::is_a(game_material, instance_class) ? fit_of(game_material, set.bytes, row, shape, false) : Fit::other_shape;
        if (result == TSUCHI_OK && fit == Fit::fits) continue;
        if (result == TSUCHI_NOT_READY) {
            refuse(problem, result, "\"shaders\": ", file_name, ": the game cannot be asked for ", material.package.get(), " yet");
        } else if (result != TSUCHI_NO_MEMORY) {
            ++counts.sets_refused;
            if (result != TSUCHI_OK)
                result = refuse(problem, TSUCHI_NOT_FOUND, "\"shaders\": ", file_name, ": the game has no material ", material.package.get());
            else if (fit == Fit::other_shaders)
                result = refuse(problem, TSUCHI_INVALID, "\"shaders\": ", file_name, " was made for another version of the game: the shaders of ",
                                material.package.get(), " are not the ones it was compiled for");
            else
                result = refuse(problem, TSUCHI_UNSUPPORTED, "\"shaders\": ", file_name, ": ", material.package.get(),
                                " is no material of the game's with shaders of its own");
        }
        forget(set);
        return result;
    }
    set.id = next_set;
    Set *kept = sets.add(set);
    const TSUCHI_Result owned = kept ? own(mod, kSet, set.id, end_set, nullptr) : TSUCHI_NO_MEMORY;
    if (owned != TSUCHI_OK) {
        forget(set);
        if (kept) sets.remove(kept);
        return owned;
    }
    ++next_set, ++counts.sets, counts.materials += set.count;
    set_id = set.id;
    return TSUCHI_OK;
}

TSUCHI_Result shaders_begin(TSUCHI_Mod *mod, const Json &named, Text &problem) {
    const char* file_name = named.kind == Json::Kind::object ? named.find("archive")->text : named.text;
    Texts where;
    if (!mod_file_path(where, mod, file_name))
        return refuse(problem, TSUCHI_NO_MEMORY, "\"shaders\": ", file_name, ": there is no memory to read it");
    uint64_t set_id = 0;
    const TSUCHI_Result result = admit_file(mod, named, where.data, problem, set_id);
    where.clear();
    return result;
}

// ---- The copy of a game material, and its shaders ----------------------------------------------------

// Asks the engine for the game material's package a second time, as a level into the loaded world.
bool ask_for_copy(const char *package) {
    ++counts.level_requests;
    static game::ClassRef streaming{"LevelStreamingDynamic"};
    const uint64_t type = game::find_class(streaming);
    const uint64_t pattern = type ? game::default_object(type) : 0;
    if (!pattern) return false;
    game::EngineCall call(pattern, "LoadLevelInstance");
    game::Value taken;
    return call.set("WorldContextObject", game::object_value(world_now)) == TSUCHI_OK && call.set_text("LevelName", package) == TSUCHI_OK &&
           call.invoke() == TSUCHI_OK && call.get("bOutSuccess", taken) == TSUCHI_OK && taken.kind == game::ValueKind::boolean && taken.boolean;
}

// Whether a material is a copy of a set's game material that nobody has taken: the engine names the package it
// loaded as the game's, with `_LevelInstance_` and a number after it.
bool is_free_copy(uint64_t object, const Material &material) {
    static char name[game::kNameBytes];
    static const char tail[] = "_LevelInstance_";
    const uint64_t package = game::outer_of(object);
    if (!package || !game::name_text(game::name_of(package), name, sizeof name)) return false;
    const char *wanted = material.package.get();
    const size_t length = strlen(wanted);
    if (strlen(name) <= length + sizeof tail - 1 || !same_path(name, wanted, length) || memcmp(name + length, tail, sizeof tail - 1)) return false;
    const uint64_t handle = game::to_handle(object);
    for (uint32_t i = 0; i < taken.count; ++i)
        if (taken[i] == handle) return false;
    return true;
}

// What the renderer's function for making a pixel shader takes and gives back: the shader's bytes as a pointer
// and a count, its hash, and a counted reference to the shader, which is given back through memory of the caller's.
struct ShaderCode {
    const uint8_t *data;
    int32_t count;
};
struct ShaderReference {
    uint64_t shader;
    ShaderReference() : shader(0) {}
    ShaderReference(const ShaderReference &other) : shader(other.shader) {}
    ~ShaderReference() {}
};
using MakePixelShader = ShaderReference (*)(uint64_t renderer, ShaderCode code, const uint8_t *hash);

// The copy gets a shader map of its own, with the mod's shaders in it. `wait` is set when the engine has not
// finished with the copy; nothing is changed then, nor on any failure.
ShaderFailure give_shaders(Set &set, uint32_t index, bool &wait) {
    namespace m = t::shader_map;
    namespace r = t::shader_map_resource;
    const shaders::Material &row = set.rows[index];
    const uint64_t copy = game::from_handle(set.materials[index].copy);
    if (!copy) return ShaderFailure::no_copy;
    Shape shape{};
    const Fit fit = fit_of(copy, set.bytes, row, shape, true);
    if (fit == Fit::not_yet) return wait = true, ShaderFailure::none;
    if (fit != Fit::fits) return fit == Fit::other_shape ? ShaderFailure::copy_shape : ShaderFailure::other_shaders;
    if (!game::writable(shape.resource + t::material_resource::game_map, 8) || !game::writable(shape.resource + t::material_resource::render_map, 8))
        return ShaderFailure::copy_shape;
    const uint64_t renderer = game::global<uint64_t>(t::data::renderer);
    if (!renderer || read_or<uint64_t>(renderer) != game::main_base() + t::data::renderer_table) return ShaderFailure::no_renderer;
    const auto make = reinterpret_cast<MakePixelShader>(game::table_entry(renderer, t::renderer_table::make_pixel_shader));
    if (!make) return ShaderFailure::no_renderer;

    auto *const list = static_cast<uint64_t *>(allocate(sizeof(uint64_t) * row.map_shaders));
    auto *const own_resource = static_cast<uint8_t *>(allocate(r::size));
    auto *const own_map = static_cast<uint8_t *>(allocate(m::size));
    auto *const content = row.old_size ? static_cast<uint8_t *>(allocate(row.new_size)) : nullptr;
    const auto leave = [&](ShaderFailure why) {
        release(list), release(own_resource), release(own_map), release(content);
        return why;
    };
    if (!list || !own_resource || !own_map || (row.old_size && !content)) return leave(ShaderFailure::no_memory);

    // A description of the material's own: the game's with the set's edits, and each name as the engine numbers it.
    if (row.old_size) {
        namespace n = t::frozen_name;
        static_assert(n::size == shaders::kNameBytes, "a name in a description takes the bytes the file leaves for it");
        shaders::build_content(set.bytes, row, reinterpret_cast<const uint8_t *>(read_or<uint64_t>(shape.map + m::content)), content);
        bool named = true;
        shaders::each_name(set.bytes, row, [&](const uint8_t *text, uint32_t length, uint32_t number, uint32_t place) {
            char name[shaders::kNameLimit + 1];
            memcpy(name, text, length), name[length] = 0;
            game::Name found{};
            if (game::name_from_utf8(name, found) != TSUCHI_OK || !found.index) named = false;
            memcpy(content + place + n::comparison, &found.index, 4), memcpy(content + place + n::display, &found.index, 4),
                memcpy(content + place + n::number, &number, 4);
        });
        if (!named) return leave(ShaderFailure::no_names);
    }

    // The mod's shaders at their places, each made by the renderer and given its hash as the game's library gives one.
    memset(list, 0, sizeof(uint64_t) * row.map_shaders);
    for (uint32_t s = row.first_shader; s < row.first_shader + row.shader_count; ++s) {
        const shaders::Shader &program = set.programs[s];
        const ShaderReference made = make(renderer, ShaderCode{set.bytes + program.payload, int32_t(program.size)}, set.bytes + program.hash);
        if (!made.shader || !game::writable(made.shader + t::shader::hash, shaders::kHashBytes)) return leave(ShaderFailure::shader_refused);
        memcpy(reinterpret_cast<void *>(made.shader + t::shader::hash), set.bytes + program.hash, shaders::kHashBytes);
        list[program.place] = made.shader; // the one reference that was given is the list's
        ++counts.shaders_made;
    }

    // The list of shaders: the game's copied, with a lock of its own that nobody holds, the mod's list, and
    // nothing preloaded from the library. It keeps which shader map of the library it stands for, so the engine
    // can fill the empty places.
    memcpy(own_resource, reinterpret_cast<const void *>(shape.map_resource), r::size);
    memset(own_resource + r::lock, 0, t::system_mutex::size);
    own_resource[r::lock + t::system_mutex::state] = uint8_t(t::constant::mutex_ready);
    own_resource[r::lock + t::system_mutex::recursive] = 1;
    const uint64_t list_at = reinterpret_cast<uint64_t>(list);
    memcpy(own_resource + r::shaders, &list_at, 8), memcpy(own_resource + r::references, &kHeldForGood, 4);
    own_resource[r::preloaded] = 0;
    // The shader map: the game's copied, with that list, and not the one the engine finds by its hash.
    memcpy(own_map, reinterpret_cast<const void *>(shape.map), m::size);
    const uint64_t resource_at = reinterpret_cast<uint64_t>(own_resource);
    memcpy(own_map + m::resource, &resource_at, 8), memcpy(own_map + m::references, &kHeldForGood, 4);
    own_map[m::flags] &= uint8_t(~t::constant::shader_map_registered);
    if (row.old_size) {
        const uint64_t content_at = reinterpret_cast<uint64_t>(content);
        memcpy(own_map + m::content, &content_at, 8), memcpy(own_map + m::content_bytes, &row.new_size, 4);
        ++counts.descriptions;
    }
    // The copy of the material draws with it from here on. What the copy held of the game's shader map stays
    // counted, which keeps the game's shader map, whose description this one may share, for as long as the game runs.
    const uint64_t map_at = reinterpret_cast<uint64_t>(own_map);
    settle();
    game::write(shape.resource + t::material_resource::game_map, map_at), game::write(shape.resource + t::material_resource::render_map, map_at);
    settle();
    return ShaderFailure::none;
}

void give_up(Set &set, uint32_t index, ShaderFailure why) {
    ++generation;
    static const char *const said[] = {"",
                                       "the engine did not load the game material a second time",
                                       "the copy of the game material is not laid out as the framework expects",
                                       "the copy of the game material has other shaders than the set was compiled for",
                                       "the game's renderer was not found",
                                       "the game's renderer made nothing of one of its shaders",
                                       "there is no memory for it",
                                       "the engine has no number for a name of its description"};
    Material &material = set.materials[index];
    // A copy that did not come may come in another world. Anything else would go wrong the same way again.
    material.stage = why == ShaderFailure::no_copy ? Stage::waiting : Stage::failed;
    material.gave_up_in = world_now;
    ++counts.failed, counts.last_failure = why;
    tell(set.mod, "shaders: ", material.path.get(), " is drawn with the shaders of ", material.package.get(), ": ", said[uint32_t(why)]);
}

// ---- The stand-in, and the slots it is put in ----------------------------------------------------------

// A source must descend from exactly this mod's instance. Validate the whole
// parent chain: the engine's uniform-copy call also walks beyond that instance.
bool source_chain(const Material& material, uint64_t source, uint64_t (&chain)[32], uint32_t& count) {
    const uint64_t plain = game::from_handle(material.plain);
    count = 0;
    bool matched = false;
    while (plain && source && count < 32) {
        for (uint32_t i = 0; i < count; ++i) if (chain[i] == source) return false;
        chain[count++] = source;
        matched = matched || source == plain;
        game::Place parent; game::Value value;
        const uint64_t type = game::class_of(source);
        const TSUCHI_Result found = find_parent(type, source, parent);
        if (found == TSUCHI_NOT_FOUND) {
            static game::ClassRef dynamic_class{"MaterialInstanceDynamic"};
            // A terminal UMaterial has no Parent. An instance with a Parent
            // that cannot be read is not a safe input to the native walk.
            return matched && !game::is_a(source, instance_class) && !game::is_a(source, dynamic_class);
        }
        if (found != TSUCHI_OK || !game::object_property(parent) || game::read_value(parent, value) != TSUCHI_OK) return false;
        if (value.kind == game::ValueKind::none) {
            uint64_t raw_parent = 0;
            return matched && game::read(parent.address, raw_parent) && !raw_parent;
        }
        if (value.kind != game::ValueKind::object || !value.object) return false;
        source = game::from_handle(value.object);
        if (!source) return false;
    }
    return false;
}
// A signature of everything a copy takes from a source: the instances of its parent chain, and the values each
// keeps in its arrays of parameters. A copy makes the renderer rebuild the stand-in's uniforms, so a stand-in is
// only copied into when its source's signature differs from the one it was last copied with. 0 when the chain
// cannot be read, which always copies.
uint64_t values_signature(const Material& material, uint64_t source) {
    static const char *const arrays[] = {"ScalarParameterValues", "VectorParameterValues", "TextureParameterValues",
                                         "FontParameterValues", "RuntimeVirtualTextureParameterValues"};
    uint64_t chain[32]; uint32_t count;
    if (!source_chain(material, source, chain, count)) return 0;
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto mix = [&hash](const void *bytes, uint64_t length) {
        for (uint64_t i = 0; i < length; ++i) hash = (hash ^ static_cast<const uint8_t *>(bytes)[i]) * 0x100000001B3ull;
    };
    for (uint32_t i = 0; i < count; ++i) {
        mix(&chain[i], 8);
        const uint64_t type = game::class_of(chain[i]);
        for (const char *name : arrays) {
            game::Place place;
            game::ArrayMemory array;
            const TSUCHI_Result found = game::find_place(type, chain[i], name, place);
            if (found == TSUCHI_NOT_FOUND) { mix("-", 1); continue; } // a material, which keeps its defaults in its expressions
            if (found != TSUCHI_OK || game::array_of(place, array) != TSUCHI_OK) return 0;
            const uint64_t inner = read_or<uint64_t>(place.property + t::property::inner);
            const int32_t bytes = inner ? read_or<int32_t>(inner + t::property::element_bytes) : 0;
            const uint64_t length = uint64_t(array.count) * uint64_t(bytes > 0 ? bytes : 0);
            if (bytes <= 0 || (length && !game::readable(array.data, length))) return 0;
            mix(&array.count, sizeof array.count);
            if (length) mix(reinterpret_cast<const void *>(array.data), length);
        }
    }
    return hash ? hash : 1;
}
// Only stable keys cross native calls. A provider can stop or its arrays can
// move during creation/copy. The actual loaded world can change before step().
struct MaterialKey {
    uint64_t set = 0, copy = 0, plain = 0, source = 0, world = 0;
    uint32_t row = 0;
    game::CallGuard owner;
    Material* record() const {
        if (!owner.permits() || !set || world_now != world || game::to_handle(game::loaded_world()) != world ||
            !game::from_handle(copy) || !game::from_handle(plain)) return nullptr;
        for (uint32_t s = 0; s < sets.count; ++s) {
            Set& candidate = sets[s];
            if (candidate.id != set || row >= candidate.count) continue;
            Material& value = candidate.materials[row];
            return value.stage == Stage::ready && value.copy == copy && value.plain == plain ? &value : nullptr;
        }
        return nullptr;
    }
    Material* current() const {
        Material* value = record();
        uint64_t chain[32]; uint32_t count;
        return value && game::from_handle(source) && source_chain(*value, game::from_handle(source), chain, count) ? value : nullptr;
    }
    static bool allowed(void* context) { return static_cast<const MaterialKey*>(context)->current() != nullptr; }
    game::CallGuard guard() const { return {allowed, const_cast<MaterialKey*>(this)}; }
};
MaterialKey key_for(const Material& material, uint64_t source, game::CallGuard owner = {}) {
    for (uint32_t s = 0; s < sets.count; ++s)
        for (uint32_t m = 0; m < sets[s].count; ++m)
            if (&sets[s].materials[m] == &material)
                return MaterialKey{sets[s].id, material.copy, material.plain, game::to_handle(source), world_now, m, owner};
    return {};
}
bool copy_values(const MaterialKey& key, uint64_t made) {
    if (!key.current() || !game::live(made)) return false;
    const uint64_t shown = game::to_handle(made);
    if (!shown || !retain_game_object(key.source)) return false;
    struct CopyPins {
        uint64_t source, shown = 0;
        ~CopyPins() { release_game_object(shown); release_game_object(source); }
    } pins{key.source};
    if (!retain_game_object(shown)) return false;
    pins.shown = shown;
    if (!key.current() || game::from_handle(shown) != made) return false;
    const uint64_t source = game::from_handle(key.source);
    game::Place parent; game::Value parent_value;
    const bool inherits = find_parent(game::class_of(source), source, parent) == TSUCHI_OK &&
                          game::read_value(parent, parent_value) == TSUCHI_OK && parent_value.kind == game::ValueKind::object &&
                          game::from_handle(parent_value.object);
    if (key.source != key.plain || inherits) {
        game::EngineCall copy(made, "K2_CopyMaterialInstanceParameters", key.guard());
        // The uniform path merges all source instance overrides, including new
        // shader-descriptor names absent from the donor UMaterial's catalogue.
        return key.current() && copy.set("Source", game::object_value(key.source)) == TSUCHI_OK && key.current() &&
               copy.set("bQuickParametersOnly", game::boolean_value(true)) == TSUCHI_OK && key.current() &&
               copy.invoke() == TSUCHI_OK && key.current();
    }
    game::EngineCall copy(made, "CopyParameterOverrides", key.guard());
    return key.current() && copy.set("MaterialInstance", game::object_value(key.source)) == TSUCHI_OK &&
           key.current() && copy.invoke() == TSUCHI_OK && key.current();
}
uint64_t make_instance(const MaterialKey& key) {
    if (!key.current() || !retain_game_object(key.source)) return 0;
    struct SourcePin { uint64_t handle; ~SourcePin() { release_game_object(handle); } } pin{key.source};
    static game::ClassRef library{"KismetMaterialLibrary"};
    const uint64_t type = game::find_class(library);
    const uint64_t pattern = type ? game::default_object(type) : 0;
    if (!pattern || !key.current()) return 0;
    uint64_t made = 0;
    {
        game::EngineCall make(pattern, "CreateDynamicMaterialInstance", key.guard());
        game::Value result;
        if (!key.current() || make.set("WorldContextObject", game::object_value(key.world)) != TSUCHI_OK || !key.current() ||
            make.set("Parent", game::object_value(key.copy)) != TSUCHI_OK || !key.current() || make.invoke() != TSUCHI_OK ||
            make.get("ReturnValue", result) != TSUCHI_OK || result.kind != game::ValueKind::object) return 0;
        made = game::from_handle(result.object);
    }
    const uint64_t handle = game::to_handle(made);
    if (!handle || !key.current() || !retain_game_object(handle)) return 0;
    if (!copy_values(key, made) || !key.current() || game::from_handle(handle) != made) {
        release_game_object(handle);
        return 0;
    }
    ++counts.stand_ins;
    return made;
}
uint64_t make_stand_in(Material& material, game::CallGuard owner = {}) {
    const MaterialKey key = key_for(material, game::from_handle(material.plain), owner);
    const uint64_t made = make_instance(key);
    if (!made) return 0;
    Material* current = key.current();
    if (!current) { release_game_object(game::to_handle(made)); return 0; }
    if (const uint64_t published = game::from_handle(current->stand_in)) {
        release_game_object(game::to_handle(made));
        return published;
    }
    release_game_object(current->stand_in); // discard a stale generation's cache lease
    current->stand_in = game::to_handle(made);
    look_afresh();
    return made;
}
// Whether a material's parents reach the mod's instance of a ready material at all, read directly. Nearly every
// material a mesh has is the game's own and never does; only one that does is walked with every check.
bool reaches_a_source(uint64_t material) {
    for (uint32_t depth = 0; material && depth < 32; ++depth) {
        for (uint32_t s = 0; s < sets.count; ++s)
            for (uint32_t m = 0; m < sets[s].count; ++m)
                if (sets[s].materials[m].stage == Stage::ready && game::from_handle(sets[s].materials[m].plain) == material) return true;
        game::Place parent;
        if (find_parent(game::class_of(material), material, parent) != TSUCHI_OK) return false;
        const uint64_t next = read_or<uint64_t>(parent.address);
        material = next && game::live(next) ? next : 0;
    }
    return false;
}
Material* source_material(uint64_t source) {
    uint64_t chain[32]; uint32_t count;
    if (!source || !sets.count || !reaches_a_source(source)) return nullptr;
    ++counts.full_walks;
    for (uint32_t s = 0; source && s < sets.count; ++s)
        for (uint32_t m = 0; m < sets[s].count; ++m) {
            Material& material = sets[s].materials[m];
            if (material.stage == Stage::ready && source_chain(material, source, chain, count)) return &material;
        }
    return nullptr;
}
void refresh_instances() {
    const uint64_t limit = next_set;
    for (uint64_t id = next_live_set(0, limit); id; id = next_live_set(id, limit)) {
        const uint32_t rows = set_by_id(id)->count;
        for (uint32_t m = 0; m < rows; ++m) {
            for (uint32_t i = 0;;) {
                Set* provider = set_by_id(id);
                if (!provider || m >= provider->count) break;
                Material& row = provider->materials[m];
                if (row.stage != Stage::ready || i >= row.derived.count) break;
                const Derived before = row.derived[i++];
                MaterialKey key = key_for(row, game::from_handle(before.source));
                key.source = before.source;
                const uint64_t shown = game::from_handle(before.stand_in), source = game::from_handle(before.source);
                const uint64_t values = shown && source ? values_signature(row, source) : 0;
                // The stand-in already has the values of a source that did not change; nothing is asked of the engine.
                const bool kept = shown && before.copied && values && values == before.values;
                const bool copied = kept || (shown && copy_values(key, shown));
                Material* current = key.record();
                if (!current) {
                    if (world_now != key.world || game::to_handle(game::loaded_world()) != key.world) return;
                    break;
                }
                for (uint32_t d = 0; d < current->derived.count; ++d)
                    if (current->derived[d].source == before.source && current->derived[d].stand_in == before.stand_in) {
                        if (current->derived[d].copied != (copied && game::from_handle(before.stand_in))) ++generation;
                current->derived[d].copied = copied && game::from_handle(before.stand_in);
                        current->derived[d].values = current->derived[d].copied ? values : 0;
                        current->derived[d].updated_at = world_steps;
                        break;
                    }
                // The next turn resolves the provider by ID, never the old array slot.
            }
        }
    }
}

// The materials that can be put in slots in this step.
void gather_pairs() {
    pairs.count = 0;
    for (uint32_t s = 0; s < sets.count; ++s) {
        for (uint32_t m = 0; m < sets[s].count; ++m) {
            Material &material = sets[s].materials[m];
            const uint64_t plain = material.stage == Stage::ready ? game::from_handle(material.plain) : 0;
            const uint64_t stand_in = plain ? game::from_handle(material.stand_in) : 0;
            if (stand_in) pairs.add(Pair{plain, stand_in, &material});
        }
    }
}

// Record ownership before taking a slot. A later source may belong to another set.
bool track_slot(Material& material, uint64_t source, uint64_t part, uint32_t slot) {
    const uint64_t handle = game::to_handle(part), source_handle = game::to_handle(source);
    if (!handle || !source_handle) return false;
    List<Placed>& placed = material.placed;
    for (uint32_t i = 0; i < placed.count; ++i)
        if (placed[i].part == handle && placed[i].slot == slot) {
            placed[i].source = source_handle;
            return true;
        }
    return placed.add(Placed{handle, slot, source_handle}) != nullptr;
}
void put_in(Material& material, uint64_t source, uint64_t part, uint32_t slot) {
    if (!track_slot(material, source, part, slot)) return;
    if (hold_material(nullptr, part, slot, source) == TSUCHI_OK) ++counts.slots_set;
}

// What has been put in a mesh's slots over its own materials, where the engine keeps it.
bool overrides_of(uint64_t part, uint64_t& data, uint32_t& count) {
    return game::material_overrides(part, data, count);
}

// Asks a mesh for the material of each of its slots: its own, or what was put there over it.
void ask_part(uint64_t part) {
    uint32_t slots = 0;
    ++counts.parts_asked;
    if (!game::material_slots(part, slots)) return;
    // What was put in a slot over the mesh's own material is read where the engine keeps it; the engine is asked
    // only for a slot that has none.
    uint64_t data = 0;
    uint32_t overridden = 0;
    if (!game::is_mesh_part(part) || !overrides_of(part, data, overridden)) data = 0, overridden = 0;
    for (uint32_t slot = 0; slot < slots && slot < kMostSlots; ++slot) {
        const uint64_t put = slot < overridden ? read_or<uint64_t>(data + 8 * uint64_t(slot)) : 0;
        const uint64_t material = put && game::live(put) ? put : game::slot_material(part, slot);
        if (Material* owner = source_material(material)) put_in(*owner, material, part, slot);
    }
}

void ask_entered_part(void *, uint64_t part) {
    if (!game::is_shader_target(part)) return;
    seen.add(game::to_handle(part));
    ask_part(part);
}

// ---- Looking at the world ------------------------------------------------------------------------------

// The material instances the engine has: a copy that is awaited, and the mods' own instances that are not known yet.
struct MaterialSearch {
    uint64_t type;
    Material *awaited;
    uint64_t copy;
    List<Material *> unknown;
};
bool look_at_material(void *context, uint64_t object) {
    auto &search = *static_cast<MaterialSearch *>(context);
    if (game::class_of(object) != search.type || is_template(object)) return true;
    const game::Name name = game::name_of(object);
    if (search.awaited && !search.copy && same(name, search.awaited->game_name) && is_free_copy(object, *search.awaited)) search.copy = object;
    for (uint32_t i = 0; i < search.unknown.count; ++i) {
        Material &material = *search.unknown[i];
        if (!same(name, material.name) || game::from_handle(material.plain)) continue;
        static char path[2 * shaders::kPathLimit + 4];
        uint64_t needed = 0;
        if (game::object_path(object, path, sizeof path, &needed) == TSUCHI_OK && same_path(path, material.path.get()))
            material.plain = game::to_handle(object);
    }
    return true;
}

// The meshes the engine has: a slot that one of the mods' instances was put in, and meshes not asked yet.
struct Hit {
    uint64_t part; // a handle
    uint32_t slot;
    uint64_t source;
};
// A walk over the engine's objects for the meshes, taken a piece at a time: a step walks for at most
// kWalkMicroseconds where there is a clock, and the next step goes on where it stopped. In the game a whole walk
// took 0.8 seconds, which a single step showed as a stall.
struct MeshWalk {
    bool active;
    bool requeue;    // this walk queues the users other than meshes again
    int32_t next;    // the place in the engine's table it goes on from
    uint32_t rounds; // walks begun since the meshes were last looked at afresh
    uint32_t visited;
    uint64_t began;
    List<Hit> hits;
};
MeshWalk walk{};
void forget_walk() { walk.active = false, walk.rounds = 0, walk.hits.count = 0; }

// What a class's objects are to a walk, found once per class: nothing, a mesh, or another user of materials.
// Whether an object uses materials is the same for every object of its class.
enum class UserKind : uint8_t { unknown, nothing, mesh, other };
struct KindOf { uint64_t type, handle; UserKind kind; };
struct Kinds {
    KindOf *slots = nullptr;
    uint32_t capacity = 0, count = 0;
    static uint32_t first(uint64_t type, uint32_t capacity) { return uint32_t((type * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1); }
    UserKind of(uint64_t object) {
        const uint64_t type = game::class_of(object);
        if (!type) return UserKind::nothing;
        uint32_t at = capacity ? first(type, capacity) : 0;
        for (; capacity && slots[at].type; at = (at + 1) & (capacity - 1))
            if (slots[at].type == type) {
                if (game::from_handle(slots[at].handle) == type) return slots[at].kind;
                break; // another class now at this address: found again below
            }
        const UserKind kind = game::is_mesh_part(object) ? UserKind::mesh : game::is_shader_target(object) ? UserKind::other : UserKind::nothing;
        remember(type, kind);
        return kind;
    }
    void remember(uint64_t type, UserKind kind) {
        const uint64_t handle = game::to_handle(type);
        if (!handle) return;
        if ((count + 1) * 2 > capacity) {
            const uint32_t room = capacity ? capacity * 2 : 1024;
            auto *grown = static_cast<KindOf *>(allocate(sizeof(KindOf) * room));
            if (!grown) return;
            memset(grown, 0, sizeof(KindOf) * room);
            for (uint32_t i = 0; i < capacity; ++i)
                if (slots[i].type) {
                    uint32_t at = first(slots[i].type, room);
                    while (grown[at].type) at = (at + 1) & (room - 1);
                    grown[at] = slots[i];
                }
            release(slots);
            slots = grown, capacity = room;
        }
        uint32_t at = first(type, capacity);
        while (slots[at].type && slots[at].type != type) at = (at + 1) & (capacity - 1);
        if (!slots[at].type) ++count;
        slots[at] = KindOf{type, handle, kind};
    }
};
Kinds kinds;

bool look_at_mesh(void *context, uint64_t object) {
    auto &search = *static_cast<MeshWalk *>(context);
    if (cost_clock && ++search.visited % 128 == 0 && cost_now() - search.began > kWalkMicroseconds) return false; // this one waits too
    const UserKind kind = kinds.of(object);
    if (kind == UserKind::nothing || is_template(object)) return true;
    if (kind == UserKind::other) {
        // Non-mesh users can replace their template or brush resource while alive, so they are asked again, but
        // only by every kRequeueEvery-th walk, besides the walk that first finds one. One still waiting from an
        // earlier walk is not added again: the queue would grow faster than it is asked from.
        const uint64_t handle = game::to_handle(object);
        if (!handle || (!search.requeue && seen.has(handle))) return true;
        if (!seen.has(handle) && !seen.add(handle)) seen.forget();
        if (!queued.has(handle) && queued.add(handle)) unasked.add(handle);
        return true;
    }
    const uint64_t handle = game::to_handle(object);
    uint64_t data = 0;
    uint32_t count = 0;
    if (!handle || !overrides_of(object, data, count)) return true;
    for (uint32_t slot = 0; slot < count && slot < kMostSlots; ++slot) {
        const uint64_t material = read_or<uint64_t>(data + 8 * uint64_t(slot));
        if (source_material(material)) search.hits.add(Hit{handle, slot, game::to_handle(material)});
    }
    if (!seen.has(handle) && seen.add(handle) && !unasked.add(handle)) seen.forget(); // with no memory to note it, everything is looked at again
    return true;
}

// A stand-in for one material whose copy is ready and whose instance is loaded, and none yet. Making one, a
// dynamic instance and the copy of its values, took about 25 ms in the game, so a step makes at most one: five
// made in one look were a 130 ms step as the glass of run lead-material-11 appeared.
void make_one_stand_in() {
    for (uint32_t s = 0; s < sets.count; ++s)
        for (uint32_t m = 0; m < sets[s].count; ++m) {
            Material &material = sets[s].materials[m];
            if (material.stage != Stage::ready || !game::from_handle(material.plain) || game::from_handle(material.stand_in)) continue;
            // One the engine would not make is tried again a look's interval later, as before, not every step.
            if (material.stand_in_tried && world_steps - material.stand_in_tried < 15) continue;
            {
                material.stand_in_tried = world_steps;
                make_stand_in(material); // native calls: the sets may change under it, so nothing is looked at after
                return;
            }
        }
}

// The walk for meshes goes on from where it stopped, for as long as a step allows. When it reaches the end of the
// engine's table, a slot an instance of a mod's was put in gets the stand-in.
void walk_some() {
    walk.began = cost_now(), walk.visited = 0;
    ++counts.walk_pieces;
    const int32_t from = walk.next;
    const bool done = game::each_object_from(walk.next, look_at_mesh, &walk);
    if (!done) {
        if (walk.next > from) --walk.next; // the object it stopped at was not looked at
        return;
    }
    walk.active = false, ++counts.walks;
    for (uint32_t i = 0; i < walk.hits.count; ++i)
        if (const uint64_t part = game::from_handle(walk.hits[i].part)) {
            const uint64_t source = game::from_handle(walk.hits[i].source);
            if (Material* owner = source_material(source)) put_in(*owner, source, part, walk.hits[i].slot);
        }
    walk.hits.count = 0;
}

// Some of the meshes that have not been asked for their materials.
void ask_some() {
    const uint64_t began = cost_now();
    for (uint32_t asked = 0; unasked.count && asked < kPartsAStep; ++asked) {
        if (cost_clock && cost_now() - began > kAskMicroseconds) break; // the rest wait for the next step
        const uint64_t part = game::from_handle(unasked[--unasked.count]);
        if (part) ask_part(part);
    }
}

// The copies: one is asked for at a time, since the engine does not say which request a copy answers, and the
// copy that appears next is that material's.
void look_at_copies(Set *&awaited_set, uint32_t &awaited) {
    awaited_set = nullptr;
    bool busy = false;
    for (uint32_t s = 0; s < sets.count; ++s) {
        for (uint32_t m = 0; m < sets[s].count; ++m) {
            Material &material = sets[s].materials[m];
            if (material.stage == Stage::came) {
                bool wait = false;
                const ShaderFailure failure = give_shaders(sets[s], m, wait);
                if (wait && world_steps - material.asked_at <= kCopyWait)
                    busy = true;
                else if (wait)
                    give_up(sets[s], m, ShaderFailure::copy_shape); // the engine never finished with it
                else if (failure != ShaderFailure::none)
                    give_up(sets[s], m, failure);
                else
                    material.stage = Stage::ready, ++counts.copies_ready, ++generation;
            } else if (material.stage == Stage::asked) {
                busy = true, awaited_set = &sets[s], awaited = m;
            }
        }
    }
    for (uint32_t s = 0; s < sets.count && !busy; ++s) {
        for (uint32_t m = 0; m < sets[s].count && !busy; ++m) {
            Material &material = sets[s].materials[m];
            if (material.stage != Stage::waiting || material.gave_up_in == world_now) continue;
            if (!ask_for_copy(material.package.get())) {
                give_up(sets[s], m, ShaderFailure::no_copy);
                continue;
            }
            material.stage = Stage::asked, material.asked_at = world_steps;
            ++counts.copies_asked;
            busy = true; // looked for from the next look on
        }
    }
}

void look() {
    Set *awaited_set = nullptr;
    uint32_t awaited = 0;
    look_at_copies(awaited_set, awaited);

    // One pass over the engine's objects for the copy that is awaited and for the mods' own instances.
    MaterialSearch search{game::find_class(instance_class), awaited_set ? &awaited_set->materials[awaited] : nullptr, 0, {}};
    for (uint32_t s = 0; s < sets.count; ++s)
        for (uint32_t m = 0; m < sets[s].count; ++m)
            if (sets[s].materials[m].stage != Stage::failed && !game::from_handle(sets[s].materials[m].plain)) search.unknown.add(&sets[s].materials[m]);
    if (search.type && (search.awaited || (search.unknown.count && (world_steps < 60 || world_steps % 300 == 0)))) {
        ++counts.material_searches;
        game::each_object(look_at_material, &search);
    }
    search.unknown.clear();
    if (search.copy && taken.add(game::to_handle(search.copy))) {
        game::keep_object(search.copy, true);
        search.awaited->copy = game::to_handle(search.copy), search.awaited->stage = Stage::came;
    } else if (search.awaited && world_steps - search.awaited->asked_at > kCopyWait) {
        give_up(*awaited_set, awaited, ShaderFailure::no_copy);
    }

    make_one_stand_in();
    gather_pairs();
    if (!pairs.count) return;

    // And a walk for the meshes, unless the last is still going: a slot one of the instances was put in gets the
    // stand-in when the walk ends, and a mesh that has not been asked for its materials is asked in the steps that follow.
    if (!walk.active) {
        walk.active = true, walk.next = 0, walk.hits.count = 0;
        walk.requeue = walk.rounds++ % kRequeueEvery == 0;
        if (walk.requeue && !unasked.count) queued.forget(); // all asked: this walk queues every user other than a mesh again
    }
    walk_some();
    ask_some();
}

void step_work() {
    if (!sets.count) {
        world_now = 0, entered.count = 0;
        return;
    }
    const uint64_t world = game::loaded_world();
    const uint64_t handle = world ? game::to_handle(world) : 0;
    if (handle != world_now) {
        // Another world. A stand-in and the slots it was put in went with the last one; a copy that was asked
        // for there and has not come is asked for again.
        world_now = handle, world_steps = 0;
        pairs.count = 0, entered.count = 0;
        look_afresh();
        const uint64_t limit = next_set;
        for (uint64_t id = next_live_set(0, limit); id; id = next_live_set(id, limit)) {
            const uint32_t rows = set_by_id(id)->count;
            for (uint32_t m = 0; m < rows; ++m) {
                Set* provider = set_by_id(id);
                if (!provider || m >= provider->count) break;
                Material& material = provider->materials[m];
                if (material.stage == Stage::asked) material.stage = Stage::waiting;
                material.asked_at = 0, material.stand_in_tried = 0;
                put_back(material); // Image widgets can survive the departing world.
                // Native restoration may have removed or moved this provider.
            }
        }
    }
    if (!world_now) return;
    ++world_steps;
    refresh_instances();
    if (game::to_handle(game::loaded_world()) != world_now) return;
    if (entered.count || unasked.count) {
        gather_pairs();
        // An actor of the game's that entered play: its meshes are asked now, whether or not they were before,
        // since the game makes an actor it uses again as new.
        for (uint32_t i = 0; pairs.count && i < entered.count; ++i)
            if (const uint64_t actor = game::from_handle(entered[i])) game::each_part(actor, ask_entered_part, nullptr);
        entered.count = 0;
        if (pairs.count)
            ask_some();
        else
            unasked.count = 0;
    }
    // A look goes over every object the engine has, so it is taken twice a second while the world settles and
    // every four seconds after that.
    if (world_steps < 30 || world_steps % (world_steps < 900 ? 15 : 120)) {
        if (world_steps >= 30) make_one_stand_in(); // the rest of those a look found ready, one a step
        if (walk.active) walk_some(); // a walk begun at a look goes on a piece at a time
        return;
    }
    look();
}

// The step, and what it cost when there were sets to keep.
void step() {
    const uint64_t began = cost_now();
    step_work();
    if (sets.count) counts.steps.add(began);
}

// From inside the game's own function: the actor is noted, and looked at in the next step.
void note_entered(uint64_t, uint64_t handle) {
    if (sets.count && world_now) entered.add(handle);
}

} // namespace

void shaders_start() {
    add_manifest_section({"shaders", shaders_well_formed, shaders_begin});
    each_frame(step);
    on_actor_entered(note_entered);
}

TSUCHI_Result admit_asset_shaders(TSUCHI_Mod* mod, const char* native_path,
                                 const char* metadata_json, uint64_t& set_id) {
    set_id = 0;
    if (!mod_usable(mod) || !native_path || !*native_path || !metadata_json) return TSUCHI_INVALID;
    JsonError error{};
    const Json* metadata = read_json(metadata_json, strlen(metadata_json), &error);
    if (!metadata)
        return error.what && !strcmp(error.what, "there is no memory to read it into")
            ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
    Text problem;
    const TSUCHI_Result result = metadata->kind == Json::Kind::object && shaders::archive::shape(*metadata)
        ? admit_file(mod, *metadata, native_path, problem, set_id) : TSUCHI_INVALID;
    problem.clear();
    release_json(metadata);
    return result;
}

void release_asset_shaders(TSUCHI_Mod* mod, uint64_t set_id) {
    if (!mod_usable(mod) || !set_id) return;
    Set* set = set_by_id(set_id);
    if (!set || set->mod != mod) return;
    // Remove the callback before restoration can stop the mod and run cleanup.
    // end_set removes the provider before any native restoration call.
    if (disown(mod, kSet, set_id) == TSUCHI_OK) end_set(nullptr, set_id);
}

ShaderCounts shader_counts() { return counts; }
uint32_t shader_generation() { return generation; }

static uint64_t drawn_with_guard(uint64_t material, game::CallGuard owner, bool* admitted = nullptr, bool* selected = nullptr) {
    if (!owner.permits() || !sets.count || !material) return material;
    Material *found = nullptr;
    for (uint32_t s = 0; s < sets.count && !found; ++s)
        for (uint32_t m = 0; m < sets[s].count && !found; ++m)
            if (game::from_handle(sets[s].materials[m].plain) == material) found = &sets[s].materials[m];
    // One that was loaded since the last look: known by its name, then by its path.
    const uint64_t type = found ? 0 : game::find_class(instance_class);
    if (type && game::class_of(material) == type) {
        const game::Name name = game::name_of(material);
        for (uint32_t s = 0; s < sets.count && !found; ++s) {
            for (uint32_t m = 0; m < sets[s].count && !found; ++m) {
                Material &candidate = sets[s].materials[m];
                if (!same(name, candidate.name) || game::from_handle(candidate.plain)) continue;
                char path[2 * shaders::kPathLimit + 4];
                uint64_t needed = 0;
                if (game::object_path(material, path, sizeof path, &needed) != TSUCHI_OK || !same_path(path, candidate.path.get())) continue;
                candidate.plain = game::to_handle(material), found = &candidate;
            }
        }
    }
    if (!found) found = source_material(material);
    if (!found || found->stage != Stage::ready) return material;
    if (selected) *selected = true;
    const MaterialKey key = key_for(*found, material, owner);
    const auto cancel = [&] { if (admitted) *admitted = false; return material; };
    if (!world_now || !key.current()) return cancel();
    if (game::from_handle(found->plain) == material) {
        const uint64_t ready = game::from_handle(found->stand_in);
        const uint64_t made = ready ? ready : make_stand_in(*found, owner);
        if (!key.current()) return cancel();
        return made ? made : material;
    }
    const uint64_t source = game::to_handle(material);
    for (uint32_t i = 0; i < found->derived.count; ++i) if (found->derived[i].source == source) {
        const Derived before = found->derived[i];
        const uint64_t ready = game::from_handle(before.stand_in);
        const bool fresh = before.updated_at == world_steps;
        const uint64_t values = fresh ? before.values : ready ? values_signature(*found, material) : 0;
        const bool kept = ready && before.copied && values && values == before.values;
        const bool copied = fresh ? before.copied : kept || (ready && copy_values(key, ready));
        Material* current = key.current();
        if (!current) return cancel();
        for (uint32_t d = 0; d < current->derived.count; ++d)
            if (current->derived[d].source == source && current->derived[d].stand_in == before.stand_in) {
                if (current->derived[d].copied != (copied && game::from_handle(before.stand_in))) ++generation;
                current->derived[d].copied = copied && game::from_handle(before.stand_in);
                current->derived[d].values = current->derived[d].copied ? values : 0;
                current->derived[d].updated_at = world_steps;
                return current->derived[d].copied ? ready : material;
            }
        return material;
    }
    const uint64_t values = values_signature(*found, material); // as the first copy takes them
    const uint64_t made = make_instance(key);
    if (!made) return key.current() ? material : cancel();
    const uint64_t handle = game::to_handle(made);
    Material* current = key.current();
    if (!current) { release_game_object(handle); return cancel(); }
    for (uint32_t i = 0; i < current->derived.count; ++i) if (current->derived[i].source == source) {
        const Derived published = current->derived[i];
        release_game_object(handle);
        const uint64_t ready = game::from_handle(published.stand_in);
        return published.copied && ready ? ready : material;
    }
    if (!retain_game_object(source)) { release_game_object(handle); return material; }
    ++generation;
    if (!current->derived.add(Derived{source, handle, world_steps, true, values})) {
        release_game_object(source); release_game_object(handle); return material;
    }
    return made;
}

uint64_t drawn_as(uint64_t material) { return drawn_with_guard(material, {}); }
bool drawn_as_restoring(uint64_t material, game::CallGuard guard, uint64_t& shown) {
    shown = material;
    const uint64_t world = game::to_handle(game::loaded_world());
    if (!guard.permits()) return false;
    bool admitted = true, selected = false;
    shown = drawn_with_guard(material, guard, &admitted, &selected);
    return admitted && guard.permits() && (!selected || game::to_handle(game::loaded_world()) == world);
}

uint64_t shader_source(uint64_t shown) {
    const uint64_t handle = game::to_handle(shown);
    for (uint32_t s = 0; handle && s < sets.count; ++s)
        for (uint32_t m = 0; m < sets[s].count; ++m) {
            Material& material = sets[s].materials[m];
            if (material.stage != Stage::ready) continue;
            if (material.stand_in == handle)
                if (const uint64_t plain = game::from_handle(material.plain)) return plain;
            for (uint32_t i = 0; i < material.derived.count; ++i)
                if (material.derived[i].stand_in == handle)
                    if (const uint64_t source = game::from_handle(material.derived[i].source)) return source;
        }
    return shown;
}
bool track_shader_slot(uint64_t source, uint64_t part, uint32_t slot) {
    Material* material = source_material(source);
    return material && track_slot(*material, source, part, slot);
}
} // namespace tsuchi
