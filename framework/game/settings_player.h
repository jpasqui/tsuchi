#pragma once
#include "mods.h"
namespace tsuchi {
void settings_player_prepare(ModRecord& record);
void settings_player_changed(ModRecord& record, const Setting& setting);
}
