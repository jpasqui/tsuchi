#include <string.h>
#include "content_placements.h"
#include "ids.h"
#include "list.h"
#include "registry.h"
namespace tsuchi {
namespace {
const Kind metadata_kind{"content.placement_values"};
struct Entry { uint64_t key; char id[kIdBytes]; const Json* values; };
List<Entry> entries;
uint64_t next_key = 1;
const char* const groups[] = {"treasures", "creatures", "pikmin"};
const char* const kinds[] = {"toggle", "integer", "number", "text"};
const uint32_t codes[] = {TSUCHI_PLACEMENT_KIND_TOGGLE, TSUCHI_PLACEMENT_KIND_INTEGER,
                          TSUCHI_PLACEMENT_KIND_NUMBER, TSUCHI_PLACEMENT_KIND_TEXT};
void ended(void*, uint64_t key) {
    for (uint32_t i = 0; i < entries.count; ++i) if (entries[i].key == key) { entries.remove(&entries[i]); return; }
}
int type(const Json* definition) {
    const Json* kind = definition ? definition->find("kind") : nullptr;
    if (!kind || kind->kind != Json::Kind::text || strlen(kind->text) != kind->bytes) return -1;
    for (int i = 0; i < 4; ++i) if (!strcmp(kind->text, kinds[i])) return i;
    return -1;
}
TSUCHI_Result find(const char* kind, const char* name, const Json*& definition) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (!id_valid(kind) || (name && !valid_mod_id(name))) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < entries.count; ++i) {
        if (!id_same(kind, entries[i].id)) continue;
        definition = name ? entries[i].values->find(name) : entries[i].values;
        return definition ? TSUCHI_OK : TSUCHI_NOT_FOUND;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result default_of(const char* kind, const char* name, int wanted, const Json*& value) {
    const Json* definition = nullptr;
    const auto result = find(kind, name, definition);
    if (result != TSUCHI_OK) return result;
    if (type(definition) != wanted) return TSUCHI_INVALID;
    value = definition->find("default");
    return TSUCHI_OK;
}
}
bool content_placements_valid(const Json& content, Text& problem) {
    for (const char* group : groups) {
        const Json* all = content.find(group);
        for (const Json* each = all ? all->first : nullptr; each; each = each->next) {
            const Json* values = each->find("placement_values");
            if (!values) continue;
            if (values->kind != Json::Kind::object) return problem.set("content: placement_values is an object"), false;
            for (const Json* value = values->first; value; value = value->next) {
                const int k = type(value);
                const Json* initial = value->find("default");
                bool valid = valid_mod_id(value->name) && value->kind == Json::Kind::object && value->count == 2 && k >= 0 && initial;
                if (valid) switch (k) {
                case 0: valid = initial->kind == Json::Kind::boolean; break;
                case 1: valid = initial->kind == Json::Kind::number && initial->whole; break;
                case 2: valid = initial->kind == Json::Kind::number && initial->number >= -3.402823466e38 && initial->number <= 3.402823466e38; break;
                case 3: valid = initial->kind == Json::Kind::text && strlen(initial->text) == initial->bytes; break;
                }
                if (!valid) return problem.set("content: a placement value needs a short name, kind, and matching default"), false;
            }
        }
    }
    return true;
}
TSUCHI_Result content_placements_begin(TSUCHI_Mod* mod, const Json& content, Text&) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    for (const char* group : groups) {
        const Json* all = content.find(group);
        for (const Json* each = all ? all->first : nullptr; each; each = each->next) {
            const Json* values = each->find("placement_values");
            if (!values) continue;
            Entry entry{next_key, {}, values};
            if (!id_join(mod_id(mod), each->name, entry.id)) return TSUCHI_INVALID;
            for (uint32_t i = 0; i < entries.count; ++i) if (id_same(entry.id, entries[i].id)) return TSUCHI_BUSY;
            if (!entries.add(entry)) return TSUCHI_NO_MEMORY;
            const auto result = own(mod, metadata_kind, entry.key, ended, nullptr);
            if (result != TSUCHI_OK) { entries.remove(&entries[entries.count - 1]); return result; }
            ++next_key;
        }
    }
    return TSUCHI_OK;
}
TSUCHI_Result content_placement_values(const char* kind, char* buffer, uint64_t capacity, uint64_t* out) {
    const Json* values = nullptr;
    auto result = find(kind, nullptr, values);
    if (result != TSUCHI_OK) return result;
    uint64_t size = 1;
    for (const Json* value = values->first; value; value = value->next) size += strlen(value->name) + 1;
    *out = size;
    if (size > capacity) return TSUCHI_CAPACITY;
    char* at = buffer;
    for (const Json* value = values->first; value; value = value->next) {
        const size_t bytes = strlen(value->name) + 1;
        memcpy(at, value->name, bytes); at += bytes;
    }
    *at = 0;
    return TSUCHI_OK;
}
TSUCHI_Result content_placement_kind(const char* kind, const char* name, TSUCHI_PlacementKind* out) {
    const Json* definition = nullptr;
    const auto result = find(kind, name, definition);
    if (result != TSUCHI_OK) return result;
    *out = codes[type(definition)];
    return TSUCHI_OK;
}
TSUCHI_Result content_placement_toggle(const char* kind, const char* name, bool* out) {
    const Json* value = nullptr; const auto result = default_of(kind, name, 0, value);
    if (result == TSUCHI_OK) *out = value->boolean;
    return result;
}
TSUCHI_Result content_placement_integer(const char* kind, const char* name, int64_t* out) {
    const Json* value = nullptr; const auto result = default_of(kind, name, 1, value);
    if (result == TSUCHI_OK) *out = value->integer;
    return result;
}
TSUCHI_Result content_placement_number(const char* kind, const char* name, float* out) {
    const Json* value = nullptr; const auto result = default_of(kind, name, 2, value);
    if (result == TSUCHI_OK) *out = float(value->number);
    return result;
}
TSUCHI_Result content_placement_text(const char* kind, const char* name, char* buffer, uint64_t capacity, uint64_t* out) {
    const Json* value = nullptr; const auto result = default_of(kind, name, 3, value);
    if (result != TSUCHI_OK) return result;
    *out = value->bytes + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, value->text, size_t(*out));
    return TSUCHI_OK;
}
}
