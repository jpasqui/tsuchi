// The manifest's "maps": what a mod changes in the game's own maps, as data. A mod cannot ship an edited copy of
// a map of the game's, so it says what has to happen when that map loads: levels of its own to load into it, and
// actors of the game's sublevels to remove, to move, or to set values of.
//
//   "maps": [{"map": "/Game/.../Cave000_F00_P", "levels": ["/Game/Mods/my_mod/Mine"], "changes": "maps/cave000.json",
//             "navigation": "maps/cave000.nav"}]
//
// and the file it names, in the mod's folder:
//
//   {"levels": {"Cave000_F00_GameArea": {"remove": ["Rock_3"], "move": {"Wall_1": {"location": [0, 0, 0], "rotation": [0, 90, 0]}},
//                                        "set": {"Lamp_2": {"Intensity": 4.5}}}}}
//
// Every mod that names the loaded map takes part, in the order the mods started. Actor edits run before
// the level initializes its actors. Prepared added ground joins the incoming native streaming flush;
// placements and navigation wait for its qualified completion. Script-added ground keeps the late path.
// What a sublevel's entry names has to be there, all of it, when
// that sublevel is first looked at: an entry made for another version of the level changes nothing and is told
// in the log. A removal wins over another mod's move or set, and an actor two mods remove is removed once. A
// sublevel the game loads again gets its changes again. A value that is set can be put back, and is, when the
// world is left or the mod stops: see map_values.cpp.
//
// `navigation` names a file of the mod's own navigation for the map, put in the place of the game's once that
// mod's ground is ready. Named navmeshes are retried as their actors and implementations arrive, without
// swapping completed ones twice, for at most 600 attempts. One mod's is used for a map: that of the mod started last.
//
// A level of a mod's may have streaming levels of its own, as a map made of several levels has. The engine loads
// those only for the map that is opened, so each is asked for here once the level that brings it has come, where
// that level puts it. They are the mod's levels like the ones its manifest names.
//
// `placements` names lists of gameplay objects for the game to place in the map and to save as it does its
// own. They are given once every level asked for is in the world: see map_placements.cpp. Explicit `mode` selects
// story day (the default), night or Olimar for the whole entry. `placement_changes` moves and removes rows of the
// game's own placement lists as the game reads them: see map_placement_changes.cpp. `ai` names
// values for creatures the game places there, applied before their AI starts: see map_ai.cpp.
#include <string.h>

#include "files.h"
#include "content_path.h"
#include "placement_mode.h"
#include "reflect.h"
#include "engine_call.h"
#include "flow.h"
#include "frame.h"
#include "host.h"
#include "hooks.h"
#include "json.h"
#include "list.h"
#include "map_ai.h"
#include "map_navigation.h"
#include "map_placement_changes.h"
#include "map_placements.h"
#include "map_tools.h"
#include "map_values.h"
#include "mods.h"
#include "names.h"
#include "objects.h"
#include "object_retention.h"
#include "registry.h"
#include "services.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {
namespace {
namespace t = game::table;

using maps::join;
using maps::tell;

const Kind kPart{"maps.part"};
game::ClassRef level_class{t::name::level_class};
game::ClassRef actor_class{t::name::engine_actor_class};

// One of the game's sublevels a mod changes: its entry in the changes file, and each level life its changes
// were made in or refused for.
struct Sublevel {
    const Json* entry;
    List<uint64_t> done_in;
    List<uint64_t> late_in; // already playing when this part started
};
// Where a level is put: a turn as the engine keeps one, a place, and a scale.
struct Put {
    double turn[4], place[3], scale[3];
};
constexpr Put kAsMade{{0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
// A level of a mod's that was asked for.
struct Stream {
    uint64_t stream; // what the engine gave back for it
    Put put;
    uint8_t depth;   // 0 for a level the manifest names, and one more for each level between that one and this
    bool awaited;    // it is to be in the world from the start, so the game's actors wait for it
    uint32_t requested; // the world step when it was asked for
    bool opened;     // it has come, and the levels it brings with it have been asked for
    bool initial = false; // native descriptor admitted at the incoming LoadMap boundary
};
constexpr uint32_t kDeepest = 8, kMostStreams = 256;
// One entry of one mod's "maps".
struct Part {
    uint64_t id;
    TSUCHI_Mod* mod;
    const Json* map;     // the manifest's entry
    const Json* changes; // the tree of the file it names, which is the part's own; null when it names none
    uint8_t* navigation; // the navigation file it names, which is the part's own; null when it names none
    uint64_t navigation_bytes;
    NavigationState navigation_state;
    List<Sublevel> sublevels;
    // In the world that is loaded:
    bool asked, failed;   // its levels were asked for; one of them could not be loaded
    List<Stream> streams; // each level asked for: those the manifest names, then those they bring with them
    bool present = false, ready = false;
    bool initial = false, initial_seen = false;
    bool late_told = false; // the log said that this world's initial boundary missed it
};
List<Part> parts;
// Registration phase does not determine mod precedence. Traverse selected order without
// moving records or changing the storage indices kept by an in-progress level edit.
// A newly registered key below this cursor waits for the next pass.
struct PartOrder {
    uint32_t rank = 0;
    uint64_t id = 0;
    Part* next() {
        Part* found = nullptr;
        for (uint32_t i = 0; i < parts.count; ++i) {
            Part& candidate = parts[i];
            const uint32_t at = mod_rank(candidate.mod);
            if (at < rank || (at == rank && candidate.id <= id)) continue;
            if (!found || at < mod_rank(found->mod) || (at == mod_rank(found->mod) && candidate.id < found->id)) found = &candidate;
        }
        if (found) rank = mod_rank(found->mod), id = found->id;
        return found;
    }
};
uint64_t next_part = 1;
// A move made before its level began play, looked at again once the actor plays: some of the game's actors
// are put back where their level saved them as play begins, and such a move is then made again.
struct EarlyMove {
    uint64_t part;  // the part's id
    uint64_t actor; // its handle
    const Json* to; // the move, in the part's changes
    const Json* entry;
};
List<EarlyMove> early_moves;
// Moves are made in mod order, so the last move of an actor is the one that wins. Only it is looked at again.
void remember_early_move(const EarlyMove& move) {
    for (uint32_t m = early_moves.count; m-- > 0;)
        if (early_moves[m].actor == move.actor) early_moves.remove(&early_moves[m]);
    early_moves.add(move);
}
using InitializeLevel = decltype(t::code_site::level_prepare)::Function;
InitializeLevel next_initialize = nullptr;
bool initialize_hook = false;
using InitialFlush = decltype(t::code_site::map_initial_flush)::Function;
InitialFlush next_initial_flush = nullptr;
bool initial_hook = false, initial_active = false;
uint64_t declaration_generation = 0, world_generation = 0;
using RemoveLevel = decltype(t::code_site::world_remove_level)::Function;
RemoveLevel next_remove_level = nullptr;
bool remove_hook = false;
// While a level is being initialized: its world, and the mode once something needs it. Reading the mode looks
// at every actor of the world's levels, so it is read only for a level that a part names.
struct Initializing {
    uint64_t world;
    bool known;
    PlacementMode mode;
};
bool initializing = false;
Initializing initialize{0, false, PlacementMode::invalid};
PlacementMode early_mode() {
    if (!initialize.known) initialize.mode = map_mode_before_play(initialize.world), initialize.known = true;
    return initialize.mode;
}
void use_world(uint64_t world);
bool remember_playing(Part& part);

uint64_t world_now = 0; // the loaded world's handle
uint32_t world_steps = 0;
uint32_t navigation_tries = 0;
bool navigated = false; // the world's navigation has been seen to, or given up on
NavigationCounts navigation_counts{};
MapCounts counts{};
Text world_path;        // the path of its package, which is how a manifest names a map

// ---- Small tools ----------------------------------------------------------------------------------

bool whole_text(const Json& value) { return value.kind == Json::Kind::text && value.bytes && strlen(value.text) == value.bytes; }

// Map arguments use the framework's engine call and its checked value conversions.
struct MapCall {
    game::EngineCall engine;
    TSUCHI_Result result;
    MapCall(TSUCHI_Mod*, uint64_t handle, const char* function) : engine(game::from_handle(handle), function), result(engine.ready()) {}
    void number(const char* path, double to) { if (result == TSUCHI_OK) result = engine.set(path, game::number_value(to)); }
    void integer(const char* path, int64_t to) { if (result == TSUCHI_OK) result = engine.set(path, game::integer_value(to)); }
    void boolean(const char* path, bool to) { if (result == TSUCHI_OK) result = engine.set(path, game::boolean_value(to)); }
    void object(const char* path, uint64_t to) { if (result == TSUCHI_OK) result = engine.set(path, game::object_value(to)); }
    void text(const char* path, const char* to) { if (result == TSUCHI_OK) result = engine.set_text(path, to); }
    void three(const char* path, const Json& numbers, const char* const names[3]) {
        char field[48];
        for (uint32_t i = 0; i < 3; ++i) {
            const size_t length = strlen(path);
            if (length + strlen(names[i]) + 2 > sizeof field) { result = TSUCHI_INVALID; return; }
            memcpy(field, path, length); field[length] = '.';
            memcpy(field + length + 1, names[i], strlen(names[i]) + 1);
            number(field, numbers.at(i)->number);
        }
    }
    bool run() { if (result == TSUCHI_OK) result = engine.invoke(); return result == TSUCHI_OK; }
    bool get(const char* path, TSUCHI_Value& out) {
        game::Value value;
        if (result != TSUCHI_OK || engine.get(path, value) != TSUCHI_OK) return false;
        if (value.kind == game::ValueKind::boolean) out.kind = TSUCHI_VALUE_BOOLEAN, out.as.boolean = value.boolean;
        else if (value.kind == game::ValueKind::object) out.kind = TSUCHI_VALUE_OBJECT, out.as.object.id = value.object;
        else return false;
        return true;
    }
    bool number_at(const char* path, double& out) {
        game::Value value;
        if (result != TSUCHI_OK || engine.get(path, value) != TSUCHI_OK) return false;
        if (value.kind == game::ValueKind::number) out = value.number;
        else if (value.kind == game::ValueKind::integer) out = double(value.integer);
        else return false;
        return true;
    }
};

const char* const kPlace[3] = {"X", "Y", "Z"};
const char* const kTurn[3] = {"Pitch", "Yaw", "Roll"};

// ---- How the two are written ----------------------------------------------------------------------

bool three_numbers(const Json* value) {
    if (!value || value->kind != Json::Kind::list || value->count != 3) return false;
    for (const Json* number = value->first; number; number = number->next)
        if (number->kind != Json::Kind::number) return false;
    return true;
}

bool maps_well_formed(const Json& section, Text& wrong) {
    if (section.kind != Json::Kind::list) return wrong.set("\"maps\" is a list, one entry for each of the game's maps the mod changes"), false;
    for (const Json* entry = section.first; entry; entry = entry->next) {
        if (entry->kind != Json::Kind::object)
            return wrong.set("\"maps\": an entry is an object with \"map\" and at least one of \"levels\", \"changes\", \"navigation\", \"placements\", \"placement_changes\" and \"ai\""), false;
        const Json* map = entry->find("map"), *levels = entry->find("levels"), *changes = entry->find("changes"), *navigation = entry->find("navigation");
        const Json* placements = entry->find("placements"), *ai = entry->find("ai"), *placement_changes = entry->find("placement_changes");
        for (const Json* member = entry->first; member; member = member->next)
            if (member != map && member != levels && member != changes && member != navigation && member != placements && member != ai && member != placement_changes &&
                strcmp(member->name, "mode"))
                return join(wrong, "\"maps\": an entry has a key the framework does not know: ", member->name), false;
        if (!map || !whole_text(*map)) return wrong.set("\"maps\": \"map\" is the path of one of the game's maps, such as /Game/Folder/Map_P"), false;
        if (const char* why = content_path::wrong(map->text, map->bytes, content_path::Names::package)) return join(wrong, "\"maps\": ", map->text, why), false;
        if (!levels && !changes && !navigation && !placements && !ai && !placement_changes)
            return wrong.set("\"maps\": an entry has at least one of \"levels\", \"changes\", \"navigation\", \"placements\", \"placement_changes\" and \"ai\""), false;
        if (levels && levels->kind != Json::Kind::list) return wrong.set("\"maps\": \"levels\" is a list of the mod's levels, each by its path"), false;
        for (const Json* level = levels ? levels->first : nullptr; level; level = level->next)
            if (!whole_text(*level)) return wrong.set("\"maps\": \"levels\" is a list of the mod's levels, each by its path"), false;
            else if (const char* why = content_path::wrong(level->text, level->bytes, content_path::Names::package)) return join(wrong, "\"maps\": ", level->text, why), false;
        if (changes && !whole_text(*changes)) return wrong.set("\"maps\": \"changes\" is the path of a file in the mod's folder"), false;
        if (navigation && !whole_text(*navigation)) return wrong.set("\"maps\": \"navigation\" is the path of a file in the mod's folder"), false;
        if (!placement_mode_well_formed(*entry, wrong) || !placements_well_formed(*entry, wrong) || !placement_changes_well_formed(*entry, wrong) ||
            !ai_well_formed(*entry, wrong))
            return false;
    }
    return true;
}

// Whether a changes file is written as it should be. `wrong` says what is not, or is left empty when there was
// no memory to say it.
bool changes_well_formed(const Json& file, Text& wrong) {
    const Json* levels = file.kind == Json::Kind::object ? file.find("levels") : nullptr;
    if (!levels || levels->kind != Json::Kind::object || file.count != 1)
        return wrong.set("it is an object with \"levels\": the game's sublevels by name, each with what to \"remove\", what to \"move\" and what to \"set\""), false;
    for (const Json* level = levels->first; level; level = level->next) {
        if (level->kind != Json::Kind::object || !level->name[0]) return wrong.set("a sublevel is named, and holds \"remove\", \"move\" or \"set\", or more than one of them"), false;
        const Json* remove = level->find("remove"), *move = level->find("move"), *set = level->find("set");
        for (const Json* member = level->first; member; member = member->next)
            if (member != remove && member != move && member != set) return join(wrong, "sublevel ", level->name, " has a key the framework does not know: ", member->name), false;
        if (set && set->kind != Json::Kind::object) return join(wrong, "sublevel ", level->name, ": \"set\" holds actors by name, each with values by their paths"), false;
        for (const Json* actor = set ? set->first : nullptr; actor; actor = actor->next) {
            if (actor->kind != Json::Kind::object || !actor->name[0] || !actor->count)
                return join(wrong, "sublevel ", level->name, ": \"set\" holds actors by name, each with values by their paths"), false;
            for (const Json* value = actor->first; value; value = value->next) {
                if (!value->name[0] || !map_value_well_formed(*value))
                    return join(wrong, "sublevel ", level->name, ", actor ", actor->name, ": a value is named by its path and is a number, boolean, text, asset reference, enum name, or a nonempty struct field object. Arrays and null are not supported"), false;
            }
        }
        if (remove && remove->kind != Json::Kind::list) return join(wrong, "sublevel ", level->name, ": \"remove\" is a list of actors' names"), false;
        for (const Json* actor = remove ? remove->first : nullptr; actor; actor = actor->next)
            if (!whole_text(*actor)) return join(wrong, "sublevel ", level->name, ": \"remove\" is a list of actors' names"), false;
        if (move && move->kind != Json::Kind::object) return join(wrong, "sublevel ", level->name, ": \"move\" holds actors by name, each with where it goes"), false;
        for (const Json* actor = move ? move->first : nullptr; actor; actor = actor->next) {
            const Json* location = actor->kind == Json::Kind::object ? actor->find("location") : nullptr;
            const Json* rotation = location ? actor->find("rotation") : nullptr, *scale = location ? actor->find("scale") : nullptr;
            if (!actor->name[0] || !three_numbers(location) || !three_numbers(rotation) || (scale && !three_numbers(scale)) || actor->count != (scale ? 3u : 2u))
                return join(wrong, "sublevel ", level->name, ", actor ", actor->name,
                            ": a move is \"location\" and \"rotation\", three numbers each, and \"scale\" when that changes too"), false;
        }
    }
    return true;
}

// ---- A mod's parts, for as long as it runs ---------------------------------------------------------

void forget(Part& part) {
    placements_end(part.id), placement_changes_end(part.id), ai_end(part.id);
    release_json(part.changes);
    release_navigation(part.navigation_state);
    release(part.navigation);
    for (uint32_t s = 0; s < part.sublevels.count; ++s) part.sublevels[s].done_in.clear(), part.sublevels[s].late_in.clear();
    part.sublevels.clear(), part.streams.clear();
}

// The mod stopped. What it removed or moved in a world that is loaded stays until that world is left. What it
// set does not: each value gets the next mod's say, or what it was.
void end_part(void*, uint64_t id) {
    for (uint32_t i = 0; i < parts.count; ++i) {
        if (parts[i].id != id) continue;
        ++declaration_generation;
        values_end(id);
        for (uint32_t m = early_moves.count; m-- > 0;)
            if (early_moves[m].part == id) early_moves.remove(&early_moves[m]);
        forget(parts[i]);
        memmove(static_cast<void*>(&parts[i]), &parts[i + 1], sizeof(Part) * (parts.count - i - 1));
        --parts.count;
        return;
    }
}

TSUCHI_Result refuse(Text& problem, TSUCHI_Result result, const char* a, const char* b = "", const char* c = "", const char* d = "") {
    join(problem, a, b, c, d);
    return result;
}

TSUCHI_Result maps_begin(TSUCHI_Mod* mod, const Json& section, Text& problem) {
    for (const Json* entry = section.first; entry; entry = entry->next) {
        Part part{next_part, mod, entry, nullptr, nullptr, 0, {}, {}, false, false, {}};
        const ModRecord* record = find_mod(mod_id(mod));
        // prepare_mods commits prepared only after its early sections return. Startup adopts that
        // context; declarations made by its code must keep the separate late dynamic path.
        part.initial = record && record->context == mod && record->state == ModState::ready && !record->prepared;
        if (part.initial && entry->find("levels") && entry->find("levels")->count && !initial_hook)
            return refuse(problem, TSUCHI_NOT_READY, "\"maps\": initial levels need the matching incoming LoadMap hook");
        if (entry->find("changes") && !initialize_hook)
            return refuse(problem, TSUCHI_NOT_READY, "\"maps\": changes need the framework's matching code patch and level initialization hook");
        if (entry->find("placement_changes") && !placement_changes_ready())
            return refuse(problem, TSUCHI_NOT_READY, "\"maps\": placement changes need the framework's matching code patch and its hooks on the game's readers of placement lists");
        if (const Json* named = entry->find("navigation")) {
            Texts where;
            char* bytes = nullptr;
            const TSUCHI_Result read = mod_file_path(where, mod, named->text) ? game::read_whole_file(where.data, bytes, part.navigation_bytes) : TSUCHI_NO_MEMORY;
            where.clear();
            if (read != TSUCHI_OK)
                return refuse(problem, read, "\"maps\": ", named->text, read == TSUCHI_NOT_FOUND ? ": the mod has no such file" : read == TSUCHI_NO_MEMORY ? ": there is no memory to read it" : ": it could not be read");
            part.navigation = reinterpret_cast<uint8_t*>(bytes);
            bool well_formed = false;
            if (!prepare_navigation(part.navigation_state, part.navigation, part.navigation_bytes, entry->find("map")->text, well_formed)) {
                const bool memory = well_formed && !part.navigation_state.parsed;
                forget(part);
                if (memory) return refuse(problem, TSUCHI_NO_MEMORY, "\"maps\": ", named->text, ": there is no memory to read it");
                return well_formed ? refuse(problem, TSUCHI_INVALID, "\"maps\": ", named->text, " has no navigation for ", entry->find("map")->text)
                                   : refuse(problem, TSUCHI_INVALID, "\"maps\": ", named->text, " is no navigation file, or is damaged");
            }
        }
        if (const Json* named = entry->find("changes")) {
            Texts where;
            char* text = nullptr;
            uint64_t bytes = 0;
            const TSUCHI_Result read = mod_file_path(where, mod, named->text) ? game::read_whole_file(where.data, text, bytes) : TSUCHI_NO_MEMORY;
            where.clear();
            if (read != TSUCHI_OK) forget(part);
            if (read != TSUCHI_OK)
                return refuse(problem, read, "\"maps\": ", named->text, read == TSUCHI_NOT_FOUND ? ": the mod has no such file" : read == TSUCHI_NO_MEMORY ? ": there is no memory to read it" : ": it could not be read");
            JsonError error{};
            part.changes = read_json(text, size_t(bytes), &error);
            release(text);
            if (!part.changes) forget(part);
            if (!part.changes) return refuse(problem, error.what ? TSUCHI_INVALID : TSUCHI_NO_MEMORY, "\"maps\": ", named->text, ": ", error.what ? error.what : "there is no memory to read it");
            Text wrong;
            if (!changes_well_formed(*part.changes, wrong)) {
                const bool said = wrong.get()[0] != 0;
                refuse(problem, TSUCHI_INVALID, "\"maps\": ", named->text, ": ", said ? wrong.get() : "there is no memory to read it");
                wrong.clear();
                forget(part);
                return said ? TSUCHI_INVALID : TSUCHI_NO_MEMORY;
            }
            for (const Json* level = part.changes->find("levels")->first; level; level = level->next) {
                if (part.sublevels.add(Sublevel{level, {}, {}})) continue;
                forget(part);
                return TSUCHI_NO_MEMORY;
            }
        }
        if (!placements_begin(part.id, mod, *entry) || !placement_changes_begin(part.id, mod, *entry)) {
            forget(part);
            return TSUCHI_NO_MEMORY;
        }
        if (const TSUCHI_Result read = ai_begin(part.id, mod, *entry, problem); read != TSUCHI_OK) {
            forget(part);
            return read;
        }
        if (!remember_playing(part)) { forget(part); return TSUCHI_NO_MEMORY; }
        Part* kept = parts.add(part);
        const TSUCHI_Result owned = kept ? own(mod, kPart, part.id, end_part, nullptr) : TSUCHI_NO_MEMORY;
        if (owned != TSUCHI_OK) {
            forget(part);
            if (kept) parts.remove(kept);
            return owned;
        }
        ++next_part, ++declaration_generation;
        // Navigation declared while a world is loaded is seen to in that world, as the game's own is.
        if (kept->navigation) navigated = false, navigation_tries = 0;
    }
    return TSUCHI_OK;
}

// ---- In the world that is loaded ------------------------------------------------------------------

// A level of the mod's into the world. What the engine gives back stands for it from then on.
uint64_t load_level(Part& part, const char* package) {
    static game::ClassRef streaming{t::name::streaming_class};
    const uint64_t type = game::find_class(streaming);
    if (!type) return 0;
    MapCall call(part.mod, game::to_handle(type), "LoadLevelInstance");
    call.object("WorldContextObject", world_now);
    call.text("LevelName", package);
    TSUCHI_Value loaded{}, stream{};
    if (!call.run() || !call.get("bOutSuccess", loaded) || !loaded.as.boolean || !call.get("ReturnValue", stream)) return 0;
    return stream.as.object.id;
}

bool level_visible(Part& part, uint64_t stream) {
    MapCall call(part.mod, stream, "IsLevelVisible");
    TSUCHI_Value visible{};
    return call.run() && call.get("ReturnValue", visible) && visible.as.boolean;
}

// ---- The levels a level of a mod's brings with it ---------------------------------------------------

const char* const kPutFields[10] = {"LevelTransform.Rotation.X",    "LevelTransform.Rotation.Y",    "LevelTransform.Rotation.Z", "LevelTransform.Rotation.W",
                                    "LevelTransform.Translation.X", "LevelTransform.Translation.Y", "LevelTransform.Translation.Z",
                                    "LevelTransform.Scale3D.X",     "LevelTransform.Scale3D.Y",     "LevelTransform.Scale3D.Z"};
static_assert(sizeof(Put) == sizeof(double[10]), "a put is ten numbers, in the order of the fields above");

bool read_put(Part& part, uint64_t stream, Put& out) {
    double numbers[10];
    for (uint32_t i = 0; i < 10; ++i) {
        TSUCHI_Value value{};
        if (host.reflection->get(part.mod, TSUCHI_Object{stream}, kPutFields[i], &value) != TSUCHI_OK || value.kind != TSUCHI_VALUE_NUMBER) return false;
        numbers[i] = value.as.number;
    }
    memcpy(&out, numbers, sizeof out);
    return true;
}

bool write_put(Part& part, uint64_t stream, const Put& put) {
    double numbers[10];
    memcpy(numbers, &put, sizeof put);
    for (uint32_t i = 0; i < 10; ++i) {
        TSUCHI_Value value{TSUCHI_VALUE_NUMBER, 0, {}};
        value.as.number = numbers[i];
        if (host.reflection->set(part.mod, TSUCHI_Object{stream}, kPutFields[i], &value) != TSUCHI_OK) return false;
    }
    return true;
}

// Where a level is in the world when the level that brings it is at `outer` and puts it at `inner`.
Put combine(const Put& outer, const Put& inner) {
    Put out{};
    const double* a = outer.turn, *b = inner.turn;
    out.turn[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out.turn[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out.turn[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out.turn[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    // The inner place, scaled and then turned as the outer level is, from the outer place.
    double v[3], once[3], twice[3];
    for (uint32_t k = 0; k < 3; ++k) v[k] = outer.scale[k] * inner.place[k];
    once[0] = a[1] * v[2] - a[2] * v[1], once[1] = a[2] * v[0] - a[0] * v[2], once[2] = a[0] * v[1] - a[1] * v[0];
    twice[0] = a[1] * once[2] - a[2] * once[1], twice[1] = a[2] * once[0] - a[0] * once[2], twice[2] = a[0] * once[1] - a[1] * once[0];
    for (uint32_t k = 0; k < 3; ++k) {
        out.place[k] = outer.place[k] + v[k] + 2 * (a[3] * once[k] + twice[k]);
        out.scale[k] = outer.scale[k] * inner.scale[k];
    }
    return out;
}

// A path to one element of an array: the array's name, then the element's place in brackets.
void element(char* out, size_t capacity, const char* array, uint32_t index) {
    char digits[10];
    uint32_t count = 0;
    do digits[count++] = char('0' + index % 10);
    while (index /= 10);
    const size_t length = strlen(array);
    if (length + count + 3 > capacity) {
        out[0] = 0;
        return;
    }
    memcpy(out, array, length);
    char* at = out + length;
    *at++ = '[';
    while (count) *at++ = digits[--count];
    *at++ = ']', *at = 0;
}

// A yes or no of a streaming level, or `otherwise` when its class has no such field.
bool flag(Part& part, uint64_t object, const char* field, bool otherwise) {
    TSUCHI_Value value{};
    return host.reflection->get(part.mod, TSUCHI_Object{object}, field, &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_BOOLEAN ? value.as.boolean : otherwise;
}

void tell_stream(Part& part, uint64_t stream, const char* function, bool to) {
    MapCall call(part.mod, stream, function);
    call.boolean(!strcmp(function, "SetShouldBeLoaded") ? "bInShouldBeLoaded" : "bInShouldBeVisible", to);
    call.run();
}

void fail_part(Part& part, bool unload);

// One level that a level of the mod's brings with it: asked for as the mod's own levels are, put where the two
// levels together put it, and loaded and shown from the start or not as its level says. The volumes that load
// and show it as the player comes near are given to the new one, and such a level is not waited for.
void bring(Part& part, const Stream& parent, uint64_t source) {
    static game::ClassRef always{t::name::always_loaded_class};
    static char package[1024];
    const TSUCHI_Object from{source};
    uint64_t needed = 0;
    package[0] = 0;
    // The level is named by the path of its world: the package, a dot, and the world's name.
    if (host.reflection->get_text(part.mod, from, "WorldAsset", package, sizeof package, &needed) != TSUCHI_OK) package[0] = 0;
    content_path::Parts path;
    if (!content_path::split(package, path)) package[0] = 0;
    else package[path.package] = 0;
    Put own{};
    uint64_t stream = 0;
    if (package[0] == '/' && parent.depth + 1u < kDeepest && part.streams.count < kMostStreams && read_put(part, source, own)) stream = load_level(part, package);
    const Put put = combine(parent.put, own);
    if (stream && !write_put(part, stream, put)) {
        tell_stream(part, stream, "SetShouldBeLoaded", false); // it would come in the wrong place
        stream = 0;
    }
    bool awaited = false;
    if (stream) {
        const bool loaded = game::is_a(game::from_handle(source), always) || flag(part, source, "bInitiallyLoaded", flag(part, source, "bShouldBeLoaded", true));
        const bool visible = flag(part, source, "bInitiallyVisible", flag(part, source, "bShouldBeVisible", true));
        if (!loaded) tell_stream(part, stream, "SetShouldBeLoaded", false);
        if (!visible) tell_stream(part, stream, "SetShouldBeVisible", false);
        TSUCHI_Value volumes{};
        uint32_t given = 0;
        if (host.reflection->get(part.mod, from, "EditorStreamingVolumes", &volumes) == TSUCHI_OK && volumes.kind == TSUCHI_VALUE_ARRAY && volumes.count && volumes.count <= 32 &&
            host.reflection->resize(part.mod, TSUCHI_Object{stream}, "EditorStreamingVolumes", volumes.count) == TSUCHI_OK) {
            for (uint32_t i = 0; i < volumes.count; ++i) {
                char path[40];
                TSUCHI_Value volume{};
                element(path, sizeof path, "EditorStreamingVolumes", i);
                if (host.reflection->get(part.mod, from, path, &volume) == TSUCHI_OK && host.reflection->set(part.mod, TSUCHI_Object{stream}, path, &volume) == TSUCHI_OK) ++given;
            }
        }
        awaited = loaded && visible && !given;
    }
    ++(stream ? counts.nested_asked : counts.nested_refused);
    if (stream && part.streams.add(Stream{stream, put, uint8_t(parent.depth + 1), awaited, world_steps, false})) return;
    if (stream) tell_stream(part, stream, "SetShouldBeLoaded", false);
    fail_part(part, false);
    tell(part.mod, "maps: ", package[0] ? package : "a level", ", which a level of this mod brings with it, could not be loaded into ", world_path.get(),
         ", so further map changes stop for this mod");
}

// Looks into a level of the mod's once it has come, and asks for the levels it brings with it. The world the
// level was saved in keeps the list.
void open(Part& part, uint32_t index) {
    const Stream parent = part.streams[index]; // a copy: the list moves as it grows
    TSUCHI_Value level{};
    const TSUCHI_Result read = host.reflection->get(part.mod, TSUCHI_Object{parent.stream}, "LoadedLevel", &level);
    if (read == TSUCHI_OK && level.kind != TSUCHI_VALUE_OBJECT) return; // not there yet
    part.streams[index].opened = true;
    const uint64_t saved_in = read == TSUCHI_OK ? game::outer_of(game::from_handle(level.as.object.id)) : 0;
    const TSUCHI_Object world{saved_in ? game::to_handle(saved_in) : 0};
    TSUCHI_Value list{};
    if (!world.id || host.reflection->get(part.mod, world, "StreamingLevels", &list) != TSUCHI_OK || list.kind != TSUCHI_VALUE_ARRAY) return;
    for (uint32_t i = 0; i < list.count && !part.failed; ++i) {
        char path[32];
        TSUCHI_Value source{};
        element(path, sizeof path, "StreamingLevels", i);
        if (host.reflection->get(part.mod, world, path, &source) == TSUCHI_OK && source.kind == TSUCHI_VALUE_OBJECT) bring(part, parent, source.as.object.id);
    }
}

void remove_actor(Part& part, uint64_t actor) {
    MapCall call(part.mod, game::to_handle(actor), "K2_DestroyActor");
    call.run();
}

// A component's mobility as it was before a move.
struct Mobility {
    uint64_t component;
    int64_t was;
};
constexpr uint32_t kMostAttached = 64;

// Snapshot the entire attachment tree before SetMobility can affect it.
bool attached(Part& part, uint64_t component, Mobility (&found)[kMostAttached], uint32_t& count, int depth) {
    TSUCHI_Value how{}, children{};
    if (count >= kMostAttached || depth > 16) return false;
    for (uint32_t i = 0; i < count; ++i) if (found[i].component == component) return false;
    if (host.reflection->get(part.mod, TSUCHI_Object{component}, "Mobility", &how) != TSUCHI_OK || how.kind != TSUCHI_VALUE_INTEGER) return false;
    found[count++] = {component, how.as.integer};
    if (host.reflection->get(part.mod, TSUCHI_Object{component}, "AttachChildren", &children) != TSUCHI_OK || children.kind != TSUCHI_VALUE_ARRAY) return false;
    for (uint32_t i = 0; i < children.count; ++i) {
        char path[40];
        TSUCHI_Value child{};
        element(path, sizeof path, "AttachChildren", i);
        if (host.reflection->get(part.mod, TSUCHI_Object{component}, path, &child) != TSUCHI_OK || child.kind != TSUCHI_VALUE_OBJECT ||
            !attached(part, child.as.object.id, found, count, depth + 1)) return false;
    }
    return true;
}

// An actor to its new place. Before its level registers its components, the engine moves even a root that may
// not move. Later, a root that may not move, as scenery's may not, is made movable for the move and put back,
// so it keeps its baked lighting. Making a root movable makes what is attached under it movable too, so each
// of those is put back as well.
bool move_actor(Part& part, uint64_t actor, const Json& to, bool early) {
    const int64_t kMovable = int64_t(game::table::constant::mobility_movable);
    const uint64_t handle = game::to_handle(actor);
    Mobility before[kMostAttached];
    uint32_t components = 0;
    if (!early) {
        MapCall call(part.mod, handle, "K2_GetRootComponent");
        TSUCHI_Value found{};
        if (!call.run() || !call.get("ReturnValue", found) || found.kind != TSUCHI_VALUE_OBJECT || !found.as.object.id) return false;
        TSUCHI_Value mobility{};
        if (host.reflection->get(part.mod, found.as.object, "Mobility", &mobility) != TSUCHI_OK || mobility.kind != TSUCHI_VALUE_INTEGER) return false;
        if (mobility.as.integer != kMovable && !attached(part, found.as.object.id, before, components, 0)) return false;
    }
    const auto set_mobility = [&](uint64_t component, int64_t value) {
        MapCall call(part.mod, component, "SetMobility");
        call.integer("NewMobility", value);
        call.run();
    };
    const bool toggled = components && before[0].was != kMovable;
    if (toggled) set_mobility(before[0].component, kMovable);
    bool moved;
    {
        MapCall call(part.mod, handle, "K2_SetActorLocationAndRotation");
        call.three("NewLocation", *to.find("location"), kPlace);
        call.three("NewRotation", *to.find("rotation"), kTurn);
        call.boolean("bTeleport", true);
        moved = call.run();
    }
    if (const Json* scale = to.find("scale")) {
        MapCall call(part.mod, handle, "SetActorScale3D");
        call.three("NewScale3D", *scale, kPlace);
        moved = call.run() && moved;
    }
    for (uint32_t i = 0; toggled && i < components; ++i)
        if (before[i].was != kMovable) set_mobility(before[i].component, before[i].was);
    return moved;
}

// A sublevel of the game's that is in the world, and an entry that has not been dealt with for it.
struct Arrived {
    uint64_t level; // the level object
    uint32_t part, sublevel;
    uint32_t first, count; // its places in `found`: the actors it removes, then those it moves, then those it sets values of
    bool taken;            // every actor it names is there
};
struct Looking {
    List<Arrived> arrived;
    List<uint64_t> found; // the actor for each name, or 0
    bool memory = true;
    bool discovery = false;
    bool early = false;
};

uint32_t count_of(const Json* list) { return list ? list->count : 0; }
uint32_t named_count(const Json& entry) { return count_of(entry.find("remove")) + count_of(entry.find("move")) + count_of(entry.find("set")); }
const char* named_at(const Json& entry, uint32_t index) {
    const Json* remove = entry.find("remove"), *move = entry.find("move");
    const uint32_t removes = count_of(remove), moves = count_of(move);
    return index < removes ? remove->at(index)->text : index < removes + moves ? move->at(index - removes)->name : entry.find("set")->at(index - removes - moves)->name;
}

bool on_map(const Part& part) {
    if (!content_path::same(part.map->find("map")->text, world_path.get())) return false;
    const PlacementMode mode = initializing ? early_mode() : current_placement_mode();
    if (!map_ground_matches(*part.map, mode) && !(initializing && mode == PlacementMode::invalid && !part.map->find("mode"))) return false;
    for (uint32_t p = 0; p < parts.count; ++p)
        if (parts[p].mod == part.mod && map_ground_overrides(*part.map, *parts[p].map, mode)) return false;
    return true;
}

bool remember_playing(Part& part) {
    const uint64_t world = game::loaded_world();
    if (!world || !part.changes) return true;
    use_world(world);
    if (!on_map(part)) return true;
    struct Snapshot { Part* part; uint64_t world; bool memory; } snapshot{&part, world, true};
    game::each_object([](void* context, uint64_t actor) {
        auto& at = *static_cast<Snapshot*>(context);
        uint8_t state = 0;
        if (!game::is_a(actor, actor_class) || !game::read(actor + t::actor_play::state, state) || !(state & t::constant::actor_play_state_mask)) return true;
        const uint64_t level = game::outer_of(actor);
        game::Place owner;
        if (!game::is_a(level, level_class) || game::find_place(game::type_of(level), level, "OwningWorld", owner) != TSUCHI_OK ||
            game::read_or<uint64_t>(owner.address) != at.world) return true;
        const game::Name name = game::name_of(game::outer_of(level));
        const uint64_t life = game::to_handle(level);
        for (uint32_t i = 0; i < at.part->sublevels.count; ++i) {
            Sublevel& sublevel = at.part->sublevels[i];
            if (!game::name_is(name, sublevel.entry->name)) continue;
            bool seen = false;
            for (uint32_t j = 0; j < sublevel.late_in.count; ++j) seen = seen || sublevel.late_in[j] == life;
            if (!seen && !sublevel.late_in.add(life)) { at.memory = false; return false; }
        }
        return true;
    }, &snapshot);
    return snapshot.memory;
}

// A shared entry's objects wait for the ground its mod selected in this mode.
bool ground_failed(const Part& part) {
    if (part.failed) return true;
    const PlacementMode mode = current_placement_mode();
    for (uint32_t p = 0; p < parts.count; ++p)
        if (parts[p].mod == part.mod && map_ground_overrides(*part.map, *parts[p].map, mode) && parts[p].failed) return true;
    return false;
}

// The game's levels: one of them belongs to a world named as the sublevel is.
bool look_at_level(void* context, uint64_t object) {
    auto& looking = *static_cast<Looking*>(context);
    if (!game::is_a(object, level_class)) return true;
    game::Place owned;
    if (game::find_place(game::type_of(object), object, "OwningWorld", owned) != TSUCHI_OK || game::read_or<uint64_t>(owned.address) != game::from_handle(world_now)) return true;
    const uint64_t owner = game::outer_of(object);
    if (!owner) return true;
    const game::Name name = game::name_of(owner);
    const uint64_t handle = game::to_handle(object);
    PartOrder order;
    while (Part* next = order.next()) {
        const uint32_t p = uint32_t(next - parts.items);
        Part& part = *next;
        if (!content_path::same(part.map->find("map")->text, world_path.get())) continue;
        for (uint32_t s = 0; s < part.sublevels.count; ++s) {
            if (!game::name_is(name, part.sublevels[s].entry->name)) continue;
            part.present = true;
            bool done = false;
            for (uint32_t i = 0; i < part.sublevels[s].done_in.count; ++i) done = done || part.sublevels[s].done_in[i] == handle;
            if (looking.early && early_mode() == PlacementMode::invalid) {
                bool needs_mode = part.map->find("mode") != nullptr;
                for (uint32_t other = 0; other < parts.count; ++other) {
                    const Part& candidate = parts[other];
                    needs_mode = needs_mode || (candidate.mod == part.mod && candidate.map->find("mode") &&
                        map_has_ground(*candidate.map) && content_path::same(candidate.map->find("map")->text, world_path.get()));
                }
                if (needs_mode) {
                    if (!done) {
                        if (!part.sublevels[s].done_in.add(handle)) looking.memory = false;
                        ++counts.entries_refused;
                        tell(part.mod, "maps: the mode could not be read before ", part.sublevels[s].entry->name,
                             " begins play, so its actor edits were refused");
                    }
                    continue;
                }
            }
            if (!on_map(part)) continue;
            if (looking.discovery || done || part.failed) continue;
            bool late = false;
            for (uint32_t i = 0; i < part.sublevels[s].late_in.count; ++i) late = late || part.sublevels[s].late_in[i] == handle;
            if (!looking.early && !late) continue;
            const uint32_t count = named_count(*part.sublevels[s].entry);
            const uint32_t first = looking.found.count;
            for (uint32_t i = 0; i < count && looking.memory; ++i) looking.memory = looking.found.add(0) != nullptr;
            looking.memory = looking.memory && looking.arrived.add(Arrived{object, p, s, first, count, false});
        }
    }
    return looking.memory;
}

// What is in those levels: the actors the entries name.
bool look_at_actor(void* context, uint64_t object) {
    auto& looking = *static_cast<Looking*>(context);
    const uint64_t level = game::outer_of(object);
    char name[256];
    bool read = false;
    for (uint32_t a = 0; a < looking.arrived.count; ++a) {
        const Arrived& arrived = looking.arrived[a];
        if (arrived.level != level) continue;
        if (!read && (!game::is_a(object, actor_class) || !game::name_text(game::name_of(object), name, sizeof name))) return true;
        read = true;
        const Json& entry = *parts[arrived.part].sublevels[arrived.sublevel].entry;
        for (uint32_t i = 0; i < arrived.count; ++i)
            if (!looking.found[arrived.first + i] && !strcmp(name, named_at(entry, i))) looking.found[arrived.first + i] = object;
    }
    return true;
}

// Whether a part is one whose levels could not be loaded, which changes nothing of the map.
bool part_failed(uint64_t id) {
    for (uint32_t p = 0; p < parts.count; ++p)
        if (parts[p].id == id) return ground_failed(parts[p]);
    return true;
}

// An initial failure gives back the streams. A nested failure after a level arrives stops the part
// without taking away ground under the player. The mod itself keeps running.
void fail_part(Part& part, bool unload) {
    part.failed = true;
    part.ready = false;
    if (unload)
        for (uint32_t i = 0; i < part.streams.count; ++i) tell_stream(part, part.streams[i].stream, "SetShouldBeLoaded", false);
}

// ---- Prepared declarations in the incoming native streaming transaction ---------------------------
// IDs and handles survive native callbacks; no Part or manifest pointer crosses one.
Part* initial_part(uint64_t id) {
    for (uint32_t i = 0; i < parts.count; ++i) if (parts[i].id == id) return &parts[i];
    return nullptr;
}
struct InitialRoot { uint64_t part; Text package; };
struct InitialStream {
    uint64_t part, handle, loaded_level;
    Put put;
    uint8_t depth;
    bool loaded, visible, opened, block_before, block_held, retained;
    bool should_load, should_show;
};
bool initial_reentered = false, initial_progressed = false;
struct InitialGuard {
    uint64_t world, generation, declarations, global;
    bool good() const {
        return !initial_reentered && world_generation == generation && declaration_generation == declarations &&
               game::from_handle(world) && world_now == world && game::global<uint64_t>(t::data::world) == global;
    }
    bool owns(uint64_t id) const {
        const Part* part = initial_part(id);
        return good() && part && part->initial && !part->failed && mod_usable(part->mod);
    }
    bool same_world() const { return world_generation == generation && world_now == world && game::from_handle(world); }
};
void initial_failure(uint64_t id, const char* why) {
    if (Part* part = initial_part(id); part && !part->failed) {
        part->asked = part->failed = true, part->ready = false;
        tell(part->mod, "maps: initial streaming refused: ", why, "; no delayed fallback was requested");
    }
}
bool initial_boolean(TSUCHI_Mod* mod, uint64_t handle, const char* field, bool value) {
    TSUCHI_Value set{TSUCHI_VALUE_BOOLEAN, 0, {}};
    set.as.boolean = value;
    TSUCHI_Value got{};
    return host.reflection->set(mod, TSUCHI_Object{handle}, field, &set) == TSUCHI_OK &&
           host.reflection->get(mod, TSUCHI_Object{handle}, field, &got) == TSUCHI_OK &&
           got.kind == TSUCHI_VALUE_BOOLEAN && got.as.boolean == value;
}
bool initial_package(TSUCHI_Mod* mod, uint64_t handle, Text& package) {
    char text[1024]; uint64_t needed = 0;
    if (host.reflection->get_text(mod, TSUCHI_Object{handle}, "WorldAsset", text, sizeof text, &needed) != TSUCHI_OK) return false;
    content_path::Parts path;
    if (!content_path::split(text, path)) return false;
    text[path.package] = 0;
    return package.set(text);
}
bool initial_same_put(const Put& a, const Put& b) {
    const auto close = [](const double* a, const double* b, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const double d = a[i] - b[i];
            if (!(d < 0.001 && d > -0.001)) return false; // also refuses NaN
        }
        return true;
    };
    if (!close(a.turn, b.turn, 4) || !close(a.place, b.place, 3) || !close(a.scale, b.scale, 3)) return false;
    return true;
}
bool initial_same_policy(TSUCHI_Mod* mod, uint64_t source, uint64_t clone) {
    const char* const fields[] = {"bInitiallyLoaded", "bInitiallyVisible", "bShouldBeLoaded", "bShouldBeVisible", "StreamingPriority"};
    for (const char* field : fields) {
        TSUCHI_Value a{}, b{};
        const TSUCHI_Result from = host.reflection->get(mod, TSUCHI_Object{source}, field, &a);
        const TSUCHI_Result to = host.reflection->get(mod, TSUCHI_Object{clone}, field, &b);
        // Only Dynamic has the initially flags. A missing field on both identical classes is unchanged.
        const bool initially = !strcmp(field, "bInitiallyLoaded") || !strcmp(field, "bInitiallyVisible");
        if (from == TSUCHI_NOT_FOUND && to == TSUCHI_NOT_FOUND && initially) continue;
        if (from != TSUCHI_OK || to != TSUCHI_OK || a.kind != b.kind) return false;
        if (a.kind == TSUCHI_VALUE_BOOLEAN) { if (a.as.boolean != b.as.boolean) return false; }
        else if (a.kind == TSUCHI_VALUE_INTEGER) { if (a.as.integer != b.as.integer) return false; }
        else return false;
    }
    return true;
}
bool initial_array(TSUCHI_Mod* mod, uint64_t handle, const char* field, uint32_t maximum, uint32_t& count) {
    TSUCHI_Value value{};
    if (host.reflection->get(mod, TSUCHI_Object{handle}, field, &value) != TSUCHI_OK ||
        value.kind != TSUCHI_VALUE_ARRAY || value.count > maximum) return false;
    count = value.count;
    return true;
}
uint64_t initial_element(TSUCHI_Mod* mod, uint64_t handle, const char* field, uint32_t index) {
    char path[64]; element(path, sizeof path, field, index);
    TSUCHI_Value value{};
    if (host.reflection->get(mod, TSUCHI_Object{handle}, path, &value) != TSUCHI_OK || value.kind != TSUCHI_VALUE_OBJECT) return 0;
    return value.as.object.id;
}
bool initial_volumes(TSUCHI_Mod* mod, uint64_t source, uint64_t clone, uint64_t parent_level) {
    uint32_t from = 0, to = 0;
    if (!initial_array(mod, source, "EditorStreamingVolumes", 32, from) ||
        !initial_array(mod, clone, "EditorStreamingVolumes", 32, to) || from != to) return false;
    static game::ClassRef volume_class{t::name::streaming_volume_class};
    for (uint32_t i = 0; i < from; ++i) {
        const uint64_t handle = initial_element(mod, source, "EditorStreamingVolumes", i);
        if (handle != initial_element(mod, clone, "EditorStreamingVolumes", i)) return false;
        if (!handle) continue;
        const uint64_t volume = game::from_handle(handle);
        if (!volume || !game::is_a(volume, volume_class) || game::outer_of(volume) != parent_level) return false;
    }
    return true;
}
bool initial_duplicate_package(TSUCHI_Mod* mod, uint64_t world, const char* package) {
    uint32_t count = 0;
    if (!initial_array(mod, world, "StreamingLevels", kMostStreams, count)) return true;
    for (uint32_t i = 0; i < count; ++i) {
        Text path;
        const uint64_t stream = initial_element(mod, world, "StreamingLevels", i);
        const bool duplicate = !stream || !initial_package(mod, stream, path) || content_path::same(package, path.get());
        path.clear();
        if (duplicate) return true;
    }
    return false;
}
void initial_release(List<InitialStream>& streams, uint64_t world, bool rollback) {
    // No RemoveStreamingLevel after a flush: removal does not unload/cancel native work.
    for (uint32_t i = streams.count; i-- > 0;) {
        InitialStream& entry = streams[i];
        const uint64_t stream = game::from_handle(entry.handle);
        if (stream && entry.block_held) {
            uint16_t policy = 0;
            if (game::read(stream + t::streaming_level::policy, policy) && (policy & t::constant::stream_block_load))
                game::write<uint16_t>(stream + t::streaming_level::policy,
                    uint16_t((policy & ~t::constant::stream_block_load) | (entry.block_before ? t::constant::stream_block_load : 0)));
        }
        if (rollback && !initial_progressed && stream && game::from_handle(world) &&
            game::read_or<uint8_t>(stream + t::streaming_level::state) == 1 &&
            !game::read_or<uint64_t>(stream + t::streaming_level::loaded, 1) &&
            !game::read_or<uint64_t>(stream + t::streaming_level::pending_unload, 1)) {
            const bool removed = t::fn::map_remove_stream(reinterpret_cast<void*>(game::from_handle(world)), reinterpret_cast<void*>(stream));
            if (!removed || game::from_handle(entry.handle) != stream || game::read_or<uint8_t>(stream + t::streaming_level::state, 255) != 0)
                if (Part* part = initial_part(entry.part)) tell(part->mod, "maps: provisional native descriptor removal was not confirmed");
        }
        if (entry.retained) release_game_object(entry.handle);
    }
    streams.clear();
}
bool initial_admit(const InitialGuard& guard, List<InitialStream>& streams, uint64_t part_id,
                   const char* package, const Put& put, uint8_t depth, uint64_t source_handle = 0,
                   uint64_t parent_level = 0, uint64_t parent_handle = 0) {
    if (!guard.owns(part_id) || streams.count >= kMostStreams || depth >= kDeepest) return false;
    Part* part = initial_part(part_id);
    TSUCHI_Mod* mod = part->mod;
    const uint64_t world = game::from_handle(guard.world), source = game::from_handle(source_handle);
    const auto parent_valid = [&] {
        const uint64_t parent = game::from_handle(parent_handle);
        return !source || (parent && game::read_or<uint64_t>(parent + t::streaming_level::loaded) == parent_level &&
            game::read_or<uint64_t>(parent_level + t::engine_level::owning_world) == world &&
            game::outer_of(source) == game::outer_of(parent_level));
    };
    if (!world || (source_handle && !source) || !parent_valid() || initial_duplicate_package(mod, guard.world, package)) return false;
    static game::ClassRef dynamic_class{t::name::streaming_class};
    static game::ClassRef base_class{t::name::streaming_base_class};
    static game::ClassRef always_class{t::name::always_loaded_class};
    const uint64_t type = source ? game::class_of(source) : game::find_class(dynamic_class);
    if (!type || (source && (!game::is_a(source, base_class) ||
        (game::read_or<uint32_t>(source + t::object::flags) & t::constant::stream_source_cdo)))) return false;
    Part context{}; context.mod = mod;
    const bool loaded = !source || game::is_a(source, always_class) ||
        flag(context, source_handle, "bInitiallyLoaded", flag(context, source_handle, "bShouldBeLoaded", false));
    const bool visible = !source || flag(context, source_handle, "bInitiallyVisible", flag(context, source_handle, "bShouldBeVisible", false));
    uint16_t original_policy = 0;
    if (source && !game::read(source + t::streaming_level::policy, original_policy)) return false;
    // Keep serialized source facts alive through duplication, validation and admission callbacks.
    struct SourcePin {
        uint64_t handle;
        ~SourcePin() { if (handle) release_game_object(handle); }
    } pin{source ? source_handle : 0};
    if (source && !retain_game_object(source_handle)) { pin.handle = 0; return false; }
    uint64_t made = 0;
    if (source) {
        made = reinterpret_cast<uint64_t>(t::fn::map_duplicate_stream(reinterpret_cast<const void*>(source),
            reinterpret_cast<void*>(world), 0, 0x0FFFFFFFu, nullptr, 0, uint32_t(t::constant::duplicate_internal_flags)));
    } else {
        uint64_t parameters[8]{}; parameters[0] = type, parameters[1] = world;
        made = reinterpret_cast<uint64_t>(t::fn::ui_construct(parameters));
    }
    const uint64_t handle = game::to_handle(made);
    if (!handle || !guard.owns(part_id) || !parent_valid() || game::class_of(made) != type || game::outer_of(made) != world ||
        game::read_or<uint8_t>(made + t::streaming_level::state, 255) != 0 ||
        game::read_or<uint64_t>(made + t::streaming_level::loaded, 1) ||
        game::read_or<uint64_t>(made + t::streaming_level::pending_unload, 1)) return false;
    if (!retain_game_object(handle)) return false;
    InitialStream entry{part_id, handle, 0, put, depth, loaded, visible, false, false, false, true, false, false};
    if (!streams.add(entry)) { release_game_object(handle); return false; }
    InitialStream& kept = streams[streams.count - 1]; // list reserved before any native call
    if (!source) {
        game::Name name{}; uint64_t packed = 0;
        if (game::name_from_utf8(package, name) != TSUCHI_OK) return false;
        memcpy(&packed, &name, sizeof packed);
        if (!guard.owns(part_id)) return false;
        t::fn::map_set_package(reinterpret_cast<void*>(made), packed);
        if (!guard.owns(part_id) || game::from_handle(handle) != made || !initial_boolean(mod, handle, "bInitiallyLoaded", true) ||
            !initial_boolean(mod, handle, "bInitiallyVisible", true) ||
            !initial_boolean(mod, handle, "bShouldBeLoaded", true) ||
            !initial_boolean(mod, handle, "bShouldBeVisible", true)) return false;
    } else if (!initial_volumes(mod, source_handle, handle, parent_level)) return false;
    Text actual;
    const bool named = initial_package(mod, handle, actual) && content_path::same(package, actual.get());
    actual.clear();
    Put read{};
    if (!named || !guard.owns(part_id) || !write_put(context, handle, put) || !read_put(context, handle, read) ||
        !initial_same_put(read, put) || !guard.owns(part_id)) return false;
    if (source && (game::from_handle(source_handle) != source || !initial_same_policy(mod, source_handle, handle) ||
        !initial_volumes(mod, source_handle, handle, parent_level) ||
        game::read_or<uint16_t>(made + t::streaming_level::policy) != original_policy)) return false;
    // Preserve the serialized current policy, including classes without Dynamic's initially fields.
    TSUCHI_Value load{}, show{};
    if (host.reflection->get(mod, TSUCHI_Object{handle}, "bShouldBeLoaded", &load) != TSUCHI_OK || load.kind != TSUCHI_VALUE_BOOLEAN ||
        host.reflection->get(mod, TSUCHI_Object{handle}, "bShouldBeVisible", &show) != TSUCHI_OK || show.kind != TSUCHI_VALUE_BOOLEAN) return false;
    kept.should_load = load.as.boolean, kept.should_show = show.as.boolean;
    uint16_t policy = 0;
    if (!game::read(made + t::streaming_level::policy, policy)) return false;
    kept.block_before = (policy & t::constant::stream_block_load) != 0;
    if (loaded) {
        kept.block_held = true;
        if (!game::write<uint16_t>(made + t::streaming_level::policy, uint16_t(policy | t::constant::stream_block_load))) return false;
    }
    if (!guard.owns(part_id) || !parent_valid()) return false;
    t::fn::map_add_stream(reinterpret_cast<void*>(world), reinterpret_cast<void*>(made));
    if (!guard.owns(part_id) || !parent_valid() || game::from_handle(handle) != made || game::read_or<uint8_t>(made + t::streaming_level::state) != 1 ||
        game::read_or<uint64_t>(made + t::streaming_level::loaded, 1) ||
        game::read_or<uint64_t>(made + t::streaming_level::pending_unload, 1)) return false;
    part = initial_part(part_id);
    if (!part || !part->streams.add(Stream{handle, put, depth, loaded && visible, 0, true, true})) return false;
    ++(depth ? counts.nested_asked : counts.levels_asked);
    return true;
}
bool initial_arrived(const InitialGuard& guard, const InitialStream& entry, uint64_t& level) {
    if (!guard.owns(entry.part) || !entry.loaded) return false;
    const uint64_t raw = game::from_handle(entry.handle);
    level = raw ? game::read_or<uint64_t>(raw + t::streaming_level::loaded) : 0;
    Part context{}; context.mod = initial_part(entry.part)->mod;
    TSUCHI_Value visible{};
    if (!level || !game::is_a(level, level_class) ||
        (entry.loaded_level && game::from_handle(entry.loaded_level) != level) ||
        game::read_or<uint64_t>(level + t::engine_level::owning_world) != game::from_handle(guard.world) ||
        flag(context, entry.handle, "bShouldBeLoaded", !entry.should_load) != entry.should_load ||
        flag(context, entry.handle, "bShouldBeVisible", !entry.should_show) != entry.should_show ||
        (entry.visible && (host.reflection->get(context.mod, TSUCHI_Object{game::to_handle(level)}, "bIsVisible", &visible) != TSUCHI_OK ||
                           visible.kind != TSUCHI_VALUE_BOOLEAN || !visible.as.boolean)) || !guard.owns(entry.part)) return false;
    return game::from_handle(entry.handle) == raw && game::read_or<uint64_t>(raw + t::streaming_level::loaded) == level;
}
bool initial_children(const InitialGuard& guard, List<InitialStream>& streams, uint32_t index) {
    const InitialStream parent = streams[index];
    uint64_t level = 0;
    if (!initial_arrived(guard, parent, level)) return false;
    const uint64_t level_handle = game::to_handle(level);
    if (!level_handle || !retain_game_object(level_handle)) return false;
    struct LevelPin { uint64_t handle; ~LevelPin() { release_game_object(handle); } } pin{level_handle};
    const uint64_t saved_world = game::outer_of(level), saved_handle = game::to_handle(saved_world);
    uint32_t count = 0;
    TSUCHI_Mod* mod = initial_part(parent.part)->mod;
    if (!saved_handle || !initial_array(mod, saved_handle, "StreamingLevels", kMostStreams, count)) return false;
    streams[index].opened = true, streams[index].loaded_level = level_handle;
    // Read/copy one child's facts before the duplicate's callbacks can mutate the declarations.
    for (uint32_t i = 0; i < count; ++i) {
        if (!guard.owns(parent.part)) return false;
        const uint64_t source_handle = initial_element(mod, saved_handle, "StreamingLevels", i);
        Text package; Part context{}; context.mod = mod; Put local{};
        if (!source_handle || !initial_package(mod, source_handle, package) || !read_put(context, source_handle, local)) {
            package.clear(); return false;
        }
        const Put combined = combine(parent.put, local);
        const bool added = initial_admit(guard, streams, parent.part, package.get(), combined,
                                         uint8_t(parent.depth + 1), source_handle, level, parent.handle);
        package.clear();
        uint64_t still = 0;
        if (!added || !initial_arrived(guard, streams[index], still)) return false;
    }
    return guard.owns(parent.part);
}
void on_initial_flush(uint64_t world, uint32_t mode) {
    if (!on_game_thread() || mode != 2 || !parts.count) return next_initial_flush(world, mode);
    if (initial_active) { initial_reentered = initial_progressed = true; return next_initial_flush(world, mode); }
    initial_active = true, initial_reentered = initial_progressed = false;
    const bool outer_initializing = initializing;
    const Initializing outer = initialize;
    use_world(world);
    initializing = true; initialize = Initializing{world, false, PlacementMode::invalid};
    const uint64_t world_handle = game::to_handle(world);
    InitialGuard guard{world_handle, world_generation, declaration_generation, game::global<uint64_t>(t::data::world)};
    List<InitialRoot> roots; List<InitialStream> streams; List<uint64_t> selected;
    bool okay = false;
    const bool retained_here = world_handle && retain_game_object(world_handle);
    okay = retained_here && streams.reserve(kMostStreams);
    // Resolve mode before holding Part/manifest references: its native story query may reenter.
    const PlacementMode mode_now = okay ? early_mode() : PlacementMode::invalid;
    okay = okay && guard.good();
    // This loop only copies recipes; on_map now uses the cached mode.
    PartOrder order;
    while (okay) {
        Part* next = order.next();
        if (!next) break;
        Part& part = *next;
        if (!part.initial || part.initial_seen || !content_path::same(part.map->find("map")->text, world_path.get())) continue;
        bool ambiguous = mode_now == PlacementMode::invalid && part.map->find("mode");
        if (mode_now == PlacementMode::invalid)
            for (uint32_t j = 0; j < parts.count; ++j)
                ambiguous = ambiguous || (parts[j].mod == part.mod && parts[j].map->find("mode") &&
                    content_path::same(parts[j].map->find("map")->text, world_path.get()));
        if (ambiguous) { part.initial_seen = true; initial_failure(part.id, "incoming map mode is unresolved"); continue; }
        if (!on_map(part)) continue;
        if (!selected.add(part.id)) { initial_failure(part.id, "no memory to track initial selection"); okay = false; break; }
        part.initial_seen = part.asked = true;
        const Json* levels = part.map->find("levels");
        for (const Json* path = levels ? levels->first : nullptr; path; path = path->next) {
            InitialRoot root{part.id, {}};
            if (roots.count >= kMostStreams || !root.package.set(path->text) || !roots.add(root)) {
                root.package.clear(); okay = false; break;
            }
        }
    }
    if (!t::bound(t::fn::ui_construct) || !t::bound(t::fn::map_add_stream) || !t::bound(t::fn::map_remove_stream) ||
        !t::bound(t::fn::map_duplicate_stream) || !t::bound(t::fn::map_set_package)) okay = false;
    for (uint32_t i = 0; okay && i < roots.count; ++i)
        okay = initial_admit(guard, streams, roots[i].part, roots[i].package.get(), kAsMade, 0);
    if (!okay) {
        for (uint32_t i = 0; guard.same_world() && i < selected.count; ++i) initial_failure(selected[i], "descriptor configuration, lifetime or native admission failed");
        // The boundary saw this world and failed before it could select every entry for it. Those entries were
        // refused here too: none of them takes the late path that serves worlds the boundary never saw.
        for (uint32_t j = 0; guard.same_world() && j < parts.count; ++j)
            if (parts[j].initial && !parts[j].asked && !parts[j].failed && content_path::same(parts[j].map->find("map")->text, world_path.get()))
                initial_failure(parts[j].id, "the incoming boundary failed before it could select this entry");
        initial_release(streams, world_handle, !initial_progressed);
    }
    // Every invocation preserves its original stock call. Extra calls are only bounded graph frontiers.
    initial_progressed = true;
    next_initial_flush(world, mode);
    okay = okay && guard.good();
    for (uint32_t pass = 0; okay && pass < kDeepest; ++pass) {
        const uint32_t before = streams.count;
        for (uint32_t i = 0; okay && i < before; ++i)
            if (!streams[i].opened) okay = initial_children(guard, streams, i);
        if (!okay || streams.count == before) break;
        initial_progressed = true;
        next_initial_flush(world, mode);
        okay = guard.good();
    }
    // Reflected fields only: no ProcessEvent/native callback can invalidate an earlier witness.
    for (uint32_t i = 0; okay && i < streams.count; ++i) {
        uint64_t level = 0;
        okay = streams[i].opened && initial_arrived(guard, streams[i], level);
    }
    for (uint32_t i = 0; i < selected.count; ++i) {
        if (!okay && guard.same_world()) initial_failure(selected[i], "required graph is unavailable, unloaded, invalid, or exceeded the bounded initial phase");
        else if (okay) if (Part* part = initial_part(selected[i])) part->ready = true;
    }
    initial_release(streams, world_handle, false);
    for (uint32_t i = 0; i < roots.count; ++i) roots[i].package.clear();
    roots.clear(); selected.clear();
    if (retained_here) release_game_object(world_handle);
    initializing = outer_initializing; initialize = outer;
    initial_active = false;
}

// A level saved without an actor has no references to it. Before play, the other actors of the level still
// hold the removed ones, so their references are set to nothing, as they would be in such a level, and each is
// told in the log of the mod that removed that actor. Each actor of the level is looked through once.
void forget_removed(Looking& looking) {
    struct Telling {
        const Looking* looking;
        const List<uint64_t>* removed;
        const List<uint32_t>* by; // the arrived entry that removed each
        uint64_t holder;
        char name[game::kNameBytes]; // the holder's, spelled once it is needed
    };
    List<uint64_t> levels;
    for (uint32_t a = 0; a < looking.arrived.count && looking.memory; ++a) {
        const uint64_t level = looking.arrived[a].level;
        bool seen = false;
        for (uint32_t l = 0; l < levels.count; ++l) seen = seen || levels[l] == level;
        if (seen) continue;
        if (!levels.add(level)) { looking.memory = false; break; }
        List<uint64_t> removed;
        List<uint32_t> by;
        for (uint32_t b = a; b < looking.arrived.count && looking.memory; ++b) {
            const Arrived& arrived = looking.arrived[b];
            if (!arrived.taken || arrived.level != level) continue;
            const uint32_t removes = count_of(parts[arrived.part].sublevels[arrived.sublevel].entry->find("remove"));
            for (uint32_t i = 0; i < removes; ++i) {
                const uint64_t actor = looking.found[arrived.first + i];
                bool listed = false;
                for (uint32_t r = 0; r < removed.count; ++r) listed = listed || removed[r] == actor;
                if (!listed && (!removed.add(actor) || !by.add(b))) { looking.memory = false; break; }
            }
        }
        const uint64_t actors = game::read_or<uint64_t>(level + t::engine_level::actors + t::array::data);
        const int32_t count = game::read_or<int32_t>(level + t::engine_level::actors + t::array::count, -1);
        if (looking.memory && removed.count && count > 0 && uint32_t(count) <= t::constant::max_objects &&
            game::readable(actors, uint64_t(count) * sizeof(uint64_t))) {
            Telling telling{&looking, &removed, &by, 0, {}};
            for (int32_t k = 0; k < count; ++k) {
                const uint64_t holder = game::read_or<uint64_t>(actors + uint64_t(k) * sizeof(uint64_t));
                bool gone = !holder || !game::live(holder);
                for (uint32_t r = 0; r < removed.count && !gone; ++r) gone = removed[r] == holder;
                if (gone) continue;
                telling.holder = holder, telling.name[0] = 0;
                game::clear_references(holder, removed.items, removed.count, [](void* context, const char* path, uint32_t target) {
                    auto& said = *static_cast<Telling*>(context);
                    const uint64_t actor = said.removed->items[target];
                    const Arrived& arrived = said.looking->arrived.items[said.by->items[target]];
                    Part& part = parts[arrived.part];
                    char gone[game::kNameBytes];
                    if (!game::name_text(game::name_of(actor), gone, sizeof gone)) gone[0] = 0;
                    if (!said.name[0] && !game::name_text(game::name_of(said.holder), said.name, sizeof said.name)) said.name[0] = 0;
                    tell(part.mod, "maps: ", path, " of ", said.name, " in ", part.sublevels[arrived.sublevel].entry->name,
                         " now refers to nothing, as it referred to the removed ", gone);
                }, &telling);
            }
        }
        removed.clear(), by.clear();
    }
    levels.clear();
}

void apply_changes(Looking& looking) {
    for (uint32_t a = 0; looking.memory && a < looking.arrived.count; ++a) {
        Arrived& arrived = looking.arrived[a];
        Part& part = parts[arrived.part];
        Sublevel& sublevel = part.sublevels[arrived.sublevel];
        if (!sublevel.done_in.add(game::to_handle(arrived.level))) { looking.memory = false; break; }
        if (!looking.early)
            tell(part.mod, "maps: ", sublevel.entry->name, " was already playing when this mod started, so its changes use the late fallback");
        arrived.taken = true;
        for (uint32_t i = 0; i < arrived.count && arrived.taken; ++i) {
            if (looking.found[arrived.first + i]) continue;
            arrived.taken = false;
            tell(part.mod, "maps: ", sublevel.entry->name, " has no actor ", named_at(*sublevel.entry, i), ", so nothing of that sublevel is changed for this mod");
        }
        ++(arrived.taken ? counts.entries_taken : counts.entries_refused);
    }
    // Removals first, then moves, then values, whichever mods they are of: a removal wins.
    enum { removing, moving, setting, kinds };
    for (int kind = removing; looking.memory && kind < kinds; ++kind) {
        for (uint32_t a = 0; a < looking.arrived.count; ++a) {
            const Arrived& arrived = looking.arrived[a];
            if (!arrived.taken) continue;
            Part& part = parts[arrived.part];
            const Json& entry = *part.sublevels[arrived.sublevel].entry;
            const uint32_t removes = count_of(entry.find("remove")), moves = count_of(entry.find("move"));
            const uint32_t from = kind == removing ? 0 : kind == moving ? removes : removes + moves;
            const uint32_t to = kind == removing ? removes : kind == moving ? removes + moves : arrived.count;
            for (uint32_t i = from; i < to; ++i) {
                const uint64_t actor = looking.found[arrived.first + i];
                bool gone = false; // removed already, for this mod or another
                for (uint32_t other = 0; other < looking.arrived.count && !gone; ++other) {
                    const Arrived& theirs = looking.arrived[other];
                    if (!theirs.taken) continue;
                    const uint32_t their_removes = count_of(parts[theirs.part].sublevels[theirs.sublevel].entry->find("remove"));
                    for (uint32_t j = 0; j < their_removes && !gone; ++j)
                        gone = looking.found[theirs.first + j] == actor && (kind != removing || other < a || (other == a && j < i));
                }
                if (gone) continue;
                if (kind == removing) {
                    remove_actor(part, actor);
                    ++counts.removed;
                    tell(part.mod, "Removed ", named_at(entry, i), " from ", entry.name, " for this map.");
                } else if (kind == setting) {
                    values_say(part.id, part.mod, game::to_handle(actor), entry, *entry.find("set")->at(i - from));
                } else if (move_actor(part, actor, *entry.find("move")->at(i - from), looking.early)) {
                    ++counts.moved;
                    tell(part.mod, "Moved ", entry.find("move")->at(i - from)->name, " in ", entry.name, " for this map.");
                    // Without memory for the record, the move is only not looked at again.
                    if (looking.early) remember_early_move(EarlyMove{part.id, game::to_handle(actor), entry.find("move")->at(i - from), &entry});
                } else
                    tell(part.mod, "maps: ", entry.find("move")->at(i - from)->name, " in ", entry.name, " could not be moved");
            }
        }
        if (kind == removing && looking.early) forget_removed(looking);
    }
    // Each value that was named gets the say of the mod started last.
    values_settle();
    looking.arrived.clear(), looking.found.clear();
}

// Each early move is looked at once its actor plays. An actor found away from where it was moved is moved
// again, as a move in a playing level is, and the mod's log says so.
void check_early_moves() {
    for (uint32_t m = early_moves.count; m-- > 0;) {
        const EarlyMove move = early_moves[m];
        Part* part = nullptr;
        for (uint32_t p = 0; p < parts.count && !part; ++p)
            if (parts[p].id == move.part) part = &parts[p];
        const uint64_t actor = game::from_handle(move.actor);
        if (!part || !actor) { early_moves.remove(&early_moves[m]); continue; }
        uint8_t state = 0;
        if (!game::read(actor + t::actor_play::state, state) || !(state & t::constant::actor_play_state_mask)) continue; // not playing yet
        early_moves.remove(&early_moves[m]);
        double at[6];
        bool read;
        {
            MapCall place(part->mod, move.actor, "K2_GetActorLocation");
            read = place.run() && place.number_at("ReturnValue.X", at[0]) && place.number_at("ReturnValue.Y", at[1]) && place.number_at("ReturnValue.Z", at[2]);
        }
        if (read) {
            MapCall turn(part->mod, move.actor, "K2_GetActorRotation");
            read = turn.run() && turn.number_at("ReturnValue.Pitch", at[3]) && turn.number_at("ReturnValue.Yaw", at[4]) && turn.number_at("ReturnValue.Roll", at[5]);
        }
        if (!read) continue;
        const Json* location = move.to->find("location"), *rotation = move.to->find("rotation");
        bool held = true;
        for (uint32_t k = 0; k < 3; ++k) {
            double turned = at[3 + k] - rotation->at(k)->number;
            turned -= 360.0 * double(int64_t(turned / 360.0 + (turned < 0 ? -0.5 : 0.5)));
            held = held && at[k] - location->at(k)->number < 1 && location->at(k)->number - at[k] < 1 && turned < 0.1 && turned > -0.1;
        }
        if (held) continue;
        const bool moved = move_actor(*part, actor, *move.to, false);
        tell(part->mod, "maps: ", move.to->name, " in ", move.entry->name, moved ? " was back in its saved place once its level began play, so it was moved again"
                                                                            : " was back in its saved place once its level began play and could not be moved again");
    }
}

void look_at_world() {
    // Each mod's levels first. Nothing of the game's is touched until all of them are in the world.
    bool all_in = true;
    bool original_levels = false;
    for (uint32_t p = 0; p < parts.count; ++p) {
        Part& part = parts[p];
        part.present = false;
        original_levels = original_levels || (part.sublevels.count && !part.failed && on_map(part));
    }
    Looking discovery;
    discovery.discovery = true;
    // Content-only entries bring ground, navigation or placements without editing the game's actors.
    // Their own streams are tracked below: neither of the whole object-table searches serves them.
    if (original_levels) game::each_object(look_at_level, &discovery);
    PartOrder order;
    while (Part* next = order.next()) {
        Part& part = *next;
        if (!on_map(part) || part.failed) continue;
        const Json* levels = part.map->find("levels");
        if (!part.asked && (!levels || !levels->count)) part.asked = true; // it brings no levels: nothing to wait for
        // The initial boundary did not see this world: it came by another load path, or the entry was
        // prepared after the world's LoadMap. Its levels take the late path, as an entry made at run time does.
        if (!part.asked && part.initial && !part.late_told) {
            part.late_told = true;
            tell(part.mod, "maps: the prepared entry missed its incoming initial streaming boundary, so its levels load as the world runs");
        }
        if (!part.asked) {
            all_in = false;
            if (part.sublevels.count ? !part.present && world_steps < 900 : world_steps < 300) continue;
            part.asked = true;
            for (const Json* level = levels->first; level && !part.failed; level = level->next) {
                const uint64_t stream = load_level(part, level->text);
                ++(stream ? counts.levels_asked : counts.levels_refused);
                if (!stream || !part.streams.add(Stream{stream, kAsMade, 0, true, world_steps, false})) {
                    if (stream) tell_stream(part, stream, "SetShouldBeLoaded", false);
                    fail_part(part, true);
                    tell(part.mod, "maps: ", level->text, " could not be loaded into ", world_path.get(), ", so nothing of that map is changed for this mod");
                }
            }
            continue;
        }
        // Every awaited stream gets 600 world updates to become visible, including nested streams.
        bool in = true;
        for (uint32_t i = 0; !part.failed && i < part.streams.count; ++i) {
            if (!part.streams[i].opened && !part.streams[i].initial) open(part, i);
            const Stream stream = part.streams[i];
            if (!stream.awaited) continue;
            const bool arrived = stream.opened && level_visible(part, stream.stream);
            if (!arrived && world_steps - stream.requested >= 600) {
                fail_part(part, false);
                tell(part.mod, "maps: an awaited level did not arrive in ", world_path.get(), ", so further map changes stop for this mod");
            }
            in = in && arrived;
        }
        part.ready = !part.failed && in;
        all_in = all_in && (part.failed || in);
    }
    if (!all_in) return;
    // Then the mods' placement lists, once for a world: what they place has its ground now.
    placements_look(world_now, world_path.get(), part_failed);
    if (!original_levels) return;
    // Only levels already playing when a part started can use this path.
    Looking looking;
    game::each_object(look_at_level, &looking);
    if (looking.memory && looking.arrived.count) game::each_object(look_at_actor, &looking);
    apply_changes(looking);
}

// The mod's navigation in the place of the game's: that of the mod started last, of those that bring any for
// this map. Tried at every step until the map's navmeshes are there.
void navigate() {
    Part* chosen = nullptr;
    for (uint32_t p = 0; p < parts.count; ++p)
        if (parts[p].navigation && !parts[p].failed && on_map(parts[p]) && (!chosen || mod_rank(parts[p].mod) > mod_rank(chosen->mod))) chosen = &parts[p];
    if (!chosen) {
        navigated = true;
        return;
    }
    const Json* levels = chosen->map->find("levels");
    if (levels && levels->count && !chosen->ready) return;
    const NavigationResult result = swap_navigation(chosen->navigation_state, game::from_handle(world_now), navigation_counts);
    if (result == NavigationResult::waiting) {
        if (++navigation_tries == 600) {
            navigated = true;
            tell(chosen->mod, "maps: stopped waiting for navigation in ", world_path.get(), " after 600 attempts");
        }
        return;
    }
    navigated = true;
    static const char* const why[] = {"", "its navmeshes did not come", "the file is for another grid than the game's", "the file has more tiles than the game's navmesh takes",
                                      "there is no memory for it", "the navmesh would not take one of its tiles"};
    if (result != NavigationResult::done) tell(chosen->mod, "maps: the navigation for ", world_path.get(), " was not put in whole: ", why[uint32_t(result)]);
    for (uint32_t p = 0; p < parts.count; ++p)
        if (parts[p].navigation && on_map(parts[p]) && &parts[p] != chosen)
            tell(parts[p].mod, "maps: the navigation of ", mod_id(chosen->mod), " is used for ", world_path.get(), ", not this mod's");
}

void use_world(uint64_t world) {
    const uint64_t handle = world ? game::to_handle(world) : 0;
    if (handle != world_now) {
        values_put_back(); // what the world that is left held, where it outlasts that world
        ++world_generation;
        world_now = handle, world_steps = 0, navigation_tries = 0, navigated = false;
        early_moves.clear();
        for (uint32_t p = 0; p < parts.count; ++p) {
            parts[p].asked = parts[p].failed = false, parts[p].streams.count = 0;
            parts[p].ready = false, parts[p].initial_seen = false, parts[p].late_told = false;
            reset_navigation(parts[p].navigation_state);
            for (uint32_t s = 0; s < parts[p].sublevels.count; ++s) parts[p].sublevels[s].done_in.count = 0;
        }
        static char path[1024];
        uint64_t needed = 0;
        const uint64_t package = world ? game::outer_of(world) : 0;
        const bool named = package && game::object_path(package, path, sizeof path, &needed) == TSUCHI_OK;
        if (!world_path.set(named ? path : "")) world_now = 0; // looked at again next step
    }
}

// Whether a level is one that a mod's part asked for or brought, as it is in the world now.
bool mod_level(uint64_t level) {
    const uint64_t handle = game::to_handle(level);
    for (uint32_t p = 0; handle && p < parts.count; ++p)
        for (uint32_t s = 0; s < parts[p].streams.count; ++s) {
            TSUCHI_Value loaded{};
            if (host.reflection->get(parts[p].mod, TSUCHI_Object{parts[p].streams[s].stream}, "LoadedLevel", &loaded) == TSUCHI_OK &&
                loaded.kind == TSUCHI_VALUE_OBJECT && loaded.as.object.id == handle) return true;
        }
    return false;
}

// A level leaving the world during play. When the game hides one of its own sublevels it sets a flag in its
// context first, and its widgets keep what holds them on the screen. Without the flag, each widget on the screen
// lets go of that holder, and a widget the game keeps aside for later, such as an action marker in its pool, is
// then only held by the game's own list, which garbage collection does not see: the next marker shown used freed
// memory. A mod's level leaving the world, as a streaming volume makes it when the camera leaves, gets the flag
// the game's own levels get.
void on_remove_level(uint64_t world, uint64_t level, uint32_t flags) {
    const uint64_t context = game::game_context();
    const uint64_t guard = context ? context + t::game_context::level_removal_guard : 0;
    uint8_t before = 1;
    const bool set = guard && level && on_game_thread() && game::read(guard, before) && !before && mod_level(level) &&
                     game::write<uint8_t>(guard, 1);
    next_remove_level(world, level, flags);
    if (set) game::write<uint8_t>(guard, 0);
}

void on_initialize(uint64_t level, int32_t budget, bool reconstruct, void* context) {
    const uint64_t world = game::read_or<uint64_t>(level + t::engine_level::owning_world);
    if (parts.count && world && world == game::global<uint64_t>(t::data::world) && on_game_thread()) {
        use_world(world);
        const bool outer_initializing = initializing;
        const Initializing outer = initialize;
        initializing = true;
        initialize = Initializing{world, false, PlacementMode::invalid};
        const uint64_t actors = game::read_or<uint64_t>(level + t::engine_level::actors + t::array::data);
        const int32_t count = game::read_or<int32_t>(level + t::engine_level::actors + t::array::count, -1);
        const bool listed = count >= 0 && uint32_t(count) <= t::constant::max_objects && (!count || game::readable(actors, uint64_t(count) * sizeof(uint64_t)));
        bool playing = false;
        for (int32_t i = 0; listed && i < count && !playing; ++i) {
            const uint64_t actor = game::read_or<uint64_t>(actors + uint64_t(i) * sizeof(uint64_t));
            playing = actor && (game::read_or<uint8_t>(actor + t::actor_play::state) & t::constant::actor_play_state_mask);
        }
        // Reregistration in a playing level must not become an early edit.
        if (!playing) {
            Looking looking;
            looking.early = true;
            look_at_level(&looking, level);
            if (looking.arrived.count && !listed) {
                for (uint32_t a = 0; a < looking.arrived.count; ++a)
                    tell(parts[looking.arrived[a].part].mod, "maps: the level's actors could not be read before play");
                looking.arrived.clear(), looking.found.clear();
            } else if (looking.arrived.count) {
                for (int32_t i = 0; i < count && looking.memory; ++i) {
                    const uint64_t actor = game::read_or<uint64_t>(actors + uint64_t(i) * sizeof(uint64_t));
                    if (actor) look_at_actor(&looking, actor);
                }
                apply_changes(looking);
            }
        }
        initializing = outer_initializing;
        initialize = outer;
    }
    next_initialize(level, budget, reconstruct, context);
}

void step() {
    if (!parts.count) return;
    const uint64_t world = game::loaded_world();
    // A pre-play hook may already have changed this world. Its first frame must
    // keep those edits and their original values, even while loaded_world is zero.
    if (!world && game::global<uint64_t>(t::data::world) == game::from_handle(world_now)) return;
    use_world(world);
    if (!world_now || !world_path.get()[0]) return;
    // Unknown mode state is retried without finishing navigation or consuming the settling delay.
    if (!placement_mode_matches(PlacementMode::story) && !placement_mode_matches(PlacementMode::night) && !placement_mode_matches(PlacementMode::olimar)) return;
    if (!navigated) navigate();
    ++world_steps;
    if (early_moves.count) check_early_moves();
    // A look goes over every object the engine has, so it is taken twice a second while the map settles and
    // every four seconds after that, for sublevels that come later.
    if (world_steps < 30 || world_steps % (world_steps < 900 ? 15 : 120)) return;
    look_at_world();
}

} // namespace

bool map_part_failed(uint64_t id) { return part_failed(id); }

bool map_part_ready(uint64_t id) {
    for (uint32_t p = 0; p < parts.count; ++p) {
        const Part& part = parts[p];
        if (part.id != id) continue;
        const PlacementMode mode = current_placement_mode();
        if (mode == PlacementMode::invalid || ground_failed(part)) return false;
        bool replaced = false;
        for (uint32_t other = 0; other < parts.count; ++other) {
            const Part& selected = parts[other];
            if (selected.mod != part.mod || !map_ground_overrides(*part.map, *selected.map, mode)) continue;
            replaced = true;
            const Json* levels = selected.map->find("levels");
            if (levels && levels->count && (!selected.ready || game::to_handle(game::loaded_world()) != world_now)) return false;
        }
        if (replaced) return true;
        const Json* levels = part.map->find("levels");
        return !part.failed && (!levels || !levels->count || (part.ready && game::to_handle(game::loaded_world()) == world_now));
    }
    return false;
}

void maps_start() {
    add_manifest_section({"maps", maps_well_formed, maps_begin, true});
    each_frame(step);
}

void map_changes_start(TSUCHI_Mod* self) {
    placement_changes_start(self);
    if (!initial_hook) {
        uint64_t id = 0;
        initial_hook = hook_site(self, t::code_site::map_initial_flush, on_initial_flush, &next_initial_flush, &id) == TSUCHI_OK;
    }
    if (!remove_hook) {
        uint64_t id = 0;
        remove_hook = hook_site(self, t::code_site::world_remove_level, on_remove_level, &next_remove_level, &id) == TSUCHI_OK;
    }
    if (initialize_hook) return;
    uint64_t id = 0;
    initialize_hook = hook_site(self, t::code_site::level_prepare, on_initialize, &next_initialize, &id) == TSUCHI_OK;
}

uint32_t map_parts() { return parts.count; }
MapCounts map_counts() {
    MapCounts all = counts;
    const ValueCounts values = value_counts();
    all.values_set = values.set, all.values_refused = values.refused, all.values_put_back = values.put_back;
    return all;
}
NavigationCounts map_navigation_counts() { return navigation_counts; }

} // namespace tsuchi
