/* Counts the world's steps and checks the pictures reported by the public time service. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription step_listener, picture_listener;
static int64_t last;
static unsigned steps, pictures;
static bool sequence, shown;

static void on_picture(void *user, const TSUCHI_TimePresented *event) {
    (void)user;
    ++pictures;
    shown = shown && event->fraction >= 0 && event->fraction <= 1 && event->seconds >= 0 && event->seconds < 1.0e30f;
    if (steps < 3) return;
    tsuchi_check_begin("presented");
    tsuchi_expect(shown, "every picture has a fraction from zero through one and finite nonnegative seconds");
    tsuchi_expect(pictures >= 1, "a picture was reported");
    tsuchi_check_end();
    tsuchi_check_begin("cancelled");
    tsuchi_expect_result(host->cancel(self, step_listener), TSUCHI_OK, "ending the step listener");
    tsuchi_expect_result(host->cancel(self, step_listener), TSUCHI_NOT_FOUND, "ending it twice");
    tsuchi_expect_result(host->cancel(self, picture_listener), TSUCHI_OK, "ending the picture listener");
    tsuchi_check_end();
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user;
    sequence = sequence && event->number >= 1 && (!steps || event->number == last + 1);
    last = event->number;
    if (++steps != 3) return;
    tsuchi_check_begin("steps");
    tsuchi_expect(sequence, "steps are numbered from one, consecutively");
    tsuchi_check_end();
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod, steps = pictures = 0, last = 0, sequence = shown = true;
    tsuchi_check_start(host, self);
    tsuchi_check_plan("steps"), tsuchi_check_plan("presented"), tsuchi_check_plan("cancelled");
    tsuchi_check_waiting("steps", "three steps of the world");
    tsuchi_check_waiting("presented", "a picture after those steps");
    tsuchi_check_waiting("cancelled", "both listeners to have reported");
    const TSUCHI_Result stepped = host->time->on_step(self, on_step, 0, &step_listener);
    const TSUCHI_Result pictured = host->time->on_presented(self, on_picture, 0, &picture_listener);
    if (stepped == TSUCHI_OK && pictured == TSUCHI_OK) return TSUCHI_OK;
    const char *const every[] = {"steps", "presented", "cancelled"};
    for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) {
        tsuchi_check_begin(every[i]);
        tsuchi_expect_result(stepped, TSUCHI_OK, "listening to steps");
        tsuchi_expect_result(pictured, TSUCHI_OK, "listening to pictures");
        tsuchi_check_end();
    }
    if (stepped == TSUCHI_OK) host->cancel(self, step_listener);
    if (pictured == TSUCHI_OK) host->cancel(self, picture_listener);
    return TSUCHI_OK;
}
