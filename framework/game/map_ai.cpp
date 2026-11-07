// The "ai" of a `maps` entry: values for creatures the game places in one of its maps.
//
//   "maps": [{"map": "/Game/.../Cave000_F00_P", "ai": "maps/cave000.ai"}]
//
// What a level's maker set for a creature's AI is in no field of the creature. The game writes it into the
// creature's generator with a serializer of its own, and the creature's AI part reads it from there, from the
// map's own list or from the save, before the part starts. The part's start then builds what it works from.
// The file holds, for each placement it names, groups of those values as the serializer writes them: see
// ai_file.h.
//
// The start is one entry in the function table of each kind of AI part, and the framework wraps it the first
// time a mod brings a file. When a part starts whose creature a file names, by placement number and class, the
// part is asked to write its values out, the file's groups are put in the place of the part's own, and the part
// reads the result before the game's start runs, including while the mod's levels are loading.
// The other serialized groups stay as they were. The reader is
// given no state stream; preservation of every internal state field by the real reader is not established.
// Supported AI parts use this route whether made from a save or from the map's own list.
//
// Every running mod that names the map takes part, in the order the mods started. A group two of them change
// has the bytes of the one started last.
#include "map_ai.h"

#include <string.h>

#include "actor_events.h"
#include "actors.h"
#include "content_path.h"
#include "frame.h"
#include "placement_mode.h"
#include "ai_file.h"
#include "files.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "map_placements.h"
#include "mods.h"
#include "names.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using game::read_or;
using maps::join;
using maps::tell;

constexpr uint32_t kKinds = 3; // of AI part, in the order of the game table's list and of a file's numbers
static_assert(t::slot_group::ai_part_start.table_count == kKinds, "one function table for each kind of AI part a file can name");

// One placement of a file: its number, and where in the file it is.
struct Named {
    uint64_t number;
    uint32_t at;
    bool unsupported_told = false;
};
// One part's file.
struct File {
    uint64_t part;
    TSUCHI_Mod* mod;
    uint8_t* bytes; // the file, which is the part's own
    uint32_t size;
    const char* map; // within the file
    PlacementMode mode;
    List<Named> named;
};
List<File> files; // in the order the mods started
AiCounts counts{};
List<uint64_t> entered;
uint64_t steps = 1;

TSUCHI_Mod* self = nullptr;
using Start = void (*)(void*);
Start next_start[kKinds] = {};

// ---- What the game knows ------------------------------------------------------------------------------

bool one_of(uint64_t value, const t::Cell<uint64_t>* offsets, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (value == game::main_base() + offsets[i]) return true;
    return false;
}

// Whether a part writes its values with the game's own serializers, which are what the shapes in ai_file.h are of.
bool own_serializers(uint64_t part, uint32_t kind) {
    const t::SlotGroup& set = t::slot_group::ai_part_write_set, &state = t::slot_group::ai_part_write_state;
    const uint64_t table = read_or<uint64_t>(part);
    return table == game::main_base() + t::slot_group::ai_part_start.tables[kind - 1] && one_of(read_or<uint64_t>(table + set.offset), set.stock, set.stock_count) &&
           one_of(read_or<uint64_t>(table + state.offset), state.stock, state.stock_count);
}

// The world owning a creature's level, including before that world's first frame.
uint64_t world_of(uint64_t creature) {
    const uint64_t level = game::outer_of(creature);
    game::Place place{};
    if (!level || game::find_place(game::type_of(level), level, "OwningWorld", place) != TSUCHI_OK) return 0;
    const uint64_t world = read_or<uint64_t>(place.address);
    return world && game::live(world) ? world : 0;
}

// The map a creature is in, by the path of its package: that of the world its level belongs to.
bool map_of(uint64_t creature, char* out, size_t capacity) {
    const uint64_t world = world_of(creature), package = world ? game::outer_of(world) : 0;
    uint64_t needed = 0;
    return package && game::object_path(package, out, capacity, &needed) == TSUCHI_OK;
}

// One of the game's enums by name, found once and remembered.
struct EnumRef {
    const char* name;
    uint64_t handle = 0;
    uint64_t missed_at = 0;
};
struct EnumSearch {
    const char* name;
    uint64_t found;
};
uint64_t find_enum(EnumRef& ref) {
    static game::ClassRef enum_class{"Enum"};
    if (const uint64_t known = game::from_handle(ref.handle)) return known;
    // Missing enums may arrive later with content. Retry after sixty frames, rather than walking all
    // objects for each creature inside the game's spawn path. Successful handles still detect unloading.
    if (ref.missed_at && steps - ref.missed_at < 60) return 0;
    EnumSearch search{ref.name, 0};
    ++counts.enum_walks;
    game::each_object(
        [](void* context, uint64_t object) {
            auto& looking = *static_cast<EnumSearch*>(context);
            if (!game::is_a(object, enum_class) || !game::name_is(game::name_of(object), looking.name)) return true;
            looking.found = object;
            return false;
        },
        &search);
    ref.handle = search.found ? game::to_handle(search.found) : 0;
    ref.missed_at = search.found ? 0 : steps;
    return search.found;
}

// Whether a number is one of an enum's values.
bool enum_has(EnumRef& ref, uint8_t value) {
    const uint64_t type = find_enum(ref);
    if (!type) return false;
    const uint64_t data = read_or<uint64_t>(type + t::enum_type::names + t::array::data);
    const int32_t count = read_or<int32_t>(type + t::enum_type::names + t::array::count);
    if (count <= 0 || count > 4096 || !game::readable(data, uint64_t(count) * t::enum_entry::size)) return false;
    for (int32_t i = 0; i < count; ++i)
        if (read_or<int64_t>(data + uint64_t(i) * t::enum_entry::size + t::enum_entry::value) == value) return true;
    return false;
}

// Whether what a placement's drops say of the game's enums are values the game has. The shapes of the bytes were
// judged when the file was read; what the values are is known only with the game running.
bool drops_known(const ai::Placement& placement) {
    static EnumRef conditions{"EDropCondition"}, demo_flags{"EDemoFlagType"};
    const ai::Change& drops = placement.changes[ai::kDrops];
    return !drops.bytes || ai::drops_known(drops.data, drops.bytes, [](uint32_t which, uint8_t value) { return enum_has(which == ai::kDropCondition ? conditions : demo_flags, value); });
}

// An array of bytes as the engine keeps one, in memory of the framework's own or the game's.
struct Bytes {
    uint64_t data;
    int32_t count, max;
};
Bytes bytes_at(const uint8_t* at) {
    Bytes out{};
    memcpy(&out.data, at + t::array::data, 8), memcpy(&out.count, at + t::array::count, 4), memcpy(&out.max, at + t::array::max, 4);
    return out;
}
bool whole(const Bytes& bytes) {
    return bytes.count >= 0 && bytes.max >= bytes.count && uint32_t(bytes.max) <= ai::kStreamLimit && (bytes.max ? bytes.data && game::readable(bytes.data, uint32_t(bytes.max)) : !bytes.data);
}

enum class Put : uint32_t { done, not_ready, not_as_expected, no_memory };

// The groups into a part: it writes its values out, the groups go in the place of its own, and it reads the result.
Put put_groups(uint64_t part, uint32_t kind, const ai::Change changes[ai::kGroups]) {
    namespace v = t::ai_values;
    if (!t::bound(t::fn::ai_values_write) || !t::bound(t::fn::ai_values_read)) return Put::not_ready;
    alignas(8) uint8_t now[v::most] = {}, before[v::most] = {};
    t::fn::ai_values_write(reinterpret_cast<void*>(part), now, before, t::constant::ai_stream_version, 0);
    // What it wrote is memory of the engine's, and the framework's to give back.
    const Bytes set = bytes_at(now + v::set), state = bytes_at(now + v::state);
    const auto give_back = [&] {
        if (set.data) game::release_engine_memory(reinterpret_cast<void*>(set.data));
        if (state.data && state.data != set.data) game::release_engine_memory(reinterpret_cast<void*>(state.data));
    };
    ai::Span spans[ai::kGroups];
    const auto* stream = reinterpret_cast<const uint8_t*>(set.data);
    if (!whole(set) || !set.count || !whole(state) || set.data == state.data || !ai::find_groups(stream, uint32_t(set.count), kind, spans)) {
        give_back();
        return Put::not_as_expected;
    }
    const uint32_t size = ai::replace_groups(stream, spans, changes, nullptr);
    auto* const made = size <= ai::kStreamLimit ? static_cast<uint8_t*>(game::engine_memory(size)) : nullptr;
    if (!made) {
        give_back();
        return size <= ai::kStreamLimit ? Put::no_memory : Put::not_as_expected;
    }
    ai::replace_groups(stream, spans, changes, made);
    // What it reads: the new values, and no state stream. This does not establish that every internal state
    // field remains unchanged; the stand-in reader and territory check cover only their observed values.
    alignas(8) uint8_t next[v::most] = {};
    const uint64_t data = reinterpret_cast<uint64_t>(made);
    const int32_t count = int32_t(size);
    memcpy(next + v::set + t::array::data, &data, 8), memcpy(next + v::set + t::array::count, &count, 4), memcpy(next + v::set + t::array::max, &count, 4);
    t::fn::ai_values_read(reinterpret_cast<void*>(part), next, t::constant::ai_stream_version, t::constant::ai_stream_version);
    game::release_engine_memory(made);
    give_back();
    return Put::done;
}

// ---- As a creature's AI part starts ---------------------------------------------------------------------

const Named* find(File& file, uint64_t number) {
    for (uint32_t i = 0; i < file.named.count; ++i)
        if (file.named[i].number == number) return &file.named[i];
    return nullptr;
}

// Puts in what running mods' files say of the creature whose part this is, if any says anything.
bool set_values(uint64_t part, uint32_t kind) {
    const uint64_t creature = read_or<uint64_t>(part + t::scene_component::owner);
    if (!creature || !game::live(creature) || read_or<uint64_t>(creature + t::game_actor::ai) != part) return true;
    const uint64_t number = game::actor_placement(creature);
    bool any = false;
    for (uint32_t f = 0; number && f < files.count && !any; ++f) any = !map_part_failed(files[f].part) && placement_mode_matches(files[f].mode) && find(files[f], number) != nullptr;
    if (!any) return true;
    char map[512], type[512], shown[19];
    uint64_t needed = 0;
    if (!map_of(creature, map, sizeof map) || game::object_path(game::class_of(creature), type, sizeof type, &needed) != TSUCHI_OK) return true;
    maps::hex(shown, number);

    // Each mod's groups in the order the mods started, so that the last to name a group has it.
    ai::Change changes[ai::kGroups] = {};
    TSUCHI_Mod* by[ai::kGroups] = {};
    TSUCHI_Mod* last = nullptr;
    for (uint32_t f = 0; f < files.count; ++f) {
        File& file = files[f];
        if (map_part_failed(file.part) || !placement_mode_matches(file.mode)) continue;
        const Named* named = content_path::same(file.map, map) ? find(file, number) : nullptr;
        if (!named) continue;
        ai::Placement placement;
        uint32_t at = named->at;
        ai::read_placement(file.bytes, file.size, at, placement); // judged when the file was read
        const char* const why = placement.kind != kind                       ? "the creature there has another kind of AI part than the file says"
                                : !content_path::same(placement.type, type)            ? "the creature there is of another class than the file says"
                                : !own_serializers(part, kind)                ? "the creature's AI part does not write its values the game's own way"
                                : !drops_known(placement)                     ? "its drops hold a value that is none of the game's"
                                                                              : nullptr;
        if (why) {
            ++counts.refused;
            tell(file.mod, "maps: the AI values for placement ", shown, " in ", map, " were not set: ", why);
            continue;
        }
        bool told = false;
        for (uint32_t which = 0; which < ai::kGroups; ++which) {
            if (!placement.changes[which].bytes) continue;
            if (by[which] && by[which] != file.mod && !told) {
                told = true;
                tell(by[which], "maps: AI values of placement ", shown, " in ", map, " are set by ", mod_id(file.mod), ", not by this mod");
            }
            changes[which] = placement.changes[which], by[which] = file.mod;
        }
        last = file.mod;
    }
    if (!last) return true;
    const Put put = put_groups(part, kind, changes);
    if (put == Put::done) {
        ++counts.set;
        return true;
    }
    ++counts.refused;
    static const char* const why[] = {"", "the game's functions for it are missing", "the creature's own values are not shaped as the framework knows them", "there is no memory for them"};
    tell(last, "maps: the AI values for placement ", shown, " in ", map, " were not set: ", why[uint32_t(put)]);
    return true;
}

// Entering play is observed even for AI parts whose start is not one of our hooked tables. Work waits
// until the next frame, outside the game's registration call, while the actor's handle still resolves.
void note_entered(uint64_t, uint64_t handle) {
    if (!files.count || !handle) return;
    entered.add(handle);
}

bool inspect_entered(uint64_t actor) {
    const uint64_t number = game::actor_placement(actor);
    if (!number) return true;
    const uint64_t part = read_or<uint64_t>(actor + t::game_actor::ai);
    if (part && one_of(read_or<uint64_t>(part), t::slot_group::ai_part_start.tables, kKinds)) return true;
    char map[512], shown[19];
    if (!map_of(actor, map, sizeof map)) return true;
    maps::hex(shown, number);
    bool waiting = false;
    for (uint32_t f = 0; f < files.count; ++f) {
        File& file = files[f];
        if (map_part_failed(file.part) || !placement_mode_matches(file.mode) || !content_path::same(file.map, map)) continue;
        Named* named = nullptr;
        for (uint32_t i = 0; i < file.named.count; ++i)
            if (file.named[i].number == number) named = &file.named[i];
        if (!named || named->unsupported_told) continue;
        if (!map_part_ready(file.part)) { waiting = true; continue; }
        named->unsupported_told = true;
        ++counts.refused;
        tell(file.mod, "maps: the AI values for placement ", shown, " in ", map, " were not set: the creature there has no supported kind of AI part");
    }
    return !waiting;
}

void step() {
    ++steps;
    if (!files.count) { entered.count = 0; return; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < entered.count; ++i) {
        const uint64_t actor = game::from_handle(entered[i]);
        if (actor && !inspect_entered(actor)) entered[kept++] = entered[i];
    }
    entered.count = kept;
}

// What the game's start of an AI part is led to: the values first, while the part has built nothing from its
// own, and then the game's start, whatever became of the values.
void started(void* part, uint32_t kind) {
    if (files.count) set_values(reinterpret_cast<uint64_t>(part), kind);
    if (next_start[kind - 1]) next_start[kind - 1](part);
}
void started_any(void* part) { started(part, ai::kAnyCreature); }
void started_bulborb(void* part) { started(part, ai::kBulborb); }
void started_larva(void* part) { started(part, ai::kLarva); }

// Wraps the start in every function table of AI parts, or in none: each only if it holds the game's own start.
bool wrap() {
    if (counts.tables) return true;
    if (!mod_usable(self)) return false;
    const t::SlotGroup& start = t::slot_group::ai_part_start;
    static void (*const wrappers[kKinds])(void*) = {started_any, started_bulborb, started_larva};
    uint64_t ids[kKinds] = {};
    uint32_t wrapped = 0;
    for (; wrapped < kKinds; ++wrapped) {
        const uint64_t entry = game::main_base() + start.tables[wrapped] + start.offset;
        void* next = nullptr;
        if (!one_of(hook_entry_base(entry), start.stock, start.stock_count) ||
            // First: a mod's own hook on the start sees the part with its values, as the game's start does.
            hook_wrap(self, entry, 0, reinterpret_cast<void*>(wrappers[wrapped]), kHookFirst, &ids[wrapped], &next) != TSUCHI_OK)
            break;
        next_start[wrapped] = reinterpret_cast<Start>(next);
    }
    if (wrapped == kKinds) {
        counts.tables = kKinds;
        return true;
    }
    while (wrapped) hook_remove(self, ids[--wrapped]), next_start[wrapped] = nullptr;
    return false;
}

void forget(File& file) {
    release(file.bytes);
    file.named.clear();
}

} // namespace

bool ai_well_formed(const Json& entry, Text& wrong) {
    const Json* named = entry.find("ai");
    const bool good = !named || (named->kind == Json::Kind::text && named->bytes && strlen(named->text) == named->bytes);
    if (!good) wrong.set("\"maps\": \"ai\" is the path of a file in the mod's folder");
    return good;
}

TSUCHI_Result ai_begin(uint64_t part, TSUCHI_Mod* mod, const Json& entry, Text& problem) {
    const Json* named = entry.find("ai");
    if (!named) return TSUCHI_OK;
    File file{part, mod, nullptr, 0, nullptr, placement_mode(entry), {}};
    const auto refuse = [&](TSUCHI_Result result, const char* why, const char* more = "") {
        join(problem, "\"maps\": ", named->text, why, more);
        forget(file);
        return result;
    };
    Texts where;
    char* bytes = nullptr;
    uint64_t size = 0;
    const TSUCHI_Result got = mod_file_path(where, mod, named->text) ? game::read_whole_file(where.data, bytes, size) : TSUCHI_NO_MEMORY;
    where.clear();
    file.bytes = reinterpret_cast<uint8_t*>(bytes);
    if (got != TSUCHI_OK) return refuse(got, got == TSUCHI_NOT_FOUND ? ": the mod has no such file" : got == TSUCHI_NO_MEMORY ? ": there is no memory to read it" : ": it could not be read");
    uint32_t at = 0, count = 0;
    ai::Read read = ai::read_head(file.bytes, size, file.map, at, count);
    file.size = uint32_t(size);
    for (uint32_t i = 0; i < count && read == ai::Read::ok; ++i) {
        ai::Placement placement;
        const uint32_t from = at;
        read = ai::read_placement(file.bytes, file.size, at, placement);
        if (read == ai::Read::ok && find(file, placement.number)) read = ai::Read::twice;
        if (read == ai::Read::ok && !file.named.add(Named{placement.number, from})) return refuse(TSUCHI_NO_MEMORY, ": there is no memory to read it");
    }
    if (read == ai::Read::ok && at != file.size) read = ai::Read::not_ai;
    static const char* const why[] = {"", " is no AI file, or is damaged", " is no AI file, or is damaged", " has a number or a path that is out of range",
                                      " has a group of values that is not shaped as the game writes it", " names a placement, or a group of one, twice"};
    if (read == ai::Read::bad_path || read == ai::Read::no_package || read == ai::Read::no_object) {
        const char* message = read == ai::Read::no_package ? content_path::kNoPackage : read == ai::Read::no_object ? content_path::kNoObject : content_path::kNoPath;
        return refuse(TSUCHI_INVALID, " contains a path that", message);
    }
    if (read != ai::Read::ok) return refuse(TSUCHI_INVALID, why[uint32_t(read)]);
    if (!content_path::same(file.map, entry.find("map")->text)) return refuse(TSUCHI_INVALID, " is made for another map, ", file.map);
    if (!wrap()) return refuse(TSUCHI_NOT_READY, ": the game's AI parts are not as the framework knows them, so their values cannot be set");
    if (!files.add(file)) return refuse(TSUCHI_NO_MEMORY, ": there is no memory to read it");
    ++counts.files, counts.placements += file.named.count;
    return TSUCHI_OK;
}

void ai_end(uint64_t part) {
    for (uint32_t i = 0; i < files.count; ++i) {
        if (files[i].part != part) continue;
        --counts.files, counts.placements -= files[i].named.count;
        forget(files[i]);
        // The others keep their order, which is the order their mods started in.
        memmove(static_cast<void*>(&files[i]), &files[i + 1], sizeof(File) * (files.count - i - 1));
        --files.count;
        return;
    }
}

void map_ai_start(TSUCHI_Mod* context) {
    self = context;
    placements_start(context);
    static bool watching = false;
    if (!watching && on_actor_entered(note_entered) && each_frame(step)) watching = true;
}
AiCounts map_ai_counts() { return counts; }

} // namespace tsuchi
