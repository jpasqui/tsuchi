// JSON, read into a tree. A mod's manifest is written in it.
//
// The reader is strict: the text has to be exactly JSON, in UTF-8, with no
// comments, no trailing commas, and no name used twice in one object. A text
// that is not is refused whole, with the place and the reason.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tsuchi {

struct Json {
    enum class Kind : uint8_t { null, boolean, number, text, list, object };
    Kind kind;
    bool boolean;      // a boolean's value
    bool whole;        // a number written with no fraction and no exponent that fits `integer`
    uint32_t count;    // how many values a list or an object holds
    int64_t integer;   // a whole number's value
    double number;     // any number's value: exact for one of up to 19 digits scaled by no more than 22 places, and close otherwise
    const char* text;  // a text's value, as UTF-8 with a terminator. It may hold a zero inside: see `bytes`.
    size_t bytes;      // how long a text is, the terminator not counted
    const char* name;  // the name this value has in the object it belongs to
    const Json* first; // a list's or an object's first value
    const Json* next;  // the value after this one in the same list or object

    // An object's value by its name, or null.
    const Json* find(const char* name) const;
    // A list's or an object's value by its place, counted from 0, or null.
    const Json* at(uint32_t index) const;
};

// Where reading stopped, and why. Lines and columns count from 1, columns in bytes.
struct JsonError {
    uint32_t line, column;
    const char* what;
    bool allocation_failure = false; // independent of the diagnostic text and position
};

// Reads JSON text. Null when it is not JSON, or there is no memory for the tree; `error`, when given, says which.
// The tree is the reader's own memory, and stays until it is released.
const Json* read_json(const char* text, size_t bytes, JsonError* error = nullptr);
void release_json(const Json* root);

} // namespace tsuchi
