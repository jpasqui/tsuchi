// Entities shown along timed pose tracks that a mod feeds (entity.hold_track and the calls with it).
#pragma once
#include <stdint.h>

#include "tsuchi/entity.h"

namespace tsuchi {

TSUCHI_Result track_hold(TSUCHI_Mod* mod, TSUCHI_Actor actor, uint32_t key);
TSUCHI_Result track_release(TSUCHI_Mod* mod, TSUCHI_Actor actor);
TSUCHI_Result track_feed(TSUCHI_Mod* mod, const void* data, uint64_t size, uint32_t* out);
TSUCHI_Result track_show(TSUCHI_Mod* mod, float now, uint32_t* out);
TSUCHI_Result track_read(TSUCHI_Mod* mod, uint32_t key, TSUCHI_Track* out);
TSUCHI_Result track_forget(TSUCHI_Mod* mod, uint32_t key);
// entity.read_poses: one record of the track layout, without a key, for each handle.
TSUCHI_Result track_read_poses(const void* actors, uint64_t size, void* buffer, uint64_t capacity, uint64_t* out);
constexpr uint64_t kPoseRecordBytes = 31;
constexpr uint64_t kPosesMost = 512;

// The arithmetic, for tests: where a track is at `now`, from its newest position, its velocity and that
// position's time.
void track_ahead(const float location[3], const float velocity[3], float time, float now, float out[3]);

} // namespace tsuchi
