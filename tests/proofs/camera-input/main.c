/* Explicit visual proof. Run alone in a played level at both 30 and 60, and keep screenshots. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"
static const TSUCHI_Host* host;
static const TSUCHI_Host* native_host;
static TSUCHI_Host captured_host;
static TSUCHI_ModService captured_mod;
static char observations[16384];
static unsigned observed_size;
static bool observations_dirty;
static TSUCHI_Result capture_log(TSUCHI_Mod* mod, const char* line) {
    const char* at = line;
    while (*at && observed_size + 2 < sizeof observations)
        observations[observed_size++] = *at++;
    if (observed_size + 1 < sizeof observations)
        observations[observed_size++] = '\n';
    observations[observed_size] = 0;
    observations_dirty = true;
    return native_host->mod->log(mod, line);
}
static TSUCHI_Mod* self;
static TSUCHI_Subscription steps, pictures;
static TSUCHI_Actor captain;
static TSUCHI_Zoom original_zoom;
static float original_heading;
static unsigned age;
static bool began, projection_reported, ready_reported;
static unsigned input_phase, input_at;
static bool synthetic_taken, finished, jump_pose_known;
static uint64_t picture_count;
static double picture_seconds;
static float jump_before_z, jump_peak_z;
static void result(const char* name, TSUCHI_Result value) {
    tsuchi_check_begin(name);
    tsuchi_expect_result(value, TSUCHI_OK, name);
    tsuchi_check_end();
}
static void on_picture(void* user, const TSUCHI_TimePresented* event) {
    (void)user;
    if (began) {
        ++picture_count;
        picture_seconds += event->seconds;
        if (jump_pose_known && age >= 420 && age <= 455) {
            TSUCHI_Pose pose;
            if (host->entity->pose(self, captain, &pose) == TSUCHI_OK && pose.location.z > jump_peak_z)
                jump_peak_z = pose.location.z;
        }
    }
    if (!began || age < 90 || age > 210 || projection_reported)
        return;
    TSUCHI_Pose pose;
    TSUCHI_ScreenPoint point;
    TSUCHI_ScreenSize size;
    if (host->entity->pose(self, captain, &pose) != TSUCHI_OK)
        return;
    if (host->camera->world_to_screen(self, 0, &pose.location, &point) != TSUCHI_OK)
        return;
    if (host->camera->screen_size(self, 0, &size) != TSUCHI_OK)
        return;
    char line[TSUCHI_CHECK_LINE] = "projected captain x/y ";
    tsuchi_check_add_real(line, point.x);
    tsuchi_check_add(line, "/");
    tsuchi_check_add_real(line, point.y);
    tsuchi_check_add(line, " viewport ");
    tsuchi_check_add_number(line, size.width);
    tsuchi_check_add(line, "/");
    tsuchi_check_add_number(line, size.height);
    host->mod->log(self, line);
    tsuchi_check_begin("projection");
    tsuchi_expect(point.x >= 0 && point.x <= size.width && point.y >= 0 && point.y <= size.height, "captain projects inside the viewport");
    tsuchi_check_end_with("compare the logged pixels with the captain in the screenshot");
    projection_reported = true;
}
static void physical(const TSUCHI_PadState* pad, const char* phase) {
    char line[TSUCHI_CHECK_LINE] = "physical PLUS ";
    tsuchi_check_add(line, phase);
    tsuchi_check_add(line, " age ");
    tsuchi_check_add_number(line, age);
    tsuchi_check_add(line, " held ");
    tsuchi_check_add_number(line, pad->held);
    tsuchi_check_add(line, " pressed ");
    tsuchi_check_add_number(line, pad->pressed);
    tsuchi_check_add(line, " connected ");
    tsuchi_check_add_number(line, pad->connected);
    host->mod->log(self, line);
}
static void timings(const char* phase) {
    char line[TSUCHI_CHECK_LINE] = "timing ";
    tsuchi_check_add(line, phase);
    tsuchi_check_add(line, " proof_steps ");
    tsuchi_check_add_number(line, age);
    tsuchi_check_add(line, " presented_pictures ");
    tsuchi_check_add_number(line, picture_count);
    tsuchi_check_add(line, " presented_seconds ");
    tsuchi_check_add_real(line, (float)picture_seconds);
    host->mod->log(self, line);
}
static void step_proof(void* user, const TSUCHI_TimeStep* event) {
    (void)user;
    (void)event;
    if (finished)
        return;
    TSUCHI_PadState pad = {0};
    if (host->input->pad(self, 0, &pad) != TSUCHI_OK)
        return;
    if (!began) {
        bool control = false;
        TSUCHI_View view;
        if (host->game->has_control(self, &control) != TSUCHI_OK || !control || host->player->captain(self, 0, &captain) != TSUCHI_OK ||
            host->camera->view(self, 0, &view) != TSUCHI_OK)
            return;
        if (!ready_reported) {
            host->mod->log(self, "camera-input ready, awaiting real ZL to begin");
            ready_reported = true;
        }
        if (!pad.connected || !(pad.held & TSUCHI_BUTTONS_ZL))
            return;
        began = true;
        result("physical-hide", host->input->hold_hidden(self, 0, TSUCHI_BUTTONS_PLUS));
        bool key = false;
        tsuchi_check_begin("keyboard");
        tsuchi_expect_result(host->input->key(self, 40, &key), TSUCHI_UNSUPPORTED, "Switch keyboard unsupported");
        tsuchi_check_end();
        host->mod->log(self, "camera-input begins on real ZL, look-at ages90-210 and heading270-330");
    }
    ++age;
    if (age == 30) {
        const TSUCHI_Look look = {captain, {0, 0, 0}};
        result("zoom-baseline", host->camera->zoom(self, 0, &original_zoom));
        TSUCHI_Zoom zoom = {original_zoom.distance * .9f, original_zoom.pitch};
        result("zoom", host->camera->hold_zoom(self, 0, &zoom));
        result("look", host->camera->hold_look(self, 0, &look));
    } else if (age == 240) {
        result("release-look", host->camera->release_look(self, 0));
        result("release-zoom", host->camera->release_zoom(self, 0));
        TSUCHI_Zoom zoom;
        tsuchi_check_begin("zoom-restored");
        tsuchi_expect_result(host->camera->zoom(self, 0, &zoom), TSUCHI_OK, "read immediately after release");
        tsuchi_expect_near(zoom.distance, original_zoom.distance, .01, "distance restored");
        tsuchi_expect_near(zoom.pitch, original_zoom.pitch, .01, "pitch restored");
        tsuchi_check_end();
        result("heading-baseline", host->camera->heading(self, 0, &original_heading));
        result("heading", host->camera->hold_heading(self, 0, original_heading + 45));
    } else if (age == 360) {
        result("captain-hurt-shake", host->camera->shake(self, 0, TSUCHI_SHAKE_KIND_CAPTAIN_HURT));
    } else if (age == 390) {
        result("stop-shakes", host->camera->stop_shakes(self, 0, true));
        result("release-heading", host->camera->release_heading(self, 0));
        float heading;
        tsuchi_check_begin("heading-restored");
        tsuchi_expect_result(host->camera->heading(self, 0, &heading), TSUCHI_OK, "read immediately after release");
        tsuchi_expect_near(heading, original_heading, .01, "heading restored");
        tsuchi_check_end();
        timings("camera-complete");
        host->mod->log(self, "camera phases complete, waiting for physical PLUS while hidden");
    }
    if (age == 420) {
        physical(&pad, "before-X-injection");
        TSUCHI_Pose pose;
        if (host->entity->pose(self, captain, &pose) == TSUCHI_OK) {
            jump_pose_known = true;
            jump_before_z = jump_peak_z = pose.location.z;
        }
        result("timed-X-take", host->input->hold_pressed(self, 0, TSUCHI_BUTTONS_X, 2));
        TSUCHI_Buttons pressed = 0;
        TSUCHI_PadState injected;
        tsuchi_check_begin("timed-X-active");
        tsuchi_expect_result(host->input->pressed(self, 0, &pressed), TSUCHI_OK, "read owned press");
        tsuchi_expect_number(pressed, TSUCHI_BUTTONS_X, "X active for two steps");
        tsuchi_expect_result(host->input->pad(self, 0, &injected), TSUCHI_OK, "read effective pad");
        tsuchi_expect((injected.held & TSUCHI_BUTTONS_X) != 0, "API pad sees X injection");
        tsuchi_expect((pad.held & TSUCHI_BUTTONS_X) == 0, "physical route did not press X");
        tsuchi_check_end();
    } else if (age == 421) {
        TSUCHI_Buttons pressed = 0;
        tsuchi_check_begin("timed-X-second-step");
        tsuchi_expect_result(host->input->pressed(self, 0, &pressed), TSUCHI_OK, "read owned press");
        tsuchi_expect_number(pressed, TSUCHI_BUTTONS_X, "still active on its second step");
        tsuchi_check_end();
    } else if (age == 422) {
        TSUCHI_Buttons pressed = TSUCHI_BUTTONS_X;
        tsuchi_check_begin("timed-X-expired");
        tsuchi_expect_result(host->input->pressed(self, 0, &pressed), TSUCHI_OK, "read owned press");
        tsuchi_expect_number(pressed, 0, "expired before the third step");
        tsuchi_expect((pad.held & TSUCHI_BUTTONS_X) == 0, "physical pad stays clear of X");
        tsuchi_check_end();
        physical(&pad, "after-X-expiry");
    } else if (age == 455 && jump_pose_known) {
        char line[TSUCHI_CHECK_LINE] = "X jump independently observed captain z before/peak/delta ";
        tsuchi_check_add_real(line, jump_before_z);
        tsuchi_check_add(line, "/");
        tsuchi_check_add_real(line, jump_peak_z);
        tsuchi_check_add(line, "/");
        tsuchi_check_add_real(line, jump_peak_z - jump_before_z);
        host->mod->log(self, line);
    }
    if (age < 420)
        return;
    if (input_phase == 0 && (pad.pressed & TSUCHI_BUTTONS_PLUS)) {
        physical(&pad, "hidden");
        input_phase = 1;
        input_at = age;
    } else if (input_phase == 1 && age >= input_at + 2) {
        TSUCHI_Pause paused = 0;
        tsuchi_check_begin("physical-hidden-ignored");
        tsuchi_expect_result(host->game->paused(self, &paused), TSUCHI_OK, "read native pause");
        tsuchi_expect_number(paused, 0, "real hidden PLUS did not open the map");
        tsuchi_check_end();
        result("release-hidden", host->input->release_hidden(self, 0));
        host->mod->log(self, "physical PLUS released, waiting for another real PLUS");
        input_phase = 2;
    } else if (input_phase == 2 && (pad.pressed & TSUCHI_BUTTONS_PLUS)) {
        physical(&pad, "visible");
        input_phase = 3;
        input_at = age;
    } else if (input_phase == 3 && age >= input_at + 2) {
        TSUCHI_Pause paused = 0;
        tsuchi_check_begin("physical-visible-acted");
        tsuchi_expect_result(host->game->paused(self, &paused), TSUCHI_OK, "read native pause");
        tsuchi_expect(paused != 0, "real visible PLUS opened the map");
        tsuchi_check_end();
        input_phase = 4;
    } else if (input_phase == 4 && age >= input_at + 30) {
        TSUCHI_Pause paused = 0;
        if (host->game->paused(self, &paused) == TSUCHI_OK && paused != 0) {
            result("synthetic-close-map", host->input->hold_pressed(self, 0, TSUCHI_BUTTONS_PLUS, 1));
            synthetic_taken = true;
        } else {
            host->mod->log(self, "physical route already closed the map, synthetic close skipped and remains unobserved");
        }
        input_phase = 5;
    } else if (input_phase == 5 && age >= input_at + 60) {
        TSUCHI_Pause paused = 0;
        TSUCHI_Buttons held = 1;
        tsuchi_check_begin(synthetic_taken ? "synthetic-acted-and-expired" : "physical-closed");
        tsuchi_expect_result(host->game->paused(self, &paused), TSUCHI_OK, "read native pause");
        tsuchi_expect_number(paused, 0, synthetic_taken ? "synthetic PLUS closed the map" : "physical route closed the map");
        if (synthetic_taken) {
            tsuchi_expect_result(host->input->pressed(self, 0, &held), TSUCHI_OK, "read injected press");
            tsuchi_expect_number(held, 0, "timed press expired");
        }
        tsuchi_check_end();
        timings("finished");
        host->mod->log(self, "camera-input proof finished");
        finished = true;
    }
}
static void on_step(void* user, const TSUCHI_TimeStep* event) {
    step_proof(user, event);
    if (observations_dirty && host->storage->write(self, "observations.log", observations, observed_size) == TSUCHI_OK) {
        observations_dirty = false;
        if (finished) {
            host->cancel(self, steps);
            host->cancel(self, pictures);
        }
    }
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host* given, TSUCHI_Mod* mod) {
    native_host = given;
    captured_host = *given;
    captured_mod = *given->mod;
    captured_mod.log = capture_log;
    captured_host.mod = &captured_mod;
    host = &captured_host;
    self = mod;
    tsuchi_check_start(host, self);
    TSUCHI_Result r = host->time->on_presented(self, on_picture, 0, &pictures);
    if (r != TSUCHI_OK)
        return r;
    return host->time->on_step(self, on_step, 0, &steps);
}
