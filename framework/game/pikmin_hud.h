// The two permanent added colors use native throw-list entries and donor icons.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"

namespace tsuchi::game {
TSUCHI_Result pikmin_hud_install(TSUCHI_Mod* self, uint8_t color, uint8_t donor);
TSUCHI_Result pikmin_hud_start(TSUCHI_Mod* self);
TSUCHI_Result pikmin_hud_register(uint8_t color, uint8_t donor);
// The texture declaration and every painted widget use the shared hold tables.
TSUCHI_Result pikmin_hud_icon(TSUCHI_Mod* owner, uint8_t color, uint64_t texture);
TSUCHI_Result pikmin_hud_icon_release(TSUCHI_Mod* owner, uint8_t color);
void pikmin_hud_refresh();
bool pikmin_hud_ready(uint8_t color);
// Toggles occupied by added Pikmin cannot be borrowed by another icon family.
bool pikmin_hud_toggle_reserved(uint8_t toggle);
bool pikmin_hud_icon_owned(uint64_t icon);
void pikmin_hud_icon_guard(bool (*owned)(uint64_t icon));
} // namespace tsuchi::game
