// The "placements" of a `maps` entry: lists of gameplay objects for the game to place in one of its maps.
//
//   "maps": [{"map": "/Game/.../Cave000_F00_P", "placements": ["/Game/Mods/my_mod/Cave000_Objects"]}]
//
// The game saves what its generators made, and nothing of an actor that is only in a level. A generator is its
// record of one placed thing, or of several of a kind. It makes a map's generators once, from the map's own
// placement lists, keeps them in the save, and makes them from the save on every later visit. A mod's list is an
// asset of the same kind as the game's lists. Its first row is the list's mark, a row that names no class, and
// every row after it is one generator. A mod can name such a list, or a list of them as the game's maps name
// theirs, whose lists are given in turn.
//
// A row is given the way the game adds a generator to a map that is running. The row is copied to the end of
// the map's generators, with copies of all it holds, and the copy gets its place in the array, its class, the
// part of the level the game's first generator belongs to, the day, and how many actors it has to make. The
// game's spawner for a generator makes the actors, and each is started where it stands as the game starts one
// it spawns. From then on they are the game's.
//
// A save that has been given a list must not be given it again, or what was collected would come back. So the
// mark goes in after the rows: a copy of the first row, which makes nothing, under a number made from the mod's
// ID and the path of the list's package. The game saves it with the generators, and a map that has it has had
// the list. The number in the asset's first row is not used: a number a tool picked can be picked twice.
#include "map_placements.h"

#include <string.h>

#include "actors.h"
#include "content_path.h"
#include "engine_call.h"
#include "placement_mode.h"
#include "game.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "names.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "sha256.h"
#include "text.h"

namespace tsuchi {
namespace {

namespace t = game::table;
namespace g = t::generator;
using game::read_or;
using maps::join;
using maps::tell;

constexpr int32_t kMostRows = 4096, kMostGenerators = 32768;
constexpr uint32_t kMostLists = 64; // that one list of lists holds
constexpr uint32_t kMostActors = 4096; // that one generator makes

// One part of a running mod that brings lists.
struct Wanted {
    uint64_t part;
    TSUCHI_Mod* mod;
    const Json* entry; // the manifest's entry
    uint64_t done_in;  // the world whose lists it has been dealt with in
};
List<Wanted> wanted; // in the order the mods started
PlacementCounts counts{};
TSUCHI_Mod* self = nullptr; // class pointers kept by native generators outlive the declaring mod

// An array as the engine keeps one.
struct Array {
    uint64_t data;
    int32_t count, max;
};
Array array_at(uint64_t at) { return {read_or<uint64_t>(at + t::array::data), read_or<int32_t>(at + t::array::count), read_or<int32_t>(at + t::array::max)}; }

// What makes the map's placed objects, or 0 while there is none.
uint64_t keeper() {
    const uint64_t framework = game::game_framework();
    const uint64_t found = framework ? read_or<uint64_t>(framework + t::game_framework::generators) : 0;
    return found && game::writable(found + t::generator_keeper::generators, t::array::size) ? found : 0;
}

// The map's generators as they are now. False when they are not as an array of generators is.
bool generators(uint64_t keeper, Array& out) {
    out = array_at(keeper + t::generator_keeper::generators);
    return out.count >= 0 && out.max >= out.count && out.count <= kMostGenerators && (!out.count || game::readable(out.data, uint64_t(out.count) * g::size));
}

bool has(const Array& all, uint64_t number) {
    for (int32_t i = 0; i < all.count; ++i)
        if (read_or<uint64_t>(all.data + uint64_t(i) * g::size + g::placement) == number) return true;
    return false;
}

// The number a list's mark is kept under: made from the mod's ID and the list's path, whose letters the engine
// does not tell apart by case. Never 0 and never the game's own "none"; 0 when there is no memory.
uint64_t mark_number(const char* mod, const char* package) {
    Text named;
    if (!join(named, "placements\n", mod, "\n", package)) return 0;
    for (char* at = named.data; *at; ++at)
        if (*at >= 'A' && *at <= 'Z') *at = char(*at + 32);
    uint8_t hash[kSha256Bytes];
    sha256(named.data, strlen(named.data), hash);
    named.clear();
    uint64_t number = 0;
    for (uint32_t i = 0; i < 8; ++i) number |= uint64_t(hash[i]) << (8 * i);
    return number && number != ~uint64_t(0) ? number : 1;
}

// A copy of a row at the end of the map's generators, the game's own way: the count is raised, room is made
// when there is none, and the row is copied with copies of all it holds. Returns its place, or -1.
int32_t append(uint64_t keeper, uint64_t row) {
    const uint64_t array = keeper + t::generator_keeper::generators;
    Array all;
    if (!generators(keeper, all) || all.count >= kMostGenerators) return -1;
    game::write(array + t::array::count, int32_t(all.count + 1));
    if (all.max <= all.count) t::fn::generator_array_grow(reinterpret_cast<void*>(array));
    const uint64_t slot = read_or<uint64_t>(array + t::array::data) + uint64_t(all.count) * g::size;
    if (read_or<int32_t>(array + t::array::max) <= all.count || !game::writable(slot, g::size)) {
        game::write(array + t::array::count, all.count); // no room came
        return -1;
    }
    t::fn::generator_copy(reinterpret_cast<void*>(slot), reinterpret_cast<void*>(row));
    game::write(slot + g::index, all.count);
    return all.count;
}

uint64_t generator_at(uint64_t keeper, int32_t place) { return read_or<uint64_t>(keeper + t::generator_keeper::generators + t::array::data) + uint64_t(place) * g::size; }

// Remove just the generator we appended. Spawn/start callbacks can append foreign rows and grow the buffer,
// so identify our row again, transfer its owned buffers to a stack row, and compact the live array. Raw moves
// are valid here as in native Grow; CopyConstruct would allocate a second set of those buffers.
void undo_row(uint64_t keeper, uint64_t number, const uint64_t* spawned = nullptr, int32_t spawned_count = 0, uint64_t spawned_type = 0) {
    Array all;
    const auto find = [&]() {
        if (!generators(keeper, all)) return int32_t(-1);
        for (int32_t i = 0; i < all.count; ++i)
            if (read_or<uint64_t>(all.data + uint64_t(i) * g::size + g::placement) == number) return i;
        return int32_t(-1);
    };
    int32_t place = find();
    // Snapshot life handles before any callback. A caller that started actors already has this snapshot,
    // which remains usable even if a start callback removed the generator altogether.
    uint64_t owned[kMostActors];
    if (!spawned) {
        if (place < 0) return; // never take actors from a foreign replacement
        spawned_type = read_or<uint64_t>(all.data + uint64_t(place) * g::size + g::class_);
        const Array actors = array_at(all.data + uint64_t(place) * g::size + g::actors);
        if (actors.count >= 0 && actors.count <= int32_t(kMostActors) && actors.max >= actors.count &&
            (!actors.count || game::readable(actors.data, uint64_t(actors.count) * 8))) {
            spawned_count = actors.count;
            for (int32_t i = 0; i < spawned_count; ++i) owned[i] = game::to_handle(read_or<uint64_t>(actors.data + uint64_t(i) * 8));
        }
        spawned = owned;
    }
    // Let retirement find the original generator/index while it is still published. Resolve each life
    // again after earlier callbacks, so a recycled actor or one transferred to another row is left alone.
    for (int32_t i = 0; i < spawned_count; ++i) {
        const uint64_t actor = game::from_handle(spawned[i]);
        if (!actor || !game::is_game_actor(actor) || game::type_of(actor) != spawned_type || game::actor_placement(actor) != number) continue;
        game::EngineCall destroy(actor, "K2_DestroyActor");
        if (destroy.ready() != TSUCHI_OK || destroy.invoke() != TSUCHI_OK) game::remove_actor(actor, true, 0, 0);
    }
    place = find();
    if (place < 0 || !game::writable(all.data, uint64_t(all.count) * g::size)) return;
    alignas(16) uint8_t detached[g::most];
    const uint64_t slot = all.data + uint64_t(place) * g::size;
    memcpy(detached, reinterpret_cast<const void*>(slot), g::size);
    memmove(reinterpret_cast<void*>(slot), reinterpret_cast<const void*>(slot + g::size), uint64_t(all.count - place - 1) * g::size);
    memset(reinterpret_cast<void*>(all.data + uint64_t(all.count - 1) * g::size), 0, g::size);
    game::write(keeper + t::generator_keeper::generators + t::array::count, all.count - 1);
    for (int32_t i = place; i < all.count - 1; ++i) game::write(all.data + uint64_t(i) * g::size + g::index, i);
    // The native destructor releases owned serialization, soft-reference and actor arrays;
    // its actor pointers are borrowed UObject references, so it does not destroy the actors itself.
    t::fn::generator_destroy(detached);
}
enum class Given : uint32_t { ok, count, generators, clock, moved, fewer };
const char* const kWhyNot[] = {"", "it asks for no actors, for too many, or has made some already", "the map's generators are not as expected, or there is no memory for one more",
                               "the map has no clock to take the day from", "the game did not keep its generator", "the game made fewer actors for it than it asks for"};

// One row into the map. `made` gets how many actors the game made for it and the framework started.
Given give_row(uint64_t keeper, uint64_t world, uint64_t row, uint64_t type, uint64_t sublevel, uint32_t& made) {
    made = 0;
    const uint64_t number = read_or<uint64_t>(row + g::placement);
    const uint32_t asked = read_or<uint32_t>(row + g::count);
    if (!asked || asked > kMostActors || read_or<int32_t>(row + g::actors + t::array::count) != 0) return Given::count;
    const uint64_t framework = game::game_framework();
    const uint64_t clock = framework ? read_or<uint64_t>(framework + t::game_framework::clock) : 0;
    const uint64_t day_of = clock ? game::table_entry(clock, t::clock_table::day) : 0;
    if (!day_of) return Given::clock;
    const auto day = uint32_t(reinterpret_cast<uint64_t (*)(void*)>(day_of)(reinterpret_cast<void*>(clock)));
    const int32_t place = append(keeper, row);
    if (place < 0) return Given::generators;
    uint64_t slot = generator_at(keeper, place);
    game::write(slot + g::class_, type);
    game::write(slot + g::sublevel, sublevel);
    game::write(slot + g::birth_day, day);
    game::write(slot + g::left, asked);
    game::write(slot + g::flags, uint32_t(read_or<uint32_t>(slot + g::flags) | t::constant::generator_added));
    t::fn::generator_spawn(reinterpret_cast<void*>(slot), reinterpret_cast<void*>(world), 0);
    // Blueprint callbacks can move the outer array. The spawned actor array owns independent storage.
    slot = generator_at(keeper, place);
    if (read_or<uint64_t>(slot + g::placement) != number) { undo_row(keeper, number); return Given::moved; }
    const Array actors = array_at(slot + g::actors);
    if (actors.count < 0 || uint32_t(actors.count) > asked || actors.max < actors.count || (actors.count && !game::readable(actors.data, uint64_t(actors.count) * 8))) {
        undo_row(keeper, number);
        return Given::moved;
    }
    uint64_t spawned[kMostActors];
    for (int32_t i = 0; i < actors.count; ++i) spawned[i] = game::to_handle(read_or<uint64_t>(actors.data + uint64_t(i) * 8));
    for (int32_t i = 0; i < actors.count; ++i) {
        const uint64_t actor = game::from_handle(spawned[i]);
        // The game read the authored values as it made the actor. Start registers its parts and position.
        if (!actor || !game::is_game_actor(actor) || game::type_of(actor) != type || game::actor_placement(actor) != number || !game::start_made(actor, type)) {
            undo_row(keeper, number, spawned, actors.count, type);
            made = 0;
            return Given::fewer;
        }
        ++made;
        // Starting parts also runs callbacks. A lost/moved generator is refused before another actor starts;
        // the saved life handles allow full actor cleanup even when its row was removed by that callback.
        if (read_or<uint64_t>(generator_at(keeper, place) + g::placement) != number) {
            undo_row(keeper, number, spawned, actors.count, type);
            made = 0;
            return Given::moved;
        }
    }
    if (made == asked) return Given::ok;
    undo_row(keeper, number, spawned, actors.count, type);
    made = 0;
    return Given::fewer;
}
// One placement list into the map. `package` is the path of the list's package: the mod's log names the list
// by it, and the list's mark is numbered from it.
void give_rows(TSUCHI_Mod* mod, const char* package, uint64_t list, uint64_t keeper, uint64_t world, const char* map) {
    const auto refuse = [&](const char* why) {
        ++counts.lists_refused;
        tell(mod, "maps: the placement list ", package, " was not given to ", map, ": ", why);
    };
    static const char kNotAList[] = "it is not laid out as the game's placement lists are, or has no rows";
    // The rows, and where a row names its class, are found by their names in the game's own description of a list.
    const uint64_t type = game::type_of(list);
    game::Place array{}, first{}, named{};
    uint64_t row_type = 0, rows = 0;
    if (game::find_place(type, list, "ActorGeneratorList", array) != TSUCHI_OK || game::find_place(type, list, "ActorGeneratorList[0]", first) != TSUCHI_OK ||
        game::struct_at(first, row_type, rows) != TSUCHI_OK || game::find_place(type, list, "ActorGeneratorList[0].SoftRefActorClass", named) != TSUCHI_OK ||
        read_or<int32_t>(row_type + t::struct_type::bytes) != int32_t(g::size))
        return refuse(kNotAList);
    const int32_t count = read_or<int32_t>(array.address + t::array::count);
    const uint64_t class_at = named.address - rows + t::soft_object::path; // within a row: its class's path, as one name
    if (count < 1 || count > kMostRows || !game::readable(rows, uint64_t(count) * g::size) || class_at + sizeof(game::Name) > g::size) return refuse(kNotAList);
    const game::Name marks = read_or<game::Name>(rows + class_at);
    if (marks.index || marks.number) return refuse("it has no mark: its first row has to name no class");
    const uint64_t mark = mark_number(mod_id(mod), package);
    Array all;
    if (!mark) return refuse("there is no memory for it");
    if (!generators(keeper, all)) return refuse("the map's generators are not as expected");
    if (has(all, mark)) { // this save has had the list: what was used up stays used up
        ++counts.lists_known;
        return;
    }
    // The part of the level the new ones belong to is that of the game's first generator, so that they are
    // awake when its own are.
    const uint64_t sublevel = read_or<uint64_t>((all.count ? all.data : rows) + g::sublevel);
    bool whole = true;
    for (int32_t r = 1; r < count; ++r) {
        const uint64_t row = rows + uint64_t(r) * g::size;
        char place[11];
        maps::decimal(place, uint32_t(r));
        const auto refuse_row = [&](const char* why) {
            ++counts.rows_refused;
            whole = false;
            tell(mod, "maps: ", package, ", row ", place, " was not placed in ", map, ": ", why);
        };
        const uint64_t number = read_or<uint64_t>(row + g::placement);
        if (!number || number == ~uint64_t(0)) {
            refuse_row("it has no placement number");
            continue;
        }
        if (generators(keeper, all) && has(all, number)) {
            ++counts.rows_known;
            tell(mod, "maps: ", package, ", row ", place, " was passed over: ", map, " has an object under its placement number already");
            continue;
        }
        static char class_path[game::kNameBytes];
        TSUCHI_Object class_asset{};
        if (!game::name_text(read_or<game::Name>(row + class_at), class_path, sizeof class_path) || host.asset->load(mod, class_path, &class_asset) != TSUCHI_OK) {
            refuse_row("its class could not be loaded");
            continue;
        }
        const uint64_t made_class = game::from_handle(class_asset.id);
        if (!game::is_class(made_class) || !game::derives_from(made_class, game::game_actor_class())) {
            refuse_row("its class is no class of the game's actors");
            continue;
        }
        TSUCHI_Object kept_class{};
        if (!self || host.asset->load(self, class_path, &kept_class) != TSUCHI_OK || game::from_handle(kept_class.id) != made_class) {
            refuse_row("its class could not be kept for the map's generators");
            continue;
        }
        uint32_t made = 0;
        const Given given = give_row(keeper, world, row, made_class, sublevel, made);
        counts.actors_made += made;
        if (given == Given::ok) ++counts.rows_given;
        else refuse_row(kWhyNot[uint32_t(given)]);
    }
    // The mark, last: the list counts as given only when its rows went in.
    const int32_t marked = whole ? append(keeper, rows) : -1;
    if (marked < 0) {
        ++counts.lists_partial;
        tell(mod, "maps: the placement list ", package, " is not marked as given to ", map, ", and what the map lacks of it is tried again the next time the map loads");
        return;
    }
    const uint64_t slot = generator_at(keeper, marked);
    game::write(slot + g::placement, mark);
    game::write(slot + g::sublevel, sublevel);
    ++counts.lists_given;
}

// The package an object is in: its outermost outer. 0 for outers that lead back into themselves.
uint64_t package_of(uint64_t object) {
    uint64_t package = 0;
    for (uint32_t depth = 0; object && depth < 4096; ++depth) package = object, object = game::outer_of(object);
    return object ? 0 : package;
}

// A list the mod names: a placement list, or a list of placement lists as the game names its own. Each placement
// list a list of lists holds is given in turn, by the path of its own package and with its own mark, so a list
// has the same mark whether the mod names it or the list of lists that holds it.
void give_list(const Wanted& from, const char* package, uint64_t keeper, uint64_t world, const char* map) {
    TSUCHI_Mod* const mod = from.mod;
    const auto refuse = [&](const char* why) {
        ++counts.lists_refused;
        tell(mod, "maps: the placement list ", package, " was not given to ", map, ": ", why);
    };
    // The asset is named as the engine names one: the package's path, a dot, and the last part of the path again.
    Text path;
    TSUCHI_Object asset{};
    const char* leaf = package;
    for (const char* at = package; *at; ++at)
        if (*at == '/') leaf = at + 1;
    const TSUCHI_Result loaded = join(path, package, ".", leaf) ? host.asset->load(mod, path.get(), &asset) : TSUCHI_NO_MEMORY;
    path.clear();
    if (loaded != TSUCHI_OK) return refuse(loaded == TSUCHI_NO_MEMORY ? "there is no memory for it" : "it could not be loaded");
    const uint64_t named = game::from_handle(asset.id), type = game::type_of(named);
    game::Place lists{};
    if (game::find_place(type, named, "AssetList", lists) != TSUCHI_OK) return give_rows(mod, package, named, keeper, world, map);
    game::Value held{};
    if (game::read_value(lists, held) != TSUCHI_OK || held.kind != game::ValueKind::array || held.count < 1 || held.count > kMostLists)
        return refuse("its AssetList holds no placement lists, or more than 64");
    uint64_t given[kMostLists], packages[kMostLists];
    for (uint32_t i = 0; i < held.count; ++i) {
        char index[11];
        maps::decimal(index, i);
        const auto refuse_held = [&](const char* why) {
            ++counts.lists_refused;
            tell(mod, "maps: the placement list ", package, " was not given to ", map, ": its AssetList[", index, "] ", why);
        };
        game::Place one{};
        game::Value inner{};
        uint64_t list = 0;
        if (!join(path, "AssetList[", index, "]")) return refuse("there is no memory for it");
        if (game::find_place(type, named, path.get(), one) == TSUCHI_OK && game::object_property(one) && game::read_value(one, inner) == TSUCHI_OK &&
            inner.kind == game::ValueKind::object)
            list = game::from_handle(inner.object);
        path.clear();
        given[i] = list, packages[i] = package_of(list);
        static char held_package[game::kNameBytes];
        if (!list || !packages[i] || !game::name_text(game::name_of(packages[i]), held_package, sizeof held_package)) {
            refuse_held("holds no placement list");
            continue;
        }
        bool shared = false;
        for (uint32_t j = 0; j < i; ++j) shared = shared || (packages[j] == packages[i] && given[j] != list);
        if (shared) {
            refuse_held("is in the package of an earlier one: each placement list needs a package of its own");
            continue;
        }
        give_rows(mod, held_package, list, keeper, world, map);
    }
}

} // namespace

bool placements_well_formed(const Json& entry, Text& wrong) {
    if (!placement_mode_well_formed(entry, wrong)) return false;
    const Json* lists = entry.find("placements");
    bool good = !lists || lists->kind == Json::Kind::list;
    for (const Json* list = good && lists ? lists->first : nullptr; list && good; list = list->next) {
        if (list->kind != Json::Kind::text) { good = false; break; }
        if (const char* why = content_path::wrong(list->text, list->bytes, content_path::Names::package)) {
            join(wrong, "\"maps\": \"placements\": ", list->text, why);
            return false;
        }
    }
    if (!good) wrong.set("\"maps\": \"placements\" is a list of the mod's placement lists, each by the path of its package");
    return good;
}

bool placements_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry) {
    const Json* lists = entry.find("placements");
    return !lists || !lists->count || wanted.add(Wanted{part, mod, &entry, 0}) != nullptr;
}

void placements_end(uint64_t part) {
    for (uint32_t i = 0; i < wanted.count; ++i) {
        if (wanted[i].part != part) continue;
        // The others keep their order, which is the order their mods started in.
        memmove(static_cast<void*>(&wanted[i]), &wanted[i + 1], sizeof(Wanted) * (wanted.count - i - 1));
        --wanted.count;
        return;
    }
}

void placements_look(uint64_t world, const char* map, bool (*failed)(uint64_t part)) {
    // A part declared after the world's lists were dealt with is dealt with on its own; the others are not given again.
    bool waiting = false;
    for (uint32_t i = 0; i < wanted.count && !waiting; ++i) waiting = wanted[i].done_in != world;
    if (!waiting || !t::bound(t::fn::generator_copy) || !t::bound(t::fn::generator_array_grow) || !t::bound(t::fn::generator_spawn) || !t::bound(t::fn::generator_destroy)) return;
    const uint64_t made_by = keeper(), address = game::from_handle(world);
    // Before the game has made the map's generators there is nothing to add to, and nothing to tell a save that
    // has a list from one that has not.
    if (!made_by || !address || read_or<uint8_t>(made_by + t::generator_keeper::built) != t::constant::generators_built) return;
    // Unreadable mode state is retried; it must not mark this world as dealt with.
    if (!placement_mode_matches(PlacementMode::story) && !placement_mode_matches(PlacementMode::night) && !placement_mode_matches(PlacementMode::olimar)) return;
    for (uint32_t i = 0; i < wanted.count; ++i) {
        if (wanted[i].done_in == world) continue;
        wanted[i].done_in = world;
        const Wanted from = wanted[i];
        if (!content_path::same(from.entry->find("map")->text, map) || failed(from.part) || !placement_mode_matches(*from.entry)) continue;
        for (const Json* list = from.entry->find("placements")->first; list; list = list->next) give_list(from, list->text, made_by, address, map);
    }
}

void placements_start(TSUCHI_Mod* context) { self = context; }

PlacementCounts map_placement_counts() { return counts; }

} // namespace tsuchi
