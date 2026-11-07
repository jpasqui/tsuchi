// The game's names: short interned strings, stored once and referred to by number.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {

// A name as the game stores it in an object or a field.
struct Name {
    uint32_t index;
    uint32_t number; // 0, or one more than the numeric suffix: "Thing_3" has number 4
};

// The engine's own limit on a name's length, and room for the longest one as
// UTF-8 with its number and a terminator.
constexpr size_t kNameLength = 1023, kNameBytes = kNameLength * 3 + 11 + 1;

// Writes the name as UTF-8 with a terminator. False when the name cannot be
// read or does not fit; `needed` then gets the bytes it takes, terminator
// included (0 when it cannot be read). Nothing is cut short.
bool name_text(Name name, char* out, size_t capacity, size_t* needed = nullptr);

// The same for a service that gives text back: TSUCHI_OK with `needed` set, TSUCHI_CAPACITY
// with `needed` set and nothing written, or TSUCHI_NOT_FOUND for a name that cannot be read.
TSUCHI_Result name_into(Name name, char* buffer, uint64_t capacity, uint64_t* needed);

// True when the name's text is exactly `text`.
bool name_is(Name name, const char* text);

} // namespace tsuchi::game
