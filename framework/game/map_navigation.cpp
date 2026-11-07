// A mod's own navigation put in the place of the game's, for the map that is loaded.
//
// The game keeps a map's navigation in the map's own package. For each navmesh the mod's file names, every tile
// the game loaded into it is taken out and the mod's are put in, each built from its stored form into a block
// from the navigation's own allocator, as the engine's loader builds one. The navmesh's actor is then told which
// tiles changed, so that paths through them are found again, and every part that marks polygons, as a pot or a
// gate has, picks its polygons again, since the ones it had picked were in the tiles taken out.
#include "map_navigation.h"

#include <string.h>

#include "content_path.h"
#include "game.h"
#include "list.h"
#include "names.h"
#include "nav_file.h"
#include "objects.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using game::read_or;

game::ClassRef navmesh_class{t::name::navmesh_class};
game::ClassRef modifier_class{t::name::nav_modifier_class};

uint32_t failed_bit() { return uint32_t(t::constant::nav_failed); } // the bit of what the navmesh's own calls answer that says no
constexpr uint32_t kMaps = 64, kNavmeshes = 512;

void* allocate_for_navmesh(uint32_t bytes) {
    const auto take = game::global_function(t::data::nav_allocate);
    return take ? take(int(bytes), 0) : nullptr;
}
void release_for_navmesh(void* block) {
    const auto give = game::global_function(t::data::nav_release);
    if (give) give(block);
}

bool is_template(uint64_t object) { return (read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags) != 0; }

// One navmesh: the game's tiles out, the mod's in. `changed` gets the places of the tiles that were touched.
NavigationResult swap_tiles(uint64_t actor, const uint8_t* file, const nav::Navmesh& mesh, List<uint32_t>& changed, NavigationCounts& counts) {
    const uint64_t implementation = read_or<uint64_t>(actor + t::navmesh_actor::implementation);
    const uint64_t navmesh = implementation ? read_or<uint64_t>(implementation + t::navmesh_implementation::mesh) : 0;
    if (!navmesh) return NavigationResult::waiting;
    // The grid has to be the game's: where it begins and how wide a tile is.
    const uint8_t* payload = file + mesh.payload;
    for (uint32_t i = 0; i < 20; ++i)
        if (read_or<uint8_t>(navmesh + i) != payload[16 + i]) return NavigationResult::other_grid;
    const int32_t max_tiles = read_or<int32_t>(navmesh + t::navmesh::max_tiles);
    if (max_tiles <= 0 || max_tiles > 65536 || mesh.tile_count > uint32_t(max_tiles)) return NavigationResult::too_many_tiles;
    const uint64_t tiles = read_or<uint64_t>(navmesh + t::navmesh::tiles);
    const uint32_t tile_bits = read_or<uint32_t>(navmesh + t::navmesh::tile_bits), poly_bits = read_or<uint32_t>(navmesh + t::navmesh::poly_bits);
    const auto note = [&](uint32_t index) {
        for (uint32_t i = 0; i < changed.count; ++i)
            if (changed[i] == index) return;
        changed.add(index); // with no memory for it the tile is still changed; only its paths are not looked for again
    };
    for (int32_t i = 0; i < max_tiles; ++i) {
        const uint64_t tile = tiles + uint64_t(i) * t::navmesh_tile::size;
        if (!read_or<uint64_t>(tile + t::navmesh_tile::header)) continue;
        const uint64_t reference = (uint64_t(read_or<uint32_t>(tile + t::navmesh_tile::salt)) << (tile_bits + poly_bits)) | (uint64_t(uint32_t(i)) << poly_bits);
        if (t::fn::nav_remove_tile(reinterpret_cast<void*>(navmesh), reference, nullptr, nullptr) & failed_bit()) continue;
        ++counts.removed;
        note(uint32_t(i));
    }
    NavigationResult result = NavigationResult::done;
    nav::each_tile(file, mesh, [&](const nav::Tile& tile) {
        auto* const block = static_cast<uint8_t*>(allocate_for_navmesh(tile.memory));
        if (!block) {
            result = NavigationResult::no_memory;
            return false;
        }
        nav::build_tile(tile, block);
        uint64_t reference = 0;
        if (t::fn::nav_add_tile(reinterpret_cast<void*>(navmesh), block, int32_t(tile.memory), int32_t(t::constant::nav_tile_owned), 0, &reference) & failed_bit()) {
            release_for_navmesh(block);
            ++counts.refused;
            result = NavigationResult::tile_refused;
            return true;
        }
        ++counts.added;
        note(uint32_t(reference >> poly_bits) & ((1u << tile_bits) - 1));
        return true;
    });
    return result;
}

// After a swap every part that marks polygons does again what it did when it started: as the kind of area its
// Blueprint gave it, it picks its polygons, and then the kind it has now is marked on them with the game's own
// call. The Blueprint's kind is read from the part's pattern: the object of the same name, with `_GEN_VARIABLE`
// after it for a Blueprint's own parts, that belongs to the actor's class or to one of its parents.
struct Modifier {
    uint64_t part, owner_class, original;
    uint32_t depth;
};
struct Modifiers {
    List<Modifier> list;
    uint64_t world;
};

bool look_at_modifier(void* context, uint64_t object) {
    auto& found = *static_cast<Modifiers*>(context);
    if (is_template(object) || !game::is_a(object, modifier_class)) return true;
    const uint64_t owner = game::outer_of(object);
    const uint64_t level = owner ? game::outer_of(owner) : 0;
    if (!level || game::outer_of(level) != found.world) return true;
    found.list.add(Modifier{object, game::class_of(owner), 0, UINT32_MAX});
    return true;
}

bool look_at_pattern(void* context, uint64_t object) {
    auto& found = *static_cast<Modifiers*>(context);
    if (!is_template(object) || !game::is_a(object, modifier_class)) return true;
    const uint64_t outer = game::outer_of(object);
    if (!outer) return true;
    // The class the pattern belongs to: its outer, or the class of the default object that holds it.
    const bool default_object = (read_or<uint32_t>(outer + t::object::flags) & t::constant::default_object_flag) != 0;
    const uint64_t belongs = default_object ? game::class_of(outer) : outer;
    char name[256];
    if (!game::name_text(game::name_of(object), name, sizeof name)) return true;
    static const char suffix[] = "_GEN_VARIABLE";
    const size_t length = strlen(name), tail = sizeof suffix - 1;
    if (length > tail && !strcmp(name + length - tail, suffix)) name[length - tail] = 0;
    for (uint32_t i = 0; i < found.list.count; ++i) {
        Modifier& modifier = found.list[i];
        uint32_t depth = 0;
        uint64_t type = modifier.owner_class;
        for (; type && depth < 48 && type != belongs; ++depth) type = read_or<uint64_t>(type + t::struct_type::super);
        if (type != belongs || depth >= modifier.depth || !game::name_is(game::name_of(modifier.part), name)) continue;
        modifier.original = read_or<uint64_t>(object + t::nav_modifier::area_class), modifier.depth = depth;
    }
    return true;
}

void pick_again(uint64_t world, NavigationCounts& counts) {
    if (!game::find_class(modifier_class)) return;
    Modifiers found{{}, world};
    game::each_object(look_at_modifier, &found);
    if (found.list.count) game::each_object(look_at_pattern, &found);
    // Picking happens once in the game: a byte on the part says it was done. A part that has not picked yet is left
    // alone, since the game will have it pick on the new tiles when it starts it. One that has picks again, as its
    // pattern's kind when the kind it has now is another; and all pick before any kind is marked.
    enum : uint32_t { kLeft, kPicked, kChanged };
    for (uint32_t i = 0; i < found.list.count; ++i) {
        Modifier& modifier = found.list[i];
        if (!read_or<uint8_t>(modifier.part + t::nav_modifier::picked)) {
            modifier.depth = kLeft;
            continue;
        }
        const uint64_t present = read_or<uint64_t>(modifier.part + t::nav_modifier::area_class);
        const bool changed = modifier.original && present && modifier.original != present;
        modifier.depth = changed ? kChanged : kPicked;
        if (changed) {
            *reinterpret_cast<uint64_t*>(modifier.part + t::nav_modifier::area_class) = modifier.original;
            modifier.original = present;
        }
        *reinterpret_cast<uint8_t*>(modifier.part + t::nav_modifier::picked) = 0;
        t::fn::nav_modifier_select(reinterpret_cast<void*>(modifier.part));
        ++counts.picked;
    }
    for (uint32_t i = 0; i < found.list.count; ++i) {
        const Modifier& modifier = found.list[i];
        if (modifier.depth != kChanged) continue;
        t::fn::nav_modifier_change(reinterpret_cast<void*>(modifier.part), reinterpret_cast<void*>(modifier.original)); // the kind it has now, on the new polygons
        ++counts.marked;
    }
    found.list.clear();
}

struct PendingNavmesh {
    nav::Navmesh mesh;
    uint64_t done, found; // a completed object life, and the actor found at this step
};
struct ParsedNavigation {
    const uint8_t* file;
    uint64_t world;
    List<PendingNavmesh> meshes;
};
struct ActorSearch {
    ParsedNavigation* navigation;
    uint64_t world;
};
bool look_at_navmesh(void* context, uint64_t object) {
    auto& search = *static_cast<ActorSearch*>(context);
    if (is_template(object) || !game::is_a(object, navmesh_class)) return true;
    const uint64_t level = game::outer_of(object);
    if (!level || game::outer_of(level) != search.world) return true;
    char name[nav::kNameLimit + 1];
    if (!game::name_text(game::name_of(object), name, sizeof name)) return true;
    const size_t length = strlen(name);
    auto& parsed = *search.navigation;
    for (uint32_t i = 0; i < parsed.meshes.count; ++i) {
        auto& pending = parsed.meshes[i];
        if (pending.done || !content_path::same(reinterpret_cast<const char*>(parsed.file + pending.mesh.name), pending.mesh.name_length, name, length)) continue;
        // An earlier actor of this name may still be alive while the streamed actor is being made. Prefer one
        // with its implementation ready; the tile swap remains responsible for judging the grid.
        const uint64_t implementation = read_or<uint64_t>(object + t::navmesh_actor::implementation);
        const bool ready = implementation && read_or<uint64_t>(implementation + t::navmesh_implementation::mesh);
        if (!pending.found || ready) pending.found = object;
    }
    return true;
}

} // namespace

bool prepare_navigation(NavigationState& state, const uint8_t* file, uint64_t bytes, const char* map_path, bool& well_formed) {
    release_navigation(state);
    well_formed = false;
    if (!file || !map_path || bytes >= (256ull << 20)) return false;
    // The parser's bounded tables are scratch space at mod start, never rebuilt on a frame retry.
    static nav::Map maps[kMaps];
    static nav::Navmesh navmeshes[kNavmeshes];
    nav::File read{file, uint32_t(bytes), maps, kMaps, 0, navmeshes, kNavmeshes, 0};
    if (nav::read(&read) != nav::Read::ok) return false;
    well_formed = true;
    auto* parsed = static_cast<ParsedNavigation*>(allocate(sizeof(ParsedNavigation)));
    if (!parsed) return false;
    *parsed = ParsedNavigation{file, 0, {}};
    state.parsed = parsed;
    const size_t length = strlen(map_path);
    bool named = false;
    for (uint32_t m = 0; m < read.map_count; ++m) {
        if (!content_path::same(reinterpret_cast<const char*>(file + maps[m].path), maps[m].path_length, map_path, length)) continue;
        named = true;
        for (uint32_t k = maps[m].first_navmesh; k < maps[m].first_navmesh + maps[m].navmesh_count; ++k) {
            if (!parsed->meshes.add(PendingNavmesh{navmeshes[k], 0, 0})) {
                release_navigation(state);
                return false;
            }
        }
    }
    return named;
}

void reset_navigation(NavigationState& state) {
    auto* parsed = static_cast<ParsedNavigation*>(state.parsed);
    if (!parsed) return;
    parsed->world = 0;
    for (uint32_t i = 0; i < parsed->meshes.count; ++i) parsed->meshes[i].done = parsed->meshes[i].found = 0;
}

void release_navigation(NavigationState& state) {
    auto* parsed = static_cast<ParsedNavigation*>(state.parsed);
    if (!parsed) return;
    parsed->meshes.clear();
    release(parsed);
    state.parsed = nullptr;
}

NavigationResult swap_navigation(NavigationState& state, uint64_t world, NavigationCounts& counts) {
    auto* parsed = static_cast<ParsedNavigation*>(state.parsed);
    if (!parsed) return NavigationResult::other_grid;
    const uint64_t world_handle = game::to_handle(world);
    if (!world_handle) return NavigationResult::waiting;
    if (parsed->world != world_handle) {
        reset_navigation(state);
        parsed->world = world_handle;
    }
    if (!t::bound(t::fn::nav_add_tile) || !t::bound(t::fn::nav_remove_tile) || !t::bound(t::fn::nav_invalidate_paths) || !game::find_class(navmesh_class)) return NavigationResult::waiting;
    bool pending = false;
    for (uint32_t i = 0; i < parsed->meshes.count; ++i) {
        auto& mesh = parsed->meshes[i];
        mesh.found = 0;
        if (mesh.done && !game::from_handle(mesh.done)) mesh.done = 0;
        if (!mesh.done) pending = true;
    }
    if (!pending) return NavigationResult::done;
    ActorSearch search{parsed, world};
    game::each_object(look_at_navmesh, &search);
    NavigationResult result = NavigationResult::done;
    bool tried = false, waiting = false;
    for (uint32_t i = 0; i < parsed->meshes.count; ++i) {
        auto& mesh = parsed->meshes[i];
        if (mesh.done) continue;
        List<uint32_t> changed;
        const NavigationResult swapped = mesh.found ? swap_tiles(mesh.found, parsed->file, mesh.mesh, changed, counts) : NavigationResult::waiting;
        if (swapped == NavigationResult::waiting) {
            waiting = true;
            continue;
        }
        tried = true;
        if (swapped != NavigationResult::done) result = swapped;
        else {
            mesh.done = game::to_handle(mesh.found);
            ++counts.navmeshes;
        }
        // The actor's paths through these tiles are found again.
        struct Indices {
            uint32_t* data;
            int32_t count, capacity;
        } indices{changed.items, int32_t(changed.count), int32_t(changed.count)};
        if (changed.count) t::fn::nav_invalidate_paths(reinterpret_cast<void*>(mesh.found), &indices);
        changed.clear();
    }
    if (tried && t::bound(t::fn::nav_modifier_select) && t::fn::nav_modifier_change) pick_again(world, counts);
    if (result != NavigationResult::done) return result;
    return waiting ? NavigationResult::waiting : NavigationResult::done;
}

} // namespace tsuchi
