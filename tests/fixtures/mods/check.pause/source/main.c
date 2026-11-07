/* Overlapping pause holds, their next-update effects and their public change events. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription frames, changes, controls;
static TSUCHI_Pause before, first_before, last_after;
static TSUCHI_PauseHold first, second;
static unsigned phase, heard, control_heard;
static bool first_control, last_control;
static TSUCHI_Result first_ended;
static const char *const names[] = {"listened", "held", "overlap", "restored", "events", "refused"};

static void on_change(void *user, const TSUCHI_GamePauseChanged *event) {
    (void)user;
    if (!heard) first_before = event->previous;
    last_after = event->paused;
    ++heard;
}
static void on_control(void *user, const TSUCHI_GameControlChanged *event) {
    (void)user;
    if (!control_heard) first_control = event->control;
    last_control = event->control;
    ++control_heard;
}

static void setup_failed(const char *why) {
    if (first.id) host->game->release_pause(self, first);
    if (second.id) host->game->release_pause(self, second);
    if (changes.id) host->cancel(self, changes);
    if (controls.id) host->cancel(self, controls);
    host->cancel(self, frames);
    for (unsigned i = 2; i < 5; ++i) {
        tsuchi_check_begin(names[i]);
        tsuchi_expect(false, why);
        tsuchi_check_end();
    }
    phase = 7;
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    const TSUCHI_GameService *api = host->game;
    if (phase == 0) {
        bool control = false;
        if (api->has_control(self, &control) != TSUCHI_OK || !control || api->paused(self, &before) != TSUCHI_OK) return;
        /* Subscribe while play is stable, then give the observer time to see its baseline. */
        tsuchi_check_begin("listened");
        const TSUCHI_Result listened = api->on_pause_changed(self, on_change, 0, &changes);
        const TSUCHI_Result control_listened = api->on_control_changed(self, on_control, 0, &controls);
        tsuchi_expect_result(listened, TSUCHI_OK, "listening for changes to pause kinds");
        tsuchi_expect_result(control_listened, TSUCHI_OK, "listening for changes to player control");
        tsuchi_check_end();
        tsuchi_check_begin("refused");
        TSUCHI_PauseHold empty = {0};
        tsuchi_expect_result(api->hold_pause(self, 0, &empty), TSUCHI_INVALID, "holding no kinds");
        tsuchi_expect_result(api->release_pause(self, (TSUCHI_PauseHold){0}), TSUCHI_INVALID, "a hold that does not exist");
        tsuchi_check_end();
        phase = listened == TSUCHI_OK && control_listened == TSUCHI_OK ? 1 : 7;
        if (phase == 7) {
            tsuchi_check_begin("held");
            tsuchi_expect(false, "the pause listener must start before holding kinds");
            tsuchi_check_end();
            setup_failed("the pause listener must start");
        }
    } else if (phase == 1) {
        /* Let the observer establish its baseline before any hold changes it. */
        phase = 2;
    } else if (phase == 2) {
        tsuchi_check_begin("held");
        const TSUCHI_Result held_first = api->hold_pause(self, TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS, &first);
        const TSUCHI_Result held_second = api->hold_pause(self, TSUCHI_PAUSE_ACTORS, &second);
        tsuchi_expect_result(held_first, TSUCHI_OK, "holding the timer and actors");
        tsuchi_expect_result(held_second, TSUCHI_OK, "holding actors a second time");
        tsuchi_expect(first.id && second.id && first.id != second.id, "the two holds have separate IDs");
        tsuchi_check_end();
        if (held_first != TSUCHI_OK || held_second != TSUCHI_OK) setup_failed("the listener and both pause holds must start");
        else phase = 3;
    } else if (phase == 3) {
        TSUCHI_Pause kinds = 0;
        const TSUCHI_Result read = api->paused(self, &kinds);
        bool control = true;
        const TSUCHI_Result control_read = api->has_control(self, &control);
        first_ended = read == TSUCHI_OK && control_read == TSUCHI_OK && !control && kinds == (before | TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS)
                           ? api->release_pause(self, first) : read == TSUCHI_OK ? TSUCHI_INVALID : read;
        phase = 4;
    } else if (phase == 4) {
        TSUCHI_Pause kinds = 0;
        tsuchi_check_begin("overlap");
        tsuchi_expect_result(first_ended, TSUCHI_OK, "both kinds were paused and control was lost at the next update, then the first hold ended");
        tsuchi_expect_result(api->release_pause(self, first), TSUCHI_NOT_FOUND, "the first hold ended only once");
        tsuchi_expect_result(api->paused(self, &kinds), TSUCHI_OK, "the next update's pause kinds");
        tsuchi_expect_number(kinds, before | TSUCHI_PAUSE_ACTORS, "the other hold still pauses actors");
        tsuchi_expect_result(api->release_pause(self, second), TSUCHI_OK, "ending the second hold");
        tsuchi_check_end();
        phase = 5;
    } else if (phase == 5) {
        TSUCHI_Pause kinds = 0;
        tsuchi_check_begin("restored");
        tsuchi_expect_result(api->paused(self, &kinds), TSUCHI_OK, "the pause kinds after both holds ended");
        tsuchi_expect_number(kinds, before, "the game's own pause kinds were restored");
        tsuchi_expect_result(api->release_pause(self, second), TSUCHI_NOT_FOUND, "the second hold ended only once");
        tsuchi_check_end();
        phase = 6;
    } else if (phase == 6) {
        /* Give the observer an update to deliver the restored state as well. */
        const unsigned expected = (unsigned)((before & (TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS)) != (TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS)) +
                                  (unsigned)((before & TSUCHI_PAUSE_TIMER) == 0) + (unsigned)((before & TSUCHI_PAUSE_ACTORS) == 0);
        tsuchi_check_begin("events");
        tsuchi_expect_number(heard, expected, "one event for each changed set of pause kinds");
        if (heard) {
            tsuchi_expect_number(first_before, before, "the first event's previous kinds");
            tsuchi_expect_number(last_after, before, "the last event's restored kinds");
        }
        tsuchi_expect_number(control_heard, 2, "control was lost and restored once");
        tsuchi_expect(!first_control && last_control, "the control events describe loss followed by restoration");
        bool control = false;
        tsuchi_expect_result(api->has_control(self, &control), TSUCHI_OK, "the restored player control");
        tsuchi_expect(control, "control is restored after both holds end");
        tsuchi_expect_result(host->cancel(self, controls), TSUCHI_OK, "ending the control listener");
        tsuchi_expect_result(host->cancel(self, controls), TSUCHI_NOT_FOUND, "ending it twice");
        tsuchi_expect_result(host->cancel(self, changes), TSUCHI_OK, "ending the pause listener");
        tsuchi_expect_result(host->cancel(self, changes), TSUCHI_NOT_FOUND, "ending it twice");
        tsuchi_expect_result(host->cancel(self, frames), TSUCHI_OK, "ending the frame listener");
        tsuchi_check_end();
        /* A failed transition is still cleaned up; it may have left the first hold active. */
        api->release_pause(self, first);
        api->release_pause(self, second);
        phase = 7;
    }
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod;
    phase = heard = control_heard = 0;
    first_control = last_control = false;
    before = first_before = last_after = 0;
    first.id = second.id = 0;
    frames.id = changes.id = controls.id = 0;
    first_ended = TSUCHI_NOT_READY;
    tsuchi_check_start(host, self);
    const TSUCHI_Result watched = host->time->on_step(self, on_step, 0, &frames);
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        tsuchi_check_plan(names[i]);
        if (watched == TSUCHI_OK) tsuchi_check_waiting(names[i], "stable play and seven world updates");
        else { tsuchi_check_begin(names[i]); tsuchi_expect_result(watched, TSUCHI_OK, "watching the game's updates"); tsuchi_check_end(); }
    }
    return TSUCHI_OK;
}
