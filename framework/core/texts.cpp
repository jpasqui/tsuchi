#include "texts.h"

#include <string.h>

#include "registry.h"

namespace tsuchi {

bool Texts::add(const char* a, const char* b, const char* c, const char* d) {
    const char* parts[] = {a, b, c, d};
    size_t bytes = 1;
    for (const char* part : parts) bytes += strlen(part);
    char* into = data;
    if (used + bytes > capacity) {
        size_t next = capacity ? capacity * 2 : 256;
        while (next < used + bytes) next *= 2;
        into = static_cast<char*>(allocate(next));
        if (!into) return false;
        if (used) memcpy(into, data, used);
        capacity = next;
    }
    char* at = into + used;
    for (const char* part : parts) { // before the old room goes: a part may be in it
        const size_t length = strlen(part);
        memmove(at, part, length);
        at += length;
    }
    *at = 0;
    used += bytes;
    if (into != data) release(data), data = into;
    return true;
}

const char* Texts::after(const char* text) const {
    const char* next = text + strlen(text) + 1;
    return next < data + used ? next : nullptr;
}

TSUCHI_Result Texts::give(char* buffer, uint64_t room, uint64_t* out) const {
    *out = used + 1; // one more terminator ends the list
    if (room < *out) return TSUCHI_CAPACITY;
    if (used) memcpy(buffer, data, used);
    buffer[used] = 0;
    return TSUCHI_OK;
}

void Texts::clear() {
    release(data);
    *this = Texts{};
}

} // namespace tsuchi
