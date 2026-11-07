#include "frame.h"

#include <string.h>

#include "registry.h"
#include "time_service.h"

namespace tsuchi {
namespace {

using Step = void (*)();
Step* steps = nullptr;
unsigned step_count = 0, step_capacity = 0;
Step* engine_frames = nullptr;
unsigned engine_count = 0, engine_capacity = 0;
Step* finishes = nullptr;
unsigned finish_count = 0, finish_capacity = 0;
long long step_number = 0;
bool stepping = false;

bool append(Step*& callbacks, unsigned& count, unsigned& capacity, Step callback) {
    if (!callback) return false;
    if (count == capacity) {
        const unsigned next = capacity ? capacity * 2 : 16;
        auto* grown = static_cast<Step*>(allocate(sizeof(Step) * next));
        if (!grown) return false;
        if (callbacks) memcpy(static_cast<void*>(grown), callbacks, sizeof(Step) * count);
        release(callbacks);
        callbacks = grown;
        capacity = next;
    }
    callbacks[count++] = callback;
    return true;
}

} // namespace

bool each_engine_frame(Step work) { return append(engine_frames, engine_count, engine_capacity, work); }
bool each_frame(Step step) { return append(steps, step_count, step_capacity, step); }
bool after_step(Step step) { return append(finishes, finish_count, finish_capacity, step); }

void frame_begin() {
    note_game_thread(); // the game's thread is the one its frames come on
    for (unsigned i = 0; i < engine_count; ++i) engine_frames[i]();
}

void frame_step() {
    const bool before = stepping;
    stepping = true;
    ++step_number;
    for (unsigned i = 0; i < step_count; ++i) steps[i]();
    if (time::step_wanted()) time::emit_step({step_number});
    for (unsigned i = 0; i < finish_count; ++i) finishes[i]();
    stepping = before;
}

bool frame_stepping() { return on_game_thread() && stepping; }

void frame_picture(float fraction, float seconds) {
    if (time::presented_wanted()) time::emit_presented({fraction, seconds});
}

void frame(float seconds) {
    frame_begin();
    frame_step();
    frame_picture(1.0f, seconds); // shown as it is: one picture per step, and all the way at it
}

} // namespace tsuchi
