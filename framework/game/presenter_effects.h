// The presenter's part for what the game draws outside the scene's parts and poses: the instances its drawer adds
// each step (the Pikmin's tops, sprouts and carried pieces), Cascade particles, and the material values a step
// gives the renderer, such as the throw line's points (DESIGN.md, "The presenter").
// presenter.cpp drives it.
#pragma once
#include <stdint.h>

#include "presenter_scene.h"
#include "tsuchi/core.h"

namespace tsuchi {

// Hooks the drawer of instances, the drawing of a scene and the particles' routes. False when one could not be
// put on; what it keeps is then shown as at 30.
bool effects_start(TSUCHI_Mod* self, uint64_t (*clock_microseconds)());
// Told at the start of every frame.
void effects_moment(const PresentedMoment& moment);
// A view family of the picture was given this world time; its particles are drawn this far between the last two
// steps. Game thread.
void effects_family(float world_seconds, float fraction);
// Before the engine draws: puts every instance at its place between the last two steps, and gives the renderer
// each material value a step set at its value between the last two steps.
void effects_present();
// Forgets every instance's steps.
void effects_forget();

// What it did, for the log's row.
struct EffectCounts {
    uint32_t crowd_parts, crowd_instances, crowd_held, renders, renders_known, sprites, meshes;
    uint32_t counters_moved, counter_sends; // carry counters put between steps, and the sends of their quads
    uint32_t material_values, material_blends, material_restores; // kept, given between steps, given back
    uint32_t material_snaps; // values that wrapped or went back across a step, shown at once
    uint64_t rendering_us; // the render thread's drawing of scenes
    // The time from the start of each scene's drawing to the next one's, the render thread's own pace.
    uint32_t render_gaps;
    float render_gap_ms[240];
    // How long each scene's drawing took.
    uint32_t scenes_timed;
    float scene_ms[240];
};
EffectCounts effect_counts(bool clear);

} // namespace tsuchi
