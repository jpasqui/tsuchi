#include "ids.h"

#include <string.h>

namespace tsuchi {
namespace {

const Kind kAddedName{"id"};
uint64_t next_key = 1;

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool mark(char c) { return c == '.' || c == '_' || c == '-'; }
char folded(char c) { return upper(c) ? char(c - 'A' + 'a') : c; }

// `length` bytes spelled as a mod's ID is.
bool spelled_as_mod_id(const char* text, size_t length) {
    if (length < 1 || length >= kModIdBytes) return false;
    for (size_t i = 0; i < length; ++i)
        if (!lower(text[i]) && !digit(text[i]) && !mark(text[i])) return false;
    const char first = text[0], last = text[length - 1];
    return (lower(first) || digit(first)) && (lower(last) || digit(last));
}

// The mod's name was taken back, or the mod stopped.
void forget(void* context, uint64_t key) {
    auto& vocabulary = *static_cast<Vocabulary*>(context);
    for (uint32_t i = vocabulary.added.count; i-- > 0;) {
        if (vocabulary.added[i].key != key) continue;
        vocabulary.added.remove(&vocabulary.added[i]);
        if (!vocabulary.added.count) vocabulary.added.clear();
        return;
    }
}

const Vocabulary::Added* find_added(const Vocabulary& vocabulary, const char* id) {
    for (uint32_t i = 0; i < vocabulary.added.count; ++i)
        if (!strcmp(vocabulary.added.items[i].id, id)) return &vocabulary.added.items[i];
    return nullptr;
}

const char* name_of(const Vocabulary& vocabulary, uint32_t value) {
    for (uint32_t i = 0; i < vocabulary.own_count; ++i)
        if (vocabulary.own[i].value == value) return vocabulary.own[i].name;
    for (uint32_t i = 0; i < vocabulary.added.count; ++i)
        if (vocabulary.added.items[i].value == value) return vocabulary.added.items[i].id;
    return nullptr;
}

} // namespace

IdForm id_form(const char* id, size_t* colon, uint32_t* number) {
    if (!id) return IdForm::malformed;
    size_t length = 0, at_colon = 0, colons = 0;
    for (; length < kIdBytes && id[length]; ++length) {
        if (id[length] != ':') continue;
        if (!colons++) at_colon = length;
    }
    if (!length || length == kIdBytes) return IdForm::malformed;
    if (id[0] == '#') {
        // A number as it would be written back: no zeros in front, and no more than 32 bits hold.
        if (length < 2 || length > 11 || (id[1] == '0' && length > 2)) return IdForm::malformed;
        uint64_t value = 0;
        for (size_t i = 1; i < length; ++i) {
            if (!digit(id[i])) return IdForm::malformed;
            value = value * 10 + uint64_t(id[i] - '0');
        }
        if (value > 0xFFFFFFFFull) return IdForm::malformed;
        if (number) *number = uint32_t(value);
        return IdForm::placeholder;
    }
    if (colons) {
        if (colons != 1 || !spelled_as_mod_id(id, at_colon) || !spelled_as_mod_id(id + at_colon + 1, length - at_colon - 1))
            return IdForm::malformed;
        if (colon) *colon = at_colon;
        return IdForm::added;
    }
    for (size_t i = 0; i < length; ++i)
        if (!lower(id[i]) && !upper(id[i]) && !digit(id[i]) && !mark(id[i])) return IdForm::malformed;
    return IdForm::plain;
}

bool id_same(const char* a, const char* b) {
    const IdForm form = id_form(a);
    if (form == IdForm::malformed || id_form(b) != form) return false;
    if (form != IdForm::plain) return !strcmp(a, b);
    for (;; ++a, ++b) {
        if (folded(*a) != folded(*b)) return false;
        if (!*a) return true;
    }
}

bool id_join(const char* mod, const char* name, char out[kIdBytes]) {
    if (!valid_mod_id(mod) || !valid_mod_id(name)) return false;
    const size_t first = strlen(mod), second = strlen(name);
    memcpy(out, mod, first);
    out[first] = ':';
    memcpy(out + first + 1, name, second + 1);
    return true;
}

bool id_split(const char* id, char mod[kModIdBytes], const char** name) {
    size_t colon = 0;
    if (id_form(id, &colon) != IdForm::added) return false;
    memcpy(mod, id, colon);
    mod[colon] = 0;
    *name = id + colon + 1;
    return true;
}

void id_placeholder(uint32_t number, char out[kIdBytes]) {
    char digits[10];
    size_t count = 0;
    do digits[count++] = char('0' + number % 10);
    while (number /= 10);
    size_t at = 0;
    out[at++] = '#';
    while (count) out[at++] = digits[--count];
    out[at] = 0;
}

TSUCHI_Result id_value(const Vocabulary& vocabulary, const char* id, uint32_t* out) {
    uint32_t number = 0;
    switch (id_form(id, nullptr, &number)) {
    case IdForm::malformed:
        return TSUCHI_INVALID;
    case IdForm::placeholder:
        if (number > vocabulary.most) return TSUCHI_NOT_FOUND;
        *out = number;
        return TSUCHI_OK;
    case IdForm::added:
        if (const Vocabulary::Added* found = find_added(vocabulary, id)) {
            *out = found->value;
            return TSUCHI_OK;
        }
        return TSUCHI_NOT_FOUND;
    case IdForm::plain:
        for (uint32_t i = 0; i < vocabulary.own_count; ++i) {
            if (!id_same(vocabulary.own[i].name, id)) continue;
            *out = vocabulary.own[i].value;
            return TSUCHI_OK;
        }
        return TSUCHI_NOT_FOUND;
    }
    return TSUCHI_INVALID;
}

void id_of(const Vocabulary& vocabulary, uint32_t value, char out[kIdBytes]) {
    if (const char* name = name_of(vocabulary, value)) memcpy(out, name, strlen(name) + 1);
    else id_placeholder(value, out);
}

TSUCHI_Result id_into(const Vocabulary& vocabulary, uint32_t value, char* buffer, uint64_t capacity, uint64_t* out) {
    char id[kIdBytes];
    id_of(vocabulary, value, id);
    const size_t bytes = strlen(id) + 1;
    *out = bytes;
    if (!buffer || capacity < bytes) return TSUCHI_CAPACITY;
    memcpy(buffer, id, bytes);
    return TSUCHI_OK;
}

bool id_named(const Vocabulary& vocabulary, uint32_t value) { return name_of(vocabulary, value) != nullptr; }

TSUCHI_Result id_add(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name, uint32_t value) {
    Vocabulary::Added entry{};
    if (!mod_usable(mod) || !name || !id_join(mod_id(mod), name, entry.id) || value > vocabulary.most) return TSUCHI_INVALID;
    if (name_of(vocabulary, value) || find_added(vocabulary, entry.id)) return TSUCHI_BUSY;
    for (uint32_t i = 0; i < vocabulary.given.count; ++i)
        if (vocabulary.given[i].value == value && strcmp(vocabulary.given[i].id, entry.id)) return TSUCHI_BUSY;
    entry.value = value;
    entry.key = next_key;
    if (!vocabulary.added.add(entry)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result owned = own(mod, kAddedName, entry.key, forget, &vocabulary);
    if (owned != TSUCHI_OK) {
        forget(&vocabulary, entry.key);
        return owned;
    }
    ++next_key;
    return TSUCHI_OK;
}

TSUCHI_Result id_remove(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name) {
    char id[kIdBytes];
    if (!mod_usable(mod) || !name || !id_join(mod_id(mod), name, id)) return TSUCHI_NOT_FOUND;
    const Vocabulary::Added* found = find_added(vocabulary, id);
    if (!found) return TSUCHI_NOT_FOUND;
    const uint64_t key = found->key;
    disown(mod, kAddedName, key);
    forget(&vocabulary, key);
    return TSUCHI_OK;
}

TSUCHI_Result id_allocate(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name,
                          uint32_t first, uint32_t last, uint32_t* out) {
    Vocabulary::Given entry{};
    if (!out || !mod_usable(mod) || !id_join(mod_id(mod), name, entry.id) || first > last || last > vocabulary.most)
        return TSUCHI_INVALID;
    for (uint32_t i = 0; i < vocabulary.given.count; ++i) {
        const auto& given = vocabulary.given[i];
        if (strcmp(given.id, entry.id)) continue;
        if (given.value < first || given.value > last) return TSUCHI_INVALID;
        if (const auto* active = find_added(vocabulary, entry.id)) {
            *out = active->value;
            return TSUCHI_OK;
        }
        const TSUCHI_Result result = id_add(vocabulary, mod, name, given.value);
        if (result == TSUCHI_OK) *out = given.value;
        return result;
    }
    uint32_t value = first;
    for (;;) {
        bool used = id_named(vocabulary, value);
        for (uint32_t i = 0; !used && i < vocabulary.given.count; ++i) used = vocabulary.given[i].value == value;
        if (!used) break;
        if (value == last) return TSUCHI_CAPACITY;
        ++value;
    }
    if (!vocabulary.given.reserve(vocabulary.given.count + 1)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result result = id_add(vocabulary, mod, name, value);
    if (result != TSUCHI_OK) return result;
    entry.value = value;
    vocabulary.given.add(entry); // room was reserved before publishing the active name
    *out = value;
    return TSUCHI_OK;
}

} // namespace tsuchi
