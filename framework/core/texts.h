// Texts one after another, each with its terminator, in room that grows: the
// names in a folder, the paths of a slot's files. A list of texts crosses to a
// mod in the same shape, with one more terminator after the last.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

struct Texts {
    char* data = nullptr;
    size_t used = 0, capacity = 0;

    // Adds one text, made of up to four parts. A part may be one of the texts already here. False when there is no memory.
    bool add(const char* a, const char* b = "", const char* c = "", const char* d = "");
    // The first text, and the one after a text; null after the last.
    const char* first() const { return used ? data : nullptr; }
    const char* after(const char* text) const;
    // Copies them into a caller's buffer as a list of texts crosses, and says how many bytes that takes.
    // TSUCHI_CAPACITY when the buffer is too small: nothing is copied, and the size is still given.
    TSUCHI_Result give(char* buffer, uint64_t room, uint64_t* out) const;
    // Gives the room back.
    void clear();
};

} // namespace tsuchi
