// The "placement_changes" of a `maps` entry: rows of the game's own placement lists to move or to remove.
//
//   "maps": [{"map": "/Game/.../Cave000_F00_P", "placement_changes": {
//       "/Game/.../ActorPlacementInfo/AP_Cave000_F00_P_Objects": {
//           "move": {"17179869271": {"location": [121, -50, 0], "rotation": [0, 28.6, 0]}},
//           "remove": ["17179869268"]}}}]
//
// The game makes its gameplay objects from generators, and a map's generators from its placement lists, not from
// its levels: moving or removing a level's actor does nothing to them. A list is an ActorPlacementInfoAsset, and a
// row of it is named by its placement number, the generator's GenerateInfo.DebugUniqueId, as a decimal number in
// text. The number stays the same when a map is cooked again, which a row's place in its list does not.
//
// The game reads a list's rows in two places. As it makes a map's generators from the lists, for a save that has
// no record of the map, it copies every row of every list into a generator. As it makes them from the save's
// record, it copies the rows of the lists an update added that the save has no record of yet. Both are wrapped
// here. Around each, the rows of every list a running mod changes are set out anew: the asset's array is pointed
// at a copy that lacks the removed rows and has the moved ones in their new places, and is pointed back at its
// own rows when the game is done. So the game reads a list that says what the mods say, and the asset itself is
// never written. The copy holds the rows' bytes, and the arrays inside them stay the asset's: the game only reads
// them, and the copy is let go of without destroying anything.
//
// What the game made from a changed list is the game's own from then on: it saves the generators it made, so a
// save keeps a removed row's object unplaced and a moved one where it was moved, with or without the mod. A save
// that already has a record of the map is made from that record, and the changes do not reach it.
#include "map_placement_changes.h"

#include <string.h>

#include "angles.h"
#include "content_path.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "names.h"
#include "objects.h"
#include "placement_mode.h"
#include "reflect.h"
#include "registry.h"
#include "text.h"

namespace tsuchi {
namespace {

namespace t = game::table;
namespace g = t::generator;
namespace x = t::engine_transform;
using game::read_or;
using maps::join;
using maps::tell;

constexpr int32_t kMostRows = 65536, kMostLists = 4096;

// One part of a running mod that changes the game's lists.
struct Wanted {
    uint64_t part;
    TSUCHI_Mod* mod;
    const Json* entry; // the manifest's entry
};
List<Wanted> wanted; // in the order they were declared, which is not the mods' order
PlacementChangeCounts counts{};
TSUCHI_Mod* self = nullptr;
using Build = decltype(t::code_site::placement_build)::Function;
using Restore = decltype(t::code_site::placement_restore)::Function;
Build next_build = nullptr;
Restore next_restore = nullptr;
bool build_hooked = false, restore_hooked = false;
uint32_t depth = 0; // the game reads lists inside a read only by way of a callback; such a read is passed on

// ---- How it is written ----------------------------------------------------------------------------------------

// A row's number, written as its decimal digits. False for anything else, and for 0, which no row has.
bool row_number(const char* text, size_t bytes, uint64_t& out) {
    if (!bytes || bytes > 20) return false;
    uint64_t number = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t digit = uint64_t(text[i] - '0');
        if (number > (~uint64_t(0) - digit) / 10) return false;
        number = number * 10 + digit;
    }
    out = number;
    return number != 0;
}

bool three_numbers(const Json* value) {
    if (!value || value->kind != Json::Kind::list || value->count != 3) return false;
    for (const Json* number = value->first; number; number = number->next)
        if (number->kind != Json::Kind::number) return false;
    return true;
}

// A number as text, for a line of a log.
const char* decimal64(char out[21], uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do digits[count++] = char('0' + value % 10);
    while (value /= 10);
    char* at = out;
    while (count) *at++ = digits[--count];
    *at = 0;
    return out;
}

// The change a list's entry makes to one row: its move, the entry's remove list for a removal, or null.
const Json* change_of(const Json& list, uint64_t number) {
    uint64_t named = 0;
    if (const Json* move = list.find("move"))
        for (const Json* row = move->first; row; row = row->next)
            if (row_number(row->name, strlen(row->name), named) && named == number) return row;
    if (const Json* remove = list.find("remove"))
        for (const Json* row = remove->first; row; row = row->next)
            if (row_number(row->text, row->bytes, named) && named == number) return remove;
    return nullptr;
}

// ---- Reading the game's lists ---------------------------------------------------------------------------------

struct Array {
    uint64_t data;
    int32_t count, max;
};
Array array_at(uint64_t at) { return {read_or<uint64_t>(at + t::array::data), read_or<int32_t>(at + t::array::count), read_or<int32_t>(at + t::array::max)}; }
bool readable_array(const Array& array, uint64_t each, int32_t most) {
    return array.count >= 0 && array.count <= most && array.max >= array.count && (!array.count || game::readable(array.data, uint64_t(array.count) * each));
}

// The package an object is in: its outermost outer. 0 for outers that lead back into themselves.
uint64_t package_of(uint64_t object) {
    uint64_t package = 0;
    for (uint32_t depth_left = 4096; object && depth_left; --depth_left) package = object, object = game::outer_of(object);
    return object ? 0 : package;
}

// The array of a world's lists of lists, as its settings keep it, or 0.
uint64_t lists_of(uint64_t world) {
    const uint64_t level = read_or<uint64_t>(world + t::world::persistent_level);
    const uint64_t settings = level ? read_or<uint64_t>(level + t::engine_level::world_settings) : 0;
    game::Place place{};
    if (!settings || game::find_place(game::type_of(settings), settings, "ActorPlacementInfoListAssetsCache", place) != TSUCHI_OK) return 0;
    return place.address;
}

// One list as the game will read it while the mods' changes are made: the asset's array, and what it held.
struct Swap {
    uint64_t array, data;
    int32_t count, max;
    void* block;
};
// One row a mod changes, and whose change it is.
struct Decision {
    uint64_t number;
    uint32_t from;     // the mod's place in `wanted`
    const Json* move;  // its move, or null for a removal
};

// What one read of the lists does: the parts that change the map, the swaps made, and the lists seen.
struct Read {
    uint64_t world;
    const char* map;
    List<uint32_t> parts;         // places in `wanted`, in the mods' order
    List<Swap> swaps;
    List<uint64_t> lists;         // the lists already set out
    List<const Json*> seen;       // the entries of mods' changes whose lists the map has
    bool memory = true;

    ~Read() {
        for (uint32_t s = swaps.count; s-- > 0;) {
            const Swap& swap = swaps[s];
            game::write(swap.array + t::array::data, swap.data);
            game::write(swap.array + t::array::count, swap.count);
            game::write(swap.array + t::array::max, swap.max);
            release(swap.block);
        }
        parts.clear(), swaps.clear(), lists.clear(), seen.clear();
    }
};

// True when `a` comes after `b` in the mods' order. Parts of one mod keep the order they were declared in.
bool later(const Wanted& a, const Wanted& b) {
    const uint32_t rank_a = mod_rank(a.mod), rank_b = mod_rank(b.mod);
    return rank_a != rank_b ? rank_a > rank_b : a.part > b.part;
}

// A mod's entry for a list, by the path of the list's package.
const Json* entry_for(const Wanted& from, const char* package) {
    for (const Json* list = from.entry->find("placement_changes")->first; list; list = list->next)
        if (content_path::same(list->name, package)) return list;
    return nullptr;
}

// Writes a move's place and turn, and its scale when it has one, into a row's two transforms.
void put_move(uint64_t row, const Json& move) {
    const Json* location = move.find("location"), *rotation = move.find("rotation"), *scale = move.find("scale");
    const float angles[3] = {float(rotation->at(0)->number), float(rotation->at(1)->number), float(rotation->at(2)->number)};
    float q[4];
    math::to_quaternion(angles, q);
    const uint32_t transforms[] = {uint32_t(g::init_transform), uint32_t(g::transform)};
    float place[3], size[3];
    for (uint32_t i = 0; i < 3; ++i) place[i] = float(location->at(i)->number), size[i] = scale ? float(scale->at(i)->number) : 0;
    // The row is the framework's own copy, so it is written as memory of the framework's.
    for (const uint32_t transform : transforms) {
        memcpy(reinterpret_cast<void*>(row + transform + x::rotation), q, sizeof q);
        memcpy(reinterpret_cast<void*>(row + transform + x::translation), place, sizeof place);
        if (scale) memcpy(reinterpret_cast<void*>(row + transform + x::scale), size, sizeof size);
    }
}

// Sets out one list for the read: each part's changes to it, the later mod's where two change one row.
void set_out(Read& read, uint64_t asset) {
    for (uint32_t i = 0; i < read.lists.count; ++i)
        if (read.lists[i] == asset) return;
    static char package[game::kNameBytes];
    const uint64_t outermost = package_of(asset);
    if (!outermost || !game::name_text(game::name_of(outermost), package, sizeof package)) return;
    // The parts that change this list, and their entries for it.
    struct Changer { uint32_t from; const Json* list; bool whole; };
    List<Changer> changers;
    for (uint32_t p = 0; p < read.parts.count; ++p)
        if (const Json* list = entry_for(wanted[read.parts[p]], package))
            if (!changers.add(Changer{read.parts[p], list, true}) || !read.seen.add(list)) read.memory = false;
    if (!changers.count) return;
    if (!read.lists.add(asset)) read.memory = false;
    const auto refuse_all = [&](const char* why) {
        for (uint32_t c = 0; c < changers.count; ++c) {
            ++counts.lists_refused;
            tell(wanted[changers[c].from].mod, "maps: the placement list ", package, " was not changed as the game read it for ", read.map, ": ", why);
        }
        changers.clear();
    };
    const uint64_t type = game::type_of(asset);
    game::Place place{}, first{};
    if (game::find_place(type, asset, "ActorGeneratorList", place) != TSUCHI_OK) return refuse_all("it is not laid out as the game's placement lists are");
    const Array rows = array_at(place.address);
    uint64_t row_type = 0, base = 0;
    if (!readable_array(rows, g::size, kMostRows) ||
        (rows.count && (game::find_place(type, asset, "ActorGeneratorList[0]", first) != TSUCHI_OK || game::struct_at(first, row_type, base) != TSUCHI_OK ||
                        read_or<int32_t>(row_type + t::struct_type::bytes) != int32_t(g::size))))
        return refuse_all("it is not laid out as the game's placement lists are");
    const auto row_at = [&](int32_t i) { return rows.data + uint64_t(i) * g::size; };
    const auto has = [&](uint64_t number) {
        for (int32_t i = 0; i < rows.count; ++i)
            if (read_or<uint64_t>(row_at(i) + g::placement) == number) return true;
        return false;
    };
    // A mod's changes to the list are made whole or not at all: an entry made for another version of the list
    // changes nothing.
    for (uint32_t c = 0; c < changers.count; ++c) {
        uint64_t number = 0;
        const Json* list = changers[c].list;
        const auto check = [&](const char* text, size_t bytes) {
            if (!row_number(text, bytes, number) || has(number)) return;
            if (changers[c].whole) ++counts.lists_refused;
            changers[c].whole = false;
            tell(wanted[changers[c].from].mod, "maps: the placement list ", package, " has no row ", text, " as the game reads it for ", read.map,
                 ", so none of this mod's changes to the list were made");
        };
        if (const Json* move = list->find("move"))
            for (const Json* row = move->first; row; row = row->next) check(row->name, strlen(row->name));
        if (const Json* remove = list->find("remove"))
            for (const Json* row = remove->first; row; row = row->next) check(row->text, row->bytes);
    }
    // Each row's change: the later mod's, where two change one row, and both mods' logs say so.
    List<Decision> decisions;
    char said[21];
    for (uint32_t c = 0; c < changers.count; ++c) {
        if (!changers[c].whole) continue;
        for (int32_t i = 0; i < rows.count; ++i) {
            const uint64_t number = read_or<uint64_t>(row_at(i) + g::placement);
            const Json* change = change_of(*changers[c].list, number);
            if (!change) continue;
            const Json* move = change->kind == Json::Kind::object ? change : nullptr;
            Decision* earlier = nullptr;
            for (uint32_t d = 0; d < decisions.count && !earlier; ++d)
                if (decisions[d].number == number) earlier = &decisions[d];
            if (earlier && earlier->from == changers[c].from) continue; // one number, two rows: the same change for both
            if (earlier) {
                const char* later = mod_id(wanted[changers[c].from].mod), *before = mod_id(wanted[earlier->from].mod);
                decimal64(said, number);
                tell(wanted[earlier->from].mod, "maps: ", package, ", row ", said, ": ", later, " changes it too, and the game uses that change, as that mod started later");
                tell(wanted[changers[c].from].mod, "maps: ", package, ", row ", said, ": this mod's change is used in place of ", before, "'s, as this mod started later");
                earlier->from = changers[c].from, earlier->move = move;
            } else if (!decisions.add(Decision{number, changers[c].from, move})) {
                read.memory = false;
            }
        }
    }
    changers.clear();
    if (!decisions.count) return;
    const auto decided = [&](uint64_t number) -> const Decision* {
        for (uint32_t d = 0; d < decisions.count; ++d)
            if (decisions[d].number == number) return &decisions[d];
        return nullptr;
    };
    // The list as the mods say it is: a copy of its rows without the removed ones, and with the moved ones moved.
    int32_t kept = 0;
    for (int32_t i = 0; i < rows.count; ++i) {
        const Decision* decision = decided(read_or<uint64_t>(row_at(i) + g::placement));
        kept += !decision || decision->move;
    }
    void* block = allocate(uint64_t(kept) * g::size + 16);
    if (!block || !read.swaps.reserve(read.swaps.count + 1)) {
        release(block);
        read.memory = false;
        for (uint32_t d = 0; d < decisions.count; ++d)
            if (d == 0 || decisions[d].from != decisions[d - 1].from)
                tell(wanted[decisions[d].from].mod, "maps: the placement list ", package, " was not changed as the game read it for ", read.map, ": there is no memory for it");
        decisions.clear();
        return;
    }
    const uint64_t copy = (reinterpret_cast<uint64_t>(block) + 15) & ~uint64_t(15);
    int32_t at = 0;
    for (int32_t i = 0; i < rows.count; ++i) {
        const uint64_t row = row_at(i), number = read_or<uint64_t>(row + g::placement);
        const Decision* decision = decided(number);
        decimal64(said, number);
        if (decision && !decision->move) {
            ++counts.rows_removed;
            tell(wanted[decision->from].mod, "maps: ", package, ", row ", said, " was removed as the game read the list for ", read.map);
            continue;
        }
        const uint64_t to = copy + uint64_t(at++) * g::size;
        memcpy(reinterpret_cast<void*>(to), reinterpret_cast<const void*>(row), g::size);
        if (!decision) continue;
        put_move(to, *decision->move);
        ++counts.rows_moved;
        tell(wanted[decision->from].mod, "maps: ", package, ", row ", said, " was moved as the game read the list for ", read.map);
    }
    decisions.clear();
    read.swaps.add(Swap{place.address, rows.data, rows.count, rows.max, block});
    game::write(place.address + t::array::data, copy);
    game::write(place.address + t::array::count, kept);
    game::write(place.address + t::array::max, kept);
    ++counts.lists_changed;
}

// Sets out every list of the world's lists of lists that a running mod changes. False when no mod changes the map.
bool begin_read(Read& read, uint64_t world, uint64_t lists) {
    static char map[game::kNameBytes];
    const uint64_t package = package_of(world);
    if (!wanted.count || !package || !game::name_text(game::name_of(package), map, sizeof map)) return false;
    read.world = world, read.map = map;
    for (uint32_t i = 0; i < wanted.count; ++i)
        if (content_path::same(wanted[i].entry->find("map")->text, map) && !read.parts.add(i)) read.memory = false;
    if (!read.parts.count) return false;
    // The mode decides whose changes are made. It is read before play, as the game reads its lists then. A map
    // loaded from the title screen can have no rule in its world yet, and then the story's own state says.
    PlacementMode mode = map_mode_before_play(world);
    if (mode == PlacementMode::invalid) mode = story_mode_from_state(true);
    if (mode == PlacementMode::invalid) {
        for (uint32_t p = 0; p < read.parts.count; ++p)
            tell(wanted[read.parts[p]].mod, "maps: the game's mode could not be read as the game read the placement lists of ", map, ", so the changes to them were not made");
        read.parts.clear();
        return false;
    }
    for (uint32_t p = read.parts.count; p-- > 0;)
        if (placement_mode(*wanted[read.parts[p]].entry) != mode) memmove(&read.parts[p], &read.parts[p + 1], sizeof(uint32_t) * (--read.parts.count - p));
    if (!read.parts.count) return false;
    // A mod can declare its changes after a later mod has: the mods' order decides, then the order a mod declared them in.
    for (uint32_t p = 1; p < read.parts.count; ++p) {
        const uint32_t moving = read.parts[p];
        uint32_t at = p;
        while (at && later(wanted[read.parts[at - 1]], wanted[moving])) {
            read.parts[at] = read.parts[at - 1];
            --at;
        }
        read.parts[at] = moving;
    }
    const Array outer = lists ? array_at(lists) : Array{0, -1, -1};
    if (readable_array(outer, 8, kMostLists)) {
        for (int32_t i = 0; i < outer.count; ++i) {
            const uint64_t held = read_or<uint64_t>(outer.data + uint64_t(i) * 8);
            game::Place place{};
            if (!held || game::find_place(game::type_of(held), held, "AssetList", place) != TSUCHI_OK) continue;
            const Array inner = array_at(place.address);
            if (!readable_array(inner, 8, kMostLists)) continue;
            for (int32_t j = 0; j < inner.count; ++j)
                if (const uint64_t asset = read_or<uint64_t>(inner.data + uint64_t(j) * 8)) set_out(read, asset);
        }
    }
    // A list a mod names that the map does not have.
    for (uint32_t p = 0; p < read.parts.count; ++p) {
        const Wanted& from = wanted[read.parts[p]];
        for (const Json* list = from.entry->find("placement_changes")->first; list; list = list->next) {
            bool there = false;
            for (uint32_t s = 0; s < read.seen.count && !there; ++s) there = read.seen[s] == list;
            if (!there) tell(from.mod, "maps: ", map, " has no placement list ", list->name, ", so the changes to it were not made");
        }
    }
    if (!read.memory) {
        for (uint32_t p = 0; p < read.parts.count; ++p) tell(wanted[read.parts[p]].mod, "maps: there was no memory for all of the changes to the placement lists of ", map);
    }
    return true;
}

// The game makes a map's generators from its lists.
void on_build(void* keeper, void* world, void* lists) {
    if (depth || !wanted.count) return next_build(keeper, world, lists);
    ++depth;
    {
        Read read;
        const uint64_t address = reinterpret_cast<uint64_t>(world);
        begin_read(read, address, lists ? reinterpret_cast<uint64_t>(lists) : lists_of(address));
        next_build(keeper, world, lists);
    }
    --depth;
}

// The game makes a map's generators from the save's record, and reads only the rows of lists that the save has
// no record of yet.
bool on_restore(void* keeper, void* world, uint32_t mode) {
    if (depth || !wanted.count) return next_restore(keeper, world, mode);
    ++depth;
    bool restored = false;
    {
        Read read;
        const uint64_t address = reinterpret_cast<uint64_t>(world);
        const bool changes = begin_read(read, address, lists_of(address));
        restored = next_restore(keeper, world, mode);
        if (changes && restored) {
            ++counts.from_save;
            for (uint32_t p = 0; p < read.parts.count; ++p)
                tell(wanted[read.parts[p]].mod, "maps: the game made ", read.map, " from the save's record of the objects placed there, which keeps each where the save has it: ",
                     "changes to its placement lists reach a save that has not been to ", read.map, " yet");
        }
    }
    --depth;
    return restored;
}

} // namespace

bool placement_changes_well_formed(const Json& entry, Text& wrong) {
    const Json* lists = entry.find("placement_changes");
    if (!lists) return true;
    static const char kShape[] = "\"maps\": \"placement_changes\" holds the game's placement lists by the path of each package, each with rows to \"move\" and rows to \"remove\"";
    if (lists->kind != Json::Kind::object || !lists->count) return wrong.set(kShape), false;
    for (const Json* list = lists->first; list; list = list->next) {
        if (const char* why = content_path::wrong(list->name, strlen(list->name), content_path::Names::package))
            return join(wrong, "\"maps\": \"placement_changes\": ", list->name, why), false;
        for (const Json* other = lists->first; other != list; other = other->next)
            if (content_path::same(other->name, list->name)) return join(wrong, "\"maps\": \"placement_changes\" names ", list->name, " twice"), false;
        const Json* move = list->kind == Json::Kind::object ? list->find("move") : nullptr, *remove = list->kind == Json::Kind::object ? list->find("remove") : nullptr;
        if (list->kind != Json::Kind::object || !list->count || list->count != uint32_t(!!move + !!remove)) return wrong.set(kShape), false;
        if (move && (move->kind != Json::Kind::object || !move->count))
            return join(wrong, "\"maps\": \"placement_changes\": ", list->name, ": \"move\" holds rows by their placement numbers, each with where it goes"), false;
        if (remove && (remove->kind != Json::Kind::list || !remove->count))
            return join(wrong, "\"maps\": \"placement_changes\": ", list->name, ": \"remove\" is a list of rows' placement numbers"), false;
        static const char kNumber[] = " is no row: a row is named by its placement number, in decimal digits and as text, such as \"17179869271\"";
        uint64_t number = 0, other = 0;
        for (const Json* row = move ? move->first : nullptr; row; row = row->next) {
            if (!row_number(row->name, strlen(row->name), number)) return join(wrong, "\"maps\": \"placement_changes\": ", list->name, ": \"move\": ", row->name, kNumber), false;
            const Json* location = row->kind == Json::Kind::object ? row->find("location") : nullptr;
            const Json* rotation = location ? row->find("rotation") : nullptr, *scale = location ? row->find("scale") : nullptr;
            if (!three_numbers(location) || !three_numbers(rotation) || (scale && !three_numbers(scale)) || row->count != (scale ? 3u : 2u))
                return join(wrong, "\"maps\": \"placement_changes\": ", list->name, ", row ", row->name,
                            ": a move is \"location\" and \"rotation\", three numbers each, and \"scale\" when that changes too"), false;
            for (const Json* earlier = move->first; earlier != row; earlier = earlier->next)
                if (row_number(earlier->name, strlen(earlier->name), other) && other == number)
                    return join(wrong, "\"maps\": \"placement_changes\": ", list->name, " moves row ", row->name, " twice"), false;
        }
        for (const Json* row = remove ? remove->first : nullptr; row; row = row->next) {
            if (row->kind != Json::Kind::text || strlen(row->text) != row->bytes || !row_number(row->text, row->bytes, number))
                return join(wrong, "\"maps\": \"placement_changes\": ", list->name, ": \"remove\": ", row->kind == Json::Kind::text ? row->text : "a value", kNumber), false;
            for (const Json* earlier = remove->first; earlier != row; earlier = earlier->next)
                if (row_number(earlier->text, earlier->bytes, other) && other == number)
                    return join(wrong, "\"maps\": \"placement_changes\": ", list->name, " removes row ", row->text, " twice"), false;
            for (const Json* moved = move ? move->first : nullptr; moved; moved = moved->next)
                if (row_number(moved->name, strlen(moved->name), other) && other == number)
                    return join(wrong, "\"maps\": \"placement_changes\": ", list->name, " both moves and removes row ", row->text), false;
        }
    }
    return true;
}

bool placement_changes_start(TSUCHI_Mod* context) {
    self = context;
    uint64_t id = 0;
    if (!build_hooked) build_hooked = hook_site(self, t::code_site::placement_build, on_build, &next_build, &id) == TSUCHI_OK;
    if (!restore_hooked) restore_hooked = hook_site(self, t::code_site::placement_restore, on_restore, &next_restore, &id) == TSUCHI_OK;
    return build_hooked && restore_hooked;
}

bool placement_changes_ready() { return build_hooked && restore_hooked; }

bool placement_changes_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry) {
    return !entry.find("placement_changes") || wanted.add(Wanted{part, mod, &entry}) != nullptr;
}

void placement_changes_end(uint64_t part) {
    for (uint32_t i = 0; i < wanted.count; ++i) {
        if (wanted[i].part != part) continue;
        // The others keep their order.
        memmove(static_cast<void*>(&wanted[i]), &wanted[i + 1], sizeof(Wanted) * (wanted.count - i - 1));
        --wanted.count;
        return;
    }
}

PlacementChangeCounts placement_change_counts() { return counts; }

} // namespace tsuchi
