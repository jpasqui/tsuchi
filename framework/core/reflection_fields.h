// Owned scalar reflection writes. Included by the reflection backend and its focused fixture.
#pragma once
#include <string.h>
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "list.h"
#include "text.h"

namespace tsuchi::field_holds {
using game::Place;
using game::Value;
using game::ValueKind;
struct Record {
    uint64_t id = 0, object = 0, property = 0;
    TSUCHI_Mod* mod = nullptr;
    Text path, array, tail, identity;
    int64_t key = 0;
    Value original, written;
};
inline List<Record*> records;
inline uint64_t next_id = 1;
inline const Kind kind{"reflection.field"};

inline bool scalar(const Value& value) {
    return value.kind == ValueKind::boolean || value.kind == ValueKind::integer ||
        (value.kind == ValueKind::number && value.number - value.number == 0);
}
inline bool equal(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == ValueKind::boolean) return a.boolean == b.boolean;
    if (a.kind == ValueKind::integer) return a.integer == b.integer;
    return a.kind == ValueKind::number && a.number == b.number;
}
inline bool convert(const TSUCHI_Value& from, Value& to) {
    to.kind = ValueKind(from.kind);
    if (to.kind == ValueKind::boolean) to.boolean = from.as.boolean;
    else if (to.kind == ValueKind::integer) to.integer = from.as.integer;
    else if (to.kind == ValueKind::number) to.number = from.as.number;
    return scalar(to);
}
inline void destroy(Record* record) {
    record->path.clear(); record->array.clear(); record->tail.clear(); record->identity.clear(); release(record);
}
inline Record* find(uint64_t id) {
    for (uint32_t i = 0; i < records.count; ++i) if (records[i]->id == id) return records[i];
    return nullptr;
}
inline const char* find_char(const char* text, char wanted) {
    for (; *text; ++text) if (*text == wanted) return text;
    return nullptr;
}
inline bool same_path(const char* a, const char* b) {
    while (*a && *b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a;
        const char y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        ++a; ++b;
    }
    return *a == *b;
}
// Only structs can be crossed. In particular, an object-reference replacement
// must never cause a hold to restore a different object's field.
inline TSUCHI_Result struct_path(uint64_t type, uint64_t base, const char* path, Place& out) {
    const size_t length = strlen(path);
    if (!length) return TSUCHI_INVALID;
    if (length > 1023) return TSUCHI_CAPACITY;
    char prefix[1024];
    for (size_t i = 0; i < length; ++i) {
        if (path[i] == '[' || path[i] == ']') return TSUCHI_UNSUPPORTED;
        if (path[i] != '.') continue;
        memcpy(prefix, path, i); prefix[i] = 0;
        Place parent; Value value;
        auto result = game::find_place(type, base, prefix, parent);
        if (result != TSUCHI_OK) return result;
        result = game::read_value(parent, value);
        if (result != TSUCHI_OK) return result;
        if (value.kind != ValueKind::structure) return TSUCHI_UNSUPPORTED;
    }
    return game::find_place(type, base, path, out);
}
inline TSUCHI_Result array_element(uint64_t type, uint64_t base, const char* array, uint32_t index,
                                   uint64_t& row_type, uint64_t& row_base) {
    char path[1040], digits[12]; uint32_t used = 0;
    do { digits[used++] = char('0' + index % 10); index /= 10; } while (index);
    size_t at = strlen(array); memcpy(path, array, at); path[at++] = '[';
    while (used) path[at++] = digits[--used];
    path[at++] = ']'; path[at] = 0;
    Place element;
    const auto result = game::find_place(type, base, path, element);
    return result == TSUCHI_OK ? game::struct_at(element, row_type, row_base) : result;
}
inline TSUCHI_Result locate(const Record& record, Place& out) {
    const uint64_t base = game::from_handle(record.object), type = base ? game::type_of(base) : 0;
    if (!type) return TSUCHI_NOT_FOUND;
    if (!*record.array.get()) return struct_path(type, base, record.path.get(), out);
    Place array; Value list;
    auto result = struct_path(type, base, record.array.get(), array);
    if (result != TSUCHI_OK) return result;
    result = game::read_value(array, list);
    if (result != TSUCHI_OK) return result;
    if (list.kind != ValueKind::array) return TSUCHI_UNSUPPORTED;
    if (list.count > 1000000) return TSUCHI_CAPACITY;
    bool found = false;
    for (uint32_t i = 0; i < list.count; ++i) {
        uint64_t row_type = 0, row_base = 0; Place key; Value value;
        result = array_element(type, base, record.array.get(), i, row_type, row_base);
        if (result != TSUCHI_OK) return result;
        result = struct_path(row_type, row_base, record.identity.get(), key);
        if (result != TSUCHI_OK) return result;
        result = game::read_value(key, value);
        if (result != TSUCHI_OK) return result;
        if (value.kind != ValueKind::integer) return TSUCHI_UNSUPPORTED;
        if (value.integer != record.key) continue;
        if (found) return TSUCHI_BUSY;
        result = struct_path(row_type, row_base, record.tail.get(), out);
        if (result != TSUCHI_OK) return result;
        // An alias of the identity cannot be changed through the hold either.
        if (out.address == key.address && out.property == key.property) return TSUCHI_INVALID;
        found = true;
    }
    return found ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}
inline TSUCHI_Result configure(Record& record, const char* path, const char* identity) {
    if (strlen(path) > 1023 || strlen(identity) > 1023) return TSUCHI_CAPACITY;
    if (!record.path.set(path)) return TSUCHI_NO_MEMORY;
    const char* open = find_char(path, '[');
    if (!open) return *identity ? TSUCHI_INVALID : TSUCHI_OK;
    if (!*identity || find_char(open + 1, '[')) return TSUCHI_UNSUPPORTED;
    const char* close = find_char(open, ']');
    if (!close || close == open + 1 || close[1] != '.' || !close[2]) return TSUCHI_INVALID;
    uint32_t index = 0;
    for (const char* p = open + 1; p < close; ++p) {
        if (*p < '0' || *p > '9' || index > 1000000) return TSUCHI_INVALID;
        index = index * 10 + uint32_t(*p - '0');
    }
    char prefix[1024]; const size_t size = size_t(open - path);
    memcpy(prefix, path, size); prefix[size] = 0;
    if (!record.array.set(prefix) || !record.tail.set(close + 2) || !record.identity.set(identity)) return TSUCHI_NO_MEMORY;
    const uint64_t base = game::from_handle(record.object), type = base ? game::type_of(base) : 0;
    if (!type) return TSUCHI_NOT_FOUND;
    Place array;
    auto result = struct_path(type, base, prefix, array);
    if (result != TSUCHI_OK) return result;
    uint64_t row_type = 0, row_base = 0; Place key; Value value;
    result = array_element(type, base, prefix, index, row_type, row_base);
    if (result != TSUCHI_OK) return result;
    result = struct_path(row_type, row_base, identity, key);
    if (result != TSUCHI_OK) return result;
    result = game::read_value(key, value);
    if (result != TSUCHI_OK) return result;
    if (value.kind != ValueKind::integer) return TSUCHI_UNSUPPORTED;
    record.key = value.integer;
    return TSUCHI_OK;
}
inline TSUCHI_Result unchanged(const Record& record, Place& place) {
    auto result = locate(record, place);
    if (result != TSUCHI_OK) return result;
    if (place.property != record.property) return TSUCHI_UNSUPPORTED;
    Value current;
    result = game::read_value(place, current);
    return result == TSUCHI_OK && !equal(current, record.written) ? TSUCHI_BUSY : result;
}
inline void remove(Record* record) {
    for (uint32_t i = 0; i < records.count; ++i) if (records[i] == record) { records.remove(&records[i]); break; }
    destroy(record);
}
inline void stopped(void*, uint64_t id) {
    Record* record = find(id); if (!record) return;
    Place place;
    if (unchanged(*record, place) == TSUCHI_OK) game::write_value(place, record->original);
    remove(record);
}
inline TSUCHI_Result hold(TSUCHI_Mod* mod, TSUCHI_Object object, const char* path,
                          const TSUCHI_Value* wanted, const char* identity, TSUCHI_FieldHold* out) {
    Value value;
    if (!convert(*wanted, value)) return TSUCHI_INVALID;
    if (records.count >= 4096) return TSUCHI_CAPACITY;
    auto* record = static_cast<Record*>(allocate(sizeof(Record)));
    if (!record) return TSUCHI_NO_MEMORY;
    *record = Record{}; record->mod = mod; record->object = object.id;
    auto result = configure(*record, path, identity);
    Place place;
    if (result == TSUCHI_OK) result = locate(*record, place);
    for (uint32_t i = 0; result == TSUCHI_OK && i < records.count; ++i) {
        const auto& existing = *records[i];
        // Different root lives cannot alias: reference traversal is prohibited.
        if (existing.object != record->object) continue;
        // Different unique keys in the same array need no full row search.
        if (*record->array.get() && record->key != existing.key &&
            same_path(record->array.get(), existing.array.get()) &&
            same_path(record->identity.get(), existing.identity.get())) continue;
        Place other;
        if (locate(*records[i], other) == TSUCHI_OK && other.address == place.address && other.property == place.property) result = TSUCHI_BUSY;
    }
    if (result == TSUCHI_OK) result = game::read_value(place, record->original);
    if (result == TSUCHI_OK && !scalar(record->original)) result = TSUCHI_UNSUPPORTED;
    if (result == TSUCHI_OK && !records.reserve(records.count + 1)) result = TSUCHI_NO_MEMORY;
    if (result != TSUCHI_OK) { destroy(record); return result; }
    record->id = next_id++; record->property = place.property;
    result = own(mod, kind, record->id, stopped, nullptr);
    if (result != TSUCHI_OK) { destroy(record); return result; }
    result = game::write_value(place, value);
    if (result == TSUCHI_OK) result = game::read_value(place, record->written);
    if (result == TSUCHI_OK && !scalar(record->written)) result = TSUCHI_INVALID;
    if (result != TSUCHI_OK) {
        // Scalar writes neither allocate nor call game code. Undo a failed readback.
        game::write_value(place, record->original);
        disown(mod, kind, record->id); destroy(record); return result;
    }
    records.add(record); out->id = record->id;
    return TSUCHI_OK;
}
inline TSUCHI_Result update(TSUCHI_Mod* mod, TSUCHI_FieldHold hold, const TSUCHI_Value* wanted) {
    Record* record = find(hold.id);
    if (!record) return TSUCHI_NOT_FOUND;
    if (record->mod != mod) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, kind, hold.id)) return TSUCHI_BUSY;
    Value value; if (!convert(*wanted, value)) return TSUCHI_INVALID;
    Place place; auto result = unchanged(*record, place);
    if (result != TSUCHI_OK) return result;
    result = game::write_value(place, value);
    if (result != TSUCHI_OK) return result;
    Value actual; result = game::read_value(place, actual);
    if (result == TSUCHI_OK && !scalar(actual)) result = TSUCHI_INVALID;
    if (result == TSUCHI_OK) record->written = actual;
    else game::write_value(place, record->written);
    return result;
}
inline TSUCHI_Result end(TSUCHI_Mod* mod, TSUCHI_FieldHold hold) {
    Record* record = find(hold.id);
    if (!record) return TSUCHI_NOT_FOUND;
    if (record->mod != mod) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, kind, hold.id)) return TSUCHI_BUSY;
    Place place; auto result = unchanged(*record, place);
    if (result == TSUCHI_OK) result = game::write_value(place, record->original);
    if (result != TSUCHI_OK && result != TSUCHI_NOT_FOUND) return result;
    disown(mod, kind, hold.id); remove(record); return TSUCHI_OK;
}
} // namespace tsuchi::field_holds
