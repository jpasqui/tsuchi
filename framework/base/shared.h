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

} // namespace tsuchi::game
