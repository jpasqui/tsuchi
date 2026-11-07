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

// What it did, for the log's row.
struct MarkerCounts {
    uint32_t marks, noted, repeated, moved;
};
MarkerCounts marker_counts(bool clear);
// The game's code that projected points since the last call, as "offset:projected/noted" each, for the log. The counts start again.
void marker_callers(char* text, unsigned capacity);

} // namespace tsuchi
