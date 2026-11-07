/* Checks the presenter in play: the world's time is the sum of its steps, bit for bit; there are thirty steps a
 * second of the clock on the wall while the game keeps up; no frame steps twice; and there are as many pictures to
 * a step as the frame limit says. `limit.txt` holds the frame limit the run is made with. With `taps.txt`, the
 * number of presses of + the run makes, each one opens or closes the map. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription steps_heard, pictures_heard, pauses_heard;
static TSUCHI_Object statics, system_library;
static int64_t limit = 30, taps_wanted = -1;

#define TICKS_PER_SECOND 19200000ull
/* The time on the wall, in the system's ticks of 1/19,200,000 of a second. A build for this machine's checks counts
 * a sixtieth of a second each time it is asked. */
static uint64_t wall(void) {
#if defined(__aarch64__)
    register uint64_t ticks __asm__("x0");
    __asm__ volatile("svc 0x1E" : "=r"(ticks) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
    return ticks;
#else
    static uint64_t ticks;
    return ticks += TICKS_PER_SECOND / 60;
#endif
}

/* The value in a float, as the world keeps it. */
static float f32(double value) { return (float)value; }

static bool call_number(TSUCHI_Object object, const char *function, TSUCHI_Actor context, double *out) {
    TSUCHI_PreparedCall call = {0};
    TSUCHI_Value value = {0};
    if (host->reflection->begin_call(self, object, function, &call) != TSUCHI_OK) return false;
    TSUCHI_Result result = TSUCHI_OK;
    if (context.id) {
        const TSUCHI_Value given = {TSUCHI_VALUE_OBJECT, 0, {.object = tsuchi_actor_as_object(context)}};
        result = host->reflection->call_set(self, call, "WorldContextObject", &given);
    }
    if (result == TSUCHI_OK) result = host->reflection->invoke(self, call);
    if (result == TSUCHI_OK) result = host->reflection->call_get(self, call, "ReturnValue", &value);
    host->reflection->end_call(self, call);
    if (result != TSUCHI_OK) return false;
    if (value.kind == TSUCHI_VALUE_NUMBER) *out = value.as.number;
    else if (value.kind == TSUCHI_VALUE_INTEGER) *out = (double)value.as.integer;
    else return false;
    return true;
}

/* What the time check has seen. */
static bool timing = false, time_done = false, time_exact = true;
static float last_time;
static uint32_t advanced, held_still;
static double bad_before, bad_after;
/* The frame check. */
static bool frames_done = false;
static double last_frame = -1;
static uint32_t frame_steps, twice;
/* The windows of ten seconds. */
static uint64_t window_start, window_steps, window_pictures, fractions_at_one, fractions_between;
static float window_longest; /* the longest frame in the window: over a step, the game fell behind as it does at 30 */
static uint32_t windows_kept, windows_wrong, windows_slow, rate_done;
static uint64_t pictures_kept, steps_total;
static char rate_note[TSUCHI_CHECK_LINE];
/* The presses. */
static uint32_t opened, closed;
static bool paused_now = false;

static void end_window(void) {
    window_start = 0, window_steps = window_pictures = 0, window_longest = 0;
}

static void on_picture(void *user, const TSUCHI_TimePresented *event) {
    (void)user;
    if (window_start) {
        ++window_pictures;
        if (event->seconds > window_longest) window_longest = event->seconds;
        if (event->fraction >= 1) ++fractions_at_one;
        else ++fractions_between;
    }
}

static void finish_rate(void) {
    if (rate_done) return;
    rate_done = 1;
    tsuchi_check_begin("rate");
    char wrong[TSUCHI_CHECK_LINE] = "";
    tsuchi_check_add(wrong, limit == 30 ? "every window of ten seconds had as many steps as pictures; "
                                        : "every window of ten seconds while the game kept up had 299 to 301 steps; ");
    tsuchi_check_add(wrong, rate_note);
    tsuchi_expect(windows_wrong == 0, wrong);
    if (limit == 30) tsuchi_check_add(rate_note, " (at 30, a step a picture, as the game paces it)");
    tsuchi_expect(windows_kept >= 3, "three windows of ten seconds while the game kept up");
    tsuchi_check_end_with(rate_note);
    tsuchi_check_begin("ratio");
    const double ratio = steps_total ? (double)pictures_kept / (double)steps_total : 0;
    if (limit == 30) tsuchi_expect_near(ratio, 1.0, 0.02, "one picture a step at 30");
    else if (limit == 60) tsuchi_expect_near(ratio, 2.0, 0.1, "two pictures a step at 60");
    else tsuchi_expect(ratio >= 1.0, "at least one picture a step");
    char note[TSUCHI_CHECK_LINE] = "pictures a step ";
    tsuchi_check_add_real(note, ratio);
    tsuchi_check_add(note, ", between steps "), tsuchi_check_add_number(note, (int64_t)fractions_between);
    tsuchi_check_add(note, ", at a step "), tsuchi_check_add_number(note, (int64_t)fractions_at_one);
    if (limit == 30) tsuchi_expect(fractions_between == 0, "at 30 every picture is at its step");
    else tsuchi_expect(fractions_between > fractions_at_one / 2, "above 30 most pictures fall between steps");
    tsuchi_check_end_with(note);
}

static void window(bool running) {
    const uint64_t now = wall();
    if (!running) return end_window();
    if (!window_start) {
        window_start = now, window_steps = window_pictures = 0, window_longest = 0;
        return;
    }
    ++window_steps;
    if (now - window_start < 10 * TICKS_PER_SECOND) return;
    const double seconds = (double)(now - window_start) / TICKS_PER_SECOND;
    const double steps = (double)window_steps * 10.0 / seconds, pictures = (double)window_pictures * 10.0 / seconds;
    if (pictures >= 295 && window_longest <= 1.0f / 30.0f + 0.001f) {
        ++windows_kept;
        steps_total += window_steps, pictures_kept += window_pictures;
        // At 30 the presenter leaves the game as it is: a step every picture, at whatever rate the game is shown.
        if (limit == 30 ? window_steps + 1 < window_pictures || window_steps > window_pictures + 1 : steps < 299 || steps > 301) ++windows_wrong;
        if (windows_kept <= 4) {
            tsuchi_check_add(rate_note, windows_kept > 1 ? "; " : "steps/pictures in 10 s ");
            tsuchi_check_add_real(rate_note, steps), tsuchi_check_add(rate_note, "/"), tsuchi_check_add_real(rate_note, pictures);
        }
    } else {
        ++windows_slow;
    }
    window_start = now, window_steps = window_pictures = 0, window_longest = 0;
    if (windows_kept >= 4) finish_rate();
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    TSUCHI_Actor captain = {0};
    bool control = false;
    host->game->has_control(self, &control);
    if (host->player->captain(self, 0, &captain) != TSUCHI_OK || !control || paused_now) {
        timing = false;
        window(false);
        return;
    }
    window(true);
    /* No frame steps twice: the engine's count of frames differs at every step. */
    double frame = -1;
    if (!frames_done && call_number(system_library, "GetFrameCount", (TSUCHI_Actor){0}, &frame)) {
        if (last_frame >= 0) {
            ++frame_steps;
            if (frame == last_frame) ++twice;
        }
        last_frame = frame;
        if (frame_steps >= 600) {
            frames_done = true;
            tsuchi_check_begin("frames");
            tsuchi_expect_number(twice, 0, "steps in a frame that had stepped already");
            tsuchi_check_end_with("600 steps, each in a frame of its own");
        }
    }
    /* The world's time: before this step's update it is the sum of every update so far. */
    double seconds = 0;
    if (time_done || !call_number(statics, "GetTimeSeconds", captain, &seconds)) return;
    const float now = f32(seconds);
    if (timing) {
        if (now == last_time) ++held_still;
        else if (now == f32((double)last_time + (double)(1.0f / 30.0f))) ++advanced;
        else if (time_exact) time_exact = false, bad_before = last_time, bad_after = now;
    }
    timing = true;
    last_time = now;
    if (advanced >= 900 || !time_exact) {
        time_done = true;
        tsuchi_check_begin("time");
        char *why = time_exact ? 0 : tsuchi_check_failing("the world's time moved by other than a thirtieth");
        if (why) {
            tsuchi_check_add(why, ": from "), tsuchi_check_add_real(why, bad_before);
            tsuchi_check_add(why, " to "), tsuchi_check_add_real(why, bad_after);
        }
        char note[TSUCHI_CHECK_LINE] = "steps that moved the world's time by exactly a thirtieth ";
        tsuchi_check_add_number(note, advanced), tsuchi_check_add(note, ", held still "), tsuchi_check_add_number(note, held_still);
        tsuchi_check_end_with(note);
    }
}

static void on_pause(void *user, const TSUCHI_GamePauseChanged *event) {
    (void)user;
    const bool was = event->previous != 0, is = event->paused != 0;
    paused_now = is;
    if (!was && is) ++opened;
    if (was && !is) ++closed;
    if (taps_wanted > 0 && (int64_t)(opened + closed) >= taps_wanted) {
        tsuchi_check_begin("taps");
        tsuchi_expect_number(opened + closed, taps_wanted, "pauses changed by the presses");
        char note[TSUCHI_CHECK_LINE] = "opened ";
        tsuchi_check_add_number(note, opened), tsuchi_check_add(note, ", closed "), tsuchi_check_add_number(note, closed);
        tsuchi_check_end_with(note);
        taps_wanted = 0;
    }
}

static int64_t number_in(const char *path) {
    char buffer[32];
    const char *lines[1];
    if (tsuchi_check_lines(path, buffer, sizeof buffer, lines, 1) != 1) return -1;
    int64_t value = 0;
    for (const char *at = lines[0]; *at >= '0' && *at <= '9'; ++at) value = value * 10 + (*at - '0');
    return value;
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod;
    tsuchi_check_start(host, self);
    limit = number_in("limit.txt");
    if (limit < 0) limit = 30;
    taps_wanted = number_in("taps.txt");
    tsuchi_check_plan("time"), tsuchi_check_plan("frames"), tsuchi_check_plan("rate"), tsuchi_check_plan("ratio");
    tsuchi_check_plan("taps");
    if (taps_wanted > 0) {
        tsuchi_check_waiting("taps", "the run's presses of +");
    } else {
        tsuchi_check_begin("taps");
        tsuchi_check_end_with("this run makes no presses");
    }
    tsuchi_check_waiting("time", "900 steps of play");
    tsuchi_check_waiting("frames", "600 steps of play");
    tsuchi_check_waiting("rate", "four windows of ten seconds of play while the game keeps up");
    TSUCHI_Result found = host->reflection->find_class(self, "GameplayStatics", &statics);
    if (found == TSUCHI_OK) found = host->reflection->find_class(self, "KismetSystemLibrary", &system_library);
    if (found == TSUCHI_OK) found = host->time->on_step(self, on_step, 0, &steps_heard);
    if (found == TSUCHI_OK) found = host->time->on_presented(self, on_picture, 0, &pictures_heard);
    if (found == TSUCHI_OK) found = host->game->on_pause_changed(self, on_pause, 0, &pauses_heard);
    if (found != TSUCHI_OK) {
        tsuchi_check_begin("time");
        tsuchi_expect_result(found, TSUCHI_OK, "finding the classes and listening to steps, pictures and pauses");
        tsuchi_check_end();
    }
    return TSUCHI_OK;
}
