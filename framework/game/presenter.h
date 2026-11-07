// The presenter: the world steps thirty times a second at any frame rate, and the pictures between steps are
// drawn where they fall between the last two steps (DESIGN.md, "The presenter").
//
// With the frame limit at 30, the default, nothing of the game is changed: every frame steps the world and
// is shown as it is. Above 30 the engine is let run faster, the world's update is skipped on the frames that do
// not step, and the camera, the clocks the renderer is given, the widgets that count frames and the controllers
// are kept right for the pictures between.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Hooks what the presenter needs. Called once when the framework starts, after hooks can be put on.
// `microseconds` tells the time, and `wait_us` holds the game's thread that many microseconds: a picture between
// steps waits for its time, and a test that takes pictures slowly waits each frame. Without the right patch the
// presenter stays at 30.
void presenter_start(TSUCHI_Mod* self, uint64_t (*microseconds)(), void (*wait_us)(uint32_t));
// Reads the framework's own settings, `rom:/tsuchi/settings.json`. Called once the game's files can be read.
void presenter_settings();
// At the start of each engine frame, with its length: whether the world steps in this frame.
bool presenter_frame(float seconds);
// At the end of the framework's own work in each engine frame, for the log's times.
void presenter_frame_done();
// True while the world's update runs a step.
bool presenter_in_step();
// True when pictures are told from the engine's drawing; false when the frame has to tell its own.
bool presenter_draws();
// The frame limit asked for, and the one in force.
uint32_t presenter_limit_wanted();
uint32_t presenter_limit();

// A mod policy takes precedence over the file while held. Release uses its latest values.
TSUCHI_Result presenter_request(bool held, uint32_t frame_limit, uint32_t sync);
void presenter_policy(uint32_t& frame_limit, uint32_t& sync);
// A separate pose policy: 0 keeps size/cadence omissions, 1 admits qualified small/sparse poses.
TSUCHI_Result presenter_pose_request(bool held, uint32_t mode);
uint32_t presenter_pose_policy();
void presentation_start();

// The input service's filter. It is given a controller's buttons after the device's read and the presenter's
// latch, with `step` true during a world step, and returns the buttons the game should see.
void presenter_camera_after(void (*apply)(uint64_t player, uint64_t view));
bool presenter_drawing_view();
void presenter_input_after(uint64_t (*filter)(uint32_t pad, uint64_t buttons, bool step));
// True while the presenter reads a controller itself, so a hook on the system's read leaves it alone.
bool presenter_sampling_input();
// Reads a controller's state from the system, with the taps latched between pictures added. Game thread only.
// False when the controller is of no style the game reads.
bool presenter_sample_input(uint32_t pad, void* state);

} // namespace tsuchi
