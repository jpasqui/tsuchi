// Native particle operations. The service owns every playback through Holds.
#pragma once
#include <stdint.h>
#include "tsuchi/effects.h"
namespace tsuchi {
struct EffectAsset { uint64_t handle; char id[TSUCHI_ID_SIZE]; };
using EffectVisit = bool (*)(void*, const EffectAsset&);
bool effects_native_ready();
uint64_t effects_native_world();
uint64_t effects_native_revision();
TSUCHI_Result effects_native_catalog(EffectVisit visit, void* context);
TSUCHI_Result effects_native_create(uint64_t asset, uint64_t parent, const TSUCHI_Vec3& place, uint64_t& emitter, uint64_t& root);
bool effects_native_alive(uint64_t emitter, uint64_t parent, uint64_t root);
void effects_native_stop(uint64_t emitter);
TSUCHI_Result effects_native_parameter(uint64_t emitter, const char* name, TSUCHI_ParameterKind kind, const TSUCHI_ParticleValue& value);
void effects_native_start(TSUCHI_Mod* self);
void effects_finished_native(uint64_t emitter);
void effects_start(TSUCHI_Mod* self);
} // namespace tsuchi
