// Global sound-engine parameters. Accepted writes are asynchronous; the internal
// queue observer keeps cleanup alive until the native command has been consumed.
#pragma once
#include "tsuchi/core.h"
#include "audio_bus_volume.h"

namespace tsuchi {
TSUCHI_Result audio_parameter(const char* name, float* value);
TSUCHI_Result audio_hold_parameter(TSUCHI_Mod* mod, const char* name, float value);
TSUCHI_Result audio_release_parameter(TSUCHI_Mod* mod, const char* name);
void audio_parameters_start(TSUCHI_Mod* framework);
// Shared native observer: a zero epoch means the engine is unavailable or terminating.
TSUCHI_Result audio_parameter_observers();
uint64_t audio_parameter_engine_epoch();
}
