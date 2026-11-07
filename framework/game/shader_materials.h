// The manifest's "shaders": a mod's own materials drawn with the shaders the mod brings.
//
// A material a mod made from scratch is, in the mod, an instance of one of the game's materials and, in the mod's
// shader set, the pixel shaders compiled from it for that game material's shader map (shader_file.h). The game
// material's package is loaded a second time, the copy is given a shader map of its own with the mod's shaders
// in it, and wherever a mesh would be drawn with the mod's instance, a stand-in made from the copy is put in the
// slot.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"
#include "cost_clock.h"
#include "reflect.h"

namespace tsuchi {

// Why a material was left with the game material's own shaders.
enum class ShaderFailure : uint32_t {
    none,
    no_copy,        // the game material's package could not be loaded a second time, or the copy did not come
    copy_shape,     // the copy is not laid out as a material with one shader map of its own
    other_shaders,  // the copy's shader map is not the one the set was compiled for
    no_renderer,    // the game's renderer was not found where the game table says
    shader_refused, // the renderer made nothing of one of the shaders
    no_memory,
    no_names,       // the engine gave no number for a name of the material's own description
};

// What has been done for mods' shader sets since the framework started.
struct ShaderCounts {
    uint32_t sets, materials;         // of running mods, now
    uint32_t material_searches;      // full object walks to resolve a missing material or an awaited copy
    uint32_t files_refused;           // files that were no shader set, or were damaged
    uint32_t sets_refused;            // sets made for other shaders than the game has, or for a material it does not have
    uint32_t copies_asked;            // second copies of a game material's package asked of the engine
    uint32_t copies_ready;            // copies that have a shader map of their own with the mod's shaders in it
    uint32_t failed;                  // materials left with the game material's shaders
    ShaderFailure last_failure;       // why the last of them was
    uint32_t shaders_made;            // shaders the game's renderer made from mods' files
    uint32_t descriptions;            // descriptions of a material's own that were built
    uint32_t stand_ins;               // instances made with copied parameters, including a later cancelled publication
    uint32_t parts_asked;             // meshes asked for the materials of their slots
    uint32_t slots_set, slots_put_back; // slots given a stand-in, and given back what they had
    uint32_t level_requests;          // times the engine was asked to load a game material's package as a level, taken or not
    uint32_t full_walks;              // materials whose parents reached a mod's instance, so were checked in full
    uint32_t walks, walk_pieces;      // walks over the engine's objects for meshes finished, and the steps' pieces of them
    CostTotal steps;                  // the shader step's own time
};
ShaderCounts shader_counts();
// A count that changes whenever what a material is drawn as may have changed: a stand-in made or let go, a copy
// ready or given up. While it and a slot stay the same, what the slot shows needs no resolving again.
uint32_t shader_generation();

// What a material is drawn as in the loaded world: the stand-in for it when it is a material of a running mod's
// shader set whose shaders are in place, and the material itself otherwise.
uint64_t drawn_as(uint64_t material);
// Restoration alone: stop native resolution when the owner or shader provider leaves.
bool drawn_as_restoring(uint64_t material, game::CallGuard guard, uint64_t& shown);
// A known stand-in's source, or the material itself. Addresses.
uint64_t shader_source(uint64_t material);
// Internal ownership for a fallback that adopts another ready shader source.
bool track_shader_slot(uint64_t source, uint64_t part, uint32_t slot);

// Admit a runtime bundle's creator shader archive. The bundle service validates
// and owns native_path; metadata_json is the exporter's complete "shaders"
// object. Uses the same archive and game-material checks as the manifest path.
// Failures publish no set and never block the mod. A valid empty set has id 0.
TSUCHI_Result admit_asset_shaders(TSUCHI_Mod* mod, const char* native_path,
                                 const char* metadata_json, uint64_t& set_id);
// Ends only this mod's set. Safe after a prior release or mod stop. The donor
// materials' ordinary asset holds retain the asset service's existing lifetime.
void release_asset_shaders(TSUCHI_Mod* mod, uint64_t set_id);

} // namespace tsuchi
