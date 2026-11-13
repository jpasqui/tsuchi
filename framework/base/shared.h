// What the engine shares by counting its sharers: a piece of text, for one.
#pragma once
#include <stdint.h>
#include <string.h>

#include "game.h"

namespace tsuchi::game {

// Lets go of one share, as the engine does: the thing goes when nobody holds
// it, and its counter when nobody watches it either. Nothing for a counter of 0.
void drop_shared(uint64_t counter);

// A piece of text as the engine hands it around, by value: what it shares, and
// who counts the sharers. A function that gives one back writes it where the
// caller says, which a C++ function returning this type does too.
struct EngineText {
    alignas(8) uint8_t bytes[table::text_value::size] = {};
    uint64_t counter() const {
        uint64_t value = 0;
        memcpy(&value, bytes + table::text_value::counter, 8);
        return value;
    }
};

// An engine array as a function hands one back by value: its block, how many elements it holds, and how many it
// has room for. The engine's array type has a destructor, so the caller says where it goes (x8); the destructor
// here is what makes a C++ function returning this type do the same. The block is the engine's: whoever takes it
// releases it with the engine's allocator.
struct EngineArray {
    void* data = nullptr;
    int32_t count = 0;
    int32_t max = 0;
    ~EngineArray() {}
};

} // namespace tsuchi::game
