// What a mod's manifest adds to the game, under "content": new treasures and their series, and new creatures.
// DESIGN.md, "New treasures" and "New creatures", has the rules.
//
//   "content": {
//     "treasure_series": {"gems": {"name": "Gem Collection"}},
//     "treasures": {"ruby": {"class": "/Game/Mods/<id>/GOtaRuby.GOtaRuby_C", "from": "OTAECLAIR", "sparklium": 120,
//                            "text": {"name": "Ruby"}, "catalog": {"series": "gems"}}},
//     "creatures": {"stonehog": {"class": "/Game/Mods/<id>/GStonehog.GStonehog_C", "from": "KOCHAPPY", "life": 450,
//                                "text": {"name": "Stonehog"}, "catalog": {}}}
//   }
//
// A treasure or a creature is made as its mod starts, before any code of the mod's, through the calls the mod's own
// code would make: the tables, the class and the icon are loaded for the mod, its rows are added as copies of
// another's, and their fields and its texts are changed in the mod's name, so they are put back when it stops.
// Two things are done past those calls. The name the framework gave the thing is written into the default
// object of its class, so every actor of the class carries it. And its rows are entered in the lists of rows by
// name that the loaded world made of the tables, where the game looks rows up: again whenever the world changes.
#include <string.h>

#include "actors.h"
#include "content.h"
#include "content_series.h"
#include "content_identity.h"
#include "content_sound.h"
#include "content_squad.h"
#include "content_placements.h"
#include "pikmin_content.h"
#include "content_path.h"
#include "records.h"
#include "frame.h"
#include "game.h"
#include "host.h"
#include "ids.h"
#include "json.h"
#include "list.h"
#include "mods.h"
#include "objects.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"
#include "save_session.h"
#include "services.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// ---- The names the game knows mods' things by --------------------------------------------------------------

const Kind kName{"content.name"};
struct Named {
    uint64_t key;
    char id[kIdBytes];
    uint64_t name; // as the game keeps it
};
List<Named> named;
uint64_t next_key = 1;

uint64_t packed(game::Name name) {
    uint64_t value = 0;
    memcpy(&value, &name, sizeof name);
    return value;
}

// The mod stopped: its thing's ID names nothing.
void forget_name(void *, uint64_t key) {
    for (uint32_t i = named.count; i-- > 0;) {
        if (named[i].key != key) continue;
        named.remove(&named[i]);
        if (!named.count) named.clear();
        return;
    }
}

// ---- What is wrong, in words for the mod's author ------------------------------------------------------------

bool say(Text &out, const char *a, const char *b = "", const char *c = "", const char *d = "", const char *e = "", const char *f = "", const char *g = "",
         const char *h = "", const char *i = "", const char *j = "", const char *k = "", const char *l = "") {
    const char *const parts[] = {a, b, c, d, e, f, g, h, i, j, k, l};
    size_t bytes = 1;
    for (const char *part : parts) bytes += strlen(part);
    if (!out.reserve(bytes)) return false;
    char *at = out.data;
    for (const char *part : parts) {
        const size_t length = strlen(part);
        memcpy(at, part, length);
        at += length;
    }
    *at = 0;
    return true;
}

// Why the game did not take something.
const char *why(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_NOT_FOUND:
        return "the game has none of that name";
    case TSUCHI_INVALID:
        return "the value is of the wrong kind for it";
    case TSUCHI_UNSUPPORTED:
        return "it is of a kind that cannot be changed";
    case TSUCHI_NOT_READY:
        return "the game cannot be asked yet";
    case TSUCHI_BUSY:
        return "the game is busy with its save";
    case TSUCHI_NO_MEMORY:
        return "there is no memory for it";
    default:
        return "the game refused it";
    }
}

// ---- How "content" is written --------------------------------------------------------------------------------

bool whole_text(const Json *value) { return value && value->kind == Json::Kind::text && strlen(value->text) == value->bytes; }
bool some_text(const Json *value) { return whole_text(value) && value->bytes; }
bool path_text(const Json *value) { return some_text(value) && !content_path::wrong(value->text, value->bytes, content_path::Names::either); }
bool whole_number(const Json *value, int64_t least) {
    return value && value->kind == Json::Kind::number && value->whole && value->integer >= least && value->integer <= INT32_MAX;
}
// A number above 0, whole or not, that a creature's life can be.
bool amount(const Json *value) { return value && value->kind == Json::Kind::number && value->number > 0 && value->number <= double(INT32_MAX); }
bool only_keys(const Json &object, const char *const *known, size_t count, const char *&stranger) {
    for (const Json *member = object.first; member; member = member->next) {
        bool found = false;
        for (size_t i = 0; i < count && !found; ++i) found = !strcmp(member->name, known[i]);
        if (!found) return stranger = member->name, false;
    }
    return true;
}
// Fields of a row, each by its path with a plain value.
bool plain_values(const Json *values) {
    if (!values || values->kind != Json::Kind::object) return false;
    for (const Json *field = values->first; field; field = field->next) {
        const bool plain = field->kind == Json::Kind::boolean || field->kind == Json::Kind::number || whole_text(field);
        if (!field->name[0] || !plain) return false;
    }
    return true;
}
// The fewest and the most Pikmin that carry a thing: one number for both, or two.
bool carriers_of(const Json *value, int32_t range[2]) {
    if (whole_number(value, 1)) return range[0] = range[1] = int32_t(value->integer), true;
    if (!value || value->kind != Json::Kind::list || value->count != 2) return false;
    const Json *least = value->first;
    if (!whole_number(least, 1) || !whole_number(least->next, least->integer)) return false;
    return range[0] = int32_t(least->integer), range[1] = int32_t(least->next->integer), true;
}
// The path of a Blueprint's class: `/Folder/Thing.Thing_C`.
bool class_path(const Json *value) {
    if (!path_text(value)) return false;
    content_path::Parts parts{};
    return !content_path::wrong(value->text, value->bytes, content_path::Names::object) && content_path::split(value->text, parts) && parts.object_bytes > 2 &&
           !strcmp(parts.object + parts.object_bytes - 2, "_C");
}

const char *const kContentKeys[] = {"treasures", "treasure_series", "creatures", "pikmin", "squad", "sound_banks"};
const char *const kTreasureKeys[] = {"class", "from", "text", "sparklium", "carriers", "battle_carriers", "icon", "values", "catalog", "placement_values"};
const char *const kTextKeys[] = {"name", "catalog_name", "result_name", "description", "olimar_notes", "louie_notes"};
const char *const kCatalogKeys[] = {"series", "values"};
const char *const kSeriesKeys[] = {"name", "bonus", "from", "icon"};
const char *const kCreatureKeys[] = {"class",           "night_class", "from", "text",   "life",  "sparklium", "carriers",
                                     "battle_carriers", "pikmin",      "icon", "values", "night", "catalog", "placement_values"};
const char *const kCreatureTextKeys[] = {"name", "scientific_name", "family", "description", "olimar_notes", "louie_notes"};
const char *const kNightKeys[] = {"life", "sparklium", "carriers", "battle_carriers", "pikmin", "values"};
const char *const kCreatureCatalogKeys[] = {"values"};

// What is wrong with one treasure as it is written, or null. `stranger` gets a key that is not known.
const char *treasure_wrong(const Json &treasure, const Json *series, const char *&stranger) {
    if (treasure.kind != Json::Kind::object)
        return "it is an object: its \"class\", the treasure it starts \"from\", its \"text\", and what else it has of its own";
    if (!only_keys(treasure, kTreasureKeys, sizeof kTreasureKeys / sizeof *kTreasureKeys, stranger)) return "it has a key the framework does not know: ";
    const char *const path_fields[] = {"class", "night_class", "icon"};
    for (const char *field : path_fields) {
        const Json *path = treasure.find(field);
        if (whole_text(path) && path->text[0] == '/') {
            if (const char *what = content_path::wrong(path->text, path->bytes, content_path::Names::either)) return stranger = path->text, what;
        }
    }
    if (!class_path(treasure.find("class"))) return "\"class\" is the path of its Blueprint class, such as /Game/Mods/example_gems/GOtaRuby.GOtaRuby_C";
    const Json *from = treasure.find("from");
    const IdForm form = whole_text(from) ? id_form(from->text) : IdForm::malformed;
    if (form != IdForm::plain && form != IdForm::added) return "\"from\" is the ID of the treasure its rows start as copies of, such as OTAECLAIR";
    const Json *text = treasure.find("text");
    if (!text || text->kind != Json::Kind::object || !some_text(text->find("name"))) return "\"text\" is an object with its \"name\", the name people see";
    if (!only_keys(*text, kTextKeys, sizeof kTextKeys / sizeof *kTextKeys, stranger)) return "\"text\" has a key the framework does not know: ";
    for (const Json *each = text->first; each; each = each->next)
        if (!whole_text(each)) return "\"text\": each of its texts is text";
    int32_t range[2];
    if (treasure.find("sparklium") && !whole_number(treasure.find("sparklium"), 0)) return "\"sparklium\" is what it is worth, a whole number from 0";
    const char *const carried[] = {"carriers", "battle_carriers"};
    for (const char *key : carried)
        if (treasure.find(key) && !carriers_of(treasure.find(key), range))
            return "\"carriers\" and \"battle_carriers\" are the fewest and the most Pikmin that carry it: two whole numbers from 1, or one for both";
    if (treasure.find("icon") && !path_text(treasure.find("icon"))) return "\"icon\" is the path of a texture, such as /Game/Mods/example_gems/T_Ruby.T_Ruby";
    if (treasure.find("values") && !plain_values(treasure.find("values")))
        return "\"values\" is an object: fields of its row by their paths, each with a number, true or false, or text";
    if (const Json *catalog = treasure.find("catalog")) {
        if (catalog->kind != Json::Kind::object || !some_text(catalog->find("series")))
            return "\"catalog\" is an object with the \"series\" it is in, by the series' short name";
        if (!only_keys(*catalog, kCatalogKeys, sizeof kCatalogKeys / sizeof *kCatalogKeys, stranger))
            return "\"catalog\" has a key the framework does not know: ";
        if (!series || !series->find(catalog->find("series")->text))
            return stranger = catalog->find("series")->text, "\"catalog\" names a series that \"treasure_series\" does not have: ";
        if (catalog->find("values") && !plain_values(catalog->find("values")))
            return "\"catalog\": \"values\" is an object: fields of its row by their paths, each with a number, true or false, or text";
    }
    return nullptr;
}

// What is wrong with what a creature has by day, or with what it has other at night; or null.
const char *numbers_wrong(const Json &stated) {
    int32_t range[2];
    if (stated.find("life") && !amount(stated.find("life"))) return "\"life\" is how much life it has, a number above 0";
    if (stated.find("sparklium") && !whole_number(stated.find("sparklium"), 0)) return "\"sparklium\" is what its body is worth, a whole number from 0";
    if (stated.find("pikmin") && !whole_number(stated.find("pikmin"), 0))
        return "\"pikmin\" is how many Pikmin an Onion makes of its body, a whole number from 0";
    const char *const carried[] = {"carriers", "battle_carriers"};
    for (const char *key : carried)
        if (stated.find(key) && !carriers_of(stated.find(key), range))
            return "\"carriers\" and \"battle_carriers\" are the fewest and the most Pikmin that carry its body: two whole numbers from 1, or one for both";
    if (stated.find("values") && !plain_values(stated.find("values")))
        return "\"values\" is an object: fields of its row by their paths, each with a number, true or false, or text";
    return nullptr;
}

// What is wrong with one creature as it is written, or null. `within` gets the part it is wrong in, and
// `stranger` a key that is not known.
const char *creature_wrong(const Json &creature, const char *&within, const char *&stranger) {
    if (creature.kind != Json::Kind::object)
        return "it is an object: its \"class\", the creature it starts \"from\", its \"text\", and what else it has of its own";
    if (!only_keys(creature, kCreatureKeys, sizeof kCreatureKeys / sizeof *kCreatureKeys, stranger)) return "it has a key the framework does not know: ";
    const char *const path_fields[] = {"class", "night_class", "icon"};
    for (const char *field : path_fields) {
        const Json *path = creature.find(field);
        if (whole_text(path) && path->text[0] == '/') {
            if (const char *what = content_path::wrong(path->text, path->bytes, content_path::Names::either)) return stranger = path->text, what;
        }
    }
    if (!class_path(creature.find("class"))) return "\"class\" is the path of its Blueprint class, such as /Game/Mods/example_zoo/GStonehog.GStonehog_C";
    if (const Json *night_class = creature.find("night_class"))
        if (!class_path(night_class) || !strcmp(night_class->text, creature.find("class")->text))
            return "\"night_class\" is the path of another Blueprint class, which the Piklopedia shows at night, such as "
                   "/Game/Mods/example_zoo/GStonehogNight.GStonehogNight_C";
    const Json *from = creature.find("from");
    const IdForm form = whole_text(from) ? id_form(from->text) : IdForm::malformed;
    if (form != IdForm::plain && form != IdForm::added) return "\"from\" is the ID of the creature its rows start as copies of, such as KOCHAPPY";
    const Json *text = creature.find("text");
    if (!text || text->kind != Json::Kind::object || !some_text(text->find("name"))) return "\"text\" is an object with its \"name\", the name people see";
    if (!only_keys(*text, kCreatureTextKeys, sizeof kCreatureTextKeys / sizeof *kCreatureTextKeys, stranger))
        return "\"text\" has a key the framework does not know: ";
    for (const Json *each = text->first; each; each = each->next)
        if (!whole_text(each)) return "\"text\": each of its texts is text";
    if (const char *what = numbers_wrong(creature)) return what;
    if (creature.find("icon") && !path_text(creature.find("icon")))
        return "\"icon\" is the path of a texture, such as /Game/Mods/example_zoo/T_Stonehog.T_Stonehog";
    if (const Json *night = creature.find("night")) {
        within = "\"night\": ";
        if (night->kind != Json::Kind::object) return "it is an object: what the creature has other at night than by day";
        if (!only_keys(*night, kNightKeys, sizeof kNightKeys / sizeof *kNightKeys, stranger)) return "it has a key the framework does not know: ";
        if (const char *what = numbers_wrong(*night)) return what;
        within = "";
    }
    if (const Json *catalog = creature.find("catalog")) {
        if (catalog->kind != Json::Kind::object)
            return "\"catalog\" is an object, which may be empty: that the Piklopedia lists it, and \"values\" of its row there";
        if (!only_keys(*catalog, kCreatureCatalogKeys, sizeof kCreatureCatalogKeys / sizeof *kCreatureCatalogKeys, stranger))
            return "\"catalog\" has a key the framework does not know: ";
        if (catalog->find("values") && !plain_values(catalog->find("values")))
            return "\"catalog\": \"values\" is an object: fields of its row by their paths, each with a number, true or false, or text";
    }
    return nullptr;
}

bool banks_valid(const Json& content, Text& wrong) {
    const Json* banks = content.find("sound_banks");
    if (!banks) return true;
    if (banks->kind != Json::Kind::object) return say(wrong, "content: sound_banks is an object"), false;
    for (const Json* bank = banks->first; bank; bank = bank->next) {
        if (!valid_mod_id(bank->name) || !some_text(bank) || !game::path_stays_within(bank->text))
            return say(wrong, "content: a sound bank has a short name and a file inside the mod"), false;
    }
    return true;
}
TSUCHI_Result banks_begin(TSUCHI_Mod* mod, const Json& content, Text&) {
    const Json* banks = content.find("sound_banks");
    for (const Json* entry = banks ? banks->first : nullptr; entry; entry = entry->next) {
        char id[kIdBytes];
        if (!id_join(mod_id(mod), entry->name, id)) return TSUCHI_INVALID;
        const auto result=content_sound_declare_file(mod,id,entry->text,host.file);
        if (result != TSUCHI_OK) return result;
    }
    return TSUCHI_OK;
}

bool well_formed(const Json &content, Text &wrong) {
    const char *stranger = "";
    if (content.kind != Json::Kind::object)
        return say(wrong, "\"content\" is an object: \"treasures\" and \"creatures\", each by a short name, and the treasures' \"treasure_series\""), false;
    if (!only_keys(content, kContentKeys, sizeof kContentKeys / sizeof *kContentKeys, stranger))
        return say(wrong, "\"content\" has a key the framework does not know: ", stranger), false;
    const Json *series = content.find("treasure_series"), *treasures = content.find("treasures"), *creatures = content.find("creatures");
    if (series && series->kind != Json::Kind::object)
        return say(wrong, "\"content\": \"treasure_series\" is an object: a short name for each series, and what it is"), false;
    for (const Json *each = series ? series->first : nullptr; each; each = each->next) {
        if (!valid_mod_id(each->name)) return say(wrong, "\"content\": series ", each->name, ": its short name is spelled as a mod's ID is"), false;
        if (each->kind != Json::Kind::object || !some_text(each->find("name")))
            return say(wrong, "\"content\": series ", each->name, ": it is an object with its \"name\", the name people see"), false;
        if (!only_keys(*each, kSeriesKeys, sizeof kSeriesKeys / sizeof *kSeriesKeys, stranger))
            return say(wrong, "\"content\": series ", each->name, ": it has a key the framework does not know: ", stranger), false;
        const Json* bonus = each->find("bonus"), *from = each->find("from"), *icon = each->find("icon");
        if (bonus && (bonus->kind != Json::Kind::number || bonus->number < 0 || bonus->number > 1))
            return say(wrong, "content: series bonus is a fraction from 0 to 1"), false;
        if (from && (!some_text(from) || id_form(from->text) != IdForm::plain))
            return say(wrong, "content: series from is a stock series row name"), false;
        if (icon && !path_text(icon)) return say(wrong, "content: series icon is a texture path"), false;
    }
    if (treasures && treasures->kind != Json::Kind::object)
        return say(wrong, "\"content\": \"treasures\" is an object: a short name for each treasure, and what it is"), false;
    for (const Json *each = treasures ? treasures->first : nullptr; each; each = each->next) {
        if (!valid_mod_id(each->name)) return say(wrong, "\"content\": treasure ", each->name, ": its short name is spelled as a mod's ID is"), false;
        stranger = "";
        if (const char *what = treasure_wrong(*each, series, stranger)) {
            const bool path = what == content_path::kNoPath;
            return say(wrong, "\"content\": treasure ", each->name, ": ", path ? stranger : what, path ? what : stranger), false;
        }
    }
    if (creatures && creatures->kind != Json::Kind::object)
        return say(wrong, "\"content\": \"creatures\" is an object: a short name for each creature, and what it is"), false;
    for (const Json *each = creatures ? creatures->first : nullptr; each; each = each->next) {
        if (!valid_mod_id(each->name)) return say(wrong, "\"content\": creature ", each->name, ": its short name is spelled as a mod's ID is"), false;
        // An ID names one thing, and a treasure of the same short name would have the same ID.
        if (treasures && treasures->kind == Json::Kind::object && treasures->find(each->name))
            return say(wrong, "\"content\": creature ", each->name, ": a treasure of the manifest has that short name, and the two would have one ID"), false;
        const char *within = "";
        stranger = "";
        if (const char *what = creature_wrong(*each, within, stranger)) {
            const bool path = what == content_path::kNoPath;
            return say(wrong, "\"content\": creature ", each->name, ": ", within, path ? stranger : what, path ? what : stranger), false;
        }
    }
    return pikmin_content_valid(content, wrong) && content_squad_valid(content, wrong) &&
           content_placements_valid(content, wrong) && banks_valid(content, wrong);
}

// ---- The numbers the framework has given out -----------------------------------------------------------------

constexpr uint32_t kFirstTreasure = 1, kFirstSeries = 1000, kFirstCreature = 1, kMostNumber = INT32_MAX;
constexpr char kNamePrefix[] = "OTATSUCHI";      // a treasure of the game's own is `OTA` and a word
constexpr char kCreaturePrefix[] = "TEKITSUCHI"; // the game's tables call a creature `Teki`
constexpr char kNightSuffix[] = "_NIGHT";        // the game looks for a creature's row at night under its name and this
constexpr size_t kGameNameBytes = sizeof kCreaturePrefix + 10, kNightNameBytes = kGameNameBytes + sizeof kNightSuffix;

struct Given {
    char id[kIdBytes];
    uint32_t number;
};
// One record in the save: a file, and in it a group of IDs with their numbers for each sort of thing.
struct Group {
    const char *word;
    uint32_t first; // the lowest number of the sort
    List<Given> given;
};
struct Numbers {
    const char *file; // within the framework's own folder of the save
    uint32_t count;   // how many groups the record has
    Group groups[2];
    uint32_t fresh; // how many were given now, and are not in the save yet
    void clear() {
        for (Group &group : groups) group.given.clear();
    }
};
enum : uint32_t { kOfTreasures, kOfSeries, kOfCreatures = 0 };
constexpr Numbers treasure_numbers() { return {"/tsuchi.content/treasures.json", 2, {{"treasures", kFirstTreasure, {}}, {"series", kFirstSeries, {}}}, 0}; }
constexpr Numbers creature_numbers() { return {"/tsuchi.content/creatures.json", 1, {{"creatures", kFirstCreature, {}}, {"", 0, {}}}, 0}; }

ContentCounts counts{};
ContentEntryCounts entry_counts{};
CreatureCounts creature_tally{};
uint32_t left = 0; // rows in the Piklopedia's table of creatures that belong to no running mod's creature now

// One group of the record: IDs, each with its number. False when it is not that.
bool take_group(const Json *group, List<Given> &out, uint32_t least) {
    if (!group) return true;
    if (group->kind != Json::Kind::object) return false;
    for (const Json *each = group->first; each; each = each->next) {
        if (id_form(each->name) != IdForm::added || each->kind != Json::Kind::number || !each->whole || each->integer < int64_t(least) ||
            each->integer > int64_t(kMostNumber))
            return false;
        Given given{};
        memcpy(given.id, each->name, strlen(each->name) + 1);
        given.number = uint32_t(each->integer);
        for (uint32_t i = 0; i < out.count; ++i)
            if (out[i].number == given.number || !strcmp(out[i].id, given.id)) return false;
        if (!out.add(given)) return false;
    }
    return true;
}

Numbers treasure_cache = treasure_numbers(), creature_cache = creature_numbers();
bool take_numbers(Numbers &out, const Json &root) {
    // Every counted copy was written with all its groups. A missing one must not erase saved IDs.
    // Older, uncounted records could omit an empty group, but must identify at least one known group.
    const bool counted = root.find("counter") != nullptr;
    bool known = false;
    for (uint32_t g = 0; g < out.count; ++g) {
        const Json *group = root.find(out.groups[g].word);
        known = known || group;
        if ((!group && counted) || !take_group(group, out.groups[g].given, out.groups[g].first)) return false;
    }
    return known;
}
void put_numbers(Written &out, const Numbers &numbers) {
    for (uint32_t g = 0; g < numbers.count; ++g) {
        out.add(", \"").add(numbers.groups[g].word).add("\": {");
        for (uint32_t i = 0; i < numbers.groups[g].given.count; ++i) {
            const Given &each = numbers.groups[g].given.items[i];
            out.add(i ? ", \"" : "\"").add(each.id).add("\": ").number(each.number);
        }
        out.add("}");
    }
}
bool take_treasures(const Json &root, const char *) { return take_numbers(treasure_cache, root); }
bool take_creatures(const Json &root, const char *) { return take_numbers(creature_cache, root); }
void put_treasures(Written &out) { put_numbers(out, treasure_cache); }
void put_creatures(Written &out) { put_numbers(out, creature_cache); }
void clear_treasures() {
    treasure_cache.clear();
    treasure_cache.fresh = 0;
}
void clear_creatures() {
    creature_cache.clear();
    creature_cache.fresh = 0;
}
const char *const treasure_keys[] = {"treasures", "series"}, *const creature_keys[] = {"creatures"};
Record treasure_record{"treasures", nullptr, treasure_keys, 2, take_treasures, put_treasures, clear_treasures};
Record creature_record{"creatures", nullptr, creature_keys, 1, take_creatures, put_creatures, clear_creatures};
Record &record_of(const Numbers &numbers) { return numbers.count == 2 ? treasure_record : creature_record; }
TSUCHI_Result read_numbers(Numbers &numbers) { return read_record(record_of(numbers)); }
// A number in decimal, in at least `digits` digits. Returns where it ends.
char *write_number(char *at, uint32_t number, uint32_t digits = 1) {
    char buffer[10];
    uint32_t count = 0;
    do buffer[count++] = char('0' + number % 10);
    while (number /= 10);
    for (uint32_t i = count; i < digits; ++i) *at++ = '0';
    while (count) *at++ = buffer[--count];
    return at;
}

TSUCHI_Result write_numbers(const Numbers &numbers) { return write_record(record_of(numbers)); }

// The number an ID has, or the one after the highest ever given, which it has from now on. 0 when there is no
// memory to note it, or no number left.
uint32_t number_of(Numbers &numbers, uint32_t sort, const char *id) {
    Group &group = numbers.groups[sort];
    uint32_t highest = group.first - 1;
    for (uint32_t i = 0; i < group.given.count; ++i) {
        if (!strcmp(group.given[i].id, id)) return group.given[i].number;
        if (group.given[i].number > highest) highest = group.given[i].number;
    }
    Given given{};
    memcpy(given.id, id, strlen(id) + 1);
    given.number = highest + 1;
    if (highest >= kMostNumber || !group.given.add(given)) return 0;
    ++numbers.fresh;
    return given.number;
}

// Reads the record of the numbers, for a mod that is about to be given some or to use its own.
TSUCHI_Result open_numbers(Numbers &numbers, Text &problem, uint32_t &unread) {
    const TSUCHI_Result result = read_numbers(numbers);
    if (result == TSUCHI_OK) return result;
    ++unread;
    say(problem, "\"content\": the numbers the framework has given out could not be read from the save: ",
        result == TSUCHI_INVALID ? "its record of them is damaged"
        : result == TSUCHI_IO    ? "the save could not be opened or read"
                                 : why(result));
    return result;
}

// Keeps the numbers that were given now in the save, before anything is made with them.
TSUCHI_Result keep_numbers(Numbers &numbers, Text &problem, uint32_t &unwritten, uint32_t &given) {
    if (!numbers.fresh) return TSUCHI_OK;
    const TSUCHI_Result result = write_numbers(numbers);
    if (result != TSUCHI_OK) {
        ++unwritten;
        say(problem, "\"content\": the numbers the framework gave it could not be kept in the save: ",
            result == TSUCHI_IO ? "the save could not be written" : why(result));
        return result;
    }
    given += numbers.fresh;
    numbers.fresh = 0;
    return TSUCHI_OK;
}

// The name the game knows a mod's thing by: the framework's word for the sort of thing, and the thing's number.
void game_name(const char *prefix, uint32_t number, char out[kGameNameBytes]) {
    const size_t length = strlen(prefix);
    memcpy(out, prefix, length);
    *write_number(out + length, number, 4) = 0;
}

// ---- The game's tables ---------------------------------------------------------------------------------------

enum Table : uint32_t { kParameters, kClasses, kIcons, kCatalog, kCreatures, kCreatureCatalog, kSeries, kTables };
const char *const kTablePaths[kTables] = {t::asset::treasure_table,         t::asset::catalog_class_table, t::asset::icon_table,
                                          t::asset::treasure_catalog_table, t::asset::creature_table,      t::asset::creature_catalog_table, t::asset::treasure_series_table};
const char *const kTableWords[kTables] = {"the treasures' table", "the Piklopedia's table of classes",
                                          "the table of icons",   "the Piklopedia's table of treasures",
                                          "the creatures' table", "the Piklopedia's table of creatures", "the treasure series table"};
uint64_t tables[kTables]; // handles, as a mod last loaded each

// The rows the framework has added since the game started, each by its table and its name as the game keeps
// one. A row stays when its mod stops, and is the mod's again when it starts once more.
struct Made {
    uint32_t table;
    uint64_t name;
};
List<Made> made;

bool is_made(uint32_t table, uint64_t name) {
    for (uint32_t i = 0; i < made.count; ++i)
        if (made[i].table == table && made[i].name == name) return true;
    return false;
}

// Where a table's row is, or 0.
uint64_t row_at(uint64_t table, uint64_t name) {
    return table && name && t::bound(t::fn::find_table_row) ? reinterpret_cast<uint64_t>(t::fn::find_table_row(reinterpret_cast<void *>(table), name)) : 0;
}

// ---- The treasures and the creatures of the mods that run ----------------------------------------------------

const Kind kTreasure{"content.treasure"};
struct Treasure {
    uint64_t key;
    char id[kIdBytes];
    char name[kGameNameBytes]; // what the game knows it by
    uint64_t packed;           // the same, as the game keeps a name
    uint64_t of_class;         // a handle to its class
    bool in_catalog;
};
List<Treasure> treasures;

const Kind kCreature{"content.creature"};
struct Creature {
    uint64_t key;
    char id[kIdBytes];
    char name[kGameNameBytes]; // what the game knows it by
    uint64_t packed;           // the same, as the game keeps a name
    uint64_t night;            // the name of its row at night, or 0 for a creature that has none
    uint64_t of_class;         // a handle to its class,
    uint64_t night_class;      // and to the class the Piklopedia shows at night, or 0
    bool in_catalog;
};
List<Creature> creatures;

// Whose a class is already, or null: the ID of the treasure or the creature, and `as` says which of the two.
const char *class_owner(uint64_t of_class, const char *&as) {
    for (uint32_t i = 0; i < treasures.count; ++i)
        if (treasures[i].of_class == of_class) return as = ", is already the treasure ", treasures[i].id;
    for (uint32_t i = 0; i < creatures.count; ++i)
        if (creatures[i].of_class == of_class || creatures[i].night_class == of_class) return as = ", is already the creature ", creatures[i].id;
    return nullptr;
}

// What the loaded world keeps rows by name in, each as a handle; 0 for one that is not there.
struct Keepers {
    uint64_t rows = 0, icons = 0, creatures = 0;
    uint64_t icon_table = 0, icon_slots = 0; // a keeper may initialize its table and list separately
    int32_t icon_count = 0, icon_free = 0, icon_bits = 0;
} seen; // those whose lists have what mods added
uint32_t icons_retry_left = 0, icons_retry_wait = 0;
game::ClassRef rows_keeper_class{"OtakaraParameterMgr"}, icons_keeper_class{"ActorTextureMgr"}, creatures_keeper_class{"TekiParameterMgr"};

Keepers keepers_now() {
    const uint64_t framework = game::game_framework();
    if (!framework) return {};
    const uint64_t rows = game::read_or<uint64_t>(framework + t::framework_treasures::rows),
                   icons = game::read_or<uint64_t>(framework + t::framework_treasures::icons);
    const uint64_t beasts = game::read_or<uint64_t>(framework + t::framework_creatures::rows);
    Keepers now{rows && game::is_a(rows, rows_keeper_class) ? game::to_handle(rows) : 0,
                icons && game::is_a(icons, icons_keeper_class) ? game::to_handle(icons) : 0,
                beasts && game::is_a(beasts, creatures_keeper_class) ? game::to_handle(beasts) : 0};
    if (now.icons) {
        const uint64_t list = icons + t::icon_keeper::rows;
        now.icon_table = game::read_or<uint64_t>(icons + t::icon_keeper::table);
        now.icon_slots = game::read_or<uint64_t>(list + t::row_list::slots);
        now.icon_count = game::read_or<int32_t>(list + t::row_list::slot_count, -1);
        now.icon_free = game::read_or<int32_t>(list + t::row_list::free_count, -1);
        now.icon_bits = game::read_or<int32_t>(list + t::row_list::bit_count, -1);
    }
    return now;
}

// The game keeps its count of the Piklopedia's treasures once it has counted: it is told to count again.
void count_again() {
    const uint64_t framework = game::game_framework();
    const uint64_t ui = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    if (ui) game::write<uint8_t>(ui + t::treasure_count::known, 0);
}

// Looks a name up in a list of rows by name. False when the list cannot be read; `row` is 0 for a name it lacks.
bool listed(uint64_t list, uint64_t name, uint64_t &row) {
    namespace l = t::row_list;
    namespace e = t::table_row_entry;
    constexpr int32_t kMostSlots = 1 << 20;
    row = 0;
    int32_t slot_count = 0, free_count = 0, bit_count = 0;
    uint64_t slots = 0, far = 0;
    if (!game::read(list + l::slots, slots) || !game::read(list + l::slot_count, slot_count) || !game::read(list + l::free_count, free_count) ||
        !game::read(list + l::bit_count, bit_count) || !game::read(list + l::used_bits_far, far))
        return false;
    if (slot_count < 0 || slot_count > kMostSlots || (slot_count && !game::readable(slots, uint64_t(slot_count) * e::size))) return false;
    const uint64_t bits = far ? far : list + l::used_bits;
    for (int32_t i = 0; i < slot_count; ++i) {
        // With no slot free, every slot is in use and no bit needs looking at.
        if (free_count > 0 && (i >= bit_count || !(game::read_or<uint32_t>(bits + 4 * uint64_t(i >> 5)) >> (i & 31) & 1))) continue;
        const uint64_t slot = slots + uint64_t(i) * e::size;
        if (game::read_or<uint64_t>(slot + e::name) != name) continue;
        row = game::read_or<uint64_t>(slot + e::row);
        return true;
    }
    return true;
}

// Enters a table's row in the list a keeper made of that table's rows, unless it is there. True when it was
// entered. `entered` and `passed` count the rows entered and the lists that could not be entered in.
bool enter(uint64_t keeper_table, uint64_t list, uint32_t table, uint64_t name, uint32_t &entered, uint32_t &passed, bool *retry = nullptr) {
    const auto refused = [&](uint32_t &reason) {
        if (retry) *retry = true;
        ++reason;
        ++passed;
        return false;
    };
    const uint64_t ours = game::from_handle(tables[table]);
    uint64_t there = 0;
    // A keeper of another table than the one the rows are in has nothing of the mods', and gets nothing.
    auto &tally = entry_counts.tables[table];
    if (!ours) return refused(tally.no_table);
    if (!game::read_or<uint64_t>(keeper_table)) return refused(tally.keeper_not_ready);
    if (game::read_or<uint64_t>(keeper_table) != ours) return refused(tally.other_table);
    if (!t::bound(t::fn::add_table_row_internal)) return refused(tally.no_insert);
    if (!listed(list, name, there)) return refused(tally.bad_list);
    if (there) return ++tally.already, false;
    const uint64_t row = row_at(ours, name);
    if (!row) return refused(tally.no_row);
    // The engine's own way of putting a row in a table's list, with the keeper's list where the table's would be.
    t::fn::add_table_row_internal(reinterpret_cast<void *>(list - t::data_table::rows), name, reinterpret_cast<void *>(row));
    if (retry && (!listed(list, name, there) || there != row)) return refused(tally.bad_list);
    ++entered;
    return true;
}

// Enters every treasure's and every creature's rows in the loaded world's lists, where they are missing.
void enter_all(bool retrying = false) {
    namespace k = t::treasure_keeper;
    namespace c = t::creature_keeper;
    const Keepers now = keepers_now();
    const uint64_t rows = game::from_handle(now.rows), icons = game::from_handle(now.icons), beasts = game::from_handle(now.creatures);
    bool catalog_grew = false, icons_pending = false;
    for (uint32_t i = 0; i < treasures.count; ++i) {
        const Treasure &each = treasures[i];
        if (rows) {
            enter(rows + k::table, rows + k::rows, kParameters, each.packed, counts.entered, counts.not_entered);
            if (each.in_catalog && enter(rows + k::catalog_table, rows + k::catalog_rows, kCatalog, each.packed, counts.entered, counts.not_entered))
                catalog_grew = true;
        }
        if (icons) enter(icons + t::icon_keeper::table, icons + t::icon_keeper::rows, kIcons, each.packed, counts.entered, counts.not_entered, &icons_pending);
    }
    if (catalog_grew) count_again();
    for (uint32_t i = 0; i < creatures.count; ++i) {
        const Creature &each = creatures[i];
        uint32_t &entered = creature_tally.entered, &passed = creature_tally.not_entered;
        if (beasts) {
            enter(beasts + c::table, beasts + c::rows, kCreatures, each.packed, entered, passed);
            if (each.night) enter(beasts + c::table, beasts + c::rows, kCreatures, each.night, entered, passed);
            if (each.in_catalog) enter(beasts + c::catalog_table, beasts + c::catalog_rows, kCreatureCatalog, each.packed, entered, passed);
        }
        if (icons) enter(icons + t::icon_keeper::table, icons + t::icon_keeper::rows, kIcons, each.packed, entered, passed, &icons_pending);
    }
    seen = keepers_now(); // include the rows this pass inserted
    if (!icons_pending)
        icons_retry_left = icons_retry_wait = 0;
    else {
        if (!retrying) icons_retry_left = 20;
        icons_retry_wait = 30;
    }
}

// A row of the Piklopedia's table of creatures counts in the Piklopedia for as long as the table has it: the game
// counts the rows, and no field of one takes it out of the count. So a row that is no running mod's creature's is
// taken out of the table with the engine's own function, which frees it. That can be done only while no loaded
// world lists the row, since a world's list points at the row itself: such a row waits until that world is gone.
// True when the table has the row no more; `removed` says whether this took it out.
bool take_out(uint64_t name, bool &removed) {
    namespace c = t::creature_keeper;
    const uint64_t table = game::from_handle(tables[kCreatureCatalog]);
    removed = false;
    if (!table || !row_at(table, name)) return true; // the table went, or the row with a table that was loaded anew
    const uint64_t framework = game::game_framework();
    const uint64_t keeper = framework ? game::read_or<uint64_t>(framework + t::framework_creatures::rows) : 0;
    uint64_t pointed = 0;
    if (keeper && (!game::is_a(keeper, creatures_keeper_class) || !listed(keeper + c::catalog_rows, name, pointed) || pointed)) return false;
    // Only a table that removes rows the engine's plain way is asked to.
    const uint64_t functions = game::read_or<uint64_t>(table);
    if (!t::bound(t::fn::remove_table_row) || !functions ||
        game::read_or<uint64_t>(functions + t::data_table_table::remove_row) != reinterpret_cast<uint64_t>(t::fn::remove_table_row))
        return false;
    t::fn::remove_table_row(reinterpret_cast<void *>(table), name);
    return removed = !row_at(table, name);
}

bool is_creature(uint64_t name) {
    for (uint32_t i = 0; i < creatures.count; ++i)
        if (creatures[i].packed == name) return true;
    return false;
}

// Takes out every row of the Piklopedia's table of creatures that the framework added and no running mod's
// creature has, where that can be done now, and counts those that have to wait.
void sweep() {
    left = 0;
    for (uint32_t i = made.count; i-- > 0;) {
        if (made[i].table != kCreatureCatalog || is_creature(made[i].name)) continue;
        bool removed = false;
        if (!take_out(made[i].name, removed)) {
            ++left;
            continue;
        }
        made.remove(&made[i]);
        creature_tally.rows_removed += removed;
    }
    if (!made.count) made.clear();
}

// Each frame: a world that has come since the last look has lists of its own, and a world that has gone lets
// go of the rows it listed.
void step() {
    if (left) sweep();
    if (!treasures.count && !creatures.count) {
        seen = Keepers{};
        icons_retry_left = icons_retry_wait = 0;
        return;
    }
    const Keepers now = keepers_now();
    if (now.rows != seen.rows || now.icons != seen.icons || now.creatures != seen.creatures || now.icon_table != seen.icon_table ||
        now.icon_slots != seen.icon_slots || now.icon_count != seen.icon_count || now.icon_free != seen.icon_free || now.icon_bits != seen.icon_bits)
        enter_all();
    else if (icons_retry_left) {
        if (icons_retry_wait)
            --icons_retry_wait;
        else {
            --icons_retry_left;
            enter_all(true);
        }
    }
}

// The mod stopped: the treasure is no running mod's.
void forget_treasure(void *, uint64_t key) {
    for (uint32_t i = treasures.count; i-- > 0;) {
        if (treasures[i].key != key) continue;
        if (treasures[i].in_catalog) count_again(); // its row goes out of its series as the mod's change to it is put back
        treasures.remove(&treasures[i]);
        if (!treasures.count) treasures.clear();
        return;
    }
}

// The mod stopped: the creature is no running mod's, and its row in the Piklopedia's table goes when it can.
void forget_creature(void *, uint64_t key) {
    for (uint32_t i = creatures.count; i-- > 0;) {
        if (creatures[i].key != key) continue;
        creatures.remove(&creatures[i]);
        if (!creatures.count) creatures.clear();
        sweep();
        return;
    }
}

// ---- Making what a manifest declares -------------------------------------------------------------------------

// One thing being made: whose it is, what sort of thing, and what to call it when something is wrong.
struct Making {
    TSUCHI_Mod *mod;
    const char *sort; // "treasure" or "creature"
    const char *short_name;
    Text &problem;
    uint32_t &rows_added;

    TSUCHI_Result refuse(TSUCHI_Result result, const char *a, const char *b = "", const char *c = "", const char *d = "", const char *e = "",
                         const char *f = "", const char *g = "") const {
        say(problem, "\"content\": ", sort, " ", short_name, ": ", a, b, c, d, e, f, g);
        return result;
    }
};

// Changes one field of a row in the mod's name, to what the manifest has for it: a number, true or false, or text.
TSUCHI_Result set_field(const Making &making, uint32_t table, const char *row, const char *path, const Json &stated) {
    TSUCHI_Result result = TSUCHI_OK;
    if (stated.kind == Json::Kind::text) {
        result = host.datatable->override_text(making.mod, {tables[table]}, row, path, stated.text);
    } else {
        TSUCHI_Value value{};
        value.kind = stated.kind == Json::Kind::boolean ? TSUCHI_VALUE_BOOLEAN : stated.whole ? TSUCHI_VALUE_INTEGER : TSUCHI_VALUE_NUMBER;
        if (stated.kind == Json::Kind::boolean)
            value.as.boolean = stated.boolean;
        else if (stated.whole)
            value.as.integer = stated.integer;
        else
            value.as.number = stated.number;
        result = host.datatable->override(making.mod, {tables[table]}, row, path, &value);
    }
    return result == TSUCHI_OK ? result : making.refuse(result, "field ", path, " of its row in ", kTableWords[table], ": ", why(result));
}
// Changes fields of a row in the mod's name: each by its path.
TSUCHI_Result set_values(const Making &making, uint32_t table, const char *row, const Json *values) {
    for (const Json *field = values ? values->first : nullptr; field; field = field->next) {
        const TSUCHI_Result result = set_field(making, table, row, field->name, *field);
        if (result != TSUCHI_OK) return result;
    }
    return TSUCHI_OK;
}
TSUCHI_Result set_number(const Making &making, uint32_t table, const char *row, const char *path, int64_t number) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_INTEGER, value.as.integer = number;
    const TSUCHI_Result result = host.datatable->override(making.mod, {tables[table]}, row, path, &value);
    return result == TSUCHI_OK ? result : making.refuse(result, "field ", path, " of its row in ", kTableWords[table], ": ", why(result));
}
TSUCHI_Result set_text(const Making &making, uint32_t table, const char *row, const char *path, const char *text) {
    const TSUCHI_Result result = host.datatable->override_text(making.mod, {tables[table]}, row, path, text);
    return result == TSUCHI_OK ? result : making.refuse(result, "field ", path, " of its row in ", kTableWords[table], ": ", why(result));
}
// Writes a class's name into a row, which keeps it without the `_C` the class ends in.
TSUCHI_Result set_class_name(const Making &making, uint32_t table, const char *row, const char *class_path) {
    const char *leaf = class_path;
    for (const char *at = class_path; *at; ++at)
        if (*at == '.') leaf = at + 1;
    Text actor_name;
    if (!actor_name.set(leaf)) return making.refuse(TSUCHI_NO_MEMORY, why(TSUCHI_NO_MEMORY));
    actor_name.data[strlen(leaf) - 2] = 0;
    const TSUCHI_Result result = set_text(making, table, row, "ActorName", actor_name.get());
    actor_name.clear();
    return result;
}

// Loads one of the game's tables for the mod, which keeps it loaded while the mod runs, and the rows with it.
TSUCHI_Result load_table(TSUCHI_Mod *mod, uint32_t table, Text &problem) {
    TSUCHI_Object loaded{};
    const TSUCHI_Result result = host.asset->load(mod, kTablePaths[table], &loaded);
    if (result != TSUCHI_OK) return say(problem, "\"content\": ", kTableWords[table], " could not be loaded: ", why(result)), result;
    tables[table] = loaded.id;
    return TSUCHI_OK;
}

// Adds the thing's row to a table as a copy of the row of the thing it starts from, unless the row is there from
// an earlier start of its mod.
TSUCHI_Result make_row(const Making &making, uint32_t table, const char *name, uint64_t packed_name, const char *from) {
    const uint64_t object = game::from_handle(tables[table]);
    if (row_at(object, packed_name)) {
        if (is_made(table, packed_name)) return TSUCHI_OK;
        return making.refuse(TSUCHI_BUSY, kTableWords[table], " already has a row named ", name, ", which the framework did not add");
    }
    const TSUCHI_Result result = host.datatable->add_row(making.mod, {tables[table]}, name, from);
    // The first table of a sort is the one that says what is a thing of that sort at all.
    const bool first = table == kParameters || table == kCreatures;
    if (result == TSUCHI_NOT_FOUND)
        return making.refuse(result, "the ", making.sort, " it starts from, ", from, first ? ", is no " : ", has no row in ",
                             first ? making.sort : kTableWords[table], first ? " the game has" : "");
    if (result != TSUCHI_OK) return making.refuse(result, "its row in ", kTableWords[table], " could not be added: ", why(result));
    made.add({table, packed_name}); // with no memory to note it, the row is there all the same
    ++making.rows_added;
    if (table != kCatalog) return TSUCHI_OK;
    // A row of the Piklopedia's is in no series until the mod's series is written into it, and is in none again
    // when that is put back: a row left behind by a mod that stopped is in no list and no count.
    const uint64_t type = game::read_or<uint64_t>(object + t::data_table::row_struct), row = row_at(object, packed_name);
    TSUCHI_Value none{};
    none.kind = TSUCHI_VALUE_INTEGER, none.as.integer = -1;
    const TSUCHI_Result rested = type && row ? write_at(type, row, "OtakaraZukanIDInfo.SeriesID", &none) : TSUCHI_NOT_FOUND;
    return rested == TSUCHI_OK ? rested : making.refuse(rested, "field OtakaraZukanIDInfo.SeriesID of its row in ", kTableWords[table], ": ", why(rested));
}

// The text the game shows under the thing's name in a namespace.
TSUCHI_Result show(const Making &making, const char *name_space, const char *name, const Json *text, const Json *otherwise = nullptr) {
    const Json *said = text ? text : otherwise;
    const TSUCHI_Result result = host.text->override(making.mod, name_space, name, said ? said->text : "");
    return result == TSUCHI_OK ? result : making.refuse(result, "its text in ", name_space, ": ", why(result));
}

// Loads a class for the mod, which keeps it loaded, and finds the default object every actor of the class is made
// from. The class has to be the class of a thing the game knows by a name, and nobody's yet. `what` is what to
// call it: "its class, ".
TSUCHI_Result take_class(const Making &making, const char *what, const char *path, uint64_t &of_class, uint64_t &pattern) {
    TSUCHI_Object loaded{};
    const TSUCHI_Result result = host.asset->load(making.mod, path, &loaded);
    if (result != TSUCHI_OK)
        return making.refuse(result, what, path, ", could not be loaded: ", result == TSUCHI_NOT_FOUND ? "the game finds no such asset" : why(result));
    const uint64_t object = game::from_handle(loaded.id);
    pattern = object && game::is_class(object) ? game::default_object(object) : 0;
    char carried[game::kNameBytes];
    uint64_t needed = 0;
    if (!pattern || text_at(game::type_of(pattern), pattern, "CID", carried, sizeof carried, &needed) != TSUCHI_OK)
        return making.refuse(TSUCHI_INVALID, what, path, ", is no class of a ", making.sort, ": its default object has no CID");
    const char *as = "";
    if (const char *owner = class_owner(loaded.id, as)) return making.refuse(TSUCHI_BUSY, what, path, as, owner, "'s");
    of_class = loaded.id;
    return TSUCHI_OK;
}

// Makes one treasure. `place` is its place in its series, from 0, and `series` the series' number.
TSUCHI_Result make_treasure(const Making &making, const Json &declared, const char *id, uint32_t number, uint32_t series, uint32_t place) {
    TSUCHI_Mod *const mod = making.mod;
    char name[kGameNameBytes];
    game_name(kNamePrefix, number, name);
    game::Name known_as{};
    TSUCHI_Result result = game::name_from_utf8(name, known_as);
    if (result != TSUCHI_OK) return making.refuse(result, "the name ", name, ": ", why(result));
    const uint64_t packed_name = packed(known_as);
    const Json *const catalog = declared.find("catalog");

    // The treasure it starts from, by the name the game knows that one by.
    const char *from = declared.find("from")->text;
    if (id_form(from) == IdForm::added) {
        const Treasure *found = nullptr;
        for (uint32_t i = 0; i < treasures.count && !found; ++i)
            if (!strcmp(treasures[i].id, from)) found = &treasures[i];
        if (!found) return making.refuse(TSUCHI_NOT_FOUND, "the treasure it starts from, ", from, ", is no treasure of a mod that is running");
        from = found->name;
    }

    // Its class, which is kept loaded for the mod, and has to be nobody else's.
    const char *const class_path = declared.find("class")->text;
    uint64_t of_class = 0, pattern = 0;
    if ((result = take_class(making, "its class, ", class_path, of_class, pattern)) != TSUCHI_OK) return result;

    // Its rows, and what the manifest says of them.
    if ((result = make_row(making, kParameters, name, packed_name, from)) != TSUCHI_OK) return result;
    if ((result = make_row(making, kClasses, name, packed_name, from)) != TSUCHI_OK) return result;
    if ((result = make_row(making, kIcons, name, packed_name, from)) != TSUCHI_OK) return result;
    if (catalog && (result = make_row(making, kCatalog, name, packed_name, from)) != TSUCHI_OK) return result;

    if ((result = set_class_name(making, kParameters, name, class_path)) != TSUCHI_OK) return result;
    if (const Json *sparklium = declared.find("sparklium"))
        if ((result = set_number(making, kParameters, name, "Kira", sparklium->integer)) != TSUCHI_OK) return result;
    int32_t story[2] = {}, battle[2] = {};
    const bool carried_by = carriers_of(declared.find("carriers"), story);
    const bool in_battle = carriers_of(declared.find("battle_carriers"), battle);
    if (carried_by) {
        if ((result = set_number(making, kParameters, name, "CarryWeightMin", story[0])) != TSUCHI_OK) return result;
        if ((result = set_number(making, kParameters, name, "CarryWeightMax", story[1])) != TSUCHI_OK) return result;
    }
    if (carried_by || in_battle) {
        const int32_t *const range = in_battle ? battle : story;
        if ((result = set_number(making, kParameters, name, "CarryWeightMinVs", range[0])) != TSUCHI_OK) return result;
        if ((result = set_number(making, kParameters, name, "CarryWeightMaxVs", range[1])) != TSUCHI_OK) return result;
    }
    if ((result = set_values(making, kParameters, name, declared.find("values"))) != TSUCHI_OK) return result;
    if ((result = set_text(making, kClasses, name, "DayCookBlueprint", class_path)) != TSUCHI_OK) return result;
    if (const Json *icon = declared.find("icon")) {
        TSUCHI_Object texture{};
        result = host.asset->load(mod, icon->text, &texture);
        if (result != TSUCHI_OK)
            return making.refuse(result, "its icon, ", icon->text,
                                 ", could not be loaded: ", result == TSUCHI_NOT_FOUND ? "the game finds no such asset" : why(result));
        if ((result = set_text(making, kIcons, name, "Texture2D", icon->text)) != TSUCHI_OK) return result;
    }
    if (catalog) {
        // The catalog shows how many of the treasure there are from TotalPlaceNum, which the copied row has
        // from the donor: a treasure of a mod is one treasure,
        // unless its catalog values say how many places it has.
        if ((result = set_number(making, kCatalog, name, "TotalPlaceNum", 1)) != TSUCHI_OK) return result;
        // Catalog identity belongs to the framework, even when values names those fields.
        if ((result = set_values(making, kCatalog, name, catalog->find("values"))) != TSUCHI_OK) return result;
        if ((result = set_number(making, kCatalog, name, "OtakaraZukanIDInfo.SeriesID", series)) != TSUCHI_OK) return result;
        if ((result = set_number(making, kCatalog, name, "OtakaraZukanIDInfo.ZukanNo", place)) != TSUCHI_OK) return result;
        if ((result = set_text(making, kCatalog, name, "DayParams[0].ActorName", name)) != TSUCHI_OK) return result;
    }

    // Its texts, under its name. One the manifest does not give is empty, so that the game has text to find.
    const Json &text = *declared.find("text");
    const Json *const said = text.find("name");
    if ((result = show(making, "OtakaraName", name, said)) != TSUCHI_OK) return result;
    if ((result = show(making, "OtakaraName_PictureBook", name, text.find("catalog_name"), said)) != TSUCHI_OK) return result;
    if ((result = show(making, "OtakaraName_Result", name, text.find("result_name"), said)) != TSUCHI_OK) return result;
    if ((result = show(making, "OtakaraDesc", name, text.find("description"))) != TSUCHI_OK) return result;
    if ((result = show(making, "OtakaraDescOlimar", name, text.find("olimar_notes"))) != TSUCHI_OK) return result;
    if ((result = show(making, "OtakaraDescLouie", name, text.find("louie_notes"))) != TSUCHI_OK) return result;

    // Last, the name every actor of the class carries, and the ID that stands for it.
    result = content_identity_hold(mod, pattern, name);
    if (result != TSUCHI_OK) return making.refuse(result, "its class, ", class_path, ": the name could not be written into its default object: ", why(result));
    result = game::add_name(mod, id, known_as);
    if (result != TSUCHI_OK) return making.refuse(result, result == TSUCHI_BUSY ? "something already has the ID " : "it could not be named ", id);
    Treasure treasure{next_key, {}, {}, packed_name, of_class, catalog != nullptr};
    memcpy(treasure.id, id, strlen(id) + 1), memcpy(treasure.name, name, sizeof name);
    if (!treasures.add(treasure)) return making.refuse(TSUCHI_NO_MEMORY, why(TSUCHI_NO_MEMORY));
    result = own(mod, kTreasure, treasure.key, forget_treasure, nullptr);
    if (result != TSUCHI_OK) {
        treasures.remove(&treasures[treasures.count - 1]);
        return making.refuse(result, why(result));
    }
    ++next_key, ++counts.declared;
    return TSUCHI_OK;
}

TSUCHI_Result make(TSUCHI_Mod *mod, const Json &content, Numbers &numbers, Text &problem) {
    const Json *const all = content.find("treasures"), *const all_series = content.find("treasure_series");
    if (!(all && all->first) && !(all_series && all_series->first)) return TSUCHI_OK;
    char id[kIdBytes];

    // The numbers first: read from the save, given where an ID has none, and kept before anything is made with them.
    TSUCHI_Result result = open_numbers(numbers, problem, counts.records_unread);
    if (result != TSUCHI_OK) return result;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const Json *const group = pass ? all : all_series;
        for (const Json *each = group ? group->first : nullptr; each; each = each->next) {
            if (!id_join(mod_id(mod), each->name, id)) return TSUCHI_INVALID;
            if (!number_of(numbers, pass ? kOfTreasures : kOfSeries, id)) return TSUCHI_NO_MEMORY;
        }
    }
    if ((result = keep_numbers(numbers, problem, counts.records_unwritten, counts.numbers_given)) != TSUCHI_OK) return result;

    // A series is its name, under its number.
    for (const Json *each = all_series ? all_series->first : nullptr; each; each = each->next) {
        char key[] = "Series0000000000";
        id_join(mod_id(mod), each->name, id);
        *write_number(key + 6, number_of(numbers, kOfSeries, id), 2) = 0;
        if (each->find("bonus") || each->find("icon")) {
            if ((result = load_table(mod, kSeries, problem)) != TSUCHI_OK) return result;
            const Making making{mod, "series", each->name, problem, counts.rows_added};
            game::Name row_name{};
            if ((result = game::name_from_utf8(key, row_name)) != TSUCHI_OK) return result;
            const Json* from = each->find("from");
            if ((result = make_row(making, kSeries, key, packed(row_name), from ? from->text : "Series01")) != TSUCHI_OK) return result;
            const uint32_t series = number_of(numbers, kOfSeries, id);
            if ((result = set_number(making, kSeries, key, "SeriesID", series)) != TSUCHI_OK) return result;
            const Json* bonus = each->find("bonus");
            Json zero{}; zero.kind = Json::Kind::number; zero.whole = true;
            if ((result = set_field(making, kSeries, key, "KiraRate", bonus ? *bonus : zero)) != TSUCHI_OK) return result;
            if (const Json* icon = each->find("icon")) {
                TSUCHI_Object loaded{};
                if ((result = host.asset->load(mod, icon->text, &loaded)) != TSUCHI_OK) return result;
                static game::ClassRef texture_class{t::name::content_texture_class};
                if (!game::is_a(game::from_handle(loaded.id), texture_class)) return TSUCHI_INVALID;
                if ((result = set_text(making, kSeries, key, "Texture", icon->text)) != TSUCHI_OK) return result;
            }
            const uint64_t framework = game::game_framework();
            const uint64_t manager = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
            const uint64_t row = row_at(game::from_handle(tables[kSeries]), packed(row_name));
            if ((result = content_series_hold(mod, manager, row, series)) != TSUCHI_OK) return result;
        }
        const char *const name_spaces[] = {"OtakaraSeries", "OtakaraSeries_Result"};
        for (const char *name_space : name_spaces) {
            result = host.text->override(mod, name_space, key, each->find("name")->text);
            if (result != TSUCHI_OK) return say(problem, "\"content\": series ", each->name, ": its name in ", name_space, ": ", why(result)), result;
        }
    }
    if (!(all && all->first)) return TSUCHI_OK;

    bool in_catalog = false;
    for (const Json *each = all->first; each; each = each->next) in_catalog = in_catalog || each->find("catalog");
    for (uint32_t table = kParameters; table <= kCatalog; ++table)
        if ((table != kCatalog || in_catalog) && (result = load_table(mod, table, problem)) != TSUCHI_OK) return result;
    for (const Json *each = all->first; each; each = each->next) {
        const Making making{mod, "treasure", each->name, problem, counts.rows_added};
        uint32_t series = 0, place = 0;
        if (const Json *catalog = each->find("catalog")) {
            const char *const in = catalog->find("series")->text;
            id_join(mod_id(mod), in, id);
            series = number_of(numbers, kOfSeries, id);
            for (const Json *before = all->first; before != each; before = before->next) {
                const Json *const other = before->find("catalog");
                place += other && !strcmp(other->find("series")->text, in);
            }
        }
        id_join(mod_id(mod), each->name, id);
        if ((result = make_treasure(making, *each, id, number_of(numbers, kOfTreasures, id), series, place)) != TSUCHI_OK) return result;
    }
    enter_all(); // in the world that is loaded now; a later world's lists are looked at when it has come
    return TSUCHI_OK;
}

// ---- Creatures -----------------------------------------------------------------------------------------------

// What a creature has by day, or at night what `night` says in place of that: a key's value from `night` when it
// has the key, and from the creature itself otherwise.
const Json *stated(const Json &by_day, const Json *at_night, const char *key) {
    const Json *const other = at_night ? at_night->find(key) : nullptr;
    return other ? other : by_day.find(key);
}

// Writes into one of a creature's rows in the creatures' table what the manifest says of it. With `at_night`, the
// row is the one the game uses for the creature at night, and gets what the creature has by day except where
// `night` says otherwise.
TSUCHI_Result set_creature(const Making &making, const char *row, const char *class_path, const Json &by_day, const Json *at_night) {
    TSUCHI_Result result = set_class_name(making, kCreatures, row, class_path);
    if (result != TSUCHI_OK) return result;
    if (const Json *life = stated(by_day, at_night, "life"))
        if ((result = set_field(making, kCreatures, row, "MaxLife", *life)) != TSUCHI_OK) return result;
    if (const Json *sparklium = stated(by_day, at_night, "sparklium"))
        if ((result = set_number(making, kCreatures, row, "Kira", sparklium->integer)) != TSUCHI_OK) return result;
    if (const Json *pikmin = stated(by_day, at_night, "pikmin"))
        if ((result = set_number(making, kCreatures, row, "CarryIncPikmins", pikmin->integer)) != TSUCHI_OK) return result;
    int32_t story[2] = {}, battle[2] = {};
    const bool carried_by = carriers_of(stated(by_day, at_night, "carriers"), story);
    const bool in_battle = carriers_of(stated(by_day, at_night, "battle_carriers"), battle);
    if (carried_by) {
        if ((result = set_number(making, kCreatures, row, "CarryWeightMin", story[0])) != TSUCHI_OK) return result;
        if ((result = set_number(making, kCreatures, row, "CarryWeightMax", story[1])) != TSUCHI_OK) return result;
    }
    if (carried_by || in_battle) {
        const int32_t *const range = in_battle ? battle : story;
        if ((result = set_number(making, kCreatures, row, "CarryWeightMinVs", range[0])) != TSUCHI_OK) return result;
        if ((result = set_number(making, kCreatures, row, "CarryWeightMaxVs", range[1])) != TSUCHI_OK) return result;
    }
    if ((result = set_values(making, kCreatures, row, by_day.find("values"))) != TSUCHI_OK) return result;
    return at_night ? set_values(making, kCreatures, row, at_night->find("values")) : TSUCHI_OK;
}

// Two of the game's names for things are one name whatever the case of their letters.
bool same_name(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'a' && *a <= 'z' ? char(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? char(*b - 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// A creature's row in the Piklopedia's table lists the actors the Piklopedia puts on its stage, by day and at
// night, each by the name of a row in the table of classes. The row is a copy of the row of the creature it
// starts from, so those that named that creature name the new one.
TSUCHI_Result stage(const Making &making, const char *row, const char *from, const char *name) {
    const char *const lists[] = {"DayParams", "NightParams"};
    uint32_t renamed = 0;
    for (const char *list : lists) {
        TSUCHI_Value length{};
        TSUCHI_Result result = host.datatable->get(making.mod, {tables[kCreatureCatalog]}, row, list, &length);
        if (result == TSUCHI_OK && length.kind != TSUCHI_VALUE_ARRAY) result = TSUCHI_INVALID;
        if (result != TSUCHI_OK)
            return making.refuse(result, "field ", list, " of its row in ", kTableWords[kCreatureCatalog], ": ",
                                 result == TSUCHI_INVALID ? "it is no list" : why(result));
        for (uint32_t i = 0; i < length.count; ++i) {
            char path[48], staged[game::kNameBytes];
            const size_t head = strlen(list);
            memcpy(path, list, head);
            path[head] = '[';
            char *const end = write_number(path + head + 1, i);
            memcpy(end, "].ActorName", sizeof "].ActorName");
            uint64_t bytes = 0;
            result = host.datatable->get_text(making.mod, {tables[kCreatureCatalog]}, row, path, staged, sizeof staged, &bytes);
            if (result != TSUCHI_OK) return making.refuse(result, "field ", path, " of its row in ", kTableWords[kCreatureCatalog], ": ", why(result));
            if (!same_name(staged, from)) continue;
            if ((result = set_text(making, kCreatureCatalog, row, path, name)) != TSUCHI_OK) return result;
            ++renamed;
        }
    }
    if (!renamed)
        return making.refuse(TSUCHI_NOT_FOUND, "the creature it starts from, ", from, ", is not among the actors its own row in ",
                             kTableWords[kCreatureCatalog], " puts on the stage");
    return TSUCHI_OK;
}

TSUCHI_Result make_creature(const Making &making, const Json &declared, const char *id, uint32_t number) {
    TSUCHI_Mod *const mod = making.mod;
    char name[kGameNameBytes], night_name[kNightNameBytes] = "";
    game_name(kCreaturePrefix, number, name);
    game::Name known_as{}, at_night{};
    TSUCHI_Result result = game::name_from_utf8(name, known_as);
    if (result != TSUCHI_OK) return making.refuse(result, "the name ", name, ": ", why(result));
    const uint64_t packed_name = packed(known_as);
    const Json *const night = declared.find("night"), *const catalog = declared.find("catalog");
    if (night) {
        memcpy(night_name, name, strlen(name));
        memcpy(night_name + strlen(name), kNightSuffix, sizeof kNightSuffix);
        if ((result = game::name_from_utf8(night_name, at_night)) != TSUCHI_OK) return making.refuse(result, "the name ", night_name, ": ", why(result));
    }

    // The creature it starts from, by the name the game knows that one by.
    const char *from = declared.find("from")->text;
    if (id_form(from) == IdForm::added) {
        const Creature *found = nullptr;
        for (uint32_t i = 0; i < creatures.count && !found; ++i)
            if (!strcmp(creatures[i].id, from)) found = &creatures[i];
        if (!found) return making.refuse(TSUCHI_NOT_FOUND, "the creature it starts from, ", from, ", is no creature of a mod that is running");
        from = found->name;
    }

    // Its class, and the class the Piklopedia shows at night when that is another: each is kept loaded for the
    // mod, and has to be nobody else's.
    const char *const class_path = declared.find("class")->text;
    const Json *const second = declared.find("night_class");
    uint64_t of_class = 0, pattern = 0, night_class = 0, night_pattern = 0;
    if ((result = take_class(making, "its class, ", class_path, of_class, pattern)) != TSUCHI_OK) return result;
    if (second && (result = take_class(making, "its night class, ", second->text, night_class, night_pattern)) != TSUCHI_OK) return result;
    if (night_class == of_class) night_class = night_pattern = 0; // one class under two paths

    // Its rows but for the Piklopedia's, and what the manifest says of them.
    if ((result = make_row(making, kCreatures, name, packed_name, from)) != TSUCHI_OK) return result;
    if (night && (result = make_row(making, kCreatures, night_name, packed(at_night), from)) != TSUCHI_OK) return result;
    if ((result = make_row(making, kClasses, name, packed_name, from)) != TSUCHI_OK) return result;
    if ((result = make_row(making, kIcons, name, packed_name, from)) != TSUCHI_OK) return result;
    if ((result = set_creature(making, name, class_path, declared, nullptr)) != TSUCHI_OK) return result;
    if (night && (result = set_creature(making, night_name, class_path, declared, night)) != TSUCHI_OK) return result;
    if ((result = set_text(making, kClasses, name, "DayCookBlueprint", class_path)) != TSUCHI_OK) return result;
    if ((result = set_text(making, kClasses, name, "NightCookBlueprint", night_class ? second->text : class_path)) != TSUCHI_OK) return result;
    if (const Json *icon = declared.find("icon")) {
        TSUCHI_Object texture{};
        result = host.asset->load(mod, icon->text, &texture);
        if (result != TSUCHI_OK)
            return making.refuse(result, "its icon, ", icon->text,
                                 ", could not be loaded: ", result == TSUCHI_NOT_FOUND ? "the game finds no such asset" : why(result));
        if ((result = set_text(making, kIcons, name, "Texture2D", icon->text)) != TSUCHI_OK) return result;
    }

    // Its texts, under its name. One the manifest does not give is empty, so that the game has text to find.
    const Json &text = *declared.find("text");
    if ((result = show(making, "GenseiName", name, text.find("name"))) != TSUCHI_OK) return result;
    const char *const scientific[] = {"GenseiScientificName", "GenseiScientificName_Picturebook", "GenseiScientificName_Ellipsis"};
    for (const char *name_space : scientific)
        if ((result = show(making, name_space, name, text.find("scientific_name"))) != TSUCHI_OK) return result;
    if ((result = show(making, "GenseiFamilyName", name, text.find("family"))) != TSUCHI_OK) return result;
    if ((result = show(making, "GenseiDesc", name, text.find("description"))) != TSUCHI_OK) return result;
    if ((result = show(making, "GenseiDescOlimar", name, text.find("olimar_notes"))) != TSUCHI_OK) return result;
    if ((result = show(making, "GenseiDescLouie", name, text.find("louie_notes"))) != TSUCHI_OK) return result;

    // Its row in the Piklopedia's table comes after all that can be refused before it, since the row counts in
    // the Piklopedia from the moment the table has it.
    if (catalog) {
        if ((result = make_row(making, kCreatureCatalog, name, packed_name, from)) != TSUCHI_OK) return result;
        if ((result = stage(making, name, from, name)) != TSUCHI_OK) return result;
        if ((result = set_values(making, kCreatureCatalog, name, catalog->find("values"))) != TSUCHI_OK) return result;
    }

    // Last, the name every actor of its classes carries, and the ID that stands for it.
    result = content_identity_hold(mod, pattern, name);
    if (result != TSUCHI_OK) return making.refuse(result, "its class, ", class_path, ": the name could not be written into its default object: ", why(result));
    if (night_pattern && (result = content_identity_hold(mod, night_pattern, name)) != TSUCHI_OK)
        return making.refuse(result, "its night class, ", second->text, ": the name could not be written into its default object: ", why(result));
    result = game::add_name(mod, id, known_as);
    if (result != TSUCHI_OK) return making.refuse(result, result == TSUCHI_BUSY ? "something already has the ID " : "it could not be named ", id);
    Creature creature{next_key, {}, {}, packed_name, night ? packed(at_night) : 0, of_class, night_class, catalog != nullptr};
    memcpy(creature.id, id, strlen(id) + 1), memcpy(creature.name, name, sizeof name);
    if (!creatures.add(creature)) return making.refuse(TSUCHI_NO_MEMORY, why(TSUCHI_NO_MEMORY));
    result = own(mod, kCreature, creature.key, forget_creature, nullptr);
    if (result != TSUCHI_OK) {
        creatures.remove(&creatures[creatures.count - 1]);
        return making.refuse(result, why(result));
    }
    ++next_key, ++creature_tally.declared;
    return TSUCHI_OK;
}

TSUCHI_Result make_creatures(TSUCHI_Mod *mod, const Json &content, Numbers &numbers, Text &problem) {
    const Json *const all = content.find("creatures");
    if (!(all && all->first)) return TSUCHI_OK;
    char id[kIdBytes];

    // The numbers first, as for treasures, in a record of the creatures' own.
    TSUCHI_Result result = open_numbers(numbers, problem, creature_tally.records_unread);
    if (result != TSUCHI_OK) return result;
    for (const Json *each = all->first; each; each = each->next) {
        if (!id_join(mod_id(mod), each->name, id)) return TSUCHI_INVALID;
        if (!number_of(numbers, kOfCreatures, id)) return TSUCHI_NO_MEMORY;
    }
    if ((result = keep_numbers(numbers, problem, creature_tally.records_unwritten, creature_tally.numbers_given)) != TSUCHI_OK) return result;

    bool in_catalog = false;
    for (const Json *each = all->first; each; each = each->next) in_catalog = in_catalog || each->find("catalog");
    const uint32_t needed[] = {kCreatures, kClasses, kIcons, kCreatureCatalog};
    for (uint32_t table : needed)
        if ((table != kCreatureCatalog || in_catalog) && (result = load_table(mod, table, problem)) != TSUCHI_OK) return result;
    for (const Json *each = all->first; each; each = each->next) {
        const Making making{mod, "creature", each->name, problem, creature_tally.rows_added};
        id_join(mod_id(mod), each->name, id);
        if ((result = make_creature(making, *each, id, number_of(numbers, kOfCreatures, id))) != TSUCHI_OK) return result;
    }
    enter_all(); // in the world that is loaded now; a later world's lists are looked at when it has come
    return TSUCHI_OK;
}

TSUCHI_Result begin(TSUCHI_Mod *mod, const Json &content, Text &problem) {
    const uint64_t first_owned = ownership_mark(mod);
    if (content.find("treasures") || content.find("treasure_series")) read_record(treasure_record);
    if (content.find("creatures")) read_record(creature_record);
    Numbers &numbers = treasure_cache;
    uint32_t old[2] = {numbers.groups[0].given.count, numbers.groups[1].given.count};
    TSUCHI_Result result = make(mod, content, numbers, problem);
    if (numbers.fresh) {
        numbers.groups[0].given.count = old[0], numbers.groups[1].given.count = old[1], numbers.fresh = 0;
    }
    if (result != TSUCHI_OK) return ++counts.refused, result;
    Numbers &beasts = creature_cache;
    const uint32_t count = beasts.groups[0].given.count;
    result = make_creatures(mod, content, beasts, problem);
    if (beasts.fresh) beasts.groups[0].given.count = count, beasts.fresh = 0;
    if (result == TSUCHI_OK) result = pikmin_content_begin(mod, content, problem);
    if (result == TSUCHI_OK) result = content_squad_begin(mod, content, problem);
    if (result == TSUCHI_OK) result = banks_begin(mod, content, problem);
    if (result == TSUCHI_OK) result = content_placements_begin(mod, content, problem);
    if (result == TSUCHI_OK) {
        if (!ownership_isolated(mod)) content_identity_accept(mod, first_owned, ownership_mark(mod));
        return result;
    }
    ++creature_tally.refused;
    sweep(); // a creature that was refused part of the way may have a row in the Piklopedia's table already
    return result;
}

} // namespace

namespace game {

bool added_name(const char *id, Name &out) {
    for (uint32_t i = 0; i < named.count; ++i) {
        if (strcmp(named[i].id, id)) continue;
        memcpy(&out, &named[i].name, sizeof out);
        return true;
    }
    return false;
}

bool added_id(Name name, char out[kIdBytes]) {
    const uint64_t wanted = packed(name);
    for (uint32_t i = 0; i < named.count; ++i) {
        if (named[i].name != wanted) continue;
        memcpy(out, named[i].id, kIdBytes);
        return true;
    }
    return false;
}

TSUCHI_Result kind_into(Name kind, char *buffer, uint64_t capacity, uint64_t *out) {
    char id[kIdBytes];
    if (!named.count || !added_id(kind, id)) return name_into(kind, buffer, capacity, out);
    *out = strlen(id) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, id, *out);
    return TSUCHI_OK;
}

TSUCHI_Result add_name(TSUCHI_Mod *mod, const char *id, Name name) {
    char whose[kModIdBytes];
    const char *rest = nullptr;
    const uint64_t wanted = packed(name);
    if (!id_split(id, whose, &rest) || strcmp(whose, mod_id(mod)) || !wanted) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < named.count; ++i)
        if (named[i].name == wanted || !strcmp(named[i].id, id)) return TSUCHI_BUSY;
    Named entry{next_key, {}, wanted};
    memcpy(entry.id, id, strlen(id) + 1);
    if (!named.add(entry)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result result = own(mod, kName, entry.key, forget_name, nullptr);
    if (result != TSUCHI_OK) return named.remove(&named[named.count - 1]), result;
    ++next_key;
    return TSUCHI_OK;
}

} // namespace game

void content_start() {
    add_manifest_section({"content", well_formed, begin});
    each_frame(step);
}

ContentCounts content_counts() {
    ContentCounts now = counts;
    now.treasures = treasures.count;
    return now;
}

ContentEntryCounts content_entry_counts() { return entry_counts; }

CreatureCounts creature_counts() {
    CreatureCounts now = creature_tally;
    now.creatures = creatures.count, now.rows_left = left;
    return now;
}

} // namespace tsuchi
