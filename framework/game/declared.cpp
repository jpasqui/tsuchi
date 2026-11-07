// What a mod's manifest declares of the game's data: changes to data tables and to texts. They are made as the mod
// starts, before any code of its runs, through the same calls the mod's code would make, so they are the mod's
// in every way: stacked with other mods' changes, and undone when the mod stops. A row that a manifest adds stays,
// as one that code adds does.
#include <string.h>

#include "game.h"
#include "host.h"
#include "ids.h"
#include "json.h"
#include "list.h"
#include "mods.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "text.h"

namespace tsuchi {
namespace {

bool whole_text(const Json& value) { return value.kind == Json::Kind::text && strlen(value.text) == value.bytes; }
bool object_named(const Json& value) { return value.kind == Json::Kind::object && value.name && value.name[0]; }

// Says what is wrong and gives the result back. With no memory to say it, the caller says that instead.
TSUCHI_Result refuse(Text& problem, TSUCHI_Result result, const char* a, const char* b = "", const char* c = "", const char* d = "", const char* e = "",
                  const char* f = "", const char* g = "", const char* h = "") {
    const char* const parts[] = {a, b, c, d, e, f, g, h};
    size_t bytes = 1;
    for (const char* part : parts) bytes += strlen(part);
    if (!problem.reserve(bytes)) return result;
    char* at = problem.data;
    for (const char* part : parts) {
        const size_t length = strlen(part);
        memcpy(at, part, length);
        at += length;
    }
    *at = 0;
    return result;
}

// Why a change was not made, in words for the mod's author.
const char* why(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_NOT_FOUND: return "the game has none of that name";
    case TSUCHI_INVALID: return "the value is of the wrong kind for it";
    case TSUCHI_UNSUPPORTED: return "it is of a kind that cannot be changed";
    case TSUCHI_NOT_READY: return "the game cannot be asked yet";
    case TSUCHI_NO_MEMORY: return "there is no memory for it";
    default: return "the game refused it";
    }
}

// ---- "tables": {"<table's path>": {"<row>": {"<path to a field>": value}}} ----------------------------------
//
// A row the table does not have is added as a copy of one it has, and is named for the mod:
// {"<mod id>:<name>": {"from": "<row to copy>", "<path>": value}}. An array's length is {"<path>": {"length": n}}.

bool length_value(const Json& value) {
    if (value.kind != Json::Kind::object || value.count != 1 || !value.first->name || strcmp(value.first->name, "length")) return false;
    const Json& length = *value.first;
    return length.kind == Json::Kind::number && length.whole && length.integer >= 0 && length.integer <= int64_t(UINT32_MAX);
}

bool tables_well_formed(const Json& section, Text& wrong) {
    if (section.kind != Json::Kind::object) return wrong.set("\"tables\" is an object: a table's path, then a row's name, then a field and its new value"), false;
    for (const Json* table = section.first; table; table = table->next) {
        if (!object_named(*table) || table->name[0] != '/') return wrong.set("\"tables\": a table is named by its path, such as /Game/Folder/Table.Table, and holds its rows"), false;
        for (const Json* row = table->first; row; row = row->next) {
            if (!object_named(*row)) return wrong.set("\"tables\": a row is named, and holds the fields to change"), false;
            const Json* from = row->find("from");
            if (from && (!whole_text(*from) || !from->bytes)) return wrong.set("\"tables\": \"from\" is the name of the row that a new row copies"), false;
            if (from && id_form(row->name) != IdForm::added)
                return refuse(wrong, TSUCHI_INVALID, "\"tables\": row ", row->name, ": a new row's name is the mod's ID, a colon, and a name, such as example.mod:big_bulborb"), false;
            for (const Json* field = row->first; field; field = field->next) {
                if (field == from) continue;
                if (field->kind == Json::Kind::object && field->name && field->name[0]) {
                    if (!length_value(*field)) return wrong.set("\"tables\": an array's length is {\"length\": n}, where n is a whole number from 0"), false;
                    continue;
                }
                const bool plain = field->kind == Json::Kind::boolean || field->kind == Json::Kind::number || whole_text(*field);
                if (!field->name || !field->name[0] || !plain)
                    return wrong.set("\"tables\": a field is named by its path, and its new value is a number, true or false, text, or {\"length\": n} for an array"), false;
            }
        }
    }
    return true;
}

namespace t = game::table;
game::ClassRef table_class{t::name::data_table_class};

// The rows that a manifest added since the game started, by the table's handle and the row's name as the game keeps
// it. A row stays when its mod stops, as `datatable.add_row` says, and is the mod's again when it starts once more.
struct Added {
    uint64_t table, name;
};
List<Added> added;

bool was_added(uint64_t table, uint64_t name) {
    for (uint32_t i = 0; i < added.count; ++i)
        if (added[i].table == table && added[i].name == name) return true;
    return false;
}

bool same_path(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// True for a table whose new rows `content` makes: those of treasures, creatures and Pikmin types. A row there is
// one of several that make up the thing, and `content` adds them together.
bool content_table(const char* path, TSUCHI_Object table) {
    const char* const paths[] = {t::asset::treasure_table, t::asset::treasure_catalog_table, t::asset::catalog_class_table, t::asset::icon_table,
                                 t::asset::creature_table, t::asset::creature_catalog_table, t::asset::pikmin_property_table};
    char own[512];
    uint64_t needed = 0;
    const uint64_t object = game::from_handle(table.id);
    const bool known = object && game::object_path(object, own, sizeof own, &needed) == TSUCHI_OK;
    for (const char* each : paths)
        if (same_path(each, path) || (known && same_path(each, own))) return true;
    return false;
}

// Adds a row that a manifest names with "from", unless the mod added it at an earlier start.
TSUCHI_Result add_row(TSUCHI_Mod* mod, const Json& table, TSUCHI_Object asset, const Json& row, const char* from, Text& problem) {
    char owner[kModIdBytes];
    const char* name = nullptr;
    if (!id_split(row.name, owner, &name) || strcmp(owner, mod_id(mod)))
        return refuse(problem, TSUCHI_INVALID, "\"tables\": ", table.name, ", row ", row.name, ": a new row's name begins with the mod's own ID, ", mod_id(mod), ", and a colon");
    if (content_table(table.name, asset))
        return refuse(problem, TSUCHI_UNSUPPORTED, "\"tables\": ", table.name, ", row ", row.name,
                      ": a new treasure, creature or Pikmin type is declared under \"content\", which adds its rows to this table");
    game::Name packed{};
    uint64_t key = 0;
    TSUCHI_Result result = game::name_from_utf8(row.name, packed);
    if (result != TSUCHI_OK)
        return refuse(problem, result == TSUCHI_CAPACITY ? TSUCHI_INVALID : result, "\"tables\": ", table.name, ", row ", row.name, ": the game cannot keep that name");
    memcpy(&key, &packed, sizeof packed);
    const uint64_t object = game::from_handle(asset.id);
    if (object && t::bound(t::fn::find_table_row) && t::fn::find_table_row(reinterpret_cast<void*>(object), key)) {
        if (was_added(asset.id, key)) return TSUCHI_OK;
        return refuse(problem, TSUCHI_BUSY, "\"tables\": ", table.name, ", row ", row.name, ", field from: the table already has a row of that name, which another mod added");
    }
    result = host.datatable->add_row(mod, asset, row.name, from);
    if (result == TSUCHI_NOT_FOUND) return refuse(problem, result, "\"tables\": ", table.name, ", row ", row.name, ", field from: the table has no row ", from, " to copy");
    if (result != TSUCHI_OK) return refuse(problem, result, "\"tables\": ", table.name, ", row ", row.name, ", field from: ", why(result));
    added.add({asset.id, key}); // with no memory to note it, the row is there all the same, and the mod's next start is refused
    return TSUCHI_OK;
}

// Why an array's length was not set.
const char* why_length(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_INVALID: return "it is no array";
    case TSUCHI_UNSUPPORTED: return "its elements are structs, or of a kind whose array cannot change length";
    case TSUCHI_CAPACITY: return "the length is more than the game allows";
    default: return why(result);
    }
}

TSUCHI_Result tables_begin(TSUCHI_Mod* mod, const Json& section, Text& problem) {
    for (const Json* table = section.first; table; table = table->next) {
        TSUCHI_Object asset{};
        TSUCHI_Result result = host.asset->load(mod, table->name, &asset); // kept loaded for as long as the mod runs, and its changes with it
        if (result != TSUCHI_OK) return refuse(problem, result, "\"tables\": ", table->name, ": ", result == TSUCHI_INVALID ? "that is no path to a table" : why(result));
        const uint64_t object = game::from_handle(asset.id);
        if (!object || !game::is_a(object, table_class))
            return refuse(problem, TSUCHI_INVALID, "\"tables\": ", table->name, ": that asset is no data table");
        if (!t::bound(t::fn::find_table_row)) return refuse(problem, TSUCHI_NOT_READY, "\"tables\": ", table->name, ": ", why(TSUCHI_NOT_READY));
        for (const Json* row = table->first; row; row = row->next) {
            const Json* from = row->find("from");
            if (from && (result = add_row(mod, *table, asset, *row, from->text, problem)) != TSUCHI_OK) return result;
            if (!from) {
                game::Name name{};
                uint64_t key = 0;
                result = game::name_from_utf8(row->name, name);
                if (result != TSUCHI_OK)
                    return refuse(problem, result, "\"tables\": ", table->name, ", row ", row->name, ": the game cannot keep that name");
                memcpy(&key, &name, sizeof name);
                if ((!key || !t::fn::find_table_row(reinterpret_cast<void*>(object), key)) && (id_form(row->name) == IdForm::added || !row->first))
                    return refuse(problem, TSUCHI_NOT_FOUND, "\"tables\": ", table->name, ", row ", row->name, ", field from: a new row requires the name of a row to copy");
            }
            // Lengths first, so that a path into an element the array did not have before is there to change.
            for (const Json* field = row->first; field; field = field->next) {
                if (field == from || field->kind != Json::Kind::object) continue;
                result = host.datatable->resize(mod, asset, row->name, field->name, uint32_t(field->first->integer));
                if (result != TSUCHI_OK) return refuse(problem, result, "\"tables\": ", table->name, ", row ", row->name, ", field ", field->name, ": ", why_length(result));
            }
            for (const Json* field = row->first; field; field = field->next) {
                if (field == from || field->kind == Json::Kind::object) continue;
                if (field->kind == Json::Kind::text) {
                    result = host.datatable->override_text(mod, asset, row->name, field->name, field->text);
                } else {
                    TSUCHI_Value value{};
                    value.kind = field->kind == Json::Kind::boolean ? TSUCHI_VALUE_BOOLEAN : field->whole ? TSUCHI_VALUE_INTEGER : TSUCHI_VALUE_NUMBER;
                    if (field->kind == Json::Kind::boolean) value.as.boolean = field->boolean;
                    else if (field->whole) value.as.integer = field->integer; // a whole number will do where a number is wanted
                    else value.as.number = field->number;
                    result = host.datatable->override(mod, asset, row->name, field->name, &value);
                }
                if (result != TSUCHI_OK) return refuse(problem, result, "\"tables\": ", table->name, ", row ", row->name, ", field ", field->name, ": ", why(result));
            }
        }
    }
    return TSUCHI_OK;
}

// ---- "text": {"<namespace>": {"<key>": "the new text"}} ------------------------------------------------------

bool text_well_formed(const Json& section, Text& wrong) {
    if (section.kind != Json::Kind::object) return wrong.set("\"text\" is an object: a namespace, then a key and its new text"), false;
    for (const Json* space = section.first; space; space = space->next) {
        if (!object_named(*space)) return wrong.set("\"text\": a namespace is named, and holds keys and their new texts"), false;
        for (const Json* entry = space->first; entry; entry = entry->next)
            if (!entry->name || !entry->name[0] || !whole_text(*entry)) return wrong.set("\"text\": a key is named, and its new text is text"), false;
    }
    return true;
}

TSUCHI_Result text_begin(TSUCHI_Mod* mod, const Json& section, Text& problem) {
    for (const Json* space = section.first; space; space = space->next) {
        for (const Json* entry = space->first; entry; entry = entry->next) {
            const TSUCHI_Result result = host.text->override(mod, space->name, entry->name, entry->text);
            if (result != TSUCHI_OK) return refuse(problem, result, "\"text\": ", space->name, ", key ", entry->name, ": ", why(result));
        }
    }
    return TSUCHI_OK;
}

} // namespace

void declared_start() {
    add_manifest_section({"tables", tables_well_formed, tables_begin});
    add_manifest_section({"text", text_well_formed, text_begin});
}

} // namespace tsuchi
