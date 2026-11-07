// The frame tick: the one place the framework runs from, once per game frame.
#pragma once

namespace tsuchi {

// A frame in three parts. The game build calls frame_begin at the start of every engine frame, frame_step when
// that frame steps the world, before the world updates, and frame_picture for each picture the engine draws.
// frame_begin notes the thread it is called on as the game's (registry.h). frame_step runs every service's own
// per-step work, then tells mods of the step. frame_picture tells mods of the picture: `fraction` is where it is
// between the last two steps, and `seconds` the time since the picture before.
void frame_begin();
void frame_step();
// True during a world step: its services' work, its mods' handlers, and the work after them. Game thread only.
bool frame_stepping();
void frame_picture(float fraction, float seconds);

// One frame that steps the world and is shown as it is: all three. A test calls this.
void frame(float seconds);

// Adds a service's per-step work. It runs in the order it was added, and all of it runs before any mod hears
// the step. False when memory runs out.
bool each_frame(void (*step)());
// Adds work for every engine frame, whether or not the world steps in it (menus and loading screens too). It runs
// at the start of the frame, before any step. False when memory runs out.
bool each_engine_frame(void (*work)());
// Adds a service's work after every mod has heard the step, before the world updates. It runs in the order it
// was added. A picture that does not step the world runs neither. False when memory runs out.
bool after_step(void (*step)());

} // namespace tsuchi
