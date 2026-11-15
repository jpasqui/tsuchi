#pragma once
#include "tsuchi/core.h"

namespace tsuchi {
TSUCHI_Result audio_hold_bus_volume(TSUCHI_Mod* mod, uint32_t bus, float volume);
TSUCHI_Result audio_release_bus_volume(TSUCHI_Mod* mod, uint32_t bus);
void audio_bus_volumes_start(TSUCHI_Mod* framework);
}
