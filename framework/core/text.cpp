#include "text.h"

#include <string.h>

#include "registry.h"

namespace tsuchi {

bool Text::reserve(size_t bytes) {
    if (bytes <= capacity) return true;
    char* grown = static_cast<char*>(allocate(bytes));
    if (!grown) return false;
    grown[0] = 0;
    release(data);
    data = grown;
    capacity = bytes;
    return true;
}

bool Text::set(const char* text) {
    const size_t bytes = strlen(text) + 1;
    if (bytes > capacity) {
        char* grown = static_cast<char*>(allocate(bytes));
        if (!grown) return false;
        release(data);
        data = grown;
        capacity = bytes;
    }
    memcpy(data, text, bytes);
    return true;
}

void Text::clear() {
    release(data);
    data = nullptr;
    capacity = 0;
}

} // namespace tsuchi
