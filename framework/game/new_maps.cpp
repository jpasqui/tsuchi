// The manifest's "new_maps": maps of a mod's own. Each is an area the player chooses in the game's list of areas
// and travels to, with progress of its own in the save.
//
//   "new_maps": {"course": {"name": "Terrain Course",
//                           "builds": [{"world": "/Game/Mods/<id>/Area005_P", "scene": "/Game/Mods/<id>/SD_Area005.SD_Area005"}]}}
//
// The game numbers its areas, and a few of the numbers belong to no area. Everything it keeps of an area it finds
// by a name with the number in it, and those names are inside a mod's built files, so a build is the map built for
// one of the free numbers. The framework gives the numbers out as mods start and keeps what it gave in the player's
// save, so a map has the same number in every run and no map comes into another's progress.
//
// What the game is given for a map: its scene in the game's list of scenes, a row for its first base in the table
// of bases, its name as text, and what the list of areas and the radar show for it, in rows of the game's tables.
// While the game's menu of areas is open, the game's list of area numbers is its own followed by the maps'; see
// "The menu of areas" below.
#include "new_maps.h"
#include "asset_bundles.h"
#include "content_path.h"
#include "records.h"

#include <string.h>

#include "engine_call.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "mods.h"
#include "names.h"
#include "objects.h"
#include "pikmin_service.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "shared.h"
#include "text.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using maps::join;
using maps::tell;

const Kind kMap{"new_maps.map"};
constexpr uint32_t kAreas = sizeof t::tables::free_areas / sizeof t::tables::free_areas[0];
// The game's own names for what belongs to an area, with the area's number where the zeros are.
constexpr char kLevelShape[] = "Area000_P", kSceneShape[] = "SD_Area000", kBaseRowShape[] = "Area000_00";
// The first area's row in the table of bases: a new area's row starts as a copy of it.
constexpr char kBaseRowToCopy[] = "Area001_00";
const char *const kBasesTable = t::asset::area_bases_table;
constexpr char kBasesTableName[] = "DT_AreaBaseCampTransition";
// Where the game reads an area's name from.
constexpr char kNameSpace[] = "MapName";
// Where the framework keeps which map has which number.
constexpr char kRecord[] = "save:/tsuchi/new_maps.json";
constexpr uint32_t kNameUnits = 160; // the longest name of an area
constexpr uint32_t kMostTotal = 10000;

game::ClassRef scene_keeper_class{"GameSceneMgr"};
game::ClassRef scene_class{"GameSceneData"};
game::ClassRef counts_class{"ActorPlacedNumAsset"};
game::ClassRef table_class{"DataTable"};
game::ClassRef widget_class{"Widget"};

TSUCHI_Mod *self = nullptr;
NewMapCounts counts{};

// ---- Small tools ----------------------------------------------------------------------------------

// A line of text put together from pieces. What does not fit is left off.
struct Line {
    char text[800];
    size_t used = 0;
    Line() { text[0] = 0; }
    Line &add(const char *piece) {
        for (; *piece && used + 1 < sizeof text; ++piece) text[used++] = *piece;
        text[used] = 0;
        return *this;
    }
    Line &number(uint32_t value) {
        char digits[12];
        char *at = digits + 11;
        *at = 0;
        do *--at = char('0' + value % 10);
        while (value /= 10);
        return add(at);
    }
};

bool whole_text(const Json *value) { return value && value->kind == Json::Kind::text && value->bytes && strlen(value->text) == value->bytes; }
bool path_text(const Json *value) { return whole_text(value) && !content_path::wrong(value->text, value->bytes, content_path::Names::either); }

// Where the three zeros of a shape are.
size_t zeros_in(const char *shape) {
    size_t at = 0;
    while (shape[at] != '0') ++at;
    return at;
}
// The last place of a character in a text, or null.
const char *last_of(const char *text, char wanted) {
    const char *found = nullptr;
    for (; *text; ++text)
        if (*text == wanted) found = text;
    return found;
}

// A name of the game's for an area: the shape, with the number in place of its zeros.
void area_name(const char *shape, uint32_t area, char *out) {
    memcpy(out, shape, strlen(shape) + 1);
    char *at = out + zeros_in(shape);
    at[0] = char('0' + area / 100 % 10), at[1] = char('0' + area / 10 % 10), at[2] = char('0' + area % 10);
}

// The number in a name of that shape, or -1 when the text is not of the shape.
int32_t area_in(const char *shape, const char *text, size_t length) {
    if (length != strlen(shape)) return -1;
    const size_t zeros = zeros_in(shape);
    int32_t area = 0;
    for (size_t i = 0; i < length; ++i) {
        if (i >= zeros && i < zeros + 3) {
            if (text[i] < '0' || text[i] > '9') return -1;
            area = area * 10 + (text[i] - '0');
        } else if (text[i] != shape[i]) {
            return -1;
        }
    }
    return area;
}

// The place of a number among the game's free areas, or -1.
int32_t slot_of(int32_t area) {
    for (uint32_t i = 0; i < kAreas; ++i)
        if (area >= 0 && t::tables::free_areas[i] == uint64_t(area)) return int32_t(i);
    return -1;
}
uint32_t area_of(uint32_t slot) { return uint32_t(t::tables::free_areas[slot]); }

// The number a level is built for: its package's name is the game's name for an area's level.
int32_t area_of_world(const char *path) {
    const char *leaf = last_of(path, '/');
    return leaf ? area_in(kLevelShape, leaf + 1, strlen(leaf + 1)) : -1;
}
// The number a scene is for: the asset is named as the game names an area's scene, whether or not the path
// says the object's name after the package's.
int32_t area_of_scene(const char *path) {
    const char *leaf = last_of(path, '/');
    if (!leaf) return -1;
    ++leaf;
    const char *dot = last_of(leaf, '.');
    const int32_t area = area_in(kSceneShape, leaf, dot ? size_t(dot - leaf) : strlen(leaf));
    return !dot || area_in(kSceneShape, dot + 1, strlen(dot + 1)) == area ? area : -1;
}

// ---- How the part is written ------------------------------------------------------------------------

bool number_in(const Json *value, double least, double most) {
    return value && value->kind == Json::Kind::number && value->number >= least && value->number <= most;
}
bool numbers(const Json *value, uint32_t count) {
    if (!value || value->kind != Json::Kind::list || value->count != count) return false;
    for (const Json *number = value->first; number; number = number->next)
        if (number->kind != Json::Kind::number) return false;
    return true;
}
// Whether every key of an object is one of those given.
bool only_keys(const Json &object, const char *const *known, uint32_t count, const char *&stray) {
    for (const Json *member = object.first; member; member = member->next) {
        bool found = false;
        for (uint32_t i = 0; i < count && !found; ++i) found = !strcmp(member->name, known[i]);
        if (!found) return stray = member->name, false;
    }
    return true;
}

bool radar_well_formed(const Json &radar, const char *map, Text &wrong) {
    static const char *const keys[] = {"texture", "offset", "size", "angle", "scale", "limits"};
    const char *stray = nullptr;
    if (radar.kind != Json::Kind::object || !path_text(radar.find("texture")))
        return join(wrong, "\"new_maps\": ", map, ": \"radar\" is an object with \"texture\", the path of a texture"), false;
    if (!only_keys(radar, keys, 6, stray)) return join(wrong, "\"new_maps\": ", map, ": \"radar\" has a key the framework does not know: ", stray), false;
    const Json *offset = radar.find("offset"), *size = radar.find("size"), *angle = radar.find("angle"), *scale = radar.find("scale"),
               *limits = radar.find("limits");
    if (offset && !numbers(offset, 2)) return join(wrong, "\"new_maps\": ", map, ": the radar's \"offset\" is two numbers"), false;
    if (size && !(number_in(size, 0, 1e7) && size->number > 0))
        return join(wrong, "\"new_maps\": ", map, ": the radar's \"size\" is a number above 0, in centimeters"), false;
    if (angle && !number_in(angle, -360, 360)) return join(wrong, "\"new_maps\": ", map, ": the radar's \"angle\" is a number from -360 to 360"), false;
    if (scale && !(number_in(scale, 0, 100) && scale->number > 0))
        return join(wrong, "\"new_maps\": ", map, ": the radar's \"scale\" is a number above 0, up to 100"), false;
    if (limits && !(numbers(limits, 4) && limits->at(0)->number < limits->at(2)->number && limits->at(3)->number < limits->at(1)->number))
        return join(wrong, "\"new_maps\": ", map,
                    ": the radar's \"limits\" are four numbers: left, top, right and bottom, with left below right and bottom below top"),
               false;
    return true;
}

bool map_well_formed(const Json &map, Text &wrong) {
    static const char *const keys[] = {"name", "builds", "preview", "radar", "pikmin", "totals"};
    const char *stray = nullptr;
    if (!valid_mod_id(map.name)) return join(wrong, "\"new_maps\": a map's name is spelled as a mod's ID is: ", map.name), false;
    if (map.kind != Json::Kind::object) return join(wrong, "\"new_maps\": ", map.name, " is an object with \"name\" and \"builds\""), false;
    if (!only_keys(map, keys, 6, stray)) return join(wrong, "\"new_maps\": ", map.name, " has a key the framework does not know: ", stray), false;
    const Json *name = map.find("name"), *builds = map.find("builds");
    size_t units = 0;
    if (!whole_text(name) || !game::utf8_to_units(name->text, nullptr, 0, units) || units > kNameUnits)
        return join(wrong, "\"new_maps\": ", map.name, ": \"name\" is the area's name, as text of up to 160 characters"), false;
    if (!builds || builds->kind != Json::Kind::list || !builds->count || builds->count > kAreas)
        return join(wrong, "\"new_maps\": ", map.name, ": \"builds\" is a list of the map's builds, one for each of the game's free areas it is built for"),
               false;
    bool built[kAreas] = {};
    for (const Json *build = builds->first; build; build = build->next) {
        const Json *world = build->kind == Json::Kind::object ? build->find("world") : nullptr, *scene = world ? build->find("scene") : nullptr;
        const Json *paths[] = {world, scene};
        for (const Json *path : paths)
            if (whole_text(path) && path->text[0] == '/') {
                if (const char *why = content_path::wrong(path->text, path->bytes, content_path::Names::either))
                    return join(wrong, "\"new_maps\": ", map.name, ": ", path->text, why), false;
            }
        if (!path_text(world) || !path_text(scene) || build->count != 2)
            return join(wrong, "\"new_maps\": ", map.name, ": a build is \"world\", the package of the map's level, and \"scene\", the path of its scene"),
                   false;
        const int32_t slot = slot_of(area_of_world(world->text));
        if (slot < 0)
            return join(wrong, "\"new_maps\": ", map.name, ": ", world->text, " is no level named for one of the game's free areas, as Area005_P is"), false;
        if (area_of_scene(scene->text) != int32_t(area_of(uint32_t(slot))))
            return join(wrong, "\"new_maps\": ", map.name, ": ", scene->text, " is no scene named for the area of its level, as SD_Area005 is for Area005_P"),
                   false;
        if (built[slot]) return join(wrong, "\"new_maps\": ", map.name, ": two builds are for the same area: ", world->text), false;
        built[slot] = true;
    }
    if (const Json *preview = map.find("preview"))
        if (!path_text(preview)) return join(wrong, "\"new_maps\": ", map.name, ": \"preview\" is the path of a texture"), false;
    if (const Json *radar = map.find("radar"))
        if (!radar_well_formed(*radar, map.name, wrong)) return false;
    if (const Json *pikmin = map.find("pikmin")) {
        bool right = pikmin->kind == Json::Kind::list;
        for (const Json *kind = right ? pikmin->first : nullptr; kind && right; kind = kind->next) {
            right = whole_text(kind) && id_valid(kind->text);
            for (const Json *other = pikmin->first; other != kind && right; other = other->next) right = !id_same(other->text, kind->text);
        }
        if (!right) return join(wrong, "\"new_maps\": ", map.name, ": \"pikmin\" is a list of types of Pikmin, each by its ID and each once"), false;
    }
    if (const Json *totals = map.find("totals")) {
        static const char *const kinds[] = {"treasures", "castaways", "flarlic"};
        bool right = totals->kind == Json::Kind::object && only_keys(*totals, kinds, 3, stray);
        for (const Json *total = right ? totals->first : nullptr; total && right; total = total->next)
            right = total->kind == Json::Kind::number && total->whole && total->integer >= 0 && total->integer <= kMostTotal;
        if (!right)
            return join(wrong, "\"new_maps\": ", map.name,
                        ": \"totals\" holds \"treasures\", \"castaways\" and \"flarlic\", each a whole number from 0 to 10000"),
                   false;
    }
    return true;
}

bool well_formed(const Json &section, Text &wrong) {
    if (section.kind != Json::Kind::object) return wrong.set("\"new_maps\" is an object: each of the mod's maps by a short name"), false;
    for (const Json *map = section.first; map; map = map->next)
        if (!map_well_formed(*map, wrong)) return false;
    return true;
}

// ---- Which map has which number ---------------------------------------------------------------------

// One of the game's free areas, by its place among them: the map it is kept for, and whether any story has been
// given a record of the area for that map.
struct Kept {
    char id[kIdBytes]; // empty when it is nobody's
    bool used;
};
Kept kept[kAreas];
struct PreviewClaim {
    uint64_t key = 0, world = 0, scene = 0, parent = 0, tally = 0, keeper = 0;
    int32_t index = -1;
    TSUCHI_Mod* owner = nullptr;
    bool retained = false, given = false;
};
PreviewClaim preview_claims[kAreas];
PreviewClaim* preview_claim(uint64_t key, uint32_t* slot = nullptr) {
    for (uint32_t i = 0; key && i < kAreas; ++i) {
        if (preview_claims[i].key != key) continue;
        if (slot) *slot = i;
        return &preview_claims[i];
    }
    return nullptr;
}
bool kept_dirty = false;
uint32_t write_wait = 0;
constexpr uint32_t kWriteWait = 300;
Written area_others, slot_others[kAreas];
void clear_kept() {
    memset(kept, 0, sizeof kept);
    area_others.clear();
    for (Written &other : slot_others) other.clear();
    kept_dirty = false, write_wait = 0;
}
bool read_kept_from(const Json &root, const char *text) {
    const Json *areas = root.find("areas");
    if (!areas || areas->kind != Json::Kind::object) return false;
    Members top(root, text);
    const char *at = nullptr;
    while (top.next())
        if (!strcmp(top.value->name, "areas")) at = top.value_text;
    Members entries(*areas, at);
    while (entries.next()) {
        const Json &entry = *entries.value;
        int32_t area = 0;
        for (const char *c = entry.name; *c && area >= 0; ++c) area = *c >= '0' && *c <= '9' && area < 100000 ? area * 10 + (*c - '0') : -1;
        const int32_t slot = entry.name[0] ? slot_of(area) : -1;
        if (slot < 0) {
            area_others.add(", ").add(entries.text, entries.bytes);
            continue;
        }
        const Json *map = entry.kind == Json::Kind::object ? entry.find("map") : nullptr;
        const Json *used = entry.find("used");
        if (!whole_text(map) || id_form(map->text) != IdForm::added || !used || used->kind != Json::Kind::boolean || kept[slot].id[0]) return false;
        for (uint32_t i = 0; i < kAreas; ++i)
            if (!strcmp(kept[i].id, map->text)) return false;
        memcpy(kept[slot].id, map->text, strlen(map->text) + 1), kept[slot].used = used->boolean;
        Members fields(entry, entries.value_text);
        while (fields.next())
            if (strcmp(fields.value->name, "map") && strcmp(fields.value->name, "used")) slot_others[slot].add(", ").add(fields.text, fields.bytes);
        if (!slot_others[slot].whole) return false;
    }
    return area_others.whole;
}
void put_kept(Written &out) {
    out.add(", \"areas\": {");
    bool first = true;
    for (uint32_t slot = 0; slot < kAreas; ++slot) {
        if (!kept[slot].id[0]) continue;
        out.add(first ? "\"" : ", \"")
            .number(area_of(slot))
            .add("\": {\"map\": \"")
            .add(kept[slot].id)
            .add(kept[slot].used ? "\", \"used\": true" : "\", \"used\": false");
        out.add(slot_others[slot].data ? slot_others[slot].data : "", slot_others[slot].used).add("}");
        first = false;
    }
    if (first && area_others.used)
        out.add(area_others.data + 2, area_others.used - 2);
    else
        out.add(area_others.data ? area_others.data : "", area_others.used);
    out.add("}");
}
const char *const kept_keys[] = {"areas"};
Record kept_record{"new_maps", kRecord, kept_keys, 1, read_kept_from, put_kept, clear_kept};
TSUCHI_Result read_kept() {
    const TSUCHI_Result result = read_record(kept_record);
    counts.record_reads = kept_record.reads, counts.record_damaged = kept_record.damaged;
    return result;
}
TSUCHI_Result write_kept() {
    const TSUCHI_Result result = write_record(kept_record);
    kept_dirty = result != TSUCHI_OK, write_wait = kept_dirty ? kWriteWait : 0;
    ++(kept_dirty ? counts.record_failures : counts.record_writes);
    return result;
}

// Whether a map will be given to the game in this run: its mod is installed and can run, and still declares it.
bool running_map(const char* id);
bool will_run(const char *id) {
    if (running_map(id)) return true;
    char owner[kModIdBytes];
    const char *name = nullptr;
    if (!id_split(id, owner, &name)) return false;
    const ModRecord *record = find_mod(owner);
    if (!record || !record->manifest || (record->state != ModState::ready && record->state != ModState::running)) return false;
    const Json *declared = record->manifest->find("new_maps");
    return declared && declared->kind == Json::Kind::object && declared->find(name);
}

const Json *build_for(const Json &map, uint32_t slot) {
    for (const Json *build = map.find("builds")->first; build; build = build->next)
        if (slot_of(area_of_world(build->find("world")->text)) == int32_t(slot)) return build;
    return nullptr;
}

// Picks the number for one map from a plan of who has which, and notes it there. The place of the number among
// the free areas, or -1 with the reason.
int32_t pick(Kept plan[kAreas], const char *id, const Json &map, Line &why) {
    for (uint32_t slot = 0; slot < kAreas; ++slot) {
        if (strcmp(plan[slot].id, id)) continue;
        if (preview_claims[slot].key) {
            why.add("area ").number(area_of(slot)).add(" is reserved for a transient preview");
            return -1;
        }
        if (build_for(map, slot)) return int32_t(slot); // the number it has, in every run
        if (plan[slot].used) {
            why.add("the player's progress in it is kept under the game's area ").number(area_of(slot)).add(", and it has no build for that area");
            return -1;
        }
        plan[slot] = Kept{}; // it never got as far as a save, so the number is free again
    }
    const Json *builds = map.find("builds");
    // A number no map has, and failing that the number of a map that is not here and never got as far as a save.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Json *build = builds->first; build; build = build->next) {
            const uint32_t slot = uint32_t(slot_of(area_of_world(build->find("world")->text)));
            if (preview_claims[slot].key) continue;
            if (pass == 0 ? plan[slot].id[0] != 0 : plan[slot].used || will_run(plan[slot].id)) continue;
            memset(&plan[slot], 0, sizeof plan[slot]);
            memcpy(plan[slot].id, id, strlen(id) + 1);
            return int32_t(slot);
        }
    }
    why.add("the game has no free area for it: ");
    for (const Json *build = builds->first; build; build = build->next) {
        const uint32_t slot = uint32_t(slot_of(area_of_world(build->find("world")->text)));
        why.add(build == builds->first ? "area " : ", area ").number(area_of(slot));
        if (preview_claims[slot].key)
            why.add(" is reserved for a transient preview");
        else if (will_run(plan[slot].id))
            why.add(" is ").add(plan[slot].id).add("'s");
        else
            why.add(" is kept for ").add(plan[slot].id).add(", which is not running, with the player's progress there");
    }
    return -1;
}

// ---- A mod's maps, for as long as it runs -----------------------------------------------------------

struct Map {
    uint64_t key;
    TSUCHI_Mod *mod;
    char id[kIdBytes];
    const Json *entry; // in the mod's manifest
    const Json *build; // the build for the number it was given
    uint32_t slot;     // the place of that number among the game's free areas
    bool given;        // its scene is in the game's list
    bool named;        // its name is the game's text for the area
    bool failed = false;
    bool prepared = false; // pictures, types, totals and radar are ready
    uint32_t messages = 0; // completed day rows, so a retry touches only missing rows
    bool shown;            // what the list of areas and the radar show for it is in the game's tables
    uint64_t radar;        // the picture of its radar, as a handle, or 0
};
List<Map> maps_running; // in the order they were given their numbers
bool running_map(const char* id) {
    for (uint32_t i = 0; i < maps_running.count; ++i)
        if (!maps_running[i].failed && mod_running(maps_running[i].mod) && !strcmp(maps_running[i].id, id)) return true;
    return false;
}
uint64_t next_key = 1;

// ---- What the game is given -------------------------------------------------------------------------

// Whether an object is the game's keeper of scenes with its list built. The game has more objects of the class:
// the pattern new ones are made from, whose list is never built.
bool keeps_scenes(uint64_t object) {
    return object && game::live(object) && game::is_a(object, scene_keeper_class) &&
           !(game::read_or<uint32_t>(object + t::object::flags) & t::constant::template_flags) &&
           game::read_or<uint8_t>(object + t::scene_manager::ready) == t::constant::scenes_ready;
}
bool look_for_keeper(void *context, uint64_t object) {
    if (!keeps_scenes(object)) return true;
    *static_cast<uint64_t *>(context) = object;
    return false;
}
// The game's keeper of scenes once its list is built, or 0: the one the game itself goes by, and failing that
// one found among the game's objects. It is asked for as a mod starts and as a world begins, not every frame.
uint64_t scene_keeper() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    uint64_t keeper = instance ? game::read_or<uint64_t>(instance + t::game_instance::scene_manager) : 0;
    if (keeps_scenes(keeper)) return keeper;
    keeper = 0;
    game::each_object(look_for_keeper, &keeper);
    return keeper;
}

struct SceneList {
    uint64_t data = 0;
    int32_t count = 0, max = 0;
    bool sound = false;
};
SceneList scene_list(uint64_t keeper) {
    SceneList list;
    const uint64_t at = keeper + t::scene_manager::scenes;
    list.data = game::read_or<uint64_t>(at + t::array::data), list.count = game::read_or<int32_t>(at + t::array::count, -1),
    list.max = game::read_or<int32_t>(at + t::array::max, -1);
    list.sound = list.count >= 0 && list.max >= list.count && list.max <= int32_t(t::constant::max_scenes) &&
                 (!list.count || game::readable(list.data, 8ull * uint64_t(list.count)));
    return list;
}

struct SceneAdded {
    uint64_t keeper, scene;
    int32_t index;
};
List<SceneAdded> scenes_added;
enum class Given { done, waiting, refused };

// The map's scene into the game's list, which the game looks an area's scene up in by name.
Given give_scene(Map &map, Line &why) {
    const uint64_t keeper = scene_keeper();
    if (!keeper || !t::bound(t::fn::scene_array_grow)) return Given::waiting;
    const char *path = map.build->find("scene")->text, *world = map.build->find("world")->text;
    char wanted[sizeof kSceneShape];
    area_name(kSceneShape, area_of(map.slot), wanted);
    TSUCHI_Object asset{};
    const TSUCHI_Result loaded = host.asset->load(map.mod, path, &asset); // kept loaded for as long as the mod runs
    if (loaded == TSUCHI_NOT_READY) return Given::waiting;
    const uint64_t scene = loaded == TSUCHI_OK ? game::from_handle(asset.id) : 0;
    if (!scene) return why.add("its scene could not be loaded: ").add(path), Given::refused;
    // The asset is a scene of that name, for the level the build names, with what every scene of an area has.
    char level[game::kNameBytes] = "";
    const size_t length = strlen(world);
    const bool names_level = game::name_text(game::read_or<game::Name>(scene + t::scene_data::world), level, sizeof level) && strlen(level) >= length &&
                             content_path::same(level, length, world, length) && (!level[length] || level[length] == '.');
    const uint64_t parent = game::read_or<uint64_t>(scene + t::scene_data::parent), tally = game::read_or<uint64_t>(scene + t::scene_data::counts);
    if (!game::is_a(scene, scene_class) || !game::name_is(game::name_of(scene), wanted) || !names_level || !parent || !game::live(parent) ||
        !game::is_a(parent, scene_class) || !tally || !game::live(tally) || !game::is_a(tally, counts_class))
        return why.add(path).add(" is no scene named ").add(wanted).add(" for the level ").add(world), Given::refused;
    SceneList list = scene_list(keeper);
    if (!list.sound) return Given::waiting;
    counts.scene_list = uint32_t(list.count);
    for (int32_t i = 0; i < list.count; ++i) {
        const uint64_t other = game::read_or<uint64_t>(list.data + 8ull * uint64_t(i));
        if (other == scene) return ++counts.scenes_found, Given::done;
        if (other && game::live(other) && game::name_is(game::name_of(other), wanted)) {
            bool owned = false;
            for (uint32_t m = 0; m < maps_running.count; ++m)
                if (&maps_running[m] != &map && maps_running[m].slot == map.slot && maps_running[m].given && !maps_running[m].failed) owned = true;
            for (uint32_t n = 0; !owned && n < scenes_added.count; ++n) {
                SceneAdded &added = scenes_added[n];
                if (added.keeper != game::to_handle(keeper) || added.index != i || added.scene != game::to_handle(other)) continue;
                if (game::write<uint64_t>(list.data + 8ull * uint64_t(i), scene)) {
                    host.asset->load(self, path, &asset), added.scene = game::to_handle(scene);
                    return ++counts.scenes_found, Given::done;
                }
            }
            return why.add("the game already has a scene named ").add(wanted).add(", from another package"), Given::refused;
        }
    }
    // One more: the count first, then room for it from the game's own function, as the game adds one.
    const uint64_t at = keeper + t::scene_manager::scenes;
    const int32_t index = list.count;
    if (index >= int32_t(t::constant::max_scenes) || !game::write<int32_t>(at + t::array::count, index + 1)) return Given::waiting;
    if (index >= list.max) t::fn::scene_array_grow(reinterpret_cast<void *>(at));
    list = scene_list(keeper);
    if (!list.sound || list.count != index + 1 || list.max <= index || !game::write<uint64_t>(list.data + 8ull * uint64_t(index), scene)) {
        game::write<int32_t>(at + t::array::count, index);
        return why.add("the game's list of scenes could not be made longer"), Given::refused;
    }
    scenes_added.add(SceneAdded{game::to_handle(keeper), game::to_handle(scene), index});
    host.asset->load(self, path, &asset);
    counts.scene_list = uint32_t(index + 1);
    ++counts.scenes_added;
    return Given::done;
}

// The game's table of bases: loaded by its path, or the one of that name the game has loaded when the path
// cannot be asked for.
bool look_for_bases(void *context, uint64_t object) {
    if (!game::is_a(object, table_class) || !game::name_is(game::name_of(object), kBasesTableName)) return true;
    *static_cast<uint64_t *>(context) = object;
    return false;
}
TSUCHI_Object bases_handle{};
TSUCHI_Object bases_table() {
    if (bases_handle.id && game::from_handle(bases_handle.id)) return bases_handle;
    bases_handle = {};
    if (host.asset->load(self, kBasesTable, &bases_handle) == TSUCHI_OK) return bases_handle;
    uint64_t found = 0;
    game::each_object(look_for_bases, &found);
    if (found && !game::object_kept(found)) game::keep_object(found, true); // every later world has to find the row there
    bases_handle.id = found ? game::to_handle(found) : 0;
    return bases_handle;
}

// The row that leads to the area's first base. The game reads the table when a world starts and files each row
// under its level, and takes an area with no row for locked. The row and what is set in it are the framework's
// own, so they stay when the map's mod stops: a row cannot be taken out, and one left pointing at the first
// area's level would give that area a second way in.
void base_row(uint32_t area) {
    char row[sizeof kBaseRowShape], level[sizeof kLevelShape];
    area_name(kBaseRowShape, area, row), area_name(kLevelShape, area, level);
    const TSUCHI_Object table = bases_table();
    TSUCHI_Value first{};
    first.kind = TSUCHI_VALUE_INTEGER, first.as.integer = 1;
    if (!table.id) return void(++counts.base_rows_failed);
    TSUCHI_Value there{};
    const bool exists = host.datatable->get(self, table, row, "BaseCampId", &there) == TSUCHI_OK;
    char destination[128] = "";
    uint64_t needed = 0;
    if (exists && there.kind == TSUCHI_VALUE_INTEGER && there.as.integer == 1 &&
        host.datatable->get_text(self, table, row, "ToLevelName", destination, sizeof destination, &needed) == TSUCHI_OK && !strcmp(destination, level))
        return void(++counts.base_rows_found);
    if ((exists || host.datatable->add_row(self, table, row, kBaseRowToCopy) == TSUCHI_OK) &&
        host.datatable->override_text(self, table, row, "ToLevelName", level) == TSUCHI_OK &&
        host.datatable->override(self, table, row, "BaseCampId", &first) == TSUCHI_OK)
        ++counts.base_rows_made;
    else
        ++counts.base_rows_failed;
}

// ---- Transient preview admission -------------------------------------------------------------------

constexpr char kPreviewParent[] = "/Game/Carrot4/GameScene/Game/SD_Area.SD_Area";
game::ClassRef preview_world_class{"World"};

TSUCHI_Result preview_context(uint64_t& world, uint64_t& story, uint64_t& keeper) {
    world = game::loaded_world();
    int32_t rule = -1;
    if (!world || !game::live(world) || !game::game_rule(rule) || rule != int32_t(t::constant::rule_story))
        return TSUCHI_NOT_READY;
    if (game::busy_with_saves_or_scenes()) return TSUCHI_BUSY;
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    story = instance ? game::read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    keeper = scene_keeper();
    if (!story || !game::live(story) || !keeper || !scene_list(keeper).sound) return TSUCHI_NOT_READY;
    // Use the reflection decoder that backs the Lua checks, without assuming
    // offsets for either pending-transition field. Null objects decode as none.
    game::Place pending, loading;
    game::Value next, queued;
    TSUCHI_Result result = game::find_place(game::type_of(keeper), keeper, "NextGameSceneReq", pending);
    if (result == TSUCHI_NOT_FOUND) return TSUCHI_UNSUPPORTED;
    if (result != TSUCHI_OK) return result;
    if (!game::object_property(pending)) return TSUCHI_UNSUPPORTED;
    result = game::read_value(pending, next);
    if (result != TSUCHI_OK) return result;
    if (next.kind != game::ValueKind::none && next.kind != game::ValueKind::object) return TSUCHI_UNSUPPORTED;
    result = game::find_place(game::type_of(keeper), keeper, "NewGameSceneList", loading);
    if (result == TSUCHI_NOT_FOUND) return TSUCHI_UNSUPPORTED;
    if (result != TSUCHI_OK) return result;
    result = game::read_value(loading, queued);
    if (result != TSUCHI_OK) return result;
    if (queued.kind != game::ValueKind::array) return TSUCHI_UNSUPPORTED;
    return (next.kind == game::ValueKind::object && next.object) || queued.count ? TSUCHI_BUSY : TSUCHI_OK;
}

// Native calls can stop the owner or replace all save UObjects. No raw pointer
// or mutable table entry captured before a callback is used after it unchecked.
struct PreviewGuard {
    uint64_t key, world, story, keeper;
    TSUCHI_Mod* owner;
    TSUCHI_Result current() const {
        const PreviewClaim* claim = preview_claim(key);
        if (!claim || claim->owner != owner || !mod_usable(owner)) return TSUCHI_NOT_FOUND;
        uint64_t now_world = 0, now_story = 0, now_keeper = 0;
        const TSUCHI_Result result = preview_context(now_world, now_story, now_keeper);
        if (result != TSUCHI_OK) return result;
        if (game::to_handle(now_world) != world || game::to_handle(now_story) != story ||
            game::to_handle(now_keeper) != keeper) return TSUCHI_NOT_READY;
        // A traveled-away UWorld may end its native life. Scene metadata and
        // counts stay pinned, and the next scene load resolves its world path.
        if (claim->given && (!game::from_handle(claim->scene) ||
            !game::from_handle(claim->parent) || !game::from_handle(claim->tally))) return TSUCHI_NOT_READY;
        return TSUCHI_OK;
    }
};

TSUCHI_Result preview_guard(uint64_t key, TSUCHI_Mod* owner, PreviewGuard& guard) {
    uint64_t world = 0, story = 0, keeper = 0;
    const TSUCHI_Result result = preview_context(world, story, keeper);
    if (result != TSUCHI_OK) return result;
    guard = {key, game::to_handle(world), game::to_handle(story), game::to_handle(keeper), owner};
    return guard.current();
}

TSUCHI_Result preview_load(const PreviewGuard& guard, TSUCHI_Mod* holder, const char* path, uint64_t& handle) {
    TSUCHI_Object loaded{};
    const TSUCHI_Result result = host.asset->load(holder, path, &loaded);
    const TSUCHI_Result current = guard.current();
    if (current != TSUCHI_OK) return current;
    if (result != TSUCHI_OK) return result;
    if (!game::from_handle(loaded.id)) return TSUCHI_NOT_READY;
    handle = loaded.id;
    return TSUCHI_OK;
}

bool preview_path(uint64_t handle, const char* wanted) {
    char path[1024]; uint64_t needed = 0;
    const uint64_t object = game::from_handle(handle);
    return object && game::object_path(object, path, sizeof path, &needed) == TSUCHI_OK &&
        content_path::same(path, strlen(path), wanted, strlen(wanted));
}

// A strict counterpart of base_row. Existing manifest admission keeps its
// original lenient behavior; preview admission must report every failed step.
TSUCHI_Result preview_base_row(const PreviewGuard& guard, uint32_t area) {
    char row[sizeof kBaseRowShape], level[sizeof kLevelShape];
    area_name(kBaseRowShape, area, row), area_name(kLevelShape, area, level);
    const TSUCHI_Object table = bases_table();
    TSUCHI_Result result = guard.current();
    if (result != TSUCHI_OK) return result;
    if (!table.id) return TSUCHI_NOT_READY;
    TSUCHI_Value first{}, there{};
    first.kind = TSUCHI_VALUE_INTEGER, first.as.integer = 1;
    result = host.datatable->get(self, table, row, "BaseCampId", &there);
    TSUCHI_Result current = guard.current();
    if (current != TSUCHI_OK) return current;
    const bool exists = result == TSUCHI_OK;
    if (!exists && result != TSUCHI_NOT_FOUND) return result;
    if (exists && there.kind == TSUCHI_VALUE_INTEGER && there.as.integer == 1) {
        char destination[128] = ""; uint64_t needed = 0;
        result = host.datatable->get_text(self, table, row, "ToLevelName", destination, sizeof destination, &needed);
        current = guard.current();
        if (current != TSUCHI_OK) return current;
        if (result == TSUCHI_OK && !strcmp(destination, level)) return TSUCHI_OK;
        if (result != TSUCHI_OK) return result;
    }
    if (!exists) {
        result = host.datatable->add_row(self, table, row, kBaseRowToCopy);
        current = guard.current();
        if (current != TSUCHI_OK) return current;
        if (result != TSUCHI_OK) return result;
    }
    result = host.datatable->override_text(self, table, row, "ToLevelName", level);
    current = guard.current();
    if (current != TSUCHI_OK) return current;
    if (result != TSUCHI_OK) return result;
    result = host.datatable->override(self, table, row, "BaseCampId", &first);
    current = guard.current();
    return current != TSUCHI_OK ? current : result;
}

TSUCHI_Result preview_insert_scene(const PreviewGuard& guard, uint32_t area, uint64_t handle, int32_t& inserted) {
    const uint64_t keeper = game::from_handle(guard.keeper), scene = game::from_handle(handle);
    if (!keeper || !scene) return TSUCHI_NOT_READY;
    char wanted[sizeof kSceneShape]; area_name(kSceneShape, area, wanted);
    SceneList list = scene_list(keeper);
    if (!list.sound) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < list.count; ++i) {
        const uint64_t other = game::read_or<uint64_t>(list.data + 8ull * uint64_t(i));
        if (other == scene) { inserted = i; return TSUCHI_OK; }
        if (other && game::live(other) && game::name_is(game::name_of(other), wanted)) {
            const PreviewClaim* claim = preview_claim(guard.key);
            if (!claim || !claim->given || claim->keeper != guard.keeper || claim->index != i ||
                game::from_handle(claim->scene) != other) return TSUCHI_BUSY;
            // Only the exact entry this claim published may be replaced. All
            // fallible work and native calls finish before this scalar commit.
            if (!game::write<uint64_t>(list.data + 8ull * uint64_t(i), scene)) return TSUCHI_NOT_READY;
            inserted = i;
            return TSUCHI_OK;
        }
    }
    const int32_t index = list.count;
    const uint64_t at = keeper + t::scene_manager::scenes;
    if (!t::bound(t::fn::scene_array_grow)) return TSUCHI_UNSUPPORTED;
    if (index >= int32_t(t::constant::max_scenes)) return TSUCHI_CAPACITY;
    if (!game::write<int32_t>(at + t::array::count, index + 1)) return TSUCHI_NOT_READY;
    if (index >= list.max) t::fn::scene_array_grow(reinterpret_cast<void*>(at));
    // Roll back our unfinished append even when the callback stopped its owner.
    // A callback's later independent entries must not be truncated.
    const auto rollback = [&] {
        if (!game::from_handle(guard.keeper)) return;
        const SceneList now = scene_list(keeper);
        if (now.sound && now.count == index + 1) game::write<int32_t>(at + t::array::count, index);
    };
    const TSUCHI_Result current = guard.current();
    if (current != TSUCHI_OK) { rollback(); return current; }
    list = scene_list(keeper);
    if (!list.sound || list.count != index + 1 || list.max <= index ||
        !game::write<uint64_t>(list.data + 8ull * uint64_t(index), scene)) {
        rollback(); return TSUCHI_NOT_READY;
    }
    inserted = index;
    return TSUCHI_OK;
}

// Called only by explicit preparation after the editor restored its baseline.
// No framework allocation record, menu, or save file is changed here.
TSUCHI_Result preview_story(const PreviewGuard& guard, uint32_t area) {
    if (!t::bound(t::fn::story_find_area) || !t::bound(t::fn::story_add_area) || !t::bound(t::fn::story_link_area)) return TSUCHI_UNSUPPORTED;
    char level[sizeof kLevelShape]; area_name(kLevelShape, area, level);
    game::Name name{};
    TSUCHI_Result result = game::name_from_utf8(level, name);
    TSUCHI_Result current = guard.current();
    if (current != TSUCHI_OK) return current;
    if (result != TSUCHI_OK) return result;
    void* story = reinterpret_cast<void*>(game::from_handle(guard.story));
    void* record = t::fn::story_find_area(story, &name);
    current = guard.current();
    if (current != TSUCHI_OK) return current;
    if (!record) {
        t::fn::story_add_area(story, &name, int32_t(t::constant::story_area_bases)); // a build that takes no count ignores it
        current = guard.current();
        if (current != TSUCHI_OK) return current;
        const bool linked = t::fn::story_link_area(story, &name, 0);
        current = guard.current();
        if (current != TSUCHI_OK) return current;
        if (!linked) return TSUCHI_NOT_READY;
        record = t::fn::story_find_area(story, &name);
        current = guard.current();
        if (current != TSUCHI_OK) return current;
        if (!record) return TSUCHI_NOT_READY;
    }
    const uint64_t bases = game::from_handle(guard.story) + t::story_save::area_bases;
    uint64_t data = 0; int32_t count = 0;
    if (!game::read(bases + t::array::data, data) || !game::read(bases + t::array::count, count) ||
        !data || count <= int32_t(area)) return TSUCHI_NOT_READY;
    const uint64_t state = data + uint64_t(area) * t::area_bases::size + t::area_bases::first_state;
    uint8_t value = 0;
    if (!game::read(state, value)) return TSUCHI_NOT_READY;
    return value != 0 || game::write(state, uint8_t(t::constant::base_open))
        ? TSUCHI_OK : TSUCHI_NOT_READY;
}

// The area's name, as the list of areas, the loading screen and the arrival read it. It is the mod's text: it goes
// back when the mod stops, and stacks with another mod's change to the same text.
void give_name(Map &map) {
    if (map.named) return;
    char key[sizeof kBaseRowShape];
    area_name(kBaseRowShape, area_of(map.slot), key);
    map.named = host.text->override(map.mod, kNameSpace, key, map.entry->find("name")->text) == TSUCHI_OK;
    ++(map.named ? counts.names_set : counts.names_failed);
}

// ---- What the list of areas and the radar show ------------------------------------------------------
//
// Beside an area the game's list shows pictures, the types of Pikmin found there and a message, and in the area
// the radar draws a picture of it. Each comes from a row of one of the game's tables. The rows for a free area
// are the framework's own: made once as copies of the first area's, emptied, and never taken out, like the row
// in the table of bases. What a map puts in them is its mod's, through the calls a mod's own code would make, so
// it goes back when the mod stops, and the rows are empty again for whichever map has the number next.

constexpr char kAreaShape[] = "Area000", kMessageRowShape[] = "Tsuchi_Area000_Day0";
// The first area's row in the tables of pictures, of types and of the radar: a new area's row starts as a copy.
constexpr char kAreaRowToCopy[] = "Area001";
constexpr uint32_t kMessages = sizeof t::row::area_messages / sizeof t::row::area_messages[0];
static_assert(kMessages <= 10, "a message's row is told apart by one digit");
// What a row of the table of types holds for its place in the list when it is for none. The game files every row
// under its place as a menu is set up, and no list is this long.
constexpr int64_t kNoPlace = 255;
// What a radar is given where the manifest says nothing.
constexpr double kRadarSize = 12000, kRadarAngle = -90, kRadarScale = 3.75;

enum UiTable : uint32_t { kPictures, kTypes, kTotals, kRadar, kDayMessages, kUiTables };
constexpr const char *kUiTablePaths[kUiTables] = {t::asset::area_pictures_table, t::asset::area_types_table, t::asset::area_totals_table, t::asset::radar_table,
                                                  t::asset::area_message_table};
TSUCHI_Object ui_tables[kUiTables];

TSUCHI_Value whole_value(int64_t whole) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_INTEGER, value.as.integer = whole;
    return value;
}
TSUCHI_Value decimal_value(double number) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_NUMBER, value.as.number = number;
    return value;
}
TSUCHI_Value object_value(TSUCHI_Object object) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_OBJECT, value.as.object = object;
    return value;
}

// One of those tables, loaded and kept loaded for the framework, so that a row made in it is there in every
// later world. Nothing while the game cannot give it.
TSUCHI_Object ui_table(UiTable which) {
    TSUCHI_Object &table = ui_tables[which];
    if (table.id && game::from_handle(table.id)) return table;
    table = {};
    if (host.asset->load(self, kUiTablePaths[which], &table) != TSUCHI_OK) table = {};
    return table;
}

// A row of the framework's own: a copy of another row of the table, made once. False when it is not there and
// cannot be made.
bool own_row(TSUCHI_Object table, const char *row, const char *from, const char *field) {
    TSUCHI_Value there{};
    if (host.datatable->get(self, table, row, field, &there) == TSUCHI_OK) return true;
    const bool made = host.datatable->add_row(self, table, row, from) == TSUCHI_OK;
    ++(made ? counts.ui_rows_made : counts.ui_rows_failed);
    return made;
}

// The two rows every area in the list has, emptied: no picture, no type of Pikmin, for no place in the list, and
// the three numbers the game reads for an area's nights at nothing. The game takes the pictures by a row's place
// among the rows it finds, so an area with no row would show the next area's.
bool area_rows(const char *area) {
    const TSUCHI_Object pictures = ui_table(kPictures), types = ui_table(kTypes);
    const TSUCHI_Value none = whole_value(kNoPlace), zero = whole_value(0);
    bool right = own_row(pictures, area, kAreaRowToCopy, "ImageTextureList") && table_resize(self, pictures, area, "ImageTextureList", 0) == TSUCHI_OK &&
                 own_row(types, area, kAreaRowToCopy, "AreaType") && host.datatable->override(self, types, area, "AreaType", &none) == TSUCHI_OK &&
                 table_resize(self, types, area, "TargetPikminColor", 0) == TSUCHI_OK && table_resize(self, types, area, "DifficultList", 3) == TSUCHI_OK;
    for (char k = '0'; k < '3' && right; ++k) {
        char field[] = "DifficultList[0]";
        field[sizeof field - 3] = k;
        right = host.datatable->override(self, types, area, field, &zero) == TSUCHI_OK;
    }
    return right;
}

// The messages the list shows under an area by day. Each row of the game's says which areas it is for, one bit
// for each area's number. A free area has a row of its own for each kind of message the game shows for all of
// its own areas: a copy that is for no area, until a map says it is for its. How many rows the map was given.
bool message_rows(Map &map) {
    const TSUCHI_Object table = ui_table(kDayMessages);
    const uint32_t area = area_of(map.slot);
    const TSUCHI_Value none = whole_value(0), mine = whole_value(int64_t(1) << area);
    char row[sizeof kMessageRowShape];
    area_name(kMessageRowShape, area, row);
    bool complete = true;
    for (uint32_t k = 0; k < kMessages; ++k) {
        if (map.messages & (uint32_t(1) << k)) continue;
        row[sizeof row - 2] = char('0' + k);
        if (!own_row(table, row, t::row::area_messages[k], "AreaBitFlags") || host.datatable->override(self, table, row, "AreaBitFlags", &none) != TSUCHI_OK ||
            host.datatable->override(map.mod, table, row, "AreaBitFlags", &mine) != TSUCHI_OK) {
            complete = false;
            continue;
        }
        map.messages |= uint32_t(1) << k;
        ++counts.messages_set;
    }
    return complete;
}

// A picture of the mod's own, loaded and kept loaded for the mod.
Given load_picture(Map &map, const char *path, const char *what, TSUCHI_Object &out, Line &why) {
    const TSUCHI_Result loaded = host.asset->load(map.mod, path, &out);
    if (loaded == TSUCHI_NOT_READY) return Given::waiting;
    if (loaded != TSUCHI_OK) return why.add("its ").add(what).add(" could not be loaded: ").add(path), Given::refused;
    return Given::done;
}

// Whether every type of Pikmin a map lists is one the list of areas shows: a type the framework has a name for,
// among those the game's list has an icon for. The game takes each as a number and looks things up by it.
bool types_known(const Json &entry, const char *&unknown) {
    const Json *pikmin = entry.find("pikmin");
    for (const Json *kind = pikmin ? pikmin->first : nullptr; kind; kind = kind->next) {
        uint32_t type = 0;
        if (id_value(pikmin::PikminType_vocabulary, kind->text, &type) != TSUCHI_OK || !id_named(pikmin::PikminType_vocabulary, type) ||
            type >= t::constant::listed_pikmin_types)
            return unknown = kind->text, false;
    }
    return true;
}

// Everything the list and the radar show for one map. A refusal says why.
Given prepare_show(Map &map, Line &why) {
    if (map.prepared) return Given::done;
    const Json &entry = *map.entry;
    const Json *preview = entry.find("preview"), *radar = entry.find("radar"), *pikmin = entry.find("pikmin"), *totals = entry.find("totals");
    for (uint32_t which = 0; which < kUiTables; ++which)
        if ((which != kTotals || totals) && (which != kRadar || radar) && !ui_table(UiTable(which)).id) return ++counts.ui_waits, Given::waiting;
    const TSUCHI_Object pictures = ui_table(kPictures), types = ui_table(kTypes);
    const uint32_t area = area_of(map.slot);
    char name[sizeof kAreaShape], level[sizeof kLevelShape];
    area_name(kAreaShape, area, name), area_name(kLevelShape, area, level);
    if (!area_rows(name)) return why.add("the game's tables of what the list of areas shows would not take rows for area ").number(area), Given::refused;

    if (preview) {
        TSUCHI_Object picture{};
        const Given loaded = load_picture(map, preview->text, "preview", picture, why);
        if (loaded != Given::done) return loaded;
        const TSUCHI_Value value = object_value(picture);
        TSUCHI_Result taken = table_resize(map.mod, pictures, name, "ImageTextureList", 1);
        if (taken == TSUCHI_OK) taken = host.datatable->override(map.mod, pictures, name, "ImageTextureList[0]", &value);
        if (taken != TSUCHI_OK)
            return why.add(taken == TSUCHI_INVALID ? "its preview is no picture the list of areas can show: " : "the game would not take its preview: ")
                       .add(preview->text),
                   Given::refused;
        ++counts.previews_set;
    }

    if (pikmin && pikmin->count) {
        TSUCHI_Result taken = table_resize(map.mod, types, name, "TargetPikminColor", pikmin->count);
        uint32_t index = 0;
        for (const Json *kind = pikmin->first; kind && taken == TSUCHI_OK; kind = kind->next, ++index) {
            uint32_t type = 0;
            Line field;
            field.add("TargetPikminColor[").number(index).add("]");
            taken = id_value(pikmin::PikminType_vocabulary, kind->text, &type);
            const TSUCHI_Value value = whole_value(type);
            if (taken == TSUCHI_OK) taken = host.datatable->override(map.mod, types, name, field.text, &value);
        }
        if (taken != TSUCHI_OK) return why.add("the game would not take its types of Pikmin"), Given::refused;
        ++counts.pikmin_set;
    }

    if (totals) {
        // The game has a row here for every number below eleven, in the order of the numbers, and reads the rows
        // by their place: one is never added.
        static const char *const kinds[][2] = {{"treasures", "GroundOtakara"}, {"castaways", "SURVIVOR"}, {"flarlic", "Wasurena"}};
        const TSUCHI_Object table = ui_table(kTotals);
        TSUCHI_Result taken = host.datatable->override_text(self, table, level, "AreaName", level);
        if (taken == TSUCHI_NOT_FOUND) return why.add("the game's table of areas has no row for area ").number(area), Given::refused;
        for (const auto &kind : kinds) {
            const Json *total = totals->find(kind[0]);
            const TSUCHI_Value value = whole_value(total ? total->integer : 0);
            if (taken == TSUCHI_OK) taken = host.datatable->override(map.mod, table, level, kind[1], &value);
        }
        if (taken != TSUCHI_OK) return why.add("the game would not take its totals"), Given::refused;
        ++counts.totals_set;
    }

    if (radar) {
        // The game finds a scene's row by the name in it, not by the row's own. The framework's row names no scene,
        // and a map's radar puts the scene's name in last, when everything else is there.
        const TSUCHI_Object table = ui_table(kRadar);
        if (!own_row(table, name, kAreaRowToCopy, "CaptureSize") || host.datatable->override_text(self, table, name, "SceneName", "") != TSUCHI_OK)
            return why.add("the game's table of radar pictures would not take a row for area ").number(area), Given::refused;
        TSUCHI_Object picture{};
        const char *path = radar->find("texture")->text;
        const Given loaded = load_picture(map, path, "radar's picture", picture, why);
        if (loaded != Given::done) return loaded;
        const Json *size = radar->find("size"), *offset = radar->find("offset"), *angle = radar->find("angle"), *scale = radar->find("scale"),
                   *limits = radar->find("limits");
        const double width = size ? size->number : kRadarSize, half = width / 2;
        const struct {
            const char *field;
            double value;
        } values[] = {{"PositionOffset.X", offset ? offset->at(0)->number : 0},
                      {"PositionOffset.Y", offset ? offset->at(1)->number : 0},
                      {"DefaultAngle", angle ? angle->number : kRadarAngle},
                      {"BaseScale", scale ? scale->number : kRadarScale},
                      {"CaptureSize", width},
                      {"CursorMoveLimit.Left", limits ? limits->at(0)->number : -half},
                      {"CursorMoveLimit.Top", limits ? limits->at(1)->number : half},
                      {"CursorMoveLimit.Right", limits ? limits->at(2)->number : half},
                      {"CursorMoveLimit.Bottom", limits ? limits->at(3)->number : -half}};
        const TSUCHI_Value drawn = object_value(picture);
        TSUCHI_Result taken = host.datatable->override(map.mod, table, name, "Texture", &drawn);
        if (taken == TSUCHI_INVALID) return why.add("its radar's picture is no picture the radar can draw: ").add(path), Given::refused;
        for (const auto &each : values) {
            const TSUCHI_Value value = decimal_value(each.value);
            if (taken == TSUCHI_OK) taken = host.datatable->override(map.mod, table, name, each.field, &value);
        }
        if (taken == TSUCHI_OK) taken = host.datatable->override_text(map.mod, table, name, "SceneName", name);
        if (taken != TSUCHI_OK) return why.add("the game would not take its radar"), Given::refused;
        map.radar = picture.id;
        ++counts.radars_set;
    }

    map.prepared = true;
    return Given::done;
}

Given show(Map &map, Line &why) {
    if (map.shown) return Given::done;
    const Given prepared = prepare_show(map, why);
    if (prepared != Given::done) return prepared;
    if (!message_rows(map)) return ++counts.ui_waits, Given::waiting;
    map.shown = true;
    return Given::done;
}

// Tells the row of an area in the table of types which place in the list it is for. The game looks the types up
// by the place of the row the player is on.
bool place_row(uint32_t area, int64_t place) {
    const TSUCHI_Object types = ui_tables[kTypes];
    if (!types.id || !game::from_handle(types.id)) return false;
    char name[sizeof kAreaShape];
    area_name(kAreaShape, area, name);
    const TSUCHI_Value value = whole_value(place);
    return host.datatable->override(self, types, name, "AreaType", &value) == TSUCHI_OK;
}

// Everything the game is given for one map. A refusal says why.
Given give(Map &map, Line &why) {
    if (!map.given) {
        const Given scene = give_scene(map, why);
        if (scene != Given::done) return scene;
        map.given = true;
    }
    base_row(area_of(map.slot));
    give_name(map);
    return show(map, why);
}

uint32_t waiting_now() {
    uint32_t waiting = 0;
    for (uint32_t i = 0; i < maps_running.count; ++i) waiting += !maps_running[i].given && !maps_running[i].failed;
    return waiting;
}

// A world has begun: what was not ready as a map's mod started is tried again, and each area's row is looked for
// again, since the game may have loaded its table anew.
void at_world() {
    for (uint32_t i = 0; i < maps_running.count; ++i) {
        Line why;
        Map &map = maps_running[i];
        if (map.failed) continue;
        if (give(map, why) == Given::refused) {
            map.failed = true, map.given = false;
            if (!kept[map.slot].used) kept[map.slot] = Kept{}, kept_dirty = true;
            tell(map.mod, "new_maps: ", map.entry->name, ": ", why.text);
        }
    }
}

// ---- The menu of areas ------------------------------------------------------------------------------
//
// The list of areas is one of the game's own menus. While one that can show the new areas is open, the game's
// list of area numbers is its own followed by the maps', so the game's code makes the rows, moves the cursor and
// starts the journey. The list is the game's own again when the menu ends.

void (*setup_before)(void *) = nullptr;
void (*destruct_before)(void *) = nullptr;

struct Lent {
    bool active = false;
    uint64_t menu = 0, panel = 0; // the menu the list was changed for, and its panel when that is drawn smaller
    uint64_t data = 0;            // the game's own list
    int32_t count = 0, max = 0;
} lent;
List<int32_t> shown;     // the numbers the menu lists: the game's own, then the maps'
List<uint32_t> shown_of; // for each number after the game's own, its map's place in `maps_running`

uint64_t areas_at() { return game::main_base() + t::data::explore_areas; }

void give_list_back() {
    if (!lent.active) return;
    const uint64_t at = areas_at();
    game::write(at + t::array::data, lent.data), game::write(at + t::array::count, lent.count), game::write(at + t::array::max, lent.max);
    lent = Lent{};
    for (uint32_t slot = 0; slot < kAreas; ++slot) place_row(area_of(slot), kNoPlace); // no list has the new areas' rows now
}

// The menu reads two lists of its own by a row's place without looking at their length: each gets an entry for
// every row, a copy of its last. The memory is the engine's, so the menu frees it as it would its own.
bool grow_zooms(uint64_t menu, uint32_t rows) {
    static const char *const lists[] = {"WorldMapZooms", "AreaIconZooms"};
    game::Place places[2];
    int32_t bytes[2] = {};
    for (uint32_t k = 0; k < 2; ++k) {
        if (game::find_place(game::type_of(menu), menu, lists[k], places[k]) != TSUCHI_OK) return false;
        const uint64_t inner = game::read_or<uint64_t>(places[k].property + t::property::inner);
        bytes[k] = inner ? game::read_or<int32_t>(inner + t::property::element_bytes) : 0;
        const int32_t have = game::read_or<int32_t>(places[k].address + t::array::count);
        if (bytes[k] <= 0 || have < 1 || have > 64 ||
            !game::readable(game::read_or<uint64_t>(places[k].address + t::array::data), uint64_t(have) * uint64_t(bytes[k])))
            return false;
    }
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t have = uint32_t(game::read_or<int32_t>(places[k].address + t::array::count));
        if (have >= rows) continue; // something else gave it the entries
        if (game::resize(places[k], rows) != TSUCHI_OK) return false;
        const uint64_t data = game::read_or<uint64_t>(places[k].address + t::array::data);
        if (!game::writable(data, uint64_t(rows) * uint64_t(bytes[k]))) return false;
        for (uint32_t row = have; row < rows; ++row)
            memcpy(reinterpret_cast<void *>(data + uint64_t(row) * uint64_t(bytes[k])),
                   reinterpret_cast<const void *>(data + uint64_t(have - 1) * uint64_t(bytes[k])), size_t(bytes[k]));
        ++counts.zooms_grown;
    }
    return true;
}

// What a row needs of the story: a record of the area, tied to its scene as the game ties its own after loading,
// and the area's first base open, which is the game's rule for listing an area.
bool ready_story(uint64_t story, uint32_t slot) {
    if (!story || !t::bound(t::fn::story_find_area) || !t::bound(t::fn::story_add_area) || !t::bound(t::fn::story_link_area)) return false;
    if (!kept[slot].used) {
        const bool was_dirty = kept_dirty;
        kept[slot].used = true;
        if (write_kept() != TSUCHI_OK) {
            kept[slot].used = false;
            kept_dirty = was_dirty;
            return false;
        }
    }
    const uint32_t area = area_of(slot);
    char level[sizeof kLevelShape];
    area_name(kLevelShape, area, level);
    game::Name key{};
    if (game::name_from_utf8(level, key) != TSUCHI_OK) return false;
    if (!t::fn::story_find_area(reinterpret_cast<void *>(story), &key)) {
        t::fn::story_add_area(reinterpret_cast<void *>(story), &key, int32_t(t::constant::story_area_bases));
        t::fn::story_link_area(reinterpret_cast<void *>(story), &key, 0);
        ++counts.area_records_made;
    }
    const uint64_t bases = story + t::story_save::area_bases;
    const uint64_t data = game::read_or<uint64_t>(bases + t::array::data);
    const uint64_t state = data + uint64_t(area) * t::area_bases::size + t::area_bases::first_state;
    if (data && int32_t(area) < game::read_or<int32_t>(bases + t::array::count) && game::read_or<uint8_t>(state, 1) == 0 &&
        game::write(state, uint8_t(t::constant::base_open)))
        ++counts.bases_opened;
    // From here on a save may hold the map's progress under this number.
    return true;
}

// Before the game builds the rows. True when the menu lists new areas.
bool list_maps(uint64_t menu) {
    give_list_back(); // a menu that never ended
    const uint64_t context = game::game_context(), story = game::read_or<uint64_t>(menu + t::explore_menu::story);
    if (!maps_running.count || !context || !story || !t::bound(t::fn::story_flags) || !t::bound(t::fn::story_find_area) || !t::bound(t::fn::story_add_area) || !t::bound(t::fn::story_link_area))
        return false;
    // Night expeditions and Olimar's Shipwreck Tale have lists of their own.
    if ((t::fn::story_flags(reinterpret_cast<void *>(context)) & t::constant::story_of_olimar) || game::read_or<uint8_t>(menu + t::explore_menu::hero) ||
        game::read_or<uint8_t>(menu + t::explore_menu::night))
        return false;
    const uint64_t at = areas_at();
    const uint64_t data = game::read_or<uint64_t>(at + t::array::data);
    const int32_t own = game::read_or<int32_t>(at + t::array::count), max = game::read_or<int32_t>(at + t::array::max);
    if (own < 1 || own > 64 || !game::readable(data, 4ull * uint64_t(own))) return false;
    shown.count = shown_of.count = 0;
    for (int32_t i = 0; i < own; ++i)
        if (!shown.add(game::read_or<int32_t>(data + 4ull * uint64_t(i)))) return false;
    for (uint32_t i = 0; i < maps_running.count; ++i) {
        const int32_t area = int32_t(area_of(maps_running[i].slot));
        bool there = !maps_running[i].given || maps_running[i].failed || (maps_running[i].prepared && !maps_running[i].shown);
        for (int32_t j = 0; j < own && !there; ++j) there = shown[uint32_t(j)] == area; // something else lists it
        if (there || !ready_story(story, maps_running[i].slot)) continue;
        if (!(shown.add(area) && shown_of.add(i))) return false;
    }
    if (!shown_of.count) return false;
    if (!grow_zooms(menu, shown.count)) return ++counts.zooms_failed, false;
    // The game must be able to resolve each new row before it sees the borrowed list.
    for (uint32_t i = 0; i < shown_of.count; ++i) {
        if (place_row(area_of(maps_running[shown_of[i]].slot), int64_t(own) + int64_t(i))) continue;
        for (uint32_t j = 0; j < i; ++j) place_row(area_of(maps_running[shown_of[j]].slot), kNoPlace);
        return false;
    }
    lent.active = true, lent.menu = menu, lent.data = data, lent.count = own, lent.max = max;
    if (!game::write(at + t::array::data, reinterpret_cast<uint64_t>(shown.items)) || !game::write(at + t::array::count, int32_t(shown.count)) ||
        !game::write(at + t::array::max, int32_t(shown.count))) {
        give_list_back();
        return false;
    }
    counts.places_set += shown_of.count;
    return true;
}
// A text of the engine's from UTF-8, for the game to copy. Give its counter to `drop_shared` afterward.
bool make_text(const char *text, game::EngineText &out) {
    size_t count = 0;
    if (!t::bound(t::fn::text_from_string) || !game::utf8_to_units(text, nullptr, 0, count)) return false;
    auto *units = static_cast<char16_t *>(allocate((count + 1) * 2));
    if (!units) return false;
    game::utf8_to_units(text, units, count, count);
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size] = {};
    const uint64_t data = reinterpret_cast<uint64_t>(units);
    const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8), memcpy(string + t::array::count, &length, 4), memcpy(string + t::array::max, &length, 4);
    out = reinterpret_cast<game::EngineText (*)(const void *)>(reinterpret_cast<uint64_t>(t::fn::text_from_string))(string);
    release(units);
    return true;
}

// The panel is laid out for the game's own rows and does not scroll: it is drawn smaller from its top edge by as
// much as the rows need to fit, and at its own size again when the menu ends.
void draw_panel(uint64_t panel, double scale) {
    if (!panel || !game::live(panel) || !game::is_a(panel, widget_class)) return;
    {
        game::EngineCall pivot(panel, "SetRenderTransformPivot");
        if (pivot.ready() != TSUCHI_OK || pivot.set("Pivot.X", game::number_value(0.5)) != TSUCHI_OK ||
            pivot.set("Pivot.Y", game::number_value(0)) != TSUCHI_OK || pivot.invoke() != TSUCHI_OK)
            return;
    }
    game::EngineCall size(panel, "SetRenderScale");
    if (size.ready() != TSUCHI_OK || size.set("Scale.X", game::number_value(scale)) != TSUCHI_OK || size.set("Scale.Y", game::number_value(scale)) != TSUCHI_OK)
        return;
    if (size.invoke() == TSUCHI_OK && scale != 1) lent.panel = panel, ++counts.panels_fitted;
}

// After the game has built the rows: the new rows get their maps' names.
void name_rows(uint64_t menu) {
    const uint64_t panel = game::read_or<uint64_t>(menu + t::explore_menu::panel);
    if (!panel) return;
    const uint32_t own = uint32_t(lent.count);
    for (uint32_t i = 0; i < shown_of.count && t::bound(t::fn::explore_row) && t::fn::explore_row_label; ++i) {
        void *row = t::fn::explore_row(reinterpret_cast<void *>(panel), int32_t(own + i));
        game::EngineText name;
        if (!row || !make_text(maps_running[shown_of[i]].entry->find("name")->text, name)) continue;
        t::fn::explore_row_label(row, &name);
        game::drop_shared(name.counter());
        ++counts.rows_named;
    }
    draw_panel(panel, double(own) / double(shown.count));
}

// ---- The small map beside the list, and the counts --------------------------------------------------
//
// Beside the list the menu draws a map of the whole world in a frame, with a mark for each of the game's areas,
// and three counts for the area the player is on. While the player is on the row of a map that has a radar, the
// frame draws that radar's picture and the game's own map with its marks is hidden. Both are as the game had
// them again when the player moves to another row, when the menu ends, and when the map's mod stops. Everything
// is done through the widgets' own functions, so the engine redraws what changed.

game::ClassRef image_class{"Image"};
game::ClassRef texture_class{"Texture2D"};
game::ClassRef rich_text_class{"CarrotRichTextBlock"};
// The game's names for the menu's parts.
constexpr char kFrame[] = "IM_Base_MapFrame", kWorldMap[] = "IM_WorldMap", kMapGroup[] = "N_Cvs_Map_Content";

struct Inset {
    uint64_t menu = 0;             // the menu the picture is shown in, or 0 for none
    uint64_t frame = 0, group = 0; // as handles: the frame that draws it, and the game's own map with its marks
    uint64_t picture = 0;          // as a handle: the picture
    uint64_t before = 0;           // as a handle: what the frame drew before, or 0 for nothing
    int64_t group_shown = 0;       // how the game's own map was shown before
    bool kept = false;             // the engine keeps `before` for the framework's sake
} inset;
int32_t watched_row = -1; // the row the player was on when the menu was last looked at
uint32_t watch_wait = 0;  // frames until it is looked at again all the same

// The object a field of an object refers to, or 0.
uint64_t object_in(uint64_t owner, const char *path) {
    game::Place place;
    game::Value value;
    if (!owner || game::find_place(game::type_of(owner), owner, path, place) != TSUCHI_OK || game::read_value(place, value) != TSUCHI_OK ||
        value.kind != game::ValueKind::object)
        return 0;
    return game::from_handle(value.object);
}

// How a widget is shown, as the engine numbers the ways, or -1.
int64_t shown_how(uint64_t widget) {
    game::Place place;
    game::Value value;
    return game::find_place(game::type_of(widget), widget, "Visibility", place) == TSUCHI_OK && game::read_value(place, value) == TSUCHI_OK &&
                   value.kind == game::ValueKind::integer
               ? value.integer
               : -1;
}

bool call_with(uint64_t object, const char *function, const char *argument, const game::Value &value) {
    game::EngineCall call(object, function);
    return call.ready() == TSUCHI_OK && call.set(argument, value) == TSUCHI_OK && call.invoke() == TSUCHI_OK;
}

// Whether hiding the group would leave the widget showing: the group is none of the panels the widget is in.
bool apart(uint64_t widget, uint64_t group) {
    for (int depth = 0; depth < 16; ++depth) {
        widget = object_in(object_in(widget, "Slot"), "Parent");
        if (!widget) return true;
        if (widget == group) return false;
    }
    return false;
}

// Gives the frame and the game's own map back as the game had them. Only what is still as the framework left it
// is changed: the game may have set either since.
void inset_back() {
    if (!inset.menu) return;
    const uint64_t frame = game::from_handle(inset.frame), group = game::from_handle(inset.group), picture = game::from_handle(inset.picture),
                   before = game::from_handle(inset.before);
    if (frame && (!picture || object_in(frame, "Brush.ResourceObject") == picture))
        call_with(frame, "SetBrushResourceObject", "ResourceObject", before ? game::object_value(inset.before) : game::Value{});
    if (group && shown_how(group) == int64_t(t::constant::widget_hidden))
        call_with(group, "SetVisibility", "InVisibility", game::integer_value(inset.group_shown));
    if (inset.kept && before) game::keep_object(before, false);
    inset = Inset{};
    ++counts.insets_put_back;
}

void show_inset(uint64_t menu, uint64_t picture_handle) {
    const uint64_t picture = game::from_handle(picture_handle), frame = object_in(menu, kFrame), world_map = object_in(menu, kWorldMap);
    const uint64_t group = object_in(object_in(world_map, "Slot"), "Parent");
    const bool usable = picture && game::is_a(picture, texture_class) && frame && game::is_a(frame, image_class) && world_map &&
                        game::is_a(world_map, image_class) && group && game::is_a(group, widget_class) && game::name_is(game::name_of(group), kMapGroup) &&
                        apart(frame, group);
    // A frame the game has hidden stays hidden: the game shows none where the player has not been yet.
    const int64_t frame_shown = usable ? shown_how(frame) : -1;
    if (frame_shown < 0 || frame_shown == int64_t(t::constant::widget_collapsed) || frame_shown == int64_t(t::constant::widget_hidden)) return inset_back();
    if (inset.menu == menu && inset.frame == game::to_handle(frame) && inset.group == game::to_handle(group) && inset.picture == picture_handle)
        return; // it is showing
    inset_back();
    const uint64_t before = object_in(frame, "Brush.ResourceObject");
    const int64_t group_shown = shown_how(group);
    if (group_shown < 0) return;
    inset.menu = menu, inset.frame = game::to_handle(frame), inset.group = game::to_handle(group), inset.picture = picture_handle;
    inset.before = before ? game::to_handle(before) : 0, inset.group_shown = group_shown;
    // What the frame drew is the frame's alone to keep, and has to be there to go back.
    if (before && before != picture && !game::object_kept(before)) inset.kept = game::keep_object(before, true);
    if (!call_with(frame, "SetBrushResourceObject", "ResourceObject", game::object_value(picture_handle)) ||
        !call_with(group, "SetVisibility", "InVisibility", game::integer_value(int64_t(t::constant::widget_hidden))))
        return inset_back();
    ++counts.insets_shown;
}

// Fill only an unknown display total. The native numerator and bracketed marks are kept;
// the manifest does not replace known totals or change the game's progress/placement records.
bool count_with_total(const char *text, uint32_t total, char *out, size_t capacity) {
    const size_t length = strlen(text);
    if (length < 5 || length >= capacity || memcmp(text + length - 3, "???", 3)) return false;
    uint32_t found = 0;
    size_t digits = 0, questions = 0;
    bool mark = false, divided = false;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (mark) {
            if (c == '[') return false;
            mark = c != ']';
        } else if (c == '[') {
            mark = true;
        } else if (!divided && c >= '0' && c <= '9') {
            if (found > kMostTotal / 10) return false;
            found = found * 10 + uint32_t(c - '0');
            if (found > kMostTotal) return false;
            ++digits;
        } else if (!divided && c == '/' && digits) {
            divided = true;
        } else if (divided && c == '?' && questions < 3) {
            ++questions;
        } else {
            return false;
        }
    }
    if (mark || !divided || questions != 3 || found > total) return false;
    Line denominator;
    denominator.number(total);
    const size_t bytes = strlen(denominator.text), prefix = length - 3;
    if (bytes >= capacity - prefix) return false;
    memcpy(out, text, prefix);
    memcpy(out + prefix, denominator.text, bytes + 1);
    return true;
}

// The three display counts for a map whose manifest declares totals.
void write_counts(uint64_t menu, const Map &map) {
    static const char *const kinds[][2] = {{"castaways", "F_Info_Rate_000"}, {"treasures", "F_Info_Rate_001"}, {"flarlic", "F_Info_Rate_002"}};
    const Json *totals = map.entry->find("totals");
    if (!totals || !t::bound(t::fn::rich_text_set)) return;
    for (const auto &kind : kinds) {
        const Json *total = totals->find(kind[0]);
        const uint64_t block = object_in(menu, kind[1]);
        game::Place place;
        static char text[1025], known[1025];
        size_t needed = 0;
        if (!block || !game::is_a(block, rich_text_class) || game::find_place(game::type_of(block), block, "Text", place) != TSUCHI_OK ||
            game::read_engine_text(place, text, sizeof text, &needed) != TSUCHI_OK ||
            !count_with_total(text, uint32_t(total ? total->integer : 0), known, sizeof known))
            continue;
        game::EngineText made;
        if (!make_text(known, made)) continue;
        t::fn::rich_text_set(reinterpret_cast<void *>(block), &made);
        game::drop_shared(made.counter());
        ++counts.counts_written;
    }
}

// The open menu, looked at when the player moves to another row and every few frames besides.
void watch_menu() {
    if (!lent.active) return;
    const uint64_t menu = lent.menu;
    if (!game::live(menu)) return inset_back(); // a menu that went without its end being seen
    const int32_t row = game::read_or<int32_t>(menu + t::explore_menu::selected, -1);
    if (row == watched_row && watch_wait) return void(--watch_wait);
    watched_row = row, watch_wait = 8;
    const int32_t place = row - lent.count;
    const Map *map = place >= 0 && uint32_t(place) < shown_of.count ? &maps_running[shown_of[uint32_t(place)]] : nullptr;
    if (map && map->radar && game::from_handle(map->radar))
        show_inset(menu, map->radar);
    else
        inset_back();
    if (map) write_counts(menu, *map);
}

void on_setup(void *menu) {
    ++counts.menus_opened;
    inset_back(); // a menu that never ended
    watched_row = -1, watch_wait = 0;
    const bool listed = list_maps(reinterpret_cast<uint64_t>(menu));
    setup_before(menu);
    if (!listed) return;
    ++counts.menus_listed;
    name_rows(reinterpret_cast<uint64_t>(menu));
    if (kept_dirty) write_wait = 0; // the game is not saving while its menu opens: the record is written at the next frame
}

void on_destruct(void *menu) {
    if (lent.active && lent.menu == reinterpret_cast<uint64_t>(menu)) {
        inset_back();
        if (lent.panel) draw_panel(lent.panel, 1);
        give_list_back();
    }
    destruct_before(menu);
}

void hook_menu() {
    const uint64_t main = game::main_base();
    uint64_t id = 0;
    const t::Slot &setup = t::slot::explore_setup;
    const t::Slot &destruct = t::slot::explore_destruct;
    // The end first: a menu whose end is not seen would leave the game with the framework's list.
    if (!destruct_before && hook_wrap(self, main + destruct.table + destruct.offset, main + destruct.stock, reinterpret_cast<void *>(&on_destruct), 0, &id,
                                      reinterpret_cast<void **>(&destruct_before)) != TSUCHI_OK)
        return;
    counts.menu_hooks = 1;
    if (hook_wrap(self, main + setup.table + setup.offset, main + setup.stock, reinterpret_cast<void *>(&on_setup), 0, &id,
                  reinterpret_cast<void **>(&setup_before)) == TSUCHI_OK)
        counts.menu_hooks = 2;
}

// ---- As a mod starts and stops ----------------------------------------------------------------------

// The mod stopped: its map's ID names nothing and the menu no longer lists it. What the game was given stays
// where the game may hold on to it; the number stays the map's in the record.
void end_map(void *, uint64_t key) {
    for (uint32_t i = 0; i < maps_running.count; ++i) {
        if (maps_running[i].key != key) continue;
        for (uint32_t j = i; j + 1 < maps_running.count; ++j) maps_running[j] = maps_running[j + 1]; // the others keep their order
        --maps_running.count;
        // A menu that is open keeps its rows until it ends; it is told nothing more of this map.
        shown_of.count = 0;
        inset_back();
        return;
    }
}

TSUCHI_Result refuse(Text &problem, TSUCHI_Result result, const char *map, const char *why) {
    ++counts.refused;
    join(problem, "\"new_maps\": ", map, *map ? ": " : "", why);
    return result;
}

TSUCHI_Result begin(TSUCHI_Mod *mod, const Json &section, Text &problem) {
    for (const Json* entry = section.first; entry; entry = entry->next) {
        char id[kIdBytes];
        if (!id_join(mod_id(mod), entry->name, id)) return TSUCHI_INVALID;
        if (running_map(id)) return refuse(problem, TSUCHI_BUSY, entry->name, "the map is already declared");
    }
    counts.declared += section.count;
    // The record is read as the first map of a run is given its number. While maps run, and while a change waits
    // to be written, what is in memory is the record.
    if (read_kept() != TSUCHI_OK)
        return refuse(problem, TSUCHI_NOT_READY, "",
                      kept_record.state == Record::State::damaged ? "the record of which map has which area is damaged"
                                                                  : "which map has which of the game's free areas could not be read from the save");
    // Every map of the mod gets its number, or none does.
    Kept plan[kAreas];
    memcpy(plan, kept, sizeof plan);
    const uint32_t first = maps_running.count;
    for (const Json *entry = section.first; entry; entry = entry->next) {
        Map map{};
        map.key = next_key, map.mod = mod, map.entry = entry;
        Line why;
        const char *unknown = nullptr;
        if (!id_join(mod_id(mod), entry->name, map.id)) return refuse(problem, TSUCHI_INVALID, entry->name, "its name is not spelled as a mod's ID is");
        if (!types_known(*entry, unknown))
            return refuse(problem, TSUCHI_NOT_FOUND, entry->name,
                          why.add("\"pikmin\": ").add(unknown).add(" is no type of Pikmin the list of areas shows").text);
        const int32_t slot = pick(plan, map.id, *entry, why);
        if (slot < 0) return refuse(problem, TSUCHI_CAPACITY, entry->name, why.text);
        map.slot = uint32_t(slot), map.build = build_for(*entry, map.slot);
        Map *running = maps_running.add(map);
        const TSUCHI_Result owned = running ? own(mod, kMap, map.key, end_map, nullptr) : TSUCHI_NO_MEMORY;
        if (owned != TSUCHI_OK) {
            if (running) --maps_running.count;
            return owned;
        }
        ++next_key;
    }
    // Then the game is given each. A scene the game will not take blocks the mod; a game that is not ready for
    // one yet is tried again when a world begins.
    for (uint32_t i = first; i < maps_running.count; ++i) {
        Line why;
        if (give(maps_running[i], why) == Given::refused) return refuse(problem, TSUCHI_INVALID, maps_running[i].entry->name, why.text);
    }
    if (memcmp(plan, kept, sizeof plan)) {
        Kept before[kAreas];
        memcpy(before, kept, sizeof before);
        memcpy(kept, plan, sizeof plan);
        if (write_kept() != TSUCHI_OK) {
            memcpy(kept, before, sizeof kept), kept_dirty = false;
            return refuse(problem, TSUCHI_IO, "", "the numbers given to its maps could not be kept in the save");
        }
    }
    if (!setup_before) hook_menu(); // with the first map there is
    return TSUCHI_OK;
}

uint64_t world_seen = 0;
uint32_t messages_wait = 0;

void step() {
    if (kept_dirty && kept_record.state == Record::State::read) {
        if (write_wait)
            --write_wait;
        else
            write_kept();
    }
    watch_menu();
    const uint64_t world = game::loaded_world();
    const uint64_t handle = world ? game::to_handle(world) : 0;
    if (handle == world_seen) {
        if (messages_wait)
            --messages_wait;
        else {
            messages_wait = 30;
            for (uint32_t i = 0; i < maps_running.count; ++i) {
                Map &map = maps_running[i];
                if (map.prepared && !map.shown && !map.failed) {
                    Line why;
                    show(map, why);
                }
            }
        }
        return;
    }
    world_seen = handle;
    if (handle && maps_running.count) at_world();
}

} // namespace

void new_maps_start(TSUCHI_Mod *own_context) {
    self = own_context;
    add_manifest_section({"new_maps", well_formed, begin});
    each_frame(step);
}

TSUCHI_Result new_map_level(const char *id, char level[10]) {
    for (uint32_t i = 0; i < maps_running.count; ++i) {
        if (strcmp(maps_running[i].id, id)) continue;
        if (maps_running[i].failed) return TSUCHI_NOT_FOUND;
        if (!maps_running[i].given) return TSUCHI_NOT_READY;
        area_name(kLevelShape, area_of(maps_running[i].slot), level);
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result prepare_new_map(const char *id, uint64_t story) {
    for (uint32_t i = 0; i < maps_running.count; ++i) {
        const Map &map = maps_running[i];
        if (strcmp(map.id, id)) continue;
        if (map.failed) return TSUCHI_NOT_FOUND;
        return map.given && ready_story(story, map.slot) ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    return TSUCHI_NOT_FOUND;
}

bool new_map_id(const char *level, char id[kIdBytes]) {
    const int32_t slot = slot_of(area_in(kLevelShape, level, strlen(level)));
    for (uint32_t i = 0; slot >= 0 && i < maps_running.count; ++i) {
        if (maps_running[i].slot != uint32_t(slot) || !maps_running[i].given) continue;
        memcpy(id, maps_running[i].id, kIdBytes);
        return true;
    }
    return false;
}

TSUCHI_Result reserve_preview_area(uint64_t key, uint32_t* area) {
    if (!area) return TSUCHI_INVALID;
    *area = 0;
    if (!key || !on_game_thread()) return TSUCHI_INVALID;
    uint64_t world = 0, story = 0, keeper = 0;
    const TSUCHI_Result context = preview_context(world, story, keeper);
    if (context != TSUCHI_OK) return context;
    uint32_t slot = 0;
    if (preview_claim(key, &slot)) { *area = area_of(slot); return TSUCHI_OK; }
    const uint64_t world_handle = game::to_handle(world), story_handle = game::to_handle(story), keeper_handle = game::to_handle(keeper);
    const TSUCHI_Result read = read_kept();
    if (read != TSUCHI_OK) return read;
    uint64_t now_world = 0, now_story = 0, now_keeper = 0;
    const TSUCHI_Result current = preview_context(now_world, now_story, now_keeper);
    if (current != TSUCHI_OK) return current;
    if (!world_handle || !story_handle || !keeper_handle || game::to_handle(now_world) != world_handle ||
        game::to_handle(now_story) != story_handle || game::to_handle(now_keeper) != keeper_handle) return TSUCHI_NOT_READY;
    const SceneList list = scene_list(keeper);
    if (!list.sound) return TSUCHI_NOT_READY;
    for (slot = 0; slot < kAreas; ++slot) {
        if (kept[slot].id[0] || preview_claims[slot].key) continue;
        bool occupied = false;
        for (uint32_t i = 0; i < maps_running.count; ++i) occupied |= maps_running[i].slot == slot;
        char wanted[sizeof kSceneShape]; area_name(kSceneShape, area_of(slot), wanted);
        for (int32_t i = 0; !occupied && i < list.count; ++i) {
            const uint64_t scene = game::read_or<uint64_t>(list.data + 8ull * uint64_t(i));
            occupied = scene && game::live(scene) && game::name_is(game::name_of(scene), wanted);
        }
        if (occupied) continue;
        preview_claims[slot] = PreviewClaim{};
        preview_claims[slot].key = key;
        *area = area_of(slot);
        return TSUCHI_OK;
    }
    return TSUCHI_CAPACITY;
}

void release_preview_area(uint64_t key) {
    if (!on_game_thread()) return;
    PreviewClaim* claim = preview_claim(key);
    if (claim && !claim->retained) *claim = PreviewClaim{};
}

TSUCHI_Result give_preview_area(TSUCHI_Mod* owner, uint64_t key, TSUCHI_AssetBundle bundle,
                               const char* world_package, const char* scene_object, bool* retained) {
    if (!retained) return TSUCHI_INVALID;
    *retained = false;
    if (!on_game_thread()) return TSUCHI_INVALID;
    uint32_t slot = 0;
    PreviewClaim* claim = preview_claim(key, &slot);
    if (!claim) return TSUCHI_NOT_FOUND;
    *retained = claim->retained;
    if (!mod_usable(owner) || !mod_usable(self)) return TSUCHI_NOT_READY;
    if (claim->owner && claim->owner != owner) return TSUCHI_DENIED;
    if (!world_package || !scene_object || !bundle.id ||
        content_path::wrong(world_package, strlen(world_package), content_path::Names::package) ||
        content_path::wrong(scene_object, strlen(scene_object), content_path::Names::object) ||
        area_of_world(world_package) != int32_t(area_of(slot)) ||
        area_of_scene(scene_object) != int32_t(area_of(slot))) return TSUCHI_INVALID;
    claim->owner = owner;
    PreviewGuard guard{};
    TSUCHI_Result result = preview_guard(key, owner, guard);
    if (result != TSUCHI_OK) return result;
    char level[sizeof kLevelShape]; area_name(kLevelShape, area_of(slot), level);
    game::Name current_level{};
    if (!game::world_name(current_level)) return TSUCHI_NOT_READY;
    if (game::name_is(current_level, level)) return TSUCHI_BUSY;

    char scene_package[1024], world_object[1024];
    if (strlen(scene_object) >= sizeof scene_package ||
        strlen(world_package) + strlen(level) + 2 > sizeof world_object) return TSUCHI_INVALID;
    memcpy(scene_package, scene_object, strlen(scene_object) + 1);
    char* dot = const_cast<char*>(last_of(scene_package, '.'));
    if (!dot) return TSUCHI_INVALID;
    *dot = 0;
    memcpy(world_object, world_package, strlen(world_package));
    world_object[strlen(world_package)] = '.';
    memcpy(world_object + strlen(world_package) + 1, level, strlen(level) + 1);
    result = asset_bundle_package(owner, bundle, world_package);
    if (result == TSUCHI_OK) result = asset_bundle_package(owner, bundle, scene_package);
    if (result != TSUCHI_OK) return result;
    result = guard.current();
    if (result != TSUCHI_OK) return result;

    uint64_t world_handle = 0, scene_handle = 0, parent_handle = 0;
    result = preview_load(guard, owner, world_object, world_handle);
    if (result == TSUCHI_OK) result = preview_load(guard, owner, scene_object, scene_handle);
    if (result == TSUCHI_OK) result = preview_load(guard, owner, kPreviewParent, parent_handle);
    if (result != TSUCHI_OK) return result;
    const uint64_t world = game::from_handle(world_handle), scene = game::from_handle(scene_handle);
    const uint64_t parent = game::from_handle(parent_handle);
    if (!world || !scene || !parent || !game::is_a(world, preview_world_class) ||
        !game::is_a(scene, scene_class) || !game::is_a(parent, scene_class) ||
        !preview_path(world_handle, world_object) || !preview_path(scene_handle, scene_object) ||
        !preview_path(parent_handle, kPreviewParent)) return TSUCHI_INVALID;
    char wanted[sizeof kSceneShape]; area_name(kSceneShape, area_of(slot), wanted);
    char opened[game::kNameBytes];
    const uint64_t tally = game::read_or<uint64_t>(scene + t::scene_data::counts);
    if (!game::name_is(game::name_of(scene), wanted) ||
        game::read_or<uint64_t>(scene + t::scene_data::parent) != parent ||
        !game::name_text(game::read_or<game::Name>(scene + t::scene_data::world), opened, sizeof opened) ||
        (!content_path::same(opened, strlen(opened), world_package, strlen(world_package)) &&
         !content_path::same(opened, strlen(opened), world_object, strlen(world_object))) ||
        !tally || !game::live(tally) || !game::is_a(tally, counts_class)) return TSUCHI_INVALID;
    const uint64_t tally_handle = game::to_handle(tally);
    char tally_object[1024], tally_package[1024]; uint64_t needed = 0;
    if (game::object_path(tally, tally_object, sizeof tally_object, &needed) != TSUCHI_OK ||
        content_path::wrong(tally_object, strlen(tally_object), content_path::Names::object)) return TSUCHI_INVALID;
    memcpy(tally_package, tally_object, strlen(tally_object) + 1);
    dot = const_cast<char*>(last_of(tally_package, '.'));
    if (!dot) return TSUCHI_INVALID;
    *dot = 0;
    result = asset_bundle_package(owner, bundle, tally_package);
    if (result != TSUCHI_OK) return result;
    result = guard.current();
    if (result != TSUCHI_OK) return result;

    claim = preview_claim(key);
    // From this point cleanup cannot prove that native references disappeared.
    // Preserve the claim before the first framework-owned pin or native write.
    claim->retained = true, *retained = true;
    const char* const paths[] = {world_object, scene_object, kPreviewParent, tally_object};
    const uint64_t handles[] = {world_handle, scene_handle, parent_handle, tally_handle};
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t pinned = 0;
        result = preview_load(guard, self, paths[i], pinned);
        if (result != TSUCHI_OK) return result;
        if (pinned != handles[i]) return TSUCHI_NOT_READY;
    }
    result = preview_base_row(guard, area_of(slot));
    int32_t inserted = -1;
    if (result == TSUCHI_OK) result = preview_insert_scene(guard, area_of(slot), scene_handle, inserted);
    if (result != TSUCHI_OK) return result;
    claim = preview_claim(key);
    if (!claim) return TSUCHI_NOT_FOUND;
    claim->world = world_handle, claim->scene = scene_handle, claim->parent = parent_handle, claim->tally = tally_handle;
    claim->keeper = guard.keeper, claim->index = inserted;
    claim->given = true;
    return TSUCHI_OK;
}

TSUCHI_Result prepare_preview_area(uint64_t key) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    uint32_t slot = 0;
    const PreviewClaim* claim = preview_claim(key, &slot);
    if (!claim || !claim->retained || !claim->given) return TSUCHI_NOT_FOUND;
    PreviewGuard guard{};
    TSUCHI_Result result = preview_guard(key, claim->owner, guard);
    if (result != TSUCHI_OK) return result;
    const uint64_t keeper = game::from_handle(guard.keeper), scene = game::from_handle(claim->scene);
    const SceneList list = scene_list(keeper);
    bool found = false;
    for (int32_t i = 0; list.sound && i < list.count; ++i)
        found |= game::read_or<uint64_t>(list.data + 8ull * uint64_t(i)) == scene;
    if (!found) return TSUCHI_NOT_READY;
    result = preview_base_row(guard, area_of(slot));
    return result == TSUCHI_OK ? preview_story(guard, area_of(slot)) : result;
}

uint32_t new_map_count() { return maps_running.count; }

NewMapCounts new_map_counts() {
    NewMapCounts all = counts;
    all.scenes_waiting = waiting_now();
    return all;
}

} // namespace tsuchi
