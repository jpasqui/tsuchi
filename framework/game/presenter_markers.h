// The presenter's part for the marks on the screen that hang from a point in the world: life gauges, the cursor,
// prompts and the other markers (DESIGN.md, "The presenter"). presenter.cpp drives it.
#pragma once
#include <stdint.h>

#include "presenter_scene.h"
#include "tsuchi/core.h"

namespace tsuchi {

// Hooks the projection to the screen, the two ways a widget is placed, and the life gauges' placement. False
// when one could not be put on; the marks are then shown as at 30.
bool markers_start(TSUCHI_Mod* self);
// Told at the start of every frame.
void markers_moment(const PresentedMoment& moment);
// Before each picture, with the picture's view in force: moves every mark the newest step placed to where its
// point is between the last two steps, as the picture's view sees it.
void markers_present();
// Forgets every mark.
void markers_forget();

// A mod's widget hung from a point over an actor (ui.hold_mark), by the widget's canvas slot: the actor, the player
// controller whose view it is placed through, and the offset over the actor's place in centimeters. On every picture
// the slot is put where the point is between the last two steps, through that picture's view, as the game's own
// marks are, and its widget is hidden while the point is behind the camera. Changing the actor or the offset of a
// hung slot starts its steps again. False when there is no room. Game thread, as the rest.
bool markers_hang(uint64_t slot, uint64_t actor, uint64_t controller, const float offset[3]);
// Lets the slot go: its widget is shown as the mod had it, and the slot stays where it was last put. False when it
// was not hung.
bool markers_unhang(uint64_t slot);
// Whether the slot is hung still: it is let go by itself once its actor, its controller or the slot is gone.
bool markers_hung(uint64_t slot);
// The actor jumped: what hangs from it shows where it is now, with no slide.
void markers_cut(uint64_t actor);

// The game's action prompts, such as Ride over Oatchi (hud.prompts_shown, hud.hold_hidden_prompts). Starts keeping
// each one the game places, at any frame rate, by hooking the projection and the prompts' placement for `self` if the
// presenter has not: 2 when it starts now, 1 when it had, 0 when it cannot.
int markers_watch_prompts(TSUCHI_Mod* self);
// One bit for each player's screen on which the game shows a prompt pointing at the actor, whether a hold hides it
// or not.
uint32_t markers_prompt_screens(uint64_t actor);
// Hides the actor's prompts on those players' screens until markers_show_prompts(key); the same key changes it.
// False when there is no room.
bool markers_hide_prompts(uint64_t key, uint64_t actor, uint32_t players);
void markers_show_prompts(uint64_t key);

// What it did, for the log's row.
struct MarkerCounts {
    uint32_t marks, noted, repeated, moved;
    uint32_t hung, hung_moved, hung_hidden; // a mod's: hung, placed, hidden behind the camera
};
MarkerCounts marker_counts(bool clear);
// The game's code that projected points since the last call, as "offset:projected/noted" each, for the log. The counts start again.
void marker_callers(char* text, unsigned capacity);

} // namespace tsuchi
