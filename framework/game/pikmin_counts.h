// Added Pikmin in the game's own counts of its Pikmin.
//
// On each world step the game's Pikmin count themselves into rows by type, leaf,
// state and player, and the counter's update publishes that bank. A first or
// forced update recounts them all. A type outside the game's own goes into row 0,
// which no total adds.
// The framework counts each added color as the game adds it to row 0, and
// adds those counts and the colors' Onion stock to every total the game asks for.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"

namespace tsuchi::game {
TSUCHI_Result pikmin_counts_start(TSUCHI_Mod* self);
// Off until the save's record holds an added color. While off, every hook passes straight to the game.
void pikmin_counts_enable(bool on);
// One added color's count, as the counter's query gives it for a stock type's row: leaf 0 for
// every leaf, state 0 for the field and the Onion, player 0 for every player. -1 while off, or
// when the arguments are outside the counter.
int64_t pikmin_counts_color(uint8_t color, int32_t leaf, int32_t state, int32_t player);
// While one is alive on the game's thread, a query for row 0 answers for this added
// color alone. The throw list's icons ask for their type's count this way.
class PikminCountScope {
public:
    explicit PikminCountScope(uint8_t color);
    ~PikminCountScope();
    PikminCountScope(const PikminCountScope&) = delete;
    PikminCountScope& operator=(const PikminCountScope&) = delete;
private:
    uint8_t outer_color_;
    uint64_t outer_thread_;
};
} // namespace tsuchi::game
