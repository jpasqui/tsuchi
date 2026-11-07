// A piece of text the framework keeps, in room that grows to whatever it has to hold.
#pragma once
#include <stddef.h>

namespace tsuchi {

struct Text {
    char* data = nullptr;
    size_t capacity = 0;

    // The text, or "" when none has been kept.
    const char* get() const { return data ? data : ""; }
    // Makes room for `bytes`, terminator included. What was kept may be lost. False when there is no memory.
    bool reserve(size_t bytes);
    // Keeps a copy. False when there is no memory, and what was kept before stays.
    bool set(const char* text);
    // Gives the room back.
    void clear();
};

} // namespace tsuchi
