// The presenter's part for what moves in the world: the places of the parts the renderer draws and the poses of
// skinned ones, shown between steps, and the jumps that must not slide (DESIGN.md, "The presenter").
// presenter.cpp drives it.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// The parts drawn between steps. One left out is shown at its steps; `test_parts` in the framework's settings
// leaves parts out, to measure what each costs.
enum PresentedPart : uint32_t {
    kPartCamera = 1,
    kPartRoots = 2,
    kPartBones = 4,
    kPartCrowds = 8,
    kPartParticles = 16,
    kPartMarkers = 32,
    kPartMaterials = 64,
    kAllParts = 127,
};

// Where the pictures of a frame are: whether the presenter runs, the steps so far, the picture's place between
// the last two, the frame's number, and the parts drawn between steps.
struct PresentedMoment {
    bool active;
    uint64_t steps;
    float fraction;
    uint64_t frame;
    uint32_t parts;
};

// Hooks the world's parts and poses, and the game's warp. False when one could not be put on; what it keeps is
// then shown as at 30.
bool scene_start(TSUCHI_Mod* self);
// Changes only pose history, leaving a native-endpoint correction pending for a blended submission.
// Does not reset roots, views, or the world clock. The scene lock also guards worker submissions.
void scene_pose_policy(bool smooth);
// Told at the start of every frame.
void scene_moment(const PresentedMoment& moment);
// The first player's view of the picture, once the camera has it: its place and its field of view in degrees.
void scene_view(const float location[3], float field_of_view);
// Before the engine draws: sends the renderer again every part and pose still between steps that the game will
// not send itself this frame.
void scene_present();
// Forgets every part and pose.
void scene_forget();

// An actor jumped: its parts and their poses, and a camera that follows it, show where it is now, with no slide.
// Called by the warps and by the actor's events (entering play, a pool's taking and taking back).
void presenter_cut(uint64_t actor);
// Implemented by presenter.cpp: forgets the views of cameras that look at or follow the actor.
void presenter_cut_camera(uint64_t actor);

// What it did, for the log's row. Counted on any thread.
struct SceneCounts {
    uint32_t roots, root_blends, root_sends, bones, bone_blends, bone_sends, cuts, jumps, off_thread;
    uint32_t unseen, reduced, small; // not sent between steps: not drawn lately, a pose made less often, one small on the screen
    uint32_t small_roots, still_roots; // parts not sent between steps: small on the screen, or moving less than a pixel
    uint32_t snaps; // places and poses whose turn, move or size snapped across a step (presenter_logic.h), shown at once
};
SceneCounts scene_counts(bool clear);

} // namespace tsuchi
