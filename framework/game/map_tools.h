// What the parts of the manifest's "maps" share: lines for a mod's log, put together from pieces.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "text.h"

namespace tsuchi::maps {

// Joins up to eight texts into one. False when there is no memory for it.
inline bool join(Text& out, const char* a, const char* b = "", const char* c = "", const char* d = "", const char* e = "", const char* f = "", const char* g = "",
                 const char* h = "") {
    const char* const pieces[] = {a, b, c, d, e, f, g, h};
    size_t bytes = 1;
    for (const char* piece : pieces) bytes += strlen(piece);
    if (!out.reserve(bytes)) return false;
    char* at = out.data;
    for (const char* piece : pieces) {
        const size_t length = strlen(piece);
        memcpy(at, piece, length);
        at += length;
    }
    *at = 0;
    return true;
}

inline void tell(TSUCHI_Mod* mod, const char* a, const char* b = "", const char* c = "", const char* d = "", const char* e = "", const char* f = "", const char* g = "",
                 const char* h = "") {
    Text line;
    if (join(line, a, b, c, d, e, f, g, h)) host.mod->log(mod, line.get());
    line.clear();
}

// A whole number as text, for a line of a log. `out` has room for ten digits and a terminator.
inline const char* decimal(char out[11], uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do digits[count++] = char('0' + value % 10);
    while (value /= 10);
    char* at = out;
    while (count) *at++ = digits[--count];
    *at = 0;
    return out;
}

// A placement number as text: `0x` and sixteen digits, with a terminator.
inline const char* hex(char out[19], uint64_t value) {
    out[0] = '0', out[1] = 'x', out[18] = 0;
    for (uint32_t i = 0; i < 16; ++i) out[17 - i] = "0123456789ABCDEF"[(value >> (4 * i)) & 15];
    return out;
}

} // namespace tsuchi::maps
