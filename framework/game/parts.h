// The parts of an actor, as the engine has them: its components. A mesh, a
// collision shape, a light, an attach point, and the parts with no place in the
// world that move the actor or decide for it.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::game {
struct CallGuard;

// True for any actor the engine has, the game's own kind or not.
bool is_engine_actor(uint64_t object);
// True for a part of an actor.
bool is_part(uint64_t object);
// True for a part with a place in the world, which can be seen and attached.
bool is_scene_part(uint64_t object);
// What the part says it belongs to, or 0. It may be gone, and need not be an actor.
uint64_t part_owner(uint64_t part);

// Calls `visit` for each part the actor owns, in the engine's order. False when
// the engine could not be asked.
bool each_part(uint64_t actor, void (*visit)(void* context, uint64_t part), void* context);

// Has the engine make a new part of a class on an actor. With a parent, which is
// one of the actor's own parts with a place in the world, the new part is attached
// under it, at a socket or bone when one is named; without, under the actor's
// root. TSUCHI_INVALID for a class that is not a class of part, a parent that is not
// the actor's, or a parent given for a part with no place in the world;
// TSUCHI_NOT_READY when the engine cannot be asked; TSUCHI_UNSUPPORTED when the engine
// made nothing or would not attach it, and nothing is left behind.
TSUCHI_Result add_part(uint64_t actor, uint64_t type, uint64_t parent, const char* socket, uint64_t& part);
// Has the engine destroy a part.
bool destroy_part(uint64_t part);
bool restore_destroy_part(uint64_t part, CallGuard guard);

// True for a part that is drawn with materials in slots: a mesh of any kind.
bool is_mesh_part(uint64_t object);
// Shader targets: meshes, particle emitter slots, projected decals, and Image brushes.
// Image widgets have one material resource and are not actor components.
bool is_shader_target(uint64_t object);
bool is_material_image(uint64_t object);
// Reflected material override array for meshes or particle emitter components.
bool material_overrides(uint64_t object, uint64_t& data, uint32_t& count);
// True for a material, of any kind.
bool is_material(uint64_t object);
// How many material slots the part has. False when the engine cannot be asked.
bool material_slots(uint64_t part, uint32_t& count);
// The material a slot is drawn with, or 0.
uint64_t slot_material(uint64_t part, uint32_t slot);
// The material put in a slot over the mesh's own, or 0 when the slot shows the mesh's own.
uint64_t slot_override(uint64_t part, uint32_t slot);
// Puts a material in a slot over the mesh's own. With 0, takes away what was put there.
bool set_slot_material(uint64_t part, uint32_t slot, uint64_t material);
bool restore_slot_material(uint64_t part, uint32_t slot, uint64_t material, CallGuard guard);

// Whether the engine draws the part. False when that cannot be told.
bool part_visible(uint64_t part, bool& visible);
// Has the engine start or stop drawing the part, and only that part.
bool set_part_visible(uint64_t part, bool visible);
bool restore_part_visible(uint64_t part, bool visible, CallGuard guard);

// A skeletal mesh, and an animation asset that can be played on one.
bool is_skeletal_part(uint64_t object);
bool is_clip(uint64_t object);
// What animates a skeletal mesh: see the animation_ constants. False when it cannot be read.
bool animation_mode(uint64_t part, uint32_t& mode);
bool set_animation_mode(uint64_t part, uint32_t mode);
bool restore_animation_mode(uint64_t part, uint32_t mode, CallGuard guard);
// One clip on a skeletal mesh, through the engine's own functions for it. Each is false when the engine could not be asked.
bool play_clip_on(uint64_t part, uint64_t clip, bool looping, float rate);
bool set_clip_rate_on(uint64_t part, float rate);
bool seek_clip_on(uint64_t part, float seconds);
bool clip_state(uint64_t part, float& position, float& rate, bool& playing);

} // namespace tsuchi::game
